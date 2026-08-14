/* zstd-vfs-test.c — standalone crash/consistency harness for sqlite-zstd-vfs.
 *
 * Exit codes: 0 = ok, 1 = corrupt, 2 = error (open/busy/io), 3 = usage.
 * The "kill" command _exit()s after COMMIT to simulate process death.
 *
 * Build: tools\build-vfs-test.ps1   Run suite: tools\run-vfs-tests.ps1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <glib.h>
#include "../src/common/sqlite-zstd-vfs.h"

static sqlite3 *
open_inner (const char *path)
{
	sqlite3 *db = NULL;

	if (zstd_vfs_register ("zstd") != SQLITE_OK)
		return NULL;
	if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
	                     "zstd") != SQLITE_OK)
	{
		fprintf (stderr, "open failed: %s\n", db ? sqlite3_errmsg (db) : "?");
		if (db)
			sqlite3_close (db);
		return NULL;
	}
	sqlite3_exec (db, "PRAGMA journal_mode=MEMORY;", NULL, NULL, NULL);
	return db;
}

static int
cmd_fill (const char *path, int n, int do_kill)
{
	sqlite3 *db = open_inner (path);
	sqlite3_stmt *ins;
	char *err = NULL;
	int i;

	if (!db)
		return 2;
	sqlite3_exec (db, "CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)",
	              NULL, NULL, NULL);
	sqlite3_exec (db, "BEGIN", NULL, NULL, NULL);
	sqlite3_prepare_v2 (db, "INSERT INTO t (v) VALUES (?)", -1, &ins, NULL);
	for (i = 0; i < n; i++)
	{
		char buf[128];
		snprintf (buf, sizeof (buf),
		          "row %d: the quick brown fox jumps over the lazy dog", i);
		sqlite3_reset (ins);
		sqlite3_bind_text (ins, 1, buf, -1, SQLITE_TRANSIENT);
		if (sqlite3_step (ins) != SQLITE_DONE)
		{
			fprintf (stderr, "insert failed: %s\n", sqlite3_errmsg (db));
			return 2;
		}
	}
	sqlite3_finalize (ins);
	if (sqlite3_exec (db, "COMMIT", NULL, NULL, &err) != SQLITE_OK)
	{
		fprintf (stderr, "commit failed: %s\n", err ? err : "?");
		return 2;
	}
	if (do_kill)
	{
		printf ("committed %d rows, dying without close\n", n);
		fflush (stdout);
		_exit (9);	/* simulate process kill: no close, no meta persist */
	}
	printf ("committed %d rows, clean close\n", n);
	sqlite3_close (db);
	zstd_vfs_shutdown ();
	return 0;
}

static int
cmd_check (const char *path)
{
	sqlite3 *db = open_inner (path);
	sqlite3_stmt *st;
	int rc, ret = 2;

	if (!db)
		return 2;
	rc = sqlite3_prepare_v2 (db, "PRAGMA quick_check(1)", -1, &st, NULL);
	if (rc != SQLITE_OK)
	{
		fprintf (stderr, "quick_check prepare rc=%d: %s\n", rc, sqlite3_errmsg (db));
		sqlite3_close (db);
		return (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) ? 1 : 2;
	}
	rc = sqlite3_step (st);
	if (rc == SQLITE_ROW)
	{
		const char *v = (const char *) sqlite3_column_text (st, 0);
		printf ("quick_check: %s\n", v ? v : "(null)");
		ret = (v && strcmp (v, "ok") == 0) ? 0 : 1;
	}
	else
	{
		fprintf (stderr, "quick_check step rc=%d: %s\n", rc, sqlite3_errmsg (db));
		ret = (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) ? 1 : 2;
	}
	sqlite3_finalize (st);

	if (ret == 0
	    && sqlite3_prepare_v2 (db, "SELECT COUNT(*) FROM t", -1, &st, NULL) == SQLITE_OK)
	{
		if (sqlite3_step (st) == SQLITE_ROW)
			printf ("rows: %d\n", sqlite3_column_int (st, 0));
		sqlite3_finalize (st);
	}
	sqlite3_close (db);
	zstd_vfs_shutdown ();
	return ret;
}

static int
cmd_geom (const char *path)	/* plain read-only open of the OUTER db */
{
	sqlite3 *db;
	sqlite3_stmt *st;
	int meta_count = -1, max_pgno = -1;

	if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return 2;
	if (sqlite3_prepare_v2 (db, "SELECT value FROM meta WHERE key='page_count'",
	                        -1, &st, NULL) == SQLITE_OK)
	{
		if (sqlite3_step (st) == SQLITE_ROW)
			meta_count = atoi ((const char *) sqlite3_column_text (st, 0));
		sqlite3_finalize (st);
	}
	else
		printf ("geom: outer not readable via read-only open (a pending WAL after kill is normal)\n");
	if (sqlite3_prepare_v2 (db, "SELECT MAX(pgno) FROM pages", -1, &st, NULL) == SQLITE_OK)
	{
		if (sqlite3_step (st) == SQLITE_ROW)
			max_pgno = sqlite3_column_int (st, 0);
		sqlite3_finalize (st);
	}
	printf ("meta_page_count=%d max_pgno=%d\n", meta_count, max_pgno);
	sqlite3_close (db);
	return 0;
}

static int
cmd_backup (const char *path, const char *dest)
{
	/* Deliberately no VFS open first: this exercises the corrupt-DB backup
	 * path where the file was never (or not cleanly) closed and the -wal
	 * still holds the un-checkpointed tail. */
	return zstd_vfs_backup_db (path, dest) == 0 ? 0 : 2;
}

static int
cmd_hold (const char *path, int seconds)	/* hold outer lock for busy tests */
{
	sqlite3 *db;

	if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
		return 2;
	if (sqlite3_exec (db, "BEGIN EXCLUSIVE", NULL, NULL, NULL) != SQLITE_OK)
	{
		fprintf (stderr, "hold: BEGIN EXCLUSIVE failed: %s\n", sqlite3_errmsg (db));
		sqlite3_close (db);
		return 2;
	}
	printf ("holding exclusive lock for %d s\n", seconds);
	fflush (stdout);
	g_usleep ((gulong) seconds * G_USEC_PER_SEC);
	sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
	sqlite3_close (db);
	return 0;
}

int
main (int argc, char **argv)
{
	if (argc >= 4 && strcmp (argv[2], "fill") == 0)
		return cmd_fill (argv[1], atoi (argv[3]), 0);
	if (argc >= 4 && strcmp (argv[2], "kill") == 0)
		return cmd_fill (argv[1], atoi (argv[3]), 1);
	if (argc >= 3 && strcmp (argv[2], "check") == 0)
		return cmd_check (argv[1]);
	if (argc >= 3 && strcmp (argv[2], "geom") == 0)
		return cmd_geom (argv[1]);
	if (argc >= 4 && strcmp (argv[2], "hold") == 0)
		return cmd_hold (argv[1], atoi (argv[3]));
	if (argc >= 4 && strcmp (argv[2], "backup") == 0)
		return cmd_backup (argv[1], argv[3]);
	fprintf (stderr, "usage: %s <db> fill N | kill N | check | geom | hold SECONDS | backup DEST\n",
	         argv[0]);
	return 3;
}
