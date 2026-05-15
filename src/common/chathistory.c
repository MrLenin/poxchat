/* PoxChat
 * Copyright (C) 2024 PoxChat Contributors
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
 * IRCv3 draft/chathistory implementation
 * See https://ircv3.net/specs/extensions/chathistory
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "poxchat.h"
#include "poxchatc.h"
#include "chathistory.h"
#include "server.h"
#include "inbound.h"
#include "text.h"
#include "fe.h"
#include "proto-irc.h"
#include "modes.h"
#include "scrollback.h"

/* Forward declarations */
static void schedule_background_fetch (session *sess);
static void schedule_before_catchup (server *serv);
static void chathistory_replay_mode (session *sess, char *nick,
                                     char *mode_str, char **params,
                                     int param_count,
                                     const message_tags_data *tags_data);

/* Get effective limit for a CHATHISTORY request.
 * Prefers the server-advertised ISUPPORT CHATHISTORY=N value (the server is
 * telling us the optimal batch size).  Falls back to CHATHISTORY_DEFAULT_LIMIT
 * when the server doesn't advertise one, and never exceeds CHATHISTORY_MAX_LIMIT
 * as a safety cap. */
static int
get_effective_limit (server *serv, int requested)
{
	int limit;

	/* Use server-advertised limit when available, otherwise the caller's value */
	if (serv->chathistory_limit > 0)
		limit = serv->chathistory_limit;
	else if (requested > 0)
		limit = requested;
	else
		limit = CHATHISTORY_DEFAULT_LIMIT;

	if (limit > CHATHISTORY_MAX_LIMIT)
		limit = CHATHISTORY_MAX_LIMIT;

	return limit;
}

/* Get the target name for a session (channel or nick for query) */
static const char *
get_target_name (session *sess)
{
	if (sess->type == SESS_CHANNEL)
		return sess->channel;
	else if (sess->type == SESS_DIALOG)
		return sess->channel; /* For dialogs, channel holds the nick */
	else
		return NULL;
}

/* --- Request queue infrastructure --- */

chreq *
chreq_new (chreq_type type, const char *reference, const char *end_ref,
           int limit, chreq_priority priority,
           gboolean is_catchup, gboolean used_msgid)
{
	chreq *req = g_new0 (chreq, 1);
	req->type = type;
	req->reference = g_strdup (reference);
	req->end_ref = g_strdup (end_ref);
	req->limit = limit;
	req->priority = priority;
	req->is_catchup = is_catchup ? 1 : 0;
	req->used_msgid = used_msgid ? 1 : 0;
	return req;
}

void
chreq_free (chreq *req)
{
	if (!req)
		return;
	g_free (req->reference);
	g_free (req->end_ref);
	g_free (req);
}

/* Check if two requests are duplicates (same type + same reference) */
static gboolean
chreq_is_dup (const chreq *a, const chreq *b)
{
	if (!a || !b)
		return FALSE;
	if (a->type != b->type)
		return FALSE;
	return g_strcmp0 (a->reference, b->reference) == 0;
}

/* Dispatch a request onto the wire immediately. Sets session flags. */
static void
chathistory_dispatch_now (session *sess, chreq *req)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	target = get_target_name (sess);
	if (!target || !target[0])
	{
		chreq_free (req);
		return;
	}

	if (!serv->have_chathistory || !serv->connected)
	{
		chreq_free (req);
		return;
	}

	sess->ch_active = req;

	/* Set compatibility flags from request */
	sess->history_loading = TRUE;
	sess->history_request_is_before = (req->type == CHREQ_BEFORE);
	sess->history_request_is_after = (req->type == CHREQ_AFTER);
	sess->history_request_used_msgid = req->used_msgid;

	effective_limit = get_effective_limit (serv, req->limit);

	switch (req->type)
	{
	case CHREQ_LATEST:
		{
			const char *ref = (req->reference && req->reference[0]) ? req->reference : "*";
			tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
			                           "CHATHISTORY LATEST %s %s %d\r\n",
			                           target, ref, effective_limit);
		}
		break;

	case CHREQ_BEFORE:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY BEFORE %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_AFTER:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY AFTER %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_AROUND:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY AROUND %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_BETWEEN:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY BETWEEN %s %s %s %d\r\n",
		                           target, req->reference, req->end_ref,
		                           effective_limit);
		break;
	}
}

void
chathistory_submit (session *sess, chreq *req)
{
	if (!sess || !req)
	{
		chreq_free (req);
		return;
	}

	/* Dedup against active request */
	if (chreq_is_dup (sess->ch_active, req))
	{
		chreq_free (req);
		return;
	}

	/* Dedup against pending request */
	if (chreq_is_dup (sess->ch_pending, req))
	{
		chreq_free (req);
		return;
	}

	/* No active request → dispatch immediately */
	if (!sess->ch_active)
	{
		chathistory_dispatch_now (sess, req);
		return;
	}

	/* Active request exists → queue as pending */
	if (!sess->ch_pending || req->priority > sess->ch_pending->priority)
	{
		/* Replace lower-priority pending with this request */
		chreq_free (sess->ch_pending);
		sess->ch_pending = req;
	}
	else
	{
		/* Equal or lower priority — drop */
		chreq_free (req);
	}
}

void
chathistory_request_complete (session *sess)
{
	chreq *pending;

	if (!sess)
		return;

	chreq_free (sess->ch_active);
	sess->ch_active = NULL;
	sess->history_loading = FALSE;

	/* Dispatch pending request if any */
	pending = sess->ch_pending;
	if (pending)
	{
		sess->ch_pending = NULL;
		chathistory_dispatch_now (sess, pending);
	}
}

void
chathistory_queue_free (session *sess)
{
	if (!sess)
		return;
	chreq_free (sess->ch_active);
	sess->ch_active = NULL;
	chreq_free (sess->ch_pending);
	sess->ch_pending = NULL;
	sess->history_loading = FALSE;
}

/* --- Request API --- */

/* Infer request priority from session state */
static chreq_priority
infer_priority (session *sess)
{
	if (sess->background_history_active)
		return CHREQ_PRI_BACKGROUND;
	if (sess->catchup_in_progress)
		return CHREQ_PRI_CATCHUP;
	return CHREQ_PRI_USER;
}

void
chathistory_request_latest (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;

	used_msgid = (reference && g_str_has_prefix (reference, "msgid="));
	req = chreq_new (CHREQ_LATEST, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_before (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	used_msgid = g_str_has_prefix (reference, "msgid=");
	req = chreq_new (CHREQ_BEFORE, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_after (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	used_msgid = g_str_has_prefix (reference, "msgid=");
	req = chreq_new (CHREQ_AFTER, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_around (session *sess, const char *reference, int limit)
{
	chreq *req;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	req = chreq_new (CHREQ_AROUND, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, FALSE);
	chathistory_submit (sess, req);
}

void
chathistory_request_between (session *sess, const char *start_ref,
                             const char *end_ref, int limit)
{
	chreq *req;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	req = chreq_new (CHREQ_BETWEEN, start_ref, end_ref, limit,
	                  infer_priority (sess), sess->catchup_in_progress, FALSE);
	chathistory_submit (sess, req);
}

void
chathistory_request_targets (server *serv, const char *start_ref,
                             const char *end_ref, int limit)
{
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	effective_limit = get_effective_limit (serv, limit);

	tcp_sendf_labeled_tracked (serv, "CHATHISTORY", NULL,
	                           "CHATHISTORY TARGETS %s %s %d\r\n",
	                           start_ref, end_ref, effective_limit);
}

void
chathistory_request_after_timestamp (session *sess, time_t timestamp, int limit)
{
	char ref[64];

	/* Format timestamp reference per IRCv3 spec.
	 * Use gint64 cast — time_t is 64-bit on Windows x64 but long is 32-bit. */
	g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT, (gint64) timestamp);
	chathistory_request_after (sess, ref, limit);
}

void
chathistory_request_after_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);

	chathistory_request_after (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_before_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);
	chathistory_request_before (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_older (session *sess)
{
	const char *reference = NULL;
	char *ref;
	chreq *req;

	if (!sess || !sess->server || !sess->server->have_chathistory)
		return;
	if (sess->history_exhausted)
		return;

	/* Try oldest_msgid first (tracks the oldest message from chathistory batches) */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
		reference = sess->oldest_msgid;
	/* Fall back to scrollback_oldest_msgid (from loaded scrollback) */
	else if (sess->scrollback_oldest_msgid && sess->scrollback_oldest_msgid[0])
		reference = sess->scrollback_oldest_msgid;

	if (!reference)
		return;

	/* Submit as USER priority — preempts queued catch-up/background requests.
	 * The queue handles dedup and serialization, no flag hacking needed. */
	ref = g_strdup_printf ("msgid=%s", reference);
	req = chreq_new (CHREQ_BEFORE, ref, NULL, prefs.hex_irc_chathistory_lines,
	                  CHREQ_PRI_USER, FALSE, TRUE);
	chathistory_submit (sess, req);
	g_free (ref);
}

/* Compare batch messages by timestamp for sorting (ascending order) */
static gint
compare_batch_msg_timestamp (gconstpointer a, gconstpointer b)
{
	const batch_message *msg_a = a;
	const batch_message *msg_b = b;

	if (msg_a->timestamp < msg_b->timestamp)
		return -1;
	if (msg_a->timestamp > msg_b->timestamp)
		return 1;
	return 0;
}

/* Find session for a target */
static session *
find_session_for_target (server *serv, const char *target)
{
	session *sess;

	if (!target || !target[0])
		return NULL;

	/* Try as channel first */
	sess = find_channel (serv, (char *)target);
	if (sess)
		return sess;

	/* Try as dialog */
	sess = find_dialog (serv, (char *)target);

	return sess;
}

/* Complete a catch-up operation.  Inserts separator, clears state, starts
 * background fetch.  Called when the catch-up loop finishes (either the gap
 * is filled, the server returned empty, or all messages were duplicates). */
static void
finish_catchup (session *sess)
{
	sess->catchup_in_progress = FALSE;
	sess->catchup_is_before = FALSE;
	sess->history_catchup_stale_count = 0;
	sess->history_catchup_retrieved = 0;

	/* Catch-up sets oldest_msgid to its oldest batch message, which may be
	 * newer than the DB's oldest.  Reset to the DB's oldest so that
	 * scroll-to-load BEFORE requests reference the true oldest known message
	 * rather than fetching history the DB already has. */
	if (sess->scrollback_oldest_msgid && sess->scrollback_oldest_msgid[0])
	{
		g_free (sess->oldest_msgid);
		sess->oldest_msgid = g_strdup (sess->scrollback_oldest_msgid);
	}

	fe_reset_scroll_top_backoff (sess);

	/* If this session was the active BEFORE target, clear it so
	 * check_before_catchup can pick the next session. */
	if (sess->server && sess->server->chathistory_before_sess == sess)
		sess->server->chathistory_before_sess = NULL;
}

void
chathistory_start_catchup (session *sess)
{
	server *serv;

	if (!sess || !sess->server)
		return;

	serv = sess->server;

	if (!prefs.hex_irc_chathistory_auto || !serv->have_chathistory)
		return;

	if (sess->history_loading || sess->catchup_in_progress)
		return;

	sess->catchup_in_progress = TRUE;

	/* Choose LATEST reference based on available scrollback */
	if (sess->scrollback_newest_msgid && sess->scrollback_newest_msgid[0])
	{
		char *ref = g_strdup_printf ("msgid=%s", sess->scrollback_newest_msgid);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
		g_free (ref);
	}
	else if (sess->scrollback_newest_time > 0)
	{
		char ref[64];
		g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT,
		            (gint64) sess->scrollback_newest_time);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
	}
	else
	{
		/* No scrollback — get most recent context */
		chathistory_request_latest (sess, NULL, prefs.hex_irc_chathistory_lines);
	}
}

void
chathistory_cancel_catchup (session *sess)
{
	if (!sess)
		return;

	sess->catchup_in_progress = FALSE;
	chathistory_queue_free (sess);
	chathistory_stop_background_fetch (sess);
}

/* Determine if a channel mode takes an argument, replicating modes.c logic.
 * type A (list modes like b,e,I) always take args.
 * type B (like k) always take args.
 * type C (like l) take args on + only.
 * type D (like n,t) never take args.
 * Nick modes (o,v,h,etc) always take args. */
static int
chathistory_mode_has_arg (server *serv, char sign, char mode)
{
	char *cm;
	int type = 0;

	/* nick modes always have an arg */
	if (serv->nick_modes[0] && strchr (serv->nick_modes, mode))
		return 1;

	/* check CHANMODES= sections (comma-separated: A,B,C,D) */
	cm = serv->chanmodes;
	if (cm)
	{
		while (*cm)
		{
			if (*cm == ',')
				type++;
			else if (*cm == mode)
			{
				switch (type)
				{
				case 0: /* type A - list modes */
				case 1: /* type B - always has arg */
					return 1;
				case 2: /* type C - arg on + only */
					return (sign == '+') ? 1 : 0;
				default: /* type D - no arg */
					return 0;
				}
			}
			cm++;
		}
	}

	return 0;
}

/* Check if 'q' is a list-type chanmode (quiet) on this server,
 * as opposed to owner prefix mode. */
static gboolean
chathistory_server_supports_quiet (server *serv)
{
	char *cm = serv->chanmodes;
	if (!cm)
		return FALSE;
	/* q must appear before the first comma (type A = list modes) */
	while (*cm && *cm != ',')
	{
		if (*cm == 'q')
			return TRUE;
		cm++;
	}
	return FALSE;
}

/* Replay a MODE message from chathistory using per-mode text events.
 * This is display-only — no nicklist or channel state updates. */
static void
chathistory_replay_mode (session *sess, char *nick, char *mode_str,
                         char **params, int param_count,
                         const message_tags_data *tags_data)
{
	server *serv = sess->server;
	char sign = '+';
	int arg_idx = 2;  /* params[0]=target, params[1]=modes, params[2..]=args */
	char *op = NULL, *deop = NULL, *voice = NULL, *devoice = NULL;
	gboolean supportsq = chathistory_server_supports_quiet (serv);

	while (*mode_str)
	{
		if (*mode_str == '+' || *mode_str == '-')
		{
			/* Flush batched modes at sign change */
			if (op)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANOP, sess, nick, op,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (op); op = NULL;
			}
			if (deop)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEOP, sess, nick, deop,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (deop); deop = NULL;
			}
			if (voice)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANVOICE, sess, nick, voice,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (voice); voice = NULL;
			}
			if (devoice)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEVOICE, sess, nick, devoice,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (devoice); devoice = NULL;
			}
			sign = *mode_str;
			mode_str++;
			continue;
		}

		char mode = *mode_str;
		char *arg = "";

		/* Consume argument if this mode takes one */
		if (chathistory_mode_has_arg (serv, sign, mode) &&
		    arg_idx < param_count && params[arg_idx])
		{
			arg = params[arg_idx];
			if (*arg == ':')
				arg++;
			arg_idx++;
		}

		/* Dispatch to per-mode text events */
		switch (sign)
		{
		case '+':
			switch (mode)
			{
			case 'b':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANBAN, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'e':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANEXEMPT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'I':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANINVITE, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'k':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANSETKEY, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'l':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANSETLIMIT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'o':
				if (op)
				{
					char *tmp = g_strconcat (op, " ", arg, NULL);
					g_free (op);
					op = tmp;
				}
				else
					op = g_strdup (arg);
				break;
			case 'h':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANHOP, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'v':
				if (voice)
				{
					char *tmp = g_strconcat (voice, " ", arg, NULL);
					g_free (voice);
					voice = tmp;
				}
				else
					voice = g_strdup (arg);
				break;
			case 'q':
				if (supportsq)
				{
					EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANQUIET, sess, nick, arg,
					                       NULL, NULL, 0, tags_data->timestamp);
					break;
				}
				/* fall through to generic if q is owner mode */
			default:
				goto genmode;
			}
			break;
		case '-':
			switch (mode)
			{
			case 'b':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANUNBAN, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'e':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMEXEMPT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'I':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMINVITE, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'k':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMKEY, sess, nick, NULL,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'l':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMLIMIT, sess, nick, NULL,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'o':
				if (deop)
				{
					char *tmp = g_strconcat (deop, " ", arg, NULL);
					g_free (deop);
					deop = tmp;
				}
				else
					deop = g_strdup (arg);
				break;
			case 'h':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEHOP, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'v':
				if (devoice)
				{
					char *tmp = g_strconcat (devoice, " ", arg, NULL);
					g_free (devoice);
					devoice = tmp;
				}
				else
					devoice = g_strdup (arg);
				break;
			case 'q':
				if (supportsq)
				{
					EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANUNQUIET, sess, nick, arg,
					                       NULL, NULL, 0, tags_data->timestamp);
					break;
				}
				/* fall through to generic if q is owner mode */
			default:
				goto genmode;
			}
			break;
		default:
			goto genmode;
		}

		mode_str++;
		continue;

	genmode:
		{
			char outbuf[4];
			outbuf[0] = sign;
			outbuf[1] = 0;
			outbuf[2] = mode;
			outbuf[3] = 0;
			if (*arg)
			{
				char *buf = g_strdup_printf ("%s %s", sess->channel, arg);
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANMODEGEN, sess, nick, outbuf,
				                       outbuf + 2, buf, 0, tags_data->timestamp);
				g_free (buf);
			}
			else
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANMODEGEN, sess, nick, outbuf,
				                       outbuf + 2, sess->channel, 0,
				                       tags_data->timestamp);
			}
		}
		mode_str++;
	}

	/* Flush any remaining batched modes */
	if (op)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANOP, sess, nick, op,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (op);
	}
	if (deop)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEOP, sess, nick, deop,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (deop);
	}
	if (voice)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANVOICE, sess, nick, voice,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (voice);
	}
	if (devoice)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEVOICE, sess, nick, devoice,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (devoice);
	}
}

/* Process a single message from the batch.
 * Returns TRUE if message was processed, FALSE if it was a duplicate. */
static gboolean
process_batch_message (server *serv, session *sess, batch_message *msg)
{
	message_tags_data tags_data;
	char *nick = NULL;
	char *host = NULL;
	char *text = NULL;

	if (!msg || !msg->command)
		return FALSE;

	/* Skip duplicate messages (already displayed from previous batches or live).
	 * Uses msgid+timestamp because some servers reuse msgids after restarts. */
	if (msg->msgid && chathistory_is_duplicate_msgid (sess, msg->msgid, msg->timestamp))
	{
		return FALSE;
	}

	/* Initialize tags data */
	memset (&tags_data, 0, sizeof (tags_data));
	tags_data.timestamp = msg->timestamp;
	if (msg->tags)
		tags_data.all_tags = msg->tags;

	if (msg->msgid)
	{
		/* Set msgid so it gets saved to scrollback via inbound functions */
		tags_data.msgid = msg->msgid;
		/* Track msgids for pagination and deduplication */
		chathistory_track_msgid_ts (sess, msg->msgid, msg->timestamp, TRUE);
		/* Set current_msgid directly for ALL message types including events.
		 * Only inbound_chanmsg/inbound_action set this from tags_data,
		 * but events (JOIN/PART/etc) also need their msgids captured.
		 * Default to non-speech; the inbound_{chanmsg,action,notice} path
		 * below will flip is_user_msg back to TRUE when appropriate. */
		g_free (sess->current_msgid);
		sess->current_msgid = g_strdup (msg->msgid);
		sess->current_msgid_is_user_msg = FALSE;
	}
	else
	{
		g_free (sess->current_msgid);
		sess->current_msgid = NULL;
		sess->current_msgid_is_user_msg = FALSE;
	}

	/* Extract nick from prefix */
	if (msg->prefix)
	{
		char *bang = strchr (msg->prefix, '!');
		if (bang)
		{
			nick = g_strndup (msg->prefix, bang - msg->prefix);
			host = g_strdup (bang + 1);
		}
		else
		{
			nick = g_strdup (msg->prefix);
		}
	}

	/* Handle different command types */
	if (g_ascii_strcasecmp (msg->command, "PRIVMSG") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;

			/* Check for CTCP ACTION */
			if (text && strncmp (text, "\001ACTION ", 8) == 0)
			{
				char *action_text = text + 8;
				char *end = strchr (action_text, '\001');
				if (end)
					*end = '\0';
				inbound_action (sess, sess->channel, nick, host ? host : "",
				                action_text, FALSE, 0, &tags_data);
			}
			else
			{
				/* If text contains \n it was collapsed from a draft/multiline
				 * batch — keep it as a single entry instead of splitting */
				if (strchr (text, '\n'))
					fe_begin_multiline_group (sess);
				inbound_chanmsg (serv, sess, sess->channel, nick, text,
				                 FALSE, 0, &tags_data);
				if (strchr (text, '\n'))
					fe_end_multiline_group (sess);
			}
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NOTICE") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;
			inbound_notice (serv, sess->channel, nick, text,
			                host ? host : "", 0, &tags_data);
		}
	}
	/* event-playback: Handle JOIN, PART, QUIT, KICK, MODE, TOPIC, NICK */
	else if (g_ascii_strcasecmp (msg->command, "JOIN") == 0)
	{
		/* Skip the JOIN that started *this* session — inbound_ujoin already
		 * displayed "Now talking on" for it.  Other historical self-JOINs
		 * (previous visits) are kept so JOIN/PART history stays balanced. */
		if (sess->join_msgid && msg->msgid &&
		    strcmp (sess->join_msgid, msg->msgid) == 0)
		{
			g_free (nick);
			g_free (host);
			return TRUE;  /* consumed, not a duplicate */
		}

		/* Historical JOIN - display only, don't modify nicklist.
		 * The current channel membership comes from NAMES, not replayed history. */
		char *account = NULL;
		if (msg->param_count >= 2)
			account = msg->params[1];
		if (account && *account == ':')
			account++;
		EMIT_SIGNAL_TIMESTAMP (XP_TE_JOIN, sess, nick, sess->channel,
		                       host ? host : "", account, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "PART") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 2)
		{
			reason = msg->params[1];
			if (reason && *reason == ':')
				reason++;
		}
		/* Historical PART - display only, don't modify nicklist */
		if (reason && *reason)
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PARTREASON, sess, nick, host ? host : "",
			                       sess->channel, reason, 0, tags_data.timestamp);
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PART, sess, nick, host ? host : "",
			                       sess->channel, NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "QUIT") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 1)
		{
			reason = msg->params[0];
			if (reason && *reason == ':')
				reason++;
		}
		/* For historical QUITs, emit directly to session instead of using inbound_quit()
		 * because inbound_quit() requires the user to be in the userlist (which they won't be
		 * for historical events - they already quit). */
		EMIT_SIGNAL_TIMESTAMP (XP_TE_QUIT, sess, nick, reason ? reason : "",
		                       host ? host : "", NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "KICK") == 0)
	{
		if (msg->param_count >= 2)
		{
			char *kicked = msg->params[1];
			char *reason = NULL;
			if (msg->param_count >= 3)
			{
				reason = msg->params[2];
				if (reason && *reason == ':')
					reason++;
			}
			/* Historical KICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_KICK, sess, nick, kicked,
			                       sess->channel, reason ? reason : "", 0,
			                       tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "TOPIC") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && *text == ':')
				text++;
			/* Historical TOPIC - display only, don't update current topic */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NEWTOPIC, sess, nick, text,
			                       sess->channel, NULL, 0, tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NICK") == 0)
	{
		if (msg->param_count >= 1)
		{
			char *newnick = msg->params[0];
			if (newnick && *newnick == ':')
				newnick++;
			/* Historical NICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANGENICK, sess, nick,
			                       newnick, NULL, NULL, 0, tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "MODE") == 0)
	{
		if (msg->param_count >= 2)
		{
			char *mode_str = msg->params[1];
			if (mode_str && *mode_str == ':')
				mode_str++;
			chathistory_replay_mode (sess, nick, mode_str,
			                         msg->params, msg->param_count,
			                         &tags_data);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "REDACT") == 0)
	{
		/* Historical REDACT: target msgid may or may not exist locally.
		 * Format: :nick!user@host REDACT <target> <msgid> [:<reason>] */
		if (msg->param_count >= 2)
		{
			char *target_msgid = msg->params[1];
			char *reason = (msg->param_count >= 3) ? msg->params[2] : NULL;
			if (target_msgid && *target_msgid == ':')
				target_msgid++;
			if (reason && *reason == ':')
				reason++;
			if (reason && !*reason)
				reason = NULL;

			{
				time_t rtime = tags_data.timestamp ? tags_data.timestamp
				                                   : time (NULL);
				/* Try visual redaction — harmless no-op if entry doesn't exist */
				fe_redact_message (sess, target_msgid, nick, reason, rtime);
				/* Mark as redacted in scrollback (preserves original text) */
				scrollback_redact_for_session (sess, target_msgid, nick, reason, rtime);
			}
		}
	}

	g_free (nick);
	g_free (host);
	return TRUE;
}

/* Find a session on this server that has a pending history request.
 * Used when FAIL response doesn't include the target channel. */
static session *
find_session_with_pending_history (server *serv)
{
	GSList *list;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv && sess->history_loading)
			return sess;
	}
	return NULL;
}

void
chathistory_handle_fail (server *serv, const char *context)
{
	session *sess = NULL;

	/* Try to find session from context (may be the target channel) */
	if (context && context[0])
		sess = find_session_for_target (serv, context);

	/* Fall back to finding any session with a pending history request */
	if (!sess)
		sess = find_session_with_pending_history (serv);

	if (!sess)
		return;

	{
		/* Save flags from active request before completing it */
		gboolean used_msgid = sess->history_request_used_msgid;

		/* Clear active request and advance queue */
		chathistory_request_complete (sess);

		if (sess->catchup_in_progress)
		{
			/* Catch-up: server rejected our reference.  If we used a msgid the
			 * server doesn't recognise (e.g. after restart), retry with timestamp. */
			if (used_msgid && sess->scrollback_newest_time > 0)
			{
				char ref[64];
				g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT,
				            (gint64) sess->scrollback_newest_time);
				chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
				return;
			}
			/* All fallbacks exhausted — finish with whatever we have */
			finish_catchup (sess);
		}
	}
}

/* --- Chunked batch processing state --- */

typedef struct {
	session *sess;
	server *serv;
	GSList *remaining;			/* next message to process */
	GSList *all_messages;		/* head of list, for freeing */
	int msg_count;				/* messages successfully processed so far */
	int raw_count;				/* total messages in batch */
	time_t oldest_timestamp;
	time_t newest_timestamp;
	char *batch_oldest_msgid;	/* owned copy */
	gboolean is_catchup;
	gboolean chathistory_end;	/* server signalled no more history (draft/chathistory-end) */
	guint idle_tag;
	scrollback_db *db;			/* for transaction begin/commit between chunks */
} chathistory_chunk_state;

static void
chunk_state_free (chathistory_chunk_state *chunk)
{
	if (!chunk)
		return;
	if (chunk->idle_tag > 0)
	{
		g_source_remove (chunk->idle_tag);
		chunk->idle_tag = 0;
	}
	if (chunk->sess && chunk->sess->chunk_state == chunk)
		chunk->sess->chunk_state = NULL;
	g_slist_free_full (chunk->all_messages, (GDestroyNotify) batch_message_free);
	g_free (chunk->batch_oldest_msgid);
	g_free (chunk);
}

/* Post-processing after all messages in a batch have been processed.
 * Handles mode flag cleanup, pagination, and background fetch scheduling. */
static void
finish_batch_processing (chathistory_chunk_state *chunk)
{
	session *sess = chunk->sess;
	server *serv = chunk->serv;

	/* Clear processing mode flags and advance the request queue.
	 * chathistory_request_complete dispatches any pending request. */
	sess->history_prepend_mode = FALSE;
	sess->history_insert_sorted_mode = FALSE;
	fe_set_batch_mode (sess, FALSE);
	chathistory_request_complete (sess);

	/* Update oldest_msgid for scroll-to-load pagination.
	 * Always update if the batch had messages, even if all were duplicates
	 * (msg_count == 0).  The batch's oldest msgid is a valid pagination
	 * cursor regardless of whether the messages were already known from
	 * scrollback DB.  Without this, BEFORE requests loop with the same
	 * reference when the overlap region is entirely in the local DB. */
	if (chunk->batch_oldest_msgid && chunk->raw_count > 0)
	{
		g_free (sess->oldest_msgid);
		sess->oldest_msgid = g_strdup (chunk->batch_oldest_msgid);
	}

	/* Server explicitly signalled end of history via draft/chathistory-end tag */
	if (chunk->chathistory_end)
		sess->history_exhausted = TRUE;

	/* Catch-up loop */
	if (chunk->is_catchup)
	{
		if (sess->catchup_is_before)
		{
			/* --- BEFORE pagination phase --- */
			sess->history_catchup_retrieved += chunk->msg_count;

			/* No more history (empty batch or chathistory-end tag) */
			if (chunk->raw_count == 0 || sess->history_exhausted)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Timestamp stop: earliest message is older than lower bound → gap bridged */
			if (sess->catchup_lower_bound > 0 && chunk->oldest_timestamp > 0 &&
			    chunk->oldest_timestamp < sess->catchup_lower_bound)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Stale count: all duplicates */
			if (chunk->msg_count == 0)
			{
				sess->history_catchup_stale_count++;
				if (sess->history_catchup_stale_count >= 3)
				{
					sess->history_exhausted = TRUE;
					finish_catchup (sess);
					chathistory_check_before_catchup (serv);
					return;
				}
			}
			else
			{
				sess->history_catchup_stale_count = 0;
			}

			/* Sanity limit — only for automatic post-connect catch-up.
			 * Scroll-to-top and gap-fill are user-driven and uncapped. */
			if (sess->history_catchup_retrieved >= CHATHISTORY_SANITY_LIMIT)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Tab switched away — pause this session, check new active */
			if (serv->chathistory_before_sess != sess ||
			    sess != current_sess)
			{
				serv->chathistory_before_sess = NULL;
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Continue BEFORE pagination after a delay */
			if (chunk->batch_oldest_msgid)
			{
				schedule_before_catchup (serv);
			}
			else
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
			}
			return;
		}

		/* --- Initial LATEST phase --- */
		if (serv->chathistory_latest_pending > 0)
			serv->chathistory_latest_pending--;

		/* All LATEST batches done → start BEFORE catch-up on active tab */
		if (serv->chathistory_latest_pending == 0)
			chathistory_check_before_catchup (serv);

		return;
	}

	/* --- Non-catch-up post-processing (scroll-to-load, background fetch) --- */

	/* Check if we hit the age limit (only for background fetching) */
	if (sess->background_history_active && chunk->oldest_timestamp > 0)
	{
		time_t max_age_cutoff = 0;
		if (prefs.hex_irc_chathistory_background_max_age > 0)
			max_age_cutoff = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
		if (max_age_cutoff > 0 && chunk->oldest_timestamp < max_age_cutoff)
		{
			sess->background_history_active = FALSE;
		}
	}

	/* All messages were duplicates — stop background fetching but don't
	 * mark history as exhausted.  During catch-up, the overlap between
	 * local DB and server history can produce all-dupe batches that don't
	 * mean the server has no more history.  The oldest_msgid update above
	 * ensures the next request uses a new cursor past the overlap. */
	if (chunk->raw_count > 0 && chunk->msg_count == 0)
	{
		sess->background_history_active = FALSE;
	}

	fe_reset_scroll_top_backoff (sess);

	/* Schedule next background fetch if active */
	if (sess->background_history_active && !sess->history_exhausted)
	{
		schedule_background_fetch (sess);
	}
}

/* Process up to CHUNK_SIZE messages from the remaining list. */
static void
process_chunk_messages (chathistory_chunk_state *chunk)
{
	int i;
	GSList *iter;

	for (i = 0, iter = chunk->remaining; iter && i < CHATHISTORY_CHUNK_SIZE;
	     iter = iter->next, i++)
	{
		batch_message *msg = iter->data;

		if (process_batch_message (chunk->serv, chunk->sess, msg))
			chunk->msg_count++;

		if (msg->timestamp > 0)
		{
			if (chunk->oldest_timestamp == 0 || msg->timestamp < chunk->oldest_timestamp)
				chunk->oldest_timestamp = msg->timestamp;
			if (msg->timestamp > chunk->newest_timestamp)
				chunk->newest_timestamp = msg->timestamp;
		}
	}
	chunk->remaining = iter;
}

/* Idle callback for chunked batch processing.  Fires at G_PRIORITY_DEFAULT_IDLE
 * (lower than socket I/O), so NAMES/WHO responses interleave between chunks. */
static gboolean
chunk_idle_cb (gpointer data)
{
	chathistory_chunk_state *chunk = data;

	/* Session may have been destroyed since we were scheduled */
	if (!is_session (chunk->sess))
	{
		chunk->idle_tag = 0;
		chunk->sess = NULL;  /* prevent chunk_state_free from clearing sess->chunk_state */
		chunk_state_free (chunk);
		return G_SOURCE_REMOVE;
	}

	if (chunk->db)
		scrollback_begin_transaction (chunk->db);

	process_chunk_messages (chunk);

	if (chunk->db)
		scrollback_commit_transaction (chunk->db);

	if (chunk->remaining == NULL)
	{
		/* All messages processed */
		chunk->idle_tag = 0;
		finish_batch_processing (chunk);
		chunk_state_free (chunk);
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

void
chathistory_cancel_chunk_processing (session *sess)
{
	chathistory_chunk_state *chunk;

	if (!sess || !sess->chunk_state)
		return;

	chunk = sess->chunk_state;

	/* Commit any in-flight transaction */
	if (chunk->db)
		scrollback_commit_transaction (chunk->db);

	/* Clear batch mode on the buffer so it renders properly */
	sess->history_prepend_mode = FALSE;
	sess->history_insert_sorted_mode = FALSE;
	fe_set_batch_mode (sess, FALSE);
	chathistory_queue_free (sess);

	chunk_state_free (chunk);
}

void
chathistory_process_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	gboolean is_catchup;
	char *batch_oldest_msgid = NULL;
	int raw_count;
	const char *network;
	scrollback_db *db;

	if (!batch || !batch->type)
		return;

	/* batch->params[0] should be the target */
	if (batch->param_count >= 1 && batch->params[0])
	{
		sess = find_session_for_target (serv, batch->params[0]);
	}

	if (!sess)
		return;

	is_catchup = sess->catchup_in_progress;

	/* Empty batch handling */
	if (!batch->messages)
	{
		gboolean used_msgid = sess->history_request_used_msgid;
		if (batch->chathistory_end)
			sess->history_exhausted = TRUE;
		chathistory_request_complete (sess);
		if (is_catchup)
		{
			/* Server may not recognize our msgid (e.g., server restart).
			 * Fall back to timestamp-based LATEST, then LATEST *.
			 * But not if chathistory-end tells us there's nothing. */
			if (!sess->history_exhausted &&
			    used_msgid && sess->scrollback_newest_time > 0)
			{
				char ref[64];
				g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT,
				            (gint64) sess->scrollback_newest_time);
				chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
				return;
			}
			/* Catch-up complete — no new messages since last disconnect.
			 * Don't set history_exhausted unless chathistory-end was sent:
			 * older history may still exist for scroll-to-top requests. */
			finish_catchup (sess);
			if (serv->chathistory_latest_pending > 0)
				serv->chathistory_latest_pending--;
			if (serv->chathistory_latest_pending == 0)
				chathistory_check_before_catchup (serv);
			return;
		}
		/* Non-catch-up empty batch: server has no more history in this direction */
		sess->history_exhausted = TRUE;
		return;
	}

	/* Sort batch messages by timestamp ascending.  The IRCv3 spec requires
	 * servers to return messages in ascending order, but not all servers
	 * comply.  Sorting here makes us robust against any server ordering and
	 * also ensures batch_oldest_msgid is captured correctly below. */
	batch->messages = g_slist_sort (batch->messages, compare_batch_msg_timestamp);

	if (batch->messages)
	{
		batch_message *first_msg = batch->messages->data;
		if (first_msg && first_msg->msgid)
			batch_oldest_msgid = first_msg->msgid;
	}

	raw_count = g_slist_length (batch->messages);

	/* Keep history_loading TRUE until finish_batch_processing clears it —
	 * this prevents new requests from being sent during chunked processing. */
	sess->history_insert_sorted_mode = TRUE;
	fe_set_batch_mode (sess, TRUE);

	network = server_get_network (serv, FALSE);
	db = network ? scrollback_open (network) : NULL;

	if (raw_count <= CHATHISTORY_CHUNK_SIZE)
	{
		/* Small batch — process synchronously */
		chathistory_chunk_state sync_state = { 0 };
		sync_state.sess = sess;
		sync_state.serv = serv;
		sync_state.remaining = batch->messages;
		sync_state.raw_count = raw_count;
		sync_state.is_catchup = is_catchup;
		sync_state.chathistory_end = batch->chathistory_end;
		sync_state.batch_oldest_msgid = (char *)batch_oldest_msgid; /* borrowed, not freed */

		if (db)
			scrollback_begin_transaction (db);

		process_chunk_messages (&sync_state);

		if (db)
			scrollback_commit_transaction (db);

		/* finish_batch_processing reads from the chunk state but doesn't
		 * free it — safe to use a stack-allocated struct here. */
		finish_batch_processing (&sync_state);
	}
	else
	{
		/* Large batch — process in chunks via idle callbacks.
		 * Steal the message list from batch so batch_info_free won't free it. */
		chathistory_chunk_state *chunk = g_new0 (chathistory_chunk_state, 1);
		chunk->sess = sess;
		chunk->serv = serv;
		chunk->all_messages = batch->messages;
		chunk->remaining = batch->messages;
		chunk->raw_count = raw_count;
		chunk->is_catchup = is_catchup;
		chunk->chathistory_end = batch->chathistory_end;
		chunk->db = db;
		chunk->batch_oldest_msgid = g_strdup (batch_oldest_msgid);
		batch->messages = NULL;  /* prevent batch_info_free from freeing */

		sess->chunk_state = chunk;

		/* Defer ALL processing to idle — this lets the socket read handler
		 * return quickly so WHO replies and GTK renders can interleave
		 * between chunks instead of being blocked by inline processing. */
		chunk->idle_tag = g_idle_add (chunk_idle_cb, chunk);
	}
}

void
chathistory_parse_isupport (server *serv, const char *value)
{
	/* Format: CHATHISTORY=<limit> or more complex options */
	if (!value || !value[0])
	{
		/* No value means feature supported but no specific limit */
		serv->chathistory_limit = 0;
		return;
	}

	/* Try to parse as simple integer limit */
	serv->chathistory_limit = atoi (value);

	/* Handle more complex formats like "limit=1000,retention=7d" */
	if (serv->chathistory_limit == 0 && strchr (value, '='))
	{
		char **tokens = g_strsplit (value, ",", 0);
		int i;

		for (i = 0; tokens[i]; i++)
		{
			if (g_str_has_prefix (tokens[i], "limit="))
			{
				serv->chathistory_limit = atoi (tokens[i] + 6);
			}
			/* TODO: Parse retention if needed */
		}

		g_strfreev (tokens);
	}
}

/* Build a dedup key from msgid + timestamp.  Some servers (e.g. Nefarious)
 * reset their msgid counter on restart, producing collisions.  Adding the
 * timestamp makes false duplicates virtually impossible. */
static char *
make_dedup_key (const char *msgid, time_t timestamp)
{
	return g_strdup_printf ("%s@%" G_GINT64_FORMAT, msgid, (gint64) timestamp);
}

void
chathistory_track_msgid (session *sess, const char *msgid, gboolean is_history)
{
	chathistory_track_msgid_ts (sess, msgid, 0, is_history);
}

void
chathistory_track_msgid_ts (session *sess, const char *msgid, time_t timestamp,
                            gboolean is_history)
{
	gboolean is_new;
	char *key;

	if (!msgid || !msgid[0])
		return;

	/* Build dedup key: msgid alone if no timestamp, msgid+timestamp otherwise.
	 * Scrollback-loaded entries have timestamps; live messages may not yet. */
	key = (timestamp > 0) ? make_dedup_key (msgid, timestamp) : g_strdup (msgid);

	/* Add to known msgids for deduplication.
	 * g_hash_table_add returns TRUE if this is a new entry. */
	if (sess->known_msgids)
		is_new = g_hash_table_add (sess->known_msgids, key);
	else
	{
		g_free (key);
		is_new = TRUE;
	}

	/* Only update oldest/newest tracking for truly new msgids.
	 * This prevents re-tracking from inbound functions after chathistory already tracked. */
	if (!is_new)
		return;

	if (is_history)
	{
		/* Historical message - update oldest_msgid if this is older
		 * (messages come in order, so the first one we see is oldest) */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
		/* Don't update newest_msgid for historical messages */
	}
	else
	{
		/* Live message - update newest_msgid */
		g_free (sess->newest_msgid);
		sess->newest_msgid = g_strdup (msgid);

		/* If we don't have an oldest yet, this is also the oldest */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
	}
}

/**
 * Check if a message with this msgid+timestamp has already been displayed.
 * Uses both fields because some servers reuse msgids after restarts.
 *
 * @param sess Session to check
 * @param msgid The message ID to check
 * @param timestamp The message timestamp (0 to match by msgid alone)
 * @return TRUE if msgid is known (duplicate), FALSE if new
 */
gboolean
chathistory_is_duplicate_msgid (session *sess, const char *msgid, time_t timestamp)
{
	if (!sess || !msgid || !msgid[0])
		return FALSE;

	if (!sess->known_msgids)
		return FALSE;

	if (timestamp > 0)
	{
		char *key = make_dedup_key (msgid, timestamp);
		gboolean found = g_hash_table_contains (sess->known_msgids, key);
		g_free (key);
		return found;
	}

	return g_hash_table_contains (sess->known_msgids, msgid);
}

gboolean
chathistory_can_request_more (session *sess)
{
	if (!sess || !sess->server)
		return FALSE;

	if (!sess->server->have_chathistory)
		return FALSE;

	if (sess->history_exhausted)
		return FALSE;

	if (sess->history_loading)
		return FALSE;

	return TRUE;
}

/* Timer callback for background history fetching */
static gboolean
background_history_timer_cb (gpointer data)
{
	session *sess = (session *)data;

	/* Clear the timer tag first */
	sess->background_history_timer = 0;

	/* Check if we should continue fetching */
	if (!sess->background_history_active)
		return G_SOURCE_REMOVE;

	if (!chathistory_can_request_more (sess))
	{
		/* Can't request more - stop background fetching */
		sess->background_history_active = FALSE;
		return G_SOURCE_REMOVE;
	}

	/* Request older history */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
	{
		chathistory_request_before_msgid (sess, sess->oldest_msgid,
		                                  prefs.hex_irc_chathistory_lines);
	}
	else
	{
		/* No oldest_msgid to reference - can't make BEFORE request */
		sess->background_history_active = FALSE;
	}

	return G_SOURCE_REMOVE;
}

/* Schedule the next background history fetch */
static void
schedule_background_fetch (session *sess)
{
	int delay_secs;

	if (!sess->background_history_active)
		return;

	if (sess->background_history_timer > 0)
		return; /* Already scheduled */

	delay_secs = prefs.hex_irc_chathistory_background_delay;
	if (delay_secs < 5)
		delay_secs = 5; /* Minimum 5 seconds between fetches */

	sess->background_history_timer = g_timeout_add_seconds (delay_secs,
	                                                        background_history_timer_cb,
	                                                        sess);
}

void
chathistory_start_background_fetch (session *sess)
{
	if (!sess || !sess->server)
		return;

	/* Check if background fetching is enabled */
	if (!prefs.hex_irc_chathistory_background)
		return;

	/* Don't start if server doesn't support chathistory */
	if (!sess->server->have_chathistory)
		return;

	/* Don't start if already exhausted */
	if (sess->history_exhausted)
		return;

	/* Don't start if no oldest_msgid to reference */
	if (!sess->oldest_msgid || !sess->oldest_msgid[0])
		return;

	/* Mark as active and schedule first fetch */
	sess->background_history_active = TRUE;
	schedule_background_fetch (sess);
}

void
chathistory_stop_background_fetch (session *sess)
{
	if (!sess)
		return;

	sess->background_history_active = FALSE;

	if (sess->background_history_timer > 0)
	{
		g_source_remove (sess->background_history_timer);
		sess->background_history_timer = 0;
	}
}

/* --- Deferred catch-up coordination --- */

/* Send LATEST for a single session as part of deferred catch-up */
static void
send_deferred_latest (session *sess)
{
	if (!sess || !sess->server)
		return;
	if (sess->history_loading || sess->catchup_in_progress || sess->history_exhausted)
		return;
	if (sess->type != SESS_CHANNEL || !sess->channel[0])
		return;

	sess->catchup_in_progress = TRUE;
	sess->catchup_is_before = FALSE;
	if (sess->scrollback_newest_time > CHATHISTORY_FUZZ_INTERVAL)
		sess->catchup_lower_bound = sess->scrollback_newest_time - CHATHISTORY_FUZZ_INTERVAL;
	else if (prefs.hex_irc_chathistory_background_max_age > 0)
		sess->catchup_lower_bound = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
	else
		sess->catchup_lower_bound = 0;

	/* Choose LATEST reference based on available scrollback */
	if (sess->scrollback_newest_msgid && sess->scrollback_newest_msgid[0])
	{
		char *ref = g_strdup_printf ("msgid=%s", sess->scrollback_newest_msgid);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
		g_free (ref);
	}
	else if (sess->scrollback_newest_time > 0)
	{
		char ref[64];
		g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT,
		            (gint64) sess->scrollback_newest_time);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
	}
	else
	{
		chathistory_request_latest (sess, NULL, prefs.hex_irc_chathistory_lines);
	}

	sess->server->chathistory_latest_pending++;
}

/* Timer callback: fires 2s after the last 366, sends LATEST for all channels */
static gboolean
chathistory_deferred_start_cb (gpointer data)
{
	server *serv = data;
	GSList *list;

	serv->chathistory_start_timer = 0;

	if (!is_server (serv) || !serv->connected || !serv->have_chathistory)
		return G_SOURCE_REMOVE;

	if (!prefs.hex_irc_chathistory_auto)
		return G_SOURCE_REMOVE;

	serv->chathistory_latest_pending = 0;

	/* Send LATEST for current_sess first (active tab gets priority) */
	if (current_sess && current_sess->server == serv)
		send_deferred_latest (current_sess);

	/* Then all other sessions on this server */
	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv && sess != current_sess)
			send_deferred_latest (sess);
	}

	/* If no sessions needed catch-up, nothing to do */
	if (serv->chathistory_latest_pending == 0)
		return G_SOURCE_REMOVE;

	return G_SOURCE_REMOVE;
}

void
chathistory_schedule_deferred (server *serv)
{
	if (!serv || !serv->have_chathistory || !prefs.hex_irc_chathistory_auto)
		return;

	/* Reset the timer — each new 366 pushes the start out by DEFERRED_DELAY */
	if (serv->chathistory_start_timer > 0)
		g_source_remove (serv->chathistory_start_timer);

	serv->chathistory_start_timer = g_timeout_add (CHATHISTORY_DEFERRED_DELAY,
	                                                chathistory_deferred_start_cb,
	                                                serv);
}

static gboolean
chathistory_before_timer_cb (gpointer data)
{
	server *serv = data;

	serv->chathistory_before_timer = 0;

	if (!is_server (serv) || !serv->connected)
		return G_SOURCE_REMOVE;

	chathistory_check_before_catchup (serv);
	return G_SOURCE_REMOVE;
}

/* Schedule check_before_catchup after CHATHISTORY_BEFORE_DELAY seconds */
static void
schedule_before_catchup (server *serv)
{
	if (serv->chathistory_before_timer > 0)
		g_source_remove (serv->chathistory_before_timer);

	serv->chathistory_before_timer = g_timeout_add_seconds (
		CHATHISTORY_BEFORE_DELAY, chathistory_before_timer_cb, serv);
}

void
chathistory_check_before_catchup (server *serv)
{
	GSList *list;
	session *target = NULL;

	if (!serv || !serv->connected || !serv->have_chathistory)
		return;

	/* Don't start BEFORE until all LATEST are done */
	if (serv->chathistory_latest_pending > 0)
		return;

	/* If there's already an active BEFORE session with a pending request, wait */
	if (serv->chathistory_before_sess &&
	    serv->chathistory_before_sess->history_loading)
		return;

	/* Prefer current_sess if it needs BEFORE catch-up */
	if (current_sess && current_sess->server == serv &&
	    current_sess->catchup_in_progress && !current_sess->history_exhausted &&
	    !current_sess->history_loading)
	{
		target = current_sess;
	}

	if (!target)
	{
		/* No active tab needs catch-up — don't start on inactive tabs.
		 * BEFORE catch-up only runs on the active tab. */
		serv->chathistory_before_sess = NULL;
		return;
	}

	/* Start/resume BEFORE catch-up on the target session */
	serv->chathistory_before_sess = target;
	target->catchup_is_before = TRUE;

	if (target->oldest_msgid && target->oldest_msgid[0])
	{
		chathistory_request_before_msgid (target, target->oldest_msgid,
		                                  CHATHISTORY_BEFORE_LIMIT);
	}
	else
	{
		/* No msgid to reference — can't paginate */
		finish_catchup (target);
		serv->chathistory_before_sess = NULL;
	}
}

void
chathistory_notify_tab_switch (session *new_sess)
{
	server *serv;

	if (!new_sess || !new_sess->server)
		return;

	serv = new_sess->server;

	if (!serv->have_chathistory)
		return;

	/* If the BEFORE target changed, the current BEFORE batch will detect
	 * the mismatch in finish_batch_processing and call check_before_catchup.
	 * But if there's no in-flight request, start immediately. */
	if (serv->chathistory_before_sess != new_sess &&
	    serv->chathistory_latest_pending == 0)
	{
		chathistory_check_before_catchup (serv);
	}
}

void
chathistory_process_targets_batch (server *serv, batch_info *batch)
{
	GSList *iter;

	if (!batch)
		return;

	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;
		session *sess;
		char *target;

		if (!msg || !msg->command)
			continue;

		/* TARGETS entries have command="CHATHISTORY", params: TARGETS <target> <timestamp> */
		if (g_ascii_strcasecmp (msg->command, "CHATHISTORY") != 0)
			continue;
		if (msg->param_count < 3)
			continue;
		if (g_ascii_strcasecmp (msg->params[0], "TARGETS") != 0)
			continue;

		target = msg->params[1];
		if (!target || !target[0])
			continue;

		/* Skip channel targets — channels get catch-up from their JOIN handler */
		if (is_channel (serv, target))
			continue;

		/* Find or create a dialog session for this DM target */
		sess = find_dialog (serv, target);
		if (!sess)
		{
			/* new_ircwindow calls scrollback_load automatically,
			 * populating scrollback_newest_msgid/time for catch-up */
			sess = new_ircwindow (serv, target, SESS_DIALOG, 0);
		}

		if (sess)
			chathistory_start_catchup (sess);
	}
}

void
chathistory_request_targets_on_reconnect (server *serv)
{
	gint64 now_val, lower_bound;
	char start_ref[64], end_ref[64];

	if (!serv->have_chathistory || !serv->connected)
		return;

	/* Only fire on reconnect, not first connect */
	if (serv->last_disconnect_time == 0)
		return;

	now_val = (gint64) time (NULL);
	lower_bound = (gint64) serv->last_disconnect_time - CHATHISTORY_FUZZ_INTERVAL;

	g_snprintf (start_ref, sizeof (start_ref),
	            "timestamp=%" G_GINT64_FORMAT, now_val + CHATHISTORY_FUZZ_INTERVAL);
	g_snprintf (end_ref, sizeof (end_ref),
	            "timestamp=%" G_GINT64_FORMAT, lower_bound);

	chathistory_request_targets (serv, start_ref, end_ref, 0);
}
