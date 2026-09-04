/* draft/persistence parser harness.  Links the real persistence.c and
 * asserts on the text it emits and on the server state it sets.  Prints
 * "ok" and exits 0 when every check passes; prints the failing check and
 * exits 1 at the first failure. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "poxchat.h"
#include "text.h"
#include "persistence.h"

/* --- app stub (persistence.c's only outside dep) --- */

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
	persistence_handle_reply (serv, "STATUS ON ON", 0);
	persistence_handle_metadata (serv, "me", "hold", "1", 0);
	persistence_reset (serv);
	CHECK (!serv->have_persistence && !serv->persistence_status_known &&
	       !serv->persistence_effective && !serv->persistence_attached &&
	       !serv->persistence_cursor_sent && !serv->persistence_hold &&
	       !serv->persistence_hold_known && !serv->persistence_tok_replay_control &&
	       !serv->persistence_tok_profile && !serv->persistence_tok_attach &&
	       !serv->persistence_tok_detach && !serv->persistence_tok_list &&
	       !serv->persistence_tok_attach_cursor, "reset clears every field");

	g_free (serv);
	printf ("ok\n");
	return 0;
}
