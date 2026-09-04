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
#include "persistence.h"

/* Strip IRCv3 name prefixes: "draft/" and vendor scopes, which are DNS
 * names ("evilnet.github.io/").  Loops so a vendor-scoped draft name
 * ("vendor.tld/draft/x") resolves too. */
static const char *
strip_namespace (const char *name)
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

	base = strip_namespace (name);
	if (g_ascii_strncasecmp (base, "persistence", 11) != 0)
		return NULL;
	if (base[11] == '\0')
		return base + 11;			/* "" — the bare name */
	if (base[11] == '/')
		return base + 12;			/* sub-path */
	return NULL;
}

gboolean
persistence_is_batch_type (const char *type)
{
	const char *sub = persistence_match (type);

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
}
