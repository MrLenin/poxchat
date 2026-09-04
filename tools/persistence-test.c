/* draft/persistence parser harness.  Links the real persistence.c and
 * asserts on the text it emits and on the server state it sets.  Prints
 * "ok" and exits 0 when every check passes; prints the failing check and
 * exits 1 at the first failure. */
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "poxchat.h"
#include "text.h"
#include "server.h"
#include "scrollback.h"
#include "chathistory.h"
#include "persistence.h"

/* --- app stubs (persistence.c's outside deps) --- */

static char captured[2048];
static int captured_count;
static int captured_event = -1;

void
text_emit (int index, session *sess, char *a, char *b, char *c, char *d,
           time_t timestamp)
{
	(void) sess;
	(void) b;
	(void) c;
	(void) d;
	(void) timestamp;
	captured_event = index;
	g_strlcpy (captured, a ? a : "", sizeof (captured));
	captured_count++;
}

static char sent[512];
static int sent_count;

void
tcp_sendf (server *serv, const char *fmt, ...)
{
	va_list ap;

	(void) serv;
	va_start (ap, fmt);
	g_vsnprintf (sent, sizeof (sent), fmt, ap);
	va_end (ap);
	sent_count++;
}

/* Scrollback stubs.  stub_cursor drives the whole cursor lookup: NULL
 * means there is no network to open a db for, which is the "attach-cursor
 * advertised but we have nothing to anchor on" path; otherwise that
 * string comes back as the global newest msgid. */
static const char *stub_cursor;
static int stub_db;			/* its address is the dummy scrollback_db handle */

char *
server_get_network (server *serv, gboolean fallback)
{
	(void) serv;
	(void) fallback;
	return stub_cursor ? (char *) "TestNet" : NULL;
}

scrollback_db *
scrollback_open (const char *network)
{
	(void) network;
	return (scrollback_db *) &stub_db;
}

char *
scrollback_get_global_newest_msgid (scrollback_db *db)
{
	(void) db;
	return stub_cursor ? g_strdup (stub_cursor) : NULL;
}

/* persistence.c must never drive catch-up itself: these count so the
 * FAIL cases can assert they stayed at zero. */
static int rearm_deferred_count;
static int rearm_targets_count;

void
chathistory_schedule_deferred (server *serv)
{
	(void) serv;
	rearm_deferred_count++;
}

void
chathistory_request_targets_on_reconnect (server *serv)
{
	(void) serv;
	rearm_targets_count++;
}

/* --- checks --- */

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf ("FAIL: %s\n", (msg)); \
		printf ("      last line: \"%s\" (%d emitted)\n", captured, captured_count); \
		exit (1); \
	} \
} while (0)

static void
reset_capture (void)
{
	captured[0] = '\0';
	captured_count = 0;
	captured_event = -1;
}

static void
reset_sent (void)
{
	sent[0] = '\0';
	sent_count = 0;
}

/* Feed one PERSISTENCE reply and return the line it printed. */
static const char *
reply (server *serv, const char *args)
{
	reset_capture ();
	persistence_handle_reply (serv, args, 0);
	return captured;
}

#define CHECK_REPLY(serv, args, expect) \
	CHECK (strcmp (reply ((serv), (args)), (expect)) == 0, "reply: " args)

int
main (void)
{
	server *serv = g_new0 (server, 1);

	serv->p_cmp = g_ascii_strcasecmp;
	g_strlcpy (serv->nick, "me", sizeof (serv->nick));
	g_strlcpy (serv->servername, "irc.example.org", sizeof (serv->servername));

	/* 1. persistence_match: the namespace-tolerant matcher. */
	CHECK (g_strcmp0 (persistence_match ("draft/persistence"), "") == 0,
	       "match draft/persistence");
	CHECK (g_strcmp0 (persistence_match ("persistence"), "") == 0,
	       "match persistence");
	CHECK (g_strcmp0 (persistence_match ("persistence/hold"), "hold") == 0,
	       "match persistence/hold");
	CHECK (g_strcmp0 (persistence_match ("draft/persistence/hold"), "hold") == 0,
	       "match draft/persistence/hold");
	CHECK (g_strcmp0 (persistence_match ("evilnet.github.io/persistence/hold"), "hold") == 0,
	       "match vendor persistence/hold");
	CHECK (g_strcmp0 (persistence_match ("evilnet.github.io/draft/persistence"), "") == 0,
	       "match vendor draft/persistence");
	CHECK (persistence_match ("draft/persistence-foo") == NULL,
	       "match rejects persistence-foo");
	CHECK (persistence_match ("avatar") == NULL, "match rejects avatar");
	CHECK (persistence_match ("") == NULL, "match rejects empty");
	CHECK (persistence_match (NULL) == NULL, "match rejects NULL");

	CHECK (persistence_is_bare_name ("draft/persistence"), "bare name: draft/persistence");
	CHECK (!persistence_is_bare_name ("draft/persistence/hold"), "bare name rejects sub-path");
	CHECK (!persistence_is_bare_name ("draft/chathistory"), "bare name rejects chathistory");

	/* 2. persistence_strip_namespace. */
	CHECK (strcmp (persistence_strip_namespace ("evilnet.github.io/bouncer-replay"),
	               "bouncer-replay") == 0, "strip vendor prefix");
	CHECK (strcmp (persistence_strip_namespace ("draft/x"), "x") == 0, "strip draft prefix");
	CHECK (strcmp (persistence_strip_namespace ("plain"), "plain") == 0, "strip leaves plain");
	CHECK (strcmp (persistence_strip_namespace ("nodot/x"), "nodot/x") == 0,
	       "strip leaves non-vendor path");

	/* 3. CAP LS value tokens: unknown ones ignored, namespaced ones accepted. */
	persistence_parse_cap_value (serv,
		"replay-control,profile,attach,evilnet.github.io/attach-cursor,bogus");
	CHECK (serv->persistence_tok_replay_control, "token replay-control");
	CHECK (serv->persistence_tok_profile, "token profile");
	CHECK (serv->persistence_tok_attach, "token attach");
	CHECK (serv->persistence_tok_attach_cursor, "token attach-cursor (vendor-scoped)");
	CHECK (!serv->persistence_tok_detach, "token detach not set");
	CHECK (!serv->persistence_tok_list, "token list not set");

	/* A narrower re-advertisement (CAP NEW) retracts the dropped tokens. */
	persistence_parse_cap_value (serv, "attach,list");
	CHECK (serv->persistence_tok_attach && serv->persistence_tok_list,
	       "narrower value keeps its own tokens");
	CHECK (!serv->persistence_tok_replay_control && !serv->persistence_tok_profile &&
	       !serv->persistence_tok_attach_cursor && !serv->persistence_tok_detach,
	       "narrower value retracts the dropped tokens");

	/* Empty tokens are skipped, not treated as a name. */
	persistence_parse_cap_value (serv, "attach,,list");
	CHECK (serv->persistence_tok_attach && serv->persistence_tok_list,
	       "empty token skipped");
	CHECK (!serv->persistence_tok_replay_control && !serv->persistence_tok_profile &&
	       !serv->persistence_tok_attach_cursor && !serv->persistence_tok_detach,
	       "empty token sets nothing else");

	/* 4. Every reply shape the spec defines. */
	CHECK_REPLY (serv, "STATUS DEFAULT ON", "Persistence: preference DEFAULT, effective ON");
	CHECK (captured_event == XP_TE_SERVTEXT, "STATUS prints a server-tab line");
	CHECK (serv->persistence_status_known, "STATUS sets status_known");
	CHECK (serv->persistence_effective, "STATUS DEFAULT ON is effective");

	CHECK_REPLY (serv, "STATUS OFF OFF", "Persistence: preference OFF, effective OFF");
	CHECK (serv->persistence_status_known, "STATUS OFF keeps status_known");
	CHECK (!serv->persistence_effective, "STATUS OFF OFF is not effective");

	CHECK_REPLY (serv, "STATUS ON", "Persistence: effective ON");
	CHECK (serv->persistence_effective, "legacy one-argument STATUS is effective");

	CHECK_REPLY (serv, "SET OFF", "Persistence: preference set to OFF");
	CHECK_REPLY (serv, "REPLAY STATUS DEFAULT ON",
	             "Persistence replay: preference DEFAULT, effective ON");
	CHECK_REPLY (serv, "REPLAY SET OFF", "Persistence replay: preference set to OFF");
	CHECK (serv->persistence_effective,
	       "REPLAY does not touch the session's effective state");

	CHECK_REPLY (serv, "PROFILE ENDOFLIST", "Persistence: end of profile list");
	CHECK_REPLY (serv, "PROFILE CREATED mobile parent=default",
	             "Persistence: profile mobile created (parent=default)");
	CHECK_REPLY (serv, "PROFILE CREATED mobile",
	             "Persistence: profile mobile created (parent=default)");
	CHECK_REPLY (serv, "PROFILE DELETED mobile", "Persistence: profile mobile deleted");
	CHECK_REPLY (serv, "PROFILE RENAMED mobile phone",
	             "Persistence: profile mobile renamed to phone");
	CHECK_REPLY (serv, "PROFILE default", "Persistence: profile default");
	CHECK_REPLY (serv, "PROFILE mobile parent=default hold=1",
	             "Persistence: profile mobile (parent=default hold=1)");
	CHECK_REPLY (serv, "PROFILE mobile channels :#a,#b",
	             "Persistence: profile mobile: channels = #a,#b");
	CHECK_REPLY (serv, "PROFILE mobile note :two words",
	             "Persistence: profile mobile: note = two words");
	CHECK_REPLY (serv, "PROFILE mobile channels", "Persistence: profile mobile: channels unset");
	CHECK_REPLY (serv, "ATTACH mobile", "Persistence: attached to profile mobile");
	CHECK_REPLY (serv, "DETACH OK", "Persistence: detached, the session was released");
	CHECK_REPLY (serv, "DETACH NOSESSION", "Persistence: no session to detach");

	/* Shapes we do not model are shown rather than swallowed. */
	CHECK_REPLY (serv, "LIST", "Persistence: LIST");
	CHECK_REPLY (serv, "SESSION 2 mobile :idle 5m", "Persistence: SESSION 2 mobile :idle 5m");

	/* Tokenizer edges: leading and repeated spaces are not words. */
	CHECK_REPLY (serv, "  STATUS   DEFAULT   ON",
	             "Persistence: preference DEFAULT, effective ON");

	/* More words than the split holds: the attribute tail is taken from
	 * the original line, so nothing is truncated. */
	CHECK_REPLY (serv, "PROFILE mobile a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8",
	             "Persistence: profile mobile (a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8)");
	CHECK_REPLY (serv, "SESSION 1 2 3 4 5 6 7 8 9", "Persistence: SESSION 1 2 3 4 5 6 7 8 9");

	/* Empty arguments must not print or crash. */
	reset_capture ();
	persistence_handle_reply (serv, "", 0);
	CHECK (captured_count == 0, "empty reply prints nothing");
	reset_capture ();
	persistence_handle_reply (serv, "   ", 0);
	CHECK (captured_count == 0, "all-space reply prints nothing");

	/* 5. Server-managed metadata. */
	reset_capture ();
	CHECK (persistence_handle_metadata (serv, "me", "hold", "1", 0), "metadata hold consumed");
	CHECK (serv->persistence_hold && serv->persistence_hold_known, "metadata hold set");
	CHECK (captured_count == 1, "metadata hold printed once");

	reset_capture ();
	persistence_handle_metadata (serv, "me", "hold", "1", 0);
	CHECK (captured_count == 0, "metadata hold resync is silent");

	reset_capture ();
	persistence_handle_metadata (serv, "me", "hold", "0", 0);
	CHECK (!serv->persistence_hold && serv->persistence_hold_known, "metadata hold cleared");
	CHECK (captured_count == 1, "metadata hold change printed");

	reset_capture ();
	persistence_handle_metadata (serv, "me", "hold", NULL, 0);
	CHECK (!serv->persistence_hold_known, "metadata hold unset clears known");
	CHECK (captured_count == 1, "metadata hold unset printed");

	/* 6. FAIL PERSISTENCE, with and without a context. */
	reset_capture ();
	persistence_handle_fail (serv, "CANNOT_DETACH", "DETACH",
	                         "Connection class enforces persistence", 0);
	CHECK (strcmp (captured, "Persistence error CANNOT_DETACH (DETACH): "
	                         "Connection class enforces persistence") == 0,
	       "fail with context");

	reset_capture ();
	persistence_handle_fail (serv, "INVALID_PARAMETERS", NULL, "Unknown subcommand", 0);
	CHECK (strcmp (captured, "Persistence error INVALID_PARAMETERS: Unknown subcommand") == 0,
	       "fail without context");

	reset_capture ();
	persistence_handle_fail (serv, "evilnet.github.io/CURSOR_UNKNOWN", NULL, "No such msgid", 0);
	CHECK (strcmp (captured, "Persistence error CURSOR_UNKNOWN: No such msgid") == 0,
	       "fail code shown with the namespace stripped");

	/* 7. persistence_reset clears everything this connection learned. */
	serv->have_persistence = TRUE;
	serv->persistence_tok_detach = TRUE;
	serv->persistence_tok_list = TRUE;
	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = TRUE;
	serv->persistence_attach_pending = TRUE;
	persistence_handle_reply (serv, "STATUS ON ON", 0);
	persistence_handle_metadata (serv, "me", "hold", "1", 0);
	persistence_reset (serv);
	CHECK (!serv->have_persistence && !serv->persistence_status_known &&
	       !serv->persistence_effective && !serv->persistence_attached &&
	       !serv->persistence_cursor_sent && !serv->persistence_attach_pending &&
	       !serv->persistence_hold &&
	       !serv->persistence_hold_known && !serv->persistence_tok_replay_control &&
	       !serv->persistence_tok_profile && !serv->persistence_tok_attach &&
	       !serv->persistence_tok_detach && !serv->persistence_tok_list &&
	       !serv->persistence_tok_attach_cursor, "reset clears every field");

	/* 8. PERSISTENCE ATTACH in the registration flight.  reset left every
	 * flag clear and persist_profile was never set, so this starts from
	 * an unconfigured connection. */
	serv->have_persistence = TRUE;

	reset_sent ();
	persistence_send_attach (serv);
	CHECK (sent_count == 0, "attach: no profile sends nothing");
	CHECK (!serv->persistence_attached, "attach: no profile leaves us unattached");

	g_strlcpy (serv->persist_profile, "desktop", sizeof (serv->persist_profile));

	serv->have_persistence = FALSE;
	reset_sent ();
	persistence_send_attach (serv);
	CHECK (sent_count == 0, "attach: without the cap sends nothing");
	serv->have_persistence = TRUE;

	/* A space in the name would become a second argument the server
	 * reads as a cursor; CR/LF would inject a line. */
	g_strlcpy (serv->persist_profile, "my desktop", sizeof (serv->persist_profile));
	reset_sent ();
	persistence_send_attach (serv);
	CHECK (sent_count == 0, "attach: a profile name with a space sends nothing");
	g_strlcpy (serv->persist_profile, "desktop", sizeof (serv->persist_profile));

	/* The attach token is a hint, not a precondition: without it we still
	 * attach and let a FAIL tell us otherwise. */
	CHECK (!serv->persistence_tok_attach, "attach: the attach token really is clear here");
	reset_sent ();
	persistence_send_attach (serv);
	CHECK (strcmp (sent, "PERSISTENCE ATTACH desktop\r\n") == 0,
	       "attach: sends without the attach token");
	CHECK (serv->persistence_attached, "attach: sets persistence_attached");
	CHECK (serv->persistence_attach_pending, "attach: the send is pending an answer");
	CHECK (!serv->persistence_cursor_sent, "attach: no cursor token, no cursor");
	CHECK (!persistence_server_drives_replay (serv),
	       "attach: without a cursor the server does not drive replay");

	reset_sent ();
	persistence_send_attach (serv);
	CHECK (sent_count == 0, "attach: a second call sends nothing");

	/* attach-cursor advertised but no network to look a cursor up in
	 * (stub_cursor NULL makes server_get_network return NULL). */
	serv->persistence_attached = FALSE;
	serv->persistence_tok_attach_cursor = TRUE;
	reset_sent ();
	persistence_send_attach (serv);
	CHECK (strcmp (sent, "PERSISTENCE ATTACH desktop\r\n") == 0,
	       "attach: cursor token but no scrollback sends the bare line");
	CHECK (serv->persistence_attached && !serv->persistence_cursor_sent,
	       "attach: no cursor available leaves cursor_sent FALSE");

	/* A cursor past the line budget is dropped, not truncated: a
	 * truncated ATTACH would lose its CRLF and swallow the CAP END. */
	{
		char *huge = g_strnfill (300, 'z');

		stub_cursor = huge;
		serv->persistence_attached = FALSE;
		reset_sent ();
		persistence_send_attach (serv);
		CHECK (strcmp (sent, "PERSISTENCE ATTACH desktop\r\n") == 0,
		       "attach: an over-long cursor is dropped, not sent");
		CHECK (!serv->persistence_cursor_sent,
		       "attach: a dropped cursor leaves cursor_sent FALSE");
		stub_cursor = NULL;
		g_free (huge);
	}

	/* The real thing: the msgid comes back and rides the ATTACH. */
	stub_cursor = "GkAAAaBqKXHeWt";
	serv->persistence_attached = FALSE;
	reset_sent ();
	persistence_send_attach (serv);
	CHECK (strcmp (sent, "PERSISTENCE ATTACH desktop GkAAAaBqKXHeWt\r\n") == 0,
	       "attach: sends the profile and the cursor");
	CHECK (serv->persistence_attached && serv->persistence_cursor_sent,
	       "attach: a cursor went out, cursor_sent set");
	CHECK (persistence_server_drives_replay (serv),
	       "attach: with a cursor the server drives replay");

	reset_sent ();
	persistence_send_attach (serv);
	CHECK (sent_count == 0, "attach: a second cursor-carrying call sends nothing");

	/* 9. FAIL fallbacks, for the FAIL that answers our own ATTACH. */
	CHECK (serv->persistence_attach_pending, "the attach is still pending an answer");
	rearm_deferred_count = 0;
	rearm_targets_count = 0;
	reset_capture ();
	persistence_handle_fail (serv, "CURSOR_UNKNOWN", "ATTACH", "No such msgid", 0);
	CHECK (!serv->persistence_cursor_sent, "CURSOR_UNKNOWN clears cursor_sent");
	CHECK (serv->persistence_attached, "CURSOR_UNKNOWN leaves us attached");
	CHECK (!persistence_server_drives_replay (serv),
	       "replay gate is off after CURSOR_UNKNOWN");
	CHECK (!serv->persistence_attach_pending, "CURSOR_UNKNOWN answers the attach");
	CHECK (rearm_deferred_count == 0 && rearm_targets_count == 0,
	       "CURSOR_UNKNOWN drives no catch-up itself: the regular call sites "
	       "run after it and now find the gate open");
	CHECK (captured_count == 1, "CURSOR_UNKNOWN still prints the error");

	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = TRUE;
	serv->persistence_attach_pending = TRUE;
	persistence_handle_fail (serv, "ACCOUNT_REQUIRED", "ATTACH",
	                         "You must be authenticated", 0);
	CHECK (!serv->persistence_attached && !serv->persistence_cursor_sent,
	       "ACCOUNT_REQUIRED clears attached and cursor_sent");
	CHECK (rearm_deferred_count == 0 && rearm_targets_count == 0,
	       "ACCOUNT_REQUIRED drives no catch-up either");

	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = TRUE;
	serv->persistence_attach_pending = TRUE;
	persistence_handle_fail (serv, "draft/INVALID_PARAMETERS", "ATTACH",
	                         "No such profile", 0);
	CHECK (!serv->persistence_attached && !serv->persistence_cursor_sent,
	       "INVALID_PARAMETERS clears attached and cursor_sent (namespaced)");

	/* An unrelated code, even while pending, touches neither flag. */
	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = TRUE;
	serv->persistence_attach_pending = TRUE;
	persistence_handle_fail (serv, "INTERNAL_ERROR", "ATTACH", "Oops", 0);
	CHECK (serv->persistence_attached && serv->persistence_cursor_sent,
	       "an unrelated FAIL code leaves the attach state alone");

	/* 10. A FAIL that is not the answer to our ATTACH must not drop the
	 * replay gate — the acknowledgement clears pending first. */
	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = TRUE;
	serv->persistence_attach_pending = TRUE;
	CHECK_REPLY (serv, "ATTACH desktop", "Persistence: attached to profile desktop");
	CHECK (!serv->persistence_attach_pending,
	       "the ATTACH acknowledgement clears pending");

	rearm_deferred_count = 0;
	rearm_targets_count = 0;
	persistence_handle_fail (serv, "INVALID_PARAMETERS", "PROFILE",
	                         "Profile is an ancestor of another profile", 0);
	CHECK (serv->persistence_attached && serv->persistence_cursor_sent,
	       "a FAIL that is not our attach's answer leaves the attach state alone");
	CHECK (persistence_server_drives_replay (serv),
	       "a FAIL that is not our attach's answer keeps the replay gate closed");
	CHECK (rearm_deferred_count == 0 && rearm_targets_count == 0,
	       "a FAIL that is not our attach's answer drives no catch-up");

	g_free (serv);
	printf ("ok\n");
	return 0;
}
