/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
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
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "poxchat.h"
#include "notify.h"
#include "cfgfiles.h"
#include "fe.h"
#include "server.h"
#include "text.h"
#include "tree.h"
#include "userlist.h"
#include "util.h"
#include "poxchatc.h"


GSList *notify_list = 0;
int notify_tag = 0;


static char *
despacify_dup (char *str)
{
	char *p, *res = g_malloc (strlen (str) + 1);

	p = res;
	while (1)
	{
		if (*str != ' ')
		{
			*p = *str;
			if (*p == 0)
				return res;
			p++;
		}
		str++;
	}
}

static int
notify_netcmp (char *str, void *serv)
{
	char *net = despacify_dup (server_get_network (serv, TRUE));

	if (rfc_casecmp (str, net) == 0)
	{
		g_free (net);
		return 0;	/* finish & return FALSE from token_foreach() */
	}

	g_free (net);
	return 1;	/* keep going... */
}

/* monitor this nick on this particular network? */

static gboolean
notify_do_network (struct notify *notify, server *serv)
{
	if (!notify->networks)	/* ALL networks for this nick */
		return TRUE;

	if (token_foreach (notify->networks, ',', notify_netcmp, serv))
		return FALSE;	/* network list doesn't contain this one */

	return TRUE;
}

struct notify_per_server *
notify_find_server_entry (struct notify *notify, struct server *serv)
{
	GSList *list = notify->server_list;
	struct notify_per_server *servnot;

	while (list)
	{
		servnot = (struct notify_per_server *) list->data;
		if (servnot->server == serv)
			return servnot;
		list = list->next;
	}

	/* not found, should we add it, or is this not a network where
      we're monitoring this nick? */
	if (!notify_do_network (notify, serv))
		return NULL;

	servnot = g_new0 (struct notify_per_server, 1);
	servnot->server = serv;
	servnot->notify = notify;
	notify->server_list = g_slist_prepend (notify->server_list, servnot);
	return servnot;
}

void
notify_save (void)
{
	int fh;
	struct notify *notify;
        // while reading the notify.conf file, elements are added by prepending to the
        // list. reverse the list before writing to disk to keep the original
        // order of the list
        GSList *list = g_slist_copy(notify_list);
        list = g_slist_reverse(list);

	fh = poxchat_open_file ("notify.conf", O_TRUNC | O_WRONLY | O_CREAT, 0600, XOF_DOMODE);
	if (fh != -1)
	{
		while (list)
		{
			notify = (struct notify *) list->data;
			/* Line format:
			 *   nick                          (legacy, nick-only)
			 *   nick networks                 (legacy, with networks filter)
			 *   nick account=X [networks=Y]   (nick + account, new)
			 *   account=X [networks=Y]        (account-only, new)
			 * Legacy nick-only / nick+networks lines remain untouched to
			 * preserve forward-compat with older poxchat reading the file. */
			if (notify->name)
				HC_IGNORE_RESULT (write (fh, notify->name, strlen (notify->name)));

			if (notify->account)
			{
				if (notify->name)
					HC_IGNORE_RESULT (write (fh, " ", 1));
				HC_IGNORE_RESULT (write (fh, "account=", 8));
				HC_IGNORE_RESULT (write (fh, notify->account, strlen (notify->account)));
			}

			if (notify->networks)
			{
				HC_IGNORE_RESULT (write (fh, " ", 1));
				if (notify->account)
				{
					HC_IGNORE_RESULT (write (fh, "networks=", 9));
					HC_IGNORE_RESULT (write (fh, notify->networks, strlen (notify->networks)));
				}
				else
				{
					/* Legacy-compatible: bare networks, no key= */
					HC_IGNORE_RESULT (write (fh, notify->networks, strlen (notify->networks)));
				}
			}
			HC_IGNORE_RESULT (write (fh, "\n", 1));
			list = list->next;
		}
		close (fh);
	}
        g_slist_free(list);
}

/* Strip one trailing \r if present (in case the conf has CRLF line endings
 * and waitline only stripped the \n). */
static void
rstrip_cr (char *s)
{
	size_t len = strlen (s);
	if (len > 0 && s[len - 1] == '\r')
		s[len - 1] = 0;
}

/* Parse one line of notify.conf. Layout:
 *   name                              (legacy: nick-only)
 *   name networks_list                (legacy: nick + bare networks)
 *   name account=X [networks=Y]       (new: nick + account)
 *   account=X [networks=Y]            (new: account-only)
 *
 * Backward compat note: old poxchat writes bare `name` or `name net_list`,
 * so we only split on key=value when we see `account=` or `networks=` as
 * a token; otherwise we treat the line as the legacy two-token form. */
static void
notify_load_line (char *line)
{
	char *sep;
	char *name = NULL;
	char *account = NULL;
	char *networks = NULL;

	rstrip_cr (line);

	/* Account-only form: "account=X [networks=Y]" */
	if (g_str_has_prefix (line, "account="))
	{
		account = line + 8;
		sep = strpbrk (account, " \t");
		if (sep)
		{
			*sep++ = 0;
			while (*sep == ' ' || *sep == '\t')
				sep++;
			if (g_str_has_prefix (sep, "networks="))
				networks = sep + 9;
		}
	}
	else
	{
		/* Name [space tail]. Tail is either "account=X [networks=Y]" or a
		 * legacy bare networks list. */
		name = line;
		sep = strpbrk (line, " \t");
		if (sep)
		{
			*sep++ = 0;
			while (*sep == ' ' || *sep == '\t')
				sep++;
			if (g_str_has_prefix (sep, "account="))
			{
				char *acct = sep + 8;
				char *sep2 = strpbrk (acct, " \t");
				account = acct;
				if (sep2)
				{
					*sep2++ = 0;
					while (*sep2 == ' ' || *sep2 == '\t')
						sep2++;
					if (g_str_has_prefix (sep2, "networks="))
						networks = sep2 + 9;
				}
			}
			else if (*sep)
			{
				/* Legacy bare networks list */
				networks = sep;
			}
		}
	}

	if ((name && *name) || (account && *account))
		notify_adduser (name, account, networks);
}

void
notify_load (void)
{
	int fh;
	char buf[256];
	int len;

	fh = poxchat_open_file ("notify.conf", O_RDONLY, 0, 0);
	if (fh == -1)
		return;

	/* Hand-rolled line reader: waitline() loses buffered content when the
	 * file ends without a trailing newline, which is easy to produce when
	 * hand-editing notify.conf. Instead, accumulate bytes into buf until
	 * we see '\n' OR EOF — either way, emit whatever we've got as a line. */
	len = 0;
	for (;;)
	{
		char c;
		int n = read (fh, &c, 1);

		if (n <= 0)
		{
			/* EOF: flush any accumulated partial line. */
			if (len > 0)
			{
				buf[len] = 0;
				if (buf[0] != '#' && buf[0] != 0)
					notify_load_line (buf);
			}
			break;
		}

		if (c == '\n' || len + 1 >= (int) sizeof (buf))
		{
			buf[len] = 0;
			if (buf[0] != '#' && buf[0] != 0)
				notify_load_line (buf);
			len = 0;
			/* If the break was the buffer filling rather than a newline,
			 * drop the rest of this overly long line silently. */
			if (c != '\n')
			{
				while (read (fh, &c, 1) == 1 && c != '\n')
					; /* discard */
			}
			continue;
		}

		buf[len++] = c;
	}

	close (fh);
}

static struct notify_per_server *
notify_find (server *serv, char *nick)
{
	GSList *list = notify_list;
	struct notify_per_server *servnot;
	struct notify *notify;

	while (list)
	{
		notify = (struct notify *) list->data;

		/* notify_find backs the MONITOR/WATCH online/offline numerics which
		 * are always nick-keyed, so account-only entries (no name) don't
		 * participate here. */
		if (!notify->name)
		{
			list = list->next;
			continue;
		}

		servnot = notify_find_server_entry (notify, serv);
		if (!servnot)
		{
			list = list->next;
			continue;
		}

		if (!serv->p_cmp (notify->name, nick))
			return servnot;

		list = list->next;
	}

	return NULL;
}

static void
notify_announce_offline (server * serv, struct notify_per_server *servnot,
								 char *nick, int quiet, 
								 const message_tags_data *tags_data)
{
	session *sess;

	sess = serv->front_session;

	servnot->ison = FALSE;
	servnot->lastoff = time (0);
	if (!quiet)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYOFFLINE, sess, nick, serv->servername,
									  server_get_network (serv, TRUE), NULL, 0,
									  tags_data->timestamp);
	fe_notify_update (nick);
	fe_notify_update (0);
}

static void
notify_announce_online (server * serv, struct notify_per_server *servnot,
								char *nick, const message_tags_data *tags_data)
{
	session *sess;

	sess = serv->front_session;

	servnot->lastseen = time (0);
	if (servnot->ison)
		return;

	servnot->ison = TRUE;
	servnot->laston = time (0);
	EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYONLINE, sess, nick, serv->servername,
					 server_get_network (serv, TRUE), NULL, 0,
					 tags_data->timestamp);
	fe_notify_update (nick);
	fe_notify_update (0);

	if (prefs.hex_notify_whois_online)
	{

	    /* Let's do whois with idle time (like in /quote WHOIS %s %s) */

	    char *wii_str = g_strdup_printf ("%s %s", nick, nick);
	    serv->p_whois (serv, wii_str);
	    g_free (wii_str);
	}
}

/* handles numeric 601 */

void
notify_set_offline (server * serv, char *nick, int quiet,
						  const message_tags_data *tags_data)
{
	struct notify_per_server *servnot;

	servnot = notify_find (serv, nick);
	if (!servnot)
		return;

	notify_announce_offline (serv, servnot, nick, quiet, tags_data);
}

/* handles numeric 604 and 600 */

void
notify_set_online (server * serv, char *nick,
						 const message_tags_data *tags_data)
{
	struct notify_per_server *servnot;

	servnot = notify_find (serv, nick);
	if (!servnot)
		return;

	notify_announce_online (serv, servnot, nick, tags_data);
}

/* monitor can send lists for numeric 730/731 */

void
notify_set_offline_list (server * serv, char *users, int quiet,
						  const message_tags_data *tags_data)
{
	struct notify_per_server *servnot;
	char nick[NICKLEN];
	char *token, *chr;

	token = strtok (users, ",");
	while (token != NULL)
	{
		chr = strchr (token, '!');
		if (chr != NULL)
			*chr = '\0';

		g_strlcpy (nick, token, sizeof(nick));

		servnot = notify_find (serv, nick);
		if (servnot)
			notify_announce_offline (serv, servnot, nick, quiet, tags_data);

		token = strtok (NULL, ",");
	}
}

void
notify_set_online_list (server * serv, char *users,
						 const message_tags_data *tags_data)
{
	struct notify_per_server *servnot;
	char nick[NICKLEN];
	char *token, *chr;

	token = strtok (users, ",");
	while (token != NULL)
	{
		chr = strchr (token, '!');
		if (chr != NULL)
			*chr = '\0';

		g_strlcpy (nick, token, sizeof(nick));

		servnot = notify_find (serv, nick);
		if (servnot)
			notify_announce_online (serv, servnot, nick, tags_data);

		token = strtok (NULL, ",");
	}
}

static void
notify_watch (server * serv, char *nick, int add)
{
	char tbuf[256];
	char addchar = '+';

	if (!add)
		addchar = '-';

	if (serv->supports_monitor)
		g_snprintf (tbuf, sizeof (tbuf), "MONITOR %c %s", addchar, nick);
	else if (serv->supports_watch)
		g_snprintf (tbuf, sizeof (tbuf), "WATCH %c%s", addchar, nick);
	else
		return;

	serv->p_raw (serv, tbuf);
}

static void
notify_watch_all (struct notify *notify, int add)
{
	server *serv;
	GSList *list = serv_list;

	/* WATCH/MONITOR are nick-keyed; nothing to register for
	 * account-only entries. They rely on passive observation. */
	if (!notify->name)
		return;

	while (list)
	{
		serv = list->data;
		if (serv->connected && serv->end_of_motd && notify_do_network (notify, serv))
			notify_watch (serv, notify->name, add);
		list = list->next;
	}
}

static void
notify_flush_watches (server * serv, GSList *from, GSList *end)
{
	char tbuf[512];
	GSList *list;
	struct notify *notify;

	serv->supports_monitor ? strcpy (tbuf, "MONITOR + ") : strcpy (tbuf, "WATCH");

	list = from;
	while (list != end)
	{
		notify = list->data;
		if (serv->supports_monitor)
			g_strlcat (tbuf, ",", sizeof(tbuf));
		else
			g_strlcat (tbuf, " +", sizeof(tbuf));
		g_strlcat (tbuf, notify->name, sizeof(tbuf));
		list = list->next;
	}
	serv->p_raw (serv, tbuf);
}

/* called when logging in. e.g. when End of motd. */

void
notify_send_watches (server * serv)
{
	struct notify *notify;
	const int format_len = serv->supports_monitor ? 1 : 2; /* just , for monitor or + and space for watch */
	GSList *list;
	GSList *point;
	GSList *send_list = NULL;
	int len = 0;

	/* Only get the list for this network. Skip entries without a nick —
	 * those are account-only friends and WATCH/MONITOR can't track them. */
	list = notify_list;
	while (list)
	{
		notify = list->data;

		if (notify->name && notify_do_network (notify, serv))
		{
			send_list = g_slist_append (send_list, notify);
		}

		list = list->next;
	}

	/* Now send that list in batches */
	point = list = send_list;
	while (list)
	{
		notify = list->data;

		len += strlen (notify->name) + format_len;
		if (len > 500)
		{
			/* Too long send existing list */
			notify_flush_watches (serv, point, list);
			len = strlen (notify->name) + format_len;
			point = list; /* We left off here */
		}

		list = g_slist_next (list);
	}

	if (len) /* We had leftovers under 500, send them all */
	{
		notify_flush_watches (serv, point, NULL);
	}

	g_slist_free (send_list);
}

/* called when receiving a ISON 303 - should this func go? */

void
notify_markonline (server *serv, char *word[], const message_tags_data *tags_data)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list = notify_list;
	int i, seen;

	while (list)
	{
		notify = (struct notify *) list->data;
		if (!notify->name)
		{
			/* ISON is nick-only; account-only entries are tracked passively. */
			list = list->next;
			continue;
		}
		servnot = notify_find_server_entry (notify, serv);
		if (!servnot)
		{
			list = list->next;
			continue;
		}
		i = 4;
		seen = FALSE;
		while (*word[i])
		{
			if (!serv->p_cmp (notify->name, word[i]))
			{
				seen = TRUE;
				notify_announce_online (serv, servnot, notify->name, tags_data);
				break;
			}
			i++;
			/* FIXME: word[] is only a 32 element array, limits notify list to
			   about 27 people */
			if (i > PDIWORDS - 5)
			{
				/*fprintf (stderr, _("*** POXCHAT WARNING: notify list too large.\n"));*/
				break;
			}
		}
		if (!seen && servnot->ison)
		{
			notify_announce_offline (serv, servnot, notify->name, FALSE, tags_data);
		}
		list = list->next;
	}
	fe_notify_update (0);
}

/* yuck! Old routine for ISON notify */

static void
notify_checklist_for_server (server *serv)
{
	char outbuf[512];
	struct notify *notify;
	GSList *list = notify_list;
	int i = 0;

	strcpy (outbuf, "ISON ");
	while (list)
	{
		notify = list->data;
		if (notify->name && notify_do_network (notify, serv))
		{
			i++;
			strcat (outbuf, notify->name);
			strcat (outbuf, " ");
			if (strlen (outbuf) > 460)
			{
				/* LAME: we can't send more than 512 bytes to the server, but     *
				 * if we split it in two packets, our offline detection wouldn't  *
				 work                                                           */
				/*fprintf (stderr, _("*** POXCHAT WARNING: notify list too large.\n"));*/
				break;
			}
		}
		list = list->next;
	}

	if (i)
		serv->p_raw (serv, outbuf);
}

int
notify_checklist (void)	/* check ISON list */
{
	struct server *serv;
	GSList *list = serv_list;

	while (list)
	{
		serv = list->data;
		if (serv->connected && serv->end_of_motd && !serv->supports_watch && !serv->supports_monitor)
		{
			notify_checklist_for_server (serv);
		}
		list = list->next;
	}
	return 1;
}

void
notify_showlist (struct session *sess, const message_tags_data *tags_data)
{
	char outbuf[256];
	struct notify *notify;
	GSList *list = notify_list;
	struct notify_per_server *servnot;
	int i = 0;

	EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYHEAD, sess, NULL, NULL, NULL, NULL, 0,
								  tags_data->timestamp);
	while (list)
	{
		const char *display;

		i++;
		notify = (struct notify *) list->data;
		display = notify->name ? notify->name : notify->account;
		if (!display)
		{
			list = list->next;
			continue;
		}
		servnot = notify_find_server_entry (notify, sess->server);
		if (servnot && servnot->ison)
			g_snprintf (outbuf, sizeof (outbuf), _("  %-20s online\n"), display);
		else
			g_snprintf (outbuf, sizeof (outbuf), _("  %-20s offline\n"), display);
		PrintTextTimeStamp (sess, outbuf, tags_data->timestamp);
		list = list->next;
	}
	if (i)
	{
		sprintf (outbuf, "%d", i);
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYNUMBER, sess, outbuf, NULL, NULL, NULL,
									  0, tags_data->timestamp);
	} else
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYEMPTY, sess, NULL, NULL, NULL, NULL, 0,
									  tags_data->timestamp);
}

int
notify_deluser (char *name)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list = notify_list;

	/* Match on nick OR account so the UI can remove either flavor of
	 * entry using a single identifier string. rfc_casecmp for nick
	 * (case-insensitive IRC collation), strcmp for account (case-
	 * sensitive per IRCv3). */
	while (list)
	{
		notify = (struct notify *) list->data;
		gboolean match = FALSE;
		if (notify->name && !rfc_casecmp (notify->name, name))
			match = TRUE;
		else if (notify->account && !strcmp (notify->account, name))
			match = TRUE;

		if (match)
		{
			fe_notify_update (notify->name);
			/* Remove the records for each server */
			while (notify->server_list)
			{
				servnot = (struct notify_per_server *) notify->server_list->data;
				notify->server_list =
					g_slist_remove (notify->server_list, servnot);
				g_free (servnot);
			}
			notify_list = g_slist_remove (notify_list, notify);
			notify_watch_all (notify, FALSE);
			g_free (notify->networks);
			g_free (notify->account);
			g_free (notify->name);
			g_free (notify);
			fe_notify_update (0);
			fe_notify_friends_changed ();
			return 1;
		}
		list = list->next;
	}
	return 0;
}

void
notify_adduser (char *name, char *account, char *networks)
{
	struct notify *notify = g_new0 (struct notify, 1);

	if (name && *name)
		notify->name = g_strndup (name, NICKLEN - 1);
	if (account && *account)
		notify->account = g_strdup (account);

	if (networks != NULL)
		notify->networks = despacify_dup (networks);
	notify->server_list = 0;
	notify_list = g_slist_prepend (notify_list, notify);
	notify_checklist ();
	fe_notify_update (notify->name);
	fe_notify_update (0);
	fe_notify_friends_changed ();
	/* Only register with the server's WATCH/MONITOR for nick-bearing
	 * entries. Pure account entries are observed passively through
	 * account-notify / account-tag / extended-join. */
	if (notify->name)
		notify_watch_all (notify, TRUE);
}

/* tree_foreach callback: given the target account in `data` cast as a
 * (const char **), scan a userlist and set *data to NULL if any user
 * carries that account. Returning FALSE stops the traversal early. */
static int
notify_user_has_account_cb (const void *key, void *data)
{
	const struct User *user = key;
	const char **target = data;
	if (user->account && *target && !strcmp (user->account, *target))
	{
		*target = NULL;
		return FALSE; /* stop traversal */
	}
	return TRUE;
}

/* Returns TRUE if any user on `serv` (in any channel we see) currently
 * carries `account`. Used to guard offline transitions so a logout from
 * one nick doesn't mark a friend offline while other nicks on the same
 * account are still present. */
static gboolean
notify_account_still_present (server *serv, const char *account)
{
	GSList *slist;
	const char *probe = account;

	for (slist = sess_list; slist && probe; slist = slist->next)
	{
		session *sess = slist->data;
		if (sess->server != serv || !sess->usertree)
			continue;
		tree_foreach (sess->usertree,
			(tree_traverse_func *) notify_user_has_account_cb, &probe);
	}

	/* probe is NULL if the callback found a match and cleared it. */
	return probe == NULL;
}

/* Mirror of notify_account_observed for the logout direction. If a user's
 * account just transitioned away from `was_account`, and nobody else on
 * the server still carries that account, any matching friend entries go
 * offline. Silently no-ops if the account is still present on the server
 * (multi-nick-per-account case). */
void
notify_account_cleared (server *serv, const char *was_account,
                        const message_tags_data *tags_data)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list;
	gboolean had_match = FALSE;

	if (!was_account || !*was_account
	    || !strcmp (was_account, "*") || !strcmp (was_account, "0"))
		return;

	if (notify_account_still_present (serv, was_account))
		return;

	for (list = notify_list; list; list = list->next)
	{
		notify = list->data;
		if (!notify->account)
			continue;
		if (strcmp (notify->account, was_account) != 0)
			continue;

		had_match = TRUE;
		servnot = notify_find_server_entry (notify, serv);
		if (servnot && servnot->ison)
			notify_announce_offline (serv, servnot,
				notify->name ? notify->name : notify->account,
				FALSE, tags_data);
	}

	/* A user's friend-ness just flipped off on this server — tell open
	 * userlists to re-sort so the former friend drops back out of the
	 * top group. */
	if (had_match)
		fe_notify_friends_changed ();
}

void
notify_account_observed (server *serv, const char *nick,
                         const char *account,
                         const message_tags_data *tags_data)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list;
	gboolean had_match = FALSE;

	/* "*" and "0" are the conventional "no account" values from ACCOUNT /
	 * extended-join. Treat NULL/empty the same. */
	if (!account || !*account
	    || !strcmp (account, "*") || !strcmp (account, "0"))
		return;

	for (list = notify_list; list; list = list->next)
	{
		notify = list->data;
		if (!notify->account)
			continue;
		if (strcmp (notify->account, account) != 0)
			continue;

		had_match = TRUE;
		servnot = notify_find_server_entry (notify, serv);
		if (!servnot)
			continue;

		notify_announce_online (serv, servnot,
			(nick && *nick) ? (char *) nick : notify->account, tags_data);
	}

	/* The observed user's friend-ness just flipped on — re-sort so they
	 * bubble to the top of any open userlist showing this server. */
	if (had_match)
		fe_notify_friends_changed ();
}

gboolean
notify_is_in_list (server *serv, const char *name, const char *account)
{
	struct notify *notify;
	GSList *list = notify_list;

	while (list)
	{
		notify = (struct notify *) list->data;
		if (name && notify->name && !serv->p_cmp (notify->name, name))
			return TRUE;
		if (account && notify->account && !strcmp (notify->account, account))
			return TRUE;
		list = list->next;
	}

	return FALSE;
}

int
notify_isnotify (struct session *sess, char *name)
{
	struct notify *notify;
	struct notify_per_server *servnot;
	GSList *list = notify_list;

	while (list)
	{
		notify = (struct notify *) list->data;
		if (notify->name && !sess->server->p_cmp (notify->name, name))
		{
			servnot = notify_find_server_entry (notify, sess->server);
			if (servnot && servnot->ison)
				return TRUE;
		}
		list = list->next;
	}

	return FALSE;
}

void
notify_cleanup ()
{
	GSList *list = notify_list;
	GSList *nslist, *srvlist;
	struct notify *notify;
	struct notify_per_server *servnot;
	struct server *serv;
	int valid;

	while (list)
	{
		/* Traverse the list of notify structures */
		notify = (struct notify *) list->data;
		nslist = notify->server_list;
		while (nslist)
		{
			/* Look at each per-server structure */
			servnot = (struct notify_per_server *) nslist->data;

			/* Check the server is valid */
			valid = FALSE;
			srvlist = serv_list;
			while (srvlist)
			{
				serv = (struct server *) srvlist->data;
				if (servnot->server == serv)
				{
					valid = serv->connected;	/* Only valid if server is too */
					break;
				}
				srvlist = srvlist->next;
			}
			if (!valid)
			{
				notify->server_list =
					g_slist_remove (notify->server_list, servnot);
				g_free (servnot);
				nslist = notify->server_list;
			} else
			{
				nslist = nslist->next;
			}
		}
		list = list->next;
	}
	fe_notify_update (0);
}
