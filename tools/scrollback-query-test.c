/* scrollback-query-test.c — standalone check for scrollback query helpers.
 * Exit 0 = pass, 1 = fail.  Build: tools\build-scrollback-query-test.ps1
 * Run:   tools\out\scrollback-query-test.exe <scratch-dir>
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "scrollback.h"

/* --- app stubs (scrollback.c's outside deps) --- */
static const char *test_xdir = ".";
char *get_xdir (void) { return (char *) test_xdir; }
void poxchat_timing_log (const char *fmt, ...) { (void) fmt; }
/* fts_index_row calls this on every save; the FTS index's own contents
 * aren't under test here, so a pass-through copy is enough to link. */
gchar *strip_color (const char *text, int len, int flags)
{
	(void) len;
	(void) flags;
	return g_strdup (text);
}

static int failures = 0;
#define CHECK(name, cond) do { \
	if (!(cond)) { printf ("FAIL: %s\n", name); failures++; } \
} while (0)

int
main (int argc, char **argv)
{
	scrollback_db *db;
	char *msgid;

	if (argc > 1)
		test_xdir = argv[1];	/* scratch dir; harness creates <dir>/scrollback/ */

	db = scrollback_open ("testnet");
	if (!db) { printf ("FAIL: open\n"); return 1; }

	/* empty DB: no rows anywhere yet */
	msgid = scrollback_get_global_newest_msgid (db);
	CHECK ("empty db -> NULL", msgid == NULL);
	g_free (msgid);

	scrollback_db_save (db, "#a", 1000, "A1", "text-a1", TRUE);
	scrollback_db_save (db, "#a", 1005, "A2", "text-a2", TRUE);
	scrollback_db_save (db, "#b", 1003, "B1", "text-b1", TRUE);
	scrollback_db_save (db, "#b", 1009, "B2", "text-b2", TRUE);
	/* newer than everything above, but must be ignored: pending
	 * placeholder (echo-message not yet confirmed by the server) */
	scrollback_db_save (db, "#a", 1020, "pending:zzz", "text-pending", TRUE);
	/* newer than A2, but must be ignored: no msgid at all */
	scrollback_db_save (db, "#a", 1010, NULL, "text-nomsgid", TRUE);

	msgid = scrollback_get_global_newest_msgid (db);
	CHECK ("global newest is B2", g_strcmp0 (msgid, "B2") == 0);
	g_free (msgid);

	msgid = scrollback_get_newest_msgid (db, "#a");
	CHECK ("per-channel #a newest is still A2", g_strcmp0 (msgid, "A2") == 0);
	g_free (msgid);

	scrollback_shutdown ();

	if (failures)
	{
		printf ("FAIL: %d failures\n", failures);
		return 1;
	}

	printf ("ok\n");
	return 0;
}
