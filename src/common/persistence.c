/* PoxChat
 * Copyright (C) 2026 PoxChat Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * IRCv3 draft/persistence — client side (Nefarious persistent sessions).
 * Spec (work in progress):
 *   https://gist.github.com/MrLenin/491f87ea4d95625a90ce525a804dbddb
 *
 * What the server sends us today without our asking for the cap:
 *   - a "metadata" batch right after the MOTD syncing our own
 *     server-managed keys, e.g.
 *       :server METADATA <nick> draft/persistence/hold private :1
 *   - the draft/persistence restoration batch (JOIN + 332/333/353/366
 *     per channel), handled in wire order in inbound_batch_add_message.
 */

#include "config.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "poxchat.h"
#include "poxchatc.h"
#include "text.h"
#include "server.h"
#include "scrollback.h"
#include "persistence.h"

/* Strip IRCv3 name prefixes: "draft/" and vendor scopes, which are DNS
 * names ("evilnet.github.io/").  Loops so a vendor-scoped draft name
 * ("vendor.tld/draft/x") resolves too. */
const char *
persistence_strip_namespace (const char *name)
{
	for (;;)
	{
		const char *slash = strchr (name, '/');
		gsize len;

		if (!slash)
			return name;
		len = (gsize) (slash - name);
		if (len == 5 && g_ascii_strncasecmp (name, "draft", 5) == 0)
			name = slash + 1;
		else if (len > 0 && memchr (name, '.', len))
			name = slash + 1;
		else
			return name;
	}
}

const char *
persistence_match (const char *name)
{
	const char *base;

	if (!name || !name[0])
		return NULL;

	base = persistence_strip_namespace (name);
	if (g_ascii_strncasecmp (base, "persistence", 11) != 0)
		return NULL;
	if (base[11] == '\0')
		return base + 11;			/* "" — the bare name */
	if (base[11] == '/')
		return base + 12;			/* sub-path */
	return NULL;
}

gboolean
persistence_is_bare_name (const char *name)
{
	const char *sub = persistence_match (name);

	return sub != NULL && sub[0] == '\0';
}

static void persistence_print (server *serv, time_t stamp,
                               const char *fmt, ...) G_GNUC_PRINTF (3, 4);

static void
persistence_print (server *serv, time_t stamp, const char *fmt, ...)
{
	va_list ap;
	char *msg;

	va_start (ap, fmt);
	msg = g_strdup_vprintf (fmt, ap);
	va_end (ap);
	EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session, msg,
	                       serv->servername, NULL, NULL, 0, stamp);
	g_free (msg);
}

static gboolean
value_is_on (const char *value)
{
	return value &&
	       (strcmp (value, "1") == 0 ||
	        g_ascii_strcasecmp (value, "on") == 0 ||
	        g_ascii_strcasecmp (value, "true") == 0);
}

/* CAP LS may carry the optional verbs the server implements as a
 * comma-separated value:
 *   draft/persistence=replay-control,profile,attach,attach-cursor
 * Each token may itself be namespaced.  The spec is explicit that the
 * value is a hint and not an inventory: unknown tokens are ignored, and
 * an absent token does not mean the verb is unsupported — so nothing
 * here gates a command, it only informs what we volunteer to try.
 *
 * The value is the whole truth of the advertisement it came in, so the
 * bits are cleared first: a CAP NEW that re-advertises the cap with a
 * narrower value retracts the tokens it dropped. */
void
persistence_parse_cap_value (server *serv, const char *value)
{
	char **tokens;
	int i;

	if (!serv || !value)
		return;

	serv->persistence_tok_replay_control = FALSE;
	serv->persistence_tok_profile = FALSE;
	serv->persistence_tok_attach = FALSE;
	serv->persistence_tok_detach = FALSE;
	serv->persistence_tok_list = FALSE;
	serv->persistence_tok_attach_cursor = FALSE;

	tokens = g_strsplit (value, ",", 0);
	for (i = 0; tokens[i]; i++)
	{
		const char *tok = persistence_strip_namespace (tokens[i]);

		if (!tok[0])
			continue;
		if (g_ascii_strcasecmp (tok, "replay-control") == 0)
			serv->persistence_tok_replay_control = TRUE;
		else if (g_ascii_strcasecmp (tok, "profile") == 0)
			serv->persistence_tok_profile = TRUE;
		else if (g_ascii_strcasecmp (tok, "attach") == 0)
			serv->persistence_tok_attach = TRUE;
		else if (g_ascii_strcasecmp (tok, "detach") == 0)
			serv->persistence_tok_detach = TRUE;
		else if (g_ascii_strcasecmp (tok, "list") == 0)
			serv->persistence_tok_list = TRUE;
		else if (g_ascii_strcasecmp (tok, "attach-cursor") == 0)
			serv->persistence_tok_attach_cursor = TRUE;
	}
	g_strfreev (tokens);
}

/* A msgid longer than this is not one we can have stored, and a cursor
 * that pushes the line past tcp_sendf's 1540-byte static buffer would be
 * truncated there — losing our CRLF, so the CAP END that follows would
 * be glued onto the ATTACH.  Drop the cursor rather than risk that. */
#define PERSISTENCE_MAX_CURSOR_LEN 256

/* PERSISTENCE ATTACH <profile> [<msgid>] — pick the profile this
 * connection is a view onto.  The server accepts it only between SASL
 * completion and CAP END, so this is called from that one window in the
 * registration flight and no reply is waited for: the flight must not
 * stall on a server that never answers.
 *
 * The attach token is deliberately not required.  The spec calls the CAP
 * value a hint and not an inventory, and says a client MUST NOT read a
 * missing token as a missing feature; a server that really lacks ATTACH
 * answers FAIL, which persistence_handle_fail turns back into legacy
 * behaviour.  attach-cursor does gate the cursor, because supplying one
 * is consent to an unsolicited chathistory-shaped replay of everything
 * since — and persistence_cursor_sent then suppresses our own catch-up,
 * so guessing wrong there would lose history rather than a round trip. */
void
persistence_send_attach (server *serv)
{
	const char *network;
	scrollback_db *db;
	char *cursor = NULL;

	if (!serv || !serv->have_persistence)
		return;
	if (!serv->persist_profile[0])
		return;			/* not configured — legacy behaviour */
	if (strpbrk (serv->persist_profile, " \r\n"))
		return;			/* never let a bad name break the registration flight */
	if (serv->persistence_attached)
		return;			/* at most one ATTACH per connection */

	if (serv->persistence_tok_attach_cursor)
	{
		network = server_get_network (serv, FALSE);
		db = network ? scrollback_open (network) : NULL;
		if (db)
			cursor = scrollback_get_global_newest_msgid (db);
		if (cursor && strlen (cursor) > PERSISTENCE_MAX_CURSOR_LEN)
		{
			g_free (cursor);
			cursor = NULL;
		}
	}

	if (cursor)
		tcp_sendf (serv, "PERSISTENCE ATTACH %s %s\r\n", serv->persist_profile, cursor);
	else
		tcp_sendf (serv, "PERSISTENCE ATTACH %s\r\n", serv->persist_profile);

	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = (cursor != NULL);
	serv->persistence_attach_pending = TRUE;
	g_free (cursor);
}

/* TRUE while the server is *expected* to replay every buffer from the
 * cursor we gave it: our own per-channel catch-up would fetch the same
 * lines again, so chathistory holds off while this holds.
 *
 * Expected, not promised.  The two flags below are our side of the
 * bargain — we asked, and nothing has said no — but the server may still
 * replay nothing at all: the user's PERSISTENCE REPLAY is OFF, server
 * policy declines, or the session is fresh enough that the cursor is
 * still known and there is simply no gap.  None of those produce a FAIL.
 * So the gate closes again the moment the replay is real
 * (chathistory_replay_wrapper_begin sets persistence_replay_seen at the
 * first bouncer-replay wrapper START), and the callers treat it as a
 * delay rather than a cancellation. */
gboolean
persistence_server_drives_replay (server *serv)
{
	return serv && serv->persistence_attached && serv->persistence_cursor_sent &&
	       !serv->persistence_replay_seen;
}

/* Registration finished (001).  The server accepts ATTACH only between
 * SASL completion and CAP END, so the window in which a FAIL could be
 * the answer to ours has closed: whatever arrives from here on is about
 * something else.  A server that neither acked nor failed our ATTACH
 * would otherwise leave attach_pending set for the whole connection,
 * handing the next unrelated FAIL the power to drop the replay gate. */
void
persistence_registration_complete (server *serv)
{
	if (!serv)
		return;
	serv->persistence_attach_pending = FALSE;
}

#define PERSISTENCE_MAX_WORDS 8

/* Split a PERSISTENCE reply's arguments in place.  Runs of spaces are
 * skipped; a word starting with ':' is the trailing parameter, so the
 * rest of the line — spaces included, colon removed — becomes one word.
 * Returns the word count. */
static int
persistence_split (char *buf, char **words)
{
	int n = 0;
	char *p = buf;

	while (*p && n < PERSISTENCE_MAX_WORDS)
	{
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (*p == ':')
		{
			words[n++] = p + 1;
			break;
		}
		words[n++] = p;
		while (*p && *p != ' ')
			p++;
		if (*p)
			*p++ = '\0';
	}
	return n;
}

void
persistence_handle_reply (server *serv, const char *args, time_t stamp)
{
	char *w[PERSISTENCE_MAX_WORDS];
	gboolean profile;
	char *buf;
	int n;

	if (!serv || !args)
		return;

	while (*args == ' ')
		args++;

	buf = g_strdup (args);
	n = persistence_split (buf, w);
	if (n == 0)					/* ":server PERSISTENCE" with no arguments */
	{
		g_free (buf);
		return;
	}
	profile = g_ascii_strcasecmp (w[0], "PROFILE") == 0;

/* buf is byte-identical to args apart from the terminators we wrote, so
 * w[i] - buf is also the offset in args: REST(i) is word i onward with
 * its spaces intact. */
#define REST(i) (args + (w[(i)] - buf))

	if (g_ascii_strcasecmp (w[0], "STATUS") == 0 && n >= 2)
	{
		/* STATUS <client-setting> <effective-setting>; the one-argument
		 * form is an earlier revision of the spec that deployed servers
		 * may still send.  Either way this is the authoritative "the
		 * server holds my session" signal, sent unsolicited between the
		 * last 005 and the end of the MOTD. */
		serv->persistence_status_known = TRUE;
		serv->persistence_effective = value_is_on (n >= 3 ? w[2] : w[1]);
		if (n >= 3)
			persistence_print (serv, stamp,
				_("Persistence: preference %s, effective %s"), w[1], w[2]);
		else
			persistence_print (serv, stamp,
				_("Persistence: effective %s"), w[1]);
	}
	else if (g_ascii_strcasecmp (w[0], "SET") == 0 && n >= 2)
		persistence_print (serv, stamp,
			_("Persistence: preference set to %s"), w[1]);
	else if (g_ascii_strcasecmp (w[0], "REPLAY") == 0 && n >= 4 &&
	         g_ascii_strcasecmp (w[1], "STATUS") == 0)
		persistence_print (serv, stamp,
			_("Persistence replay: preference %s, effective %s"), w[2], w[3]);
	else if (g_ascii_strcasecmp (w[0], "REPLAY") == 0 && n >= 3 &&
	         g_ascii_strcasecmp (w[1], "SET") == 0)
		persistence_print (serv, stamp,
			_("Persistence replay: preference set to %s"), w[2]);
	else if (profile && n >= 2 && g_ascii_strcasecmp (w[1], "ENDOFLIST") == 0)
		persistence_print (serv, stamp, _("Persistence: end of profile list"));
	else if (profile && n >= 3 && g_ascii_strcasecmp (w[1], "CREATED") == 0)
		/* The spec's acknowledgement always carries parent=<parent>; a
		 * server that omits it created the profile under the implicit
		 * default, so the fallback reads like the real attribute. */
		persistence_print (serv, stamp, _("Persistence: profile %s created (%s)"),
		                   w[2], n >= 4 ? REST (3) : "parent=default");
	else if (profile && n >= 3 && g_ascii_strcasecmp (w[1], "DELETED") == 0)
		persistence_print (serv, stamp, _("Persistence: profile %s deleted"), w[2]);
	else if (profile && n >= 4 && g_ascii_strcasecmp (w[1], "RENAMED") == 0)
		persistence_print (serv, stamp, _("Persistence: profile %s renamed to %s"),
		                   w[2], w[3]);
	else if (profile && n == 2)
		/* LIST line for a profile with no reported attributes. */
		persistence_print (serv, stamp, _("Persistence: profile %s"), w[1]);
	else if (profile && n >= 3 && strchr (w[2], '='))
		/* LIST line: the attributes are key=value pairs, and the spec
		 * says to tolerate ones we do not know, so show them verbatim. */
		persistence_print (serv, stamp, _("Persistence: profile %s (%s)"),
		                   w[1], REST (2));
	else if (profile && n >= 4)
		/* GET reply / SET acknowledgement with an effective value. */
		persistence_print (serv, stamp, _("Persistence: profile %s: %s = %s"),
		                   w[1], w[2], w[3]);
	else if (profile && n == 3)
		/* Same, without the trailing parameter: the key is unset. */
		persistence_print (serv, stamp, _("Persistence: profile %s: %s unset"),
		                   w[1], w[2]);
	else if (g_ascii_strcasecmp (w[0], "ATTACH") == 0 && n >= 2)
	{
		/* Our ATTACH was accepted: a later FAIL is about something else
		 * and must not drop the replay gate. */
		serv->persistence_attach_pending = FALSE;
		persistence_print (serv, stamp,
			_("Persistence: attached to profile %s"), w[1]);
	}
	else if (g_ascii_strcasecmp (w[0], "DETACH") == 0 && n >= 2 &&
	         g_ascii_strcasecmp (w[1], "OK") == 0)
		persistence_print (serv, stamp,
			_("Persistence: detached, the session was released"));
	else if (g_ascii_strcasecmp (w[0], "DETACH") == 0 && n >= 2 &&
	         g_ascii_strcasecmp (w[1], "NOSESSION") == 0)
		persistence_print (serv, stamp, _("Persistence: no session to detach"));
	else
		/* LIST, SESSION and whatever a newer server adds: show it rather
		 * than swallow it. */
		persistence_print (serv, stamp, _("Persistence: %s"), args);

#undef REST

	g_free (buf);
}

void
persistence_handle_fail (server *serv, const char *code, const char *context,
                         const char *text, time_t stamp)
{
	const char *bare;

	if (!serv)
		return;

	/* Codes are matched — and shown — with the namespace stripped, so a
	 * vendor-scoped code reads the same as the spec's bare one. */
	bare = code ? persistence_strip_namespace (code) : "";

	/* Only the FAIL that answers our own registration-time ATTACH may
	 * touch the replay gate.  A later FAIL — /persistence attach from
	 * the user, a PROFILE DELETE refused because the profile is an
	 * ancestor — carries the same codes but says nothing about whether
	 * the server is still replaying from our cursor. */
	if (serv->persistence_attach_pending)
	{
		if (g_ascii_strcasecmp (bare, "CURSOR_UNKNOWN") == 0)
			/* Server evicted our anchor: it falls back to last-activity
			 * replay, which may be short.  Opening the gate is all that
			 * is needed — this FAIL always arrives before 001, so the
			 * regular call sites still run afterwards and now find the
			 * gate open (366 -> chathistory_schedule_deferred, end of
			 * login -> chathistory_request_targets_on_reconnect).
			 * Calling them from here instead would send CHATHISTORY
			 * TARGETS before registration and arm a LATEST timer before
			 * a single channel had been rejoined. */
			serv->persistence_cursor_sent = FALSE;
		else if (g_ascii_strcasecmp (bare, "ACCOUNT_REQUIRED") == 0 ||
		         g_ascii_strcasecmp (bare, "INVALID_PARAMETERS") == 0)
		{
			/* ATTACH was rejected outright — a plain client this session. */
			serv->persistence_attached = FALSE;
			serv->persistence_cursor_sent = FALSE;
		}
		/* ATTACH is the only command outstanding in the window between
		 * SASL and CAP END, so any FAIL there is its answer. */
		serv->persistence_attach_pending = FALSE;
	}

	if (context && context[0])
		persistence_print (serv, stamp, _("Persistence error %s (%s): %s"),
		                   bare, context, text ? text : "");
	else
		persistence_print (serv, stamp, _("Persistence error %s: %s"),
		                   bare, text ? text : "");
}

gboolean
persistence_handle_metadata (server *serv, const char *target,
                             const char *key, const char *value,
                             time_t stamp)
{
	gboolean ours;

	if (!serv || !key)
		return FALSE;

	/* Server-managed keys are private to the account, so a notification
	 * is about us unless the target says otherwise. */
	ours = !target || strcmp (target, "*") == 0 ||
	       serv->p_cmp (target, serv->nick) == 0;

	/* hold — the account-scope persistence preference the server keeps
	 * in response to PERSISTENCE SET.  On means the server holds this
	 * session across disconnects and restores channel state itself on
	 * reconnect (the draft/persistence batch).  Learned after the MOTD,
	 * so it is state for later decisions, not the autojoin gate — that
	 * needs the pre-MOTD PERSISTENCE STATUS line, which requires
	 * negotiating the cap. */
	if (ours && g_ascii_strcasecmp (key, "hold") == 0)
	{
		gboolean known = value != NULL;
		gboolean on = value_is_on (value);

		if (known == (gboolean) serv->persistence_hold_known &&
		    on == (gboolean) serv->persistence_hold)
			return TRUE;			/* resync of what we already know */

		serv->persistence_hold_known = known;
		serv->persistence_hold = on;
		if (!known)
			persistence_print (serv, stamp,
				_("Persistence: hold preference cleared, the server default applies"));
		else if (on)
			persistence_print (serv, stamp,
				_("Persistence: the server is holding this session across disconnects"));
		else
			persistence_print (serv, stamp,
				_("Persistence: the server is not holding this session"));
		return TRUE;
	}

	/* Anything else under the prefix (profile/..., future keys): show it
	 * tersely rather than as a raw metadata line. */
	if (value)
		persistence_print (serv, stamp, ours ? _("Persistence: %s = %s")
		                                     : _("Persistence [%s]: %s = %s"),
		                   ours ? key : target, ours ? value : key, value);
	else
		persistence_print (serv, stamp, ours ? _("Persistence: %s unset")
		                                     : _("Persistence [%s]: %s unset"),
		                   ours ? key : target, key);
	return TRUE;
}

void
persistence_reset (server *serv)
{
	if (!serv)
		return;
	serv->persistence_hold_known = FALSE;
	serv->persistence_hold = FALSE;
	serv->have_persistence = FALSE;
	serv->persistence_tok_replay_control = FALSE;
	serv->persistence_tok_profile = FALSE;
	serv->persistence_tok_attach = FALSE;
	serv->persistence_tok_detach = FALSE;
	serv->persistence_tok_list = FALSE;
	serv->persistence_tok_attach_cursor = FALSE;
	serv->persistence_status_known = FALSE;
	serv->persistence_effective = FALSE;
	serv->persistence_attached = FALSE;
	serv->persistence_cursor_sent = FALSE;
	serv->persistence_attach_pending = FALSE;
	serv->persistence_replay_seen = FALSE;
}
