/* sqlite-zstd-vfs.c — Transparent page-level zstd compression VFS for SQLite
 *
 * Architecture: "database inside a database"
 * - The on-disk file is a normal SQLite DB (the "outer DB")
 * - It contains a `pages` table holding compressed pages of the "inner DB"
 * - This VFS intercepts the inner DB's page I/O and translates to SQL on
 *   the outer DB.  The inner SQLite is unaware of compression.
 *
 * Based on concepts from sqlite_zstd_vfs (MIT, Mike Lin) adapted to pure C.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sqlite3.h>
#include <zstd.h>
#include <zdict.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "sqlite-zstd-vfs.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define COMPRESS_RAW       0   /* stored uncompressed */
#define COMPRESS_ZSTD      1   /* zstd without dictionary */
#define COMPRESS_ZSTD_DICT 2   /* zstd with dictionary */

#define DICT_PAGE_THRESHOLD  200   /* train dict after this many pages */
#define DICT_TRAINING_SAMPLES 2000
#define DICT_SIZE            (32 * 1024)

/* ------------------------------------------------------------------ */
/*  Structs                                                            */
/* ------------------------------------------------------------------ */

/* VFS-level state (one global instance) */
typedef struct {
	sqlite3_vfs base;        /* must be first — SQLite casts to this */
	sqlite3_vfs *real_vfs;   /* default OS VFS */
} zstd_vfs;

/* Per-file state for a compressed main database */
typedef struct {
	sqlite3_file base;       /* must be first */

	/* outer database */
	sqlite3 *outer_db;
	char *outer_path;

	/* inner database geometry */
	int page_size;           /* 0 until first write */
	int page_count;
	int meta_page_count_saved;	/* last page_count written to meta */
	sqlite3_int64 file_size; /* page_count * page_size */

	/* zstd contexts */
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
	ZSTD_CDict *cdict;      /* NULL until dictionary trained */
	ZSTD_DDict *ddict;

	/* prepared statements on outer DB */
	sqlite3_stmt *stmt_read;
	sqlite3_stmt *stmt_write;
	sqlite3_stmt *stmt_delete_above;
	sqlite3_stmt *stmt_max_pgno;

	/* transaction / lock state */
	int lock_level;
	int in_transaction;
} zstd_vfs_file;

/* Per-file state for passthrough (journal, temp, etc.) */
typedef struct {
	sqlite3_file base;       /* must be first */
	sqlite3_file *real_file; /* allocated for the real VFS's szOsFile */
} zstd_vfs_passthru;

/* Global VFS instance */
static zstd_vfs *g_vfs = NULL;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static sqlite3_io_methods zstd_vfs_io_methods;
static sqlite3_io_methods zstd_vfs_passthru_methods;

/* ------------------------------------------------------------------ */
/*  Outer DB helpers                                                   */
/* ------------------------------------------------------------------ */

static int
outer_db_init (zstd_vfs_file *f, const char *path, int flags)
{
	int rc;
	char *errmsg = NULL;
	char *wal_path;

	f->outer_path = g_strdup (path);

	/* A -wal sidecar at open means the previous session did not close
	 * cleanly (a clean close checkpoints and removes it); SQLite replays
	 * it below.  Log it so unclean exits stay diagnosable. */
	wal_path = g_strdup_printf ("%s-wal", path);
	if (g_file_test (wal_path, G_FILE_TEST_EXISTS))
		g_message ("zstd-vfs: WAL sidecar present at open (unclean previous exit), replaying: %s", path);
	g_free (wal_path);

	/* Open outer DB using the real (OS) VFS — avoids recursion */
	rc = sqlite3_open_v2 (path, &f->outer_db,
	                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK)
		return rc;

	/* Outer DB pragmas.  Order matters: locking_mode=EXCLUSIVE must be in
	 * effect before the first WAL-mode access so SQLite uses a heap
	 * wal-index and never needs a -shm file (single-process access is our
	 * model; see docs/design/2026-08-12-outer-wal-design.md). */
	sqlite3_exec (f->outer_db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, NULL);
	{
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2 (f->outer_db, "PRAGMA journal_mode=WAL;", -1, &st, NULL) == SQLITE_OK)
		{
			if (sqlite3_step (st) == SQLITE_ROW)
			{
				const char *mode = (const char *) sqlite3_column_text (st, 0);
				if (!mode || g_ascii_strcasecmp (mode, "wal") != 0)
					g_warning ("zstd-vfs: outer DB refused WAL, running journal_mode=%s: %s",
					           mode ? mode : "(null)", path);
			}
			sqlite3_finalize (st);
		}
	}
	sqlite3_exec (f->outer_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
	/* A write burst (e.g. chathistory backfill) balloons the -wal to its
	 * high-water mark, and SQLite never shrinks the file on its own — after
	 * a checkpoint it merely rewinds and overwrites in place.  With a size
	 * limit, the restart checkpoint truncates the file back down, so one
	 * burst doesn't pin megabytes on disk for the rest of the session. */
	sqlite3_exec (f->outer_db, "PRAGMA journal_size_limit=4194304;", NULL, NULL, NULL);

	/* Create schema */
	rc = sqlite3_exec (f->outer_db,
		"CREATE TABLE IF NOT EXISTS pages ("
		"  pgno          INTEGER PRIMARY KEY,"
		"  data          BLOB NOT NULL,"
		"  is_compressed INTEGER NOT NULL DEFAULT 1"
		");"
		"CREATE TABLE IF NOT EXISTS meta ("
		"  key   TEXT PRIMARY KEY,"
		"  value BLOB"
		");",
		NULL, NULL, &errmsg);
	if (rc != SQLITE_OK)
	{
		g_warning ("zstd-vfs: schema creation failed: %s", errmsg ? errmsg : "unknown");
		sqlite3_free (errmsg);
		return rc;
	}

	/* Prepare statements */
	rc = sqlite3_prepare_v2 (f->outer_db,
		"SELECT data, is_compressed FROM pages WHERE pgno = ?",
		-1, &f->stmt_read, NULL);
	if (rc != SQLITE_OK) goto fail;

	rc = sqlite3_prepare_v2 (f->outer_db,
		"INSERT OR REPLACE INTO pages (pgno, data, is_compressed) VALUES (?, ?, ?)",
		-1, &f->stmt_write, NULL);
	if (rc != SQLITE_OK) goto fail;

	rc = sqlite3_prepare_v2 (f->outer_db,
		"DELETE FROM pages WHERE pgno > ?",
		-1, &f->stmt_delete_above, NULL);
	if (rc != SQLITE_OK) goto fail;

	rc = sqlite3_prepare_v2 (f->outer_db,
		"SELECT MAX(pgno) FROM pages",
		-1, &f->stmt_max_pgno, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Load meta geometry (diagnostic only — authoritative values derived below) */
	{
		sqlite3_stmt *s;
		int meta_page_size = 0, meta_page_count = 0;
		int max_pgno = 0;

		rc = sqlite3_prepare_v2 (f->outer_db,
			"SELECT value FROM meta WHERE key = 'page_size'",
			-1, &s, NULL);
		if (rc == SQLITE_OK)
		{
			if (sqlite3_step (s) == SQLITE_ROW)
			{
				const char *v = (const char *)sqlite3_column_text (s, 0);
				if (v)
					meta_page_size = atoi (v);
			}
			sqlite3_finalize (s);
		}

		rc = sqlite3_prepare_v2 (f->outer_db,
			"SELECT value FROM meta WHERE key = 'page_count'",
			-1, &s, NULL);
		if (rc == SQLITE_OK)
		{
			if (sqlite3_step (s) == SQLITE_ROW)
			{
				const char *v = (const char *)sqlite3_column_text (s, 0);
				if (v)
					meta_page_count = atoi (v);
			}
			sqlite3_finalize (s);
		}

		/* meta.page_count is only refreshed at commit/close; after an unclean
		 * exit it is stale and must never shrink the file — a too-small size
		 * reads as a truncated (i.e. "corrupt") inner DB.  The pages table,
		 * covered by the outer DB's own journal, is the committed truth. */
		sqlite3_reset (f->stmt_max_pgno);
		rc = sqlite3_step (f->stmt_max_pgno);
		if (rc == SQLITE_ROW)
			max_pgno = sqlite3_column_int (f->stmt_max_pgno, 0);
		else
		{
			/* MAX(pgno) is an aggregate — it always returns exactly one
			 * row on success.  Anything else (BUSY, I/O error, ...) must
			 * not be swallowed: doing so leaves max_pgno at 0, which
			 * presents a healthy-looking *empty* database and invites
			 * the app to silently rebuild over real history. */
			g_warning ("zstd-vfs: %s: MAX(pgno) query failed (%d): %s",
			           path, rc, sqlite3_errmsg (f->outer_db));
			sqlite3_reset (f->stmt_max_pgno);
			return SQLITE_ERROR;
		}
		sqlite3_reset (f->stmt_max_pgno);

		f->page_size = meta_page_size;
		if (f->page_size <= 0 && max_pgno >= 1)
		{
			/* Fall back to the inner header: page 1 is stored raw and
			 * carries the page size at bytes 16-17, big-endian (1 = 64K). */
			sqlite3_reset (f->stmt_read);
			sqlite3_bind_int (f->stmt_read, 1, 1);
			if (sqlite3_step (f->stmt_read) == SQLITE_ROW
			    && sqlite3_column_int (f->stmt_read, 1) == COMPRESS_RAW
			    && sqlite3_column_bytes (f->stmt_read, 0) >= 100)
			{
				const unsigned char *hdr = sqlite3_column_blob (f->stmt_read, 0);
				int ps = (hdr[16] << 8) | hdr[17];

				/* Only accept plausible SQLite page sizes: 1 means 64K,
				 * otherwise a power of two in [512, 32768].  Anything
				 * else is a corrupt/foreign header — leave page_size at
				 * 0 rather than trust a bogus value. */
				if (ps == 1)
					f->page_size = 65536;
				else if (ps >= 512 && ps <= 32768 && (ps & (ps - 1)) == 0)
					f->page_size = ps;
				else
					g_warning ("zstd-vfs: %s: implausible page_size %d in "
					           "page-1 header, ignoring", path, ps);
			}
			sqlite3_reset (f->stmt_read);
		}

		f->page_count = max_pgno;
		f->meta_page_count_saved = meta_page_count;
		if (f->page_size > 0)
			f->file_size = (sqlite3_int64)f->page_size * f->page_count;
		else if (max_pgno >= 1)
		{
			/* Pages exist but page_size could not be determined (meta
			 * missing and the page-1 header fallback above rejected an
			 * implausible value) — this is a populated database we
			 * cannot read, NOT an empty one.  Presenting it as empty
			 * (page_count = 0) would let SQLite happily write a fresh
			 * page 1 straight over real history — the same
			 * silent-data-loss door the MAX(pgno) check above closes.
			 * Fail the open instead of guessing. */
			g_warning ("zstd-vfs: %s: %d page(s) present but page_size could "
			           "not be determined — refusing to open as empty",
			           path, max_pgno);
			return SQLITE_CORRUPT;
		}
		else
			/* Genuinely fresh/empty database: page_count is already 0. */
			f->page_count = 0;

		if (meta_page_count > 0 && meta_page_count != f->page_count)
			g_message ("zstd-vfs: %s: derived page_count %d (stale meta said %d) — self-healed",
			           path, f->page_count, meta_page_count);
	}

	/* Load dictionary if available */
	{
		sqlite3_stmt *s;
		rc = sqlite3_prepare_v2 (f->outer_db,
			"SELECT value FROM meta WHERE key = 'zstd_dict'",
			-1, &s, NULL);
		if (rc == SQLITE_OK)
		{
			if (sqlite3_step (s) == SQLITE_ROW)
			{
				const void *blob = sqlite3_column_blob (s, 0);
				int sz = sqlite3_column_bytes (s, 0);
				if (blob && sz > 0)
				{
					f->cdict = ZSTD_createCDict (blob, sz, 3);
					f->ddict = ZSTD_createDDict (blob, sz);
				}
			}
			sqlite3_finalize (s);
		}
	}

	return SQLITE_OK;

fail:
	g_warning ("zstd-vfs: prepare failed: %s", sqlite3_errmsg (f->outer_db));
	return SQLITE_ERROR;
}

static void
outer_db_save_meta_int (zstd_vfs_file *f, const char *key, int value)
{
	sqlite3_stmt *s;
	char buf[32];

	g_snprintf (buf, sizeof (buf), "%d", value);
	if (sqlite3_prepare_v2 (f->outer_db,
		"INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
		-1, &s, NULL) == SQLITE_OK)
	{
		sqlite3_bind_text (s, 1, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text (s, 2, buf, -1, SQLITE_TRANSIENT);
		sqlite3_step (s);
		sqlite3_finalize (s);
	}
}

static void
outer_db_close (zstd_vfs_file *f)
{
	if (f->stmt_read)         sqlite3_finalize (f->stmt_read);
	if (f->stmt_write)        sqlite3_finalize (f->stmt_write);
	if (f->stmt_delete_above) sqlite3_finalize (f->stmt_delete_above);
	if (f->stmt_max_pgno)     sqlite3_finalize (f->stmt_max_pgno);

	if (f->outer_db)
	{
		/* Persist geometry */
		if (f->page_size > 0)
			outer_db_save_meta_int (f, "page_size", f->page_size);
		if (f->page_count > 0)
			outer_db_save_meta_int (f, "page_count", f->page_count);

		sqlite3_close (f->outer_db);
	}

	if (f->cctx) ZSTD_freeCCtx (f->cctx);
	if (f->dctx) ZSTD_freeDCtx (f->dctx);
	if (f->cdict) ZSTD_freeCDict (f->cdict);
	if (f->ddict) ZSTD_freeDDict (f->ddict);

	g_free (f->outer_path);
}

/* ------------------------------------------------------------------ */
/*  Compression helpers                                                */
/* ------------------------------------------------------------------ */

/* Compress a page.  Returns compressed blob (caller frees) or NULL
 * if compression is not worthwhile.  Sets *method. */
static void *
compress_page (zstd_vfs_file *f, const void *data, int data_len,
               int *out_len, int *method)
{
	size_t bound, result;
	void *buf;

	bound = ZSTD_compressBound (data_len);
	buf = g_malloc (bound);

	if (f->cdict)
	{
		result = ZSTD_compress_usingCDict (f->cctx, buf, bound,
		                                   data, data_len, f->cdict);
		*method = COMPRESS_ZSTD_DICT;
	}
	else
	{
		result = ZSTD_compressCCtx (f->cctx, buf, bound,
		                            data, data_len, 3);
		*method = COMPRESS_ZSTD;
	}

	if (ZSTD_isError (result) || (int)result >= data_len)
	{
		/* compression failed or expanded — store raw */
		g_free (buf);
		return NULL;
	}

	*out_len = (int)result;
	return buf;
}

/* Decompress a page into buf (which has buf_len bytes available). */
static int
decompress_page (zstd_vfs_file *f, const void *src, int src_len,
                 void *buf, int buf_len, int method)
{
	size_t result;

	if (method == COMPRESS_ZSTD_DICT && f->ddict)
		result = ZSTD_decompress_usingDDict (f->dctx, buf, buf_len,
		                                     src, src_len, f->ddict);
	else
		result = ZSTD_decompressDCtx (f->dctx, buf, buf_len, src, src_len);

	if (ZSTD_isError (result))
	{
		g_warning ("zstd-vfs: decompression failed: %s",
		           ZSTD_getErrorName (result));
		return SQLITE_IOERR_READ;
	}

	return SQLITE_OK;
}

/* ------------------------------------------------------------------ */
/*  Dictionary training                                                */
/* ------------------------------------------------------------------ */

static void
train_dictionary (zstd_vfs_file *f)
{
	sqlite3_stmt *sel;
	int rc, sample_count = 0;
	GByteArray *samples;
	size_t *sizes;
	void *dict_buf;
	size_t dict_size;
	sqlite3_stmt *ins;

	if (f->cdict)
		return;  /* already have one */

	if (f->page_count < DICT_PAGE_THRESHOLD)
		return;

	rc = sqlite3_prepare_v2 (f->outer_db,
		"SELECT data FROM pages WHERE is_compressed = 0 AND pgno > 1 "
		"ORDER BY RANDOM() LIMIT ?",
		-1, &sel, NULL);
	if (rc != SQLITE_OK)
		return;

	sqlite3_bind_int (sel, 1, DICT_TRAINING_SAMPLES);

	samples = g_byte_array_new ();
	sizes = g_new0 (size_t, DICT_TRAINING_SAMPLES);

	while (sqlite3_step (sel) == SQLITE_ROW && sample_count < DICT_TRAINING_SAMPLES)
	{
		const void *blob = sqlite3_column_blob (sel, 0);
		int len = sqlite3_column_bytes (sel, 0);
		if (blob && len > 0)
		{
			g_byte_array_append (samples, blob, len);
			sizes[sample_count++] = len;
		}
	}
	sqlite3_finalize (sel);

	if (sample_count < 50)
	{
		g_byte_array_free (samples, TRUE);
		g_free (sizes);
		return;
	}

	dict_buf = g_malloc (DICT_SIZE);
	dict_size = ZDICT_trainFromBuffer (dict_buf, DICT_SIZE,
	                                   samples->data, sizes, sample_count);

	g_byte_array_free (samples, TRUE);
	g_free (sizes);

	if (ZDICT_isError (dict_size))
	{
		g_warning ("zstd-vfs: dictionary training failed: %s",
		           ZDICT_getErrorName (dict_size));
		g_free (dict_buf);
		return;
	}

	/* Save dictionary to outer DB */
	rc = sqlite3_prepare_v2 (f->outer_db,
		"INSERT OR REPLACE INTO meta (key, value) VALUES ('zstd_dict', ?)",
		-1, &ins, NULL);
	if (rc == SQLITE_OK)
	{
		sqlite3_bind_blob (ins, 1, dict_buf, (int)dict_size, SQLITE_TRANSIENT);
		sqlite3_step (ins);
		sqlite3_finalize (ins);
	}

	/* Activate dictionary */
	f->cdict = ZSTD_createCDict (dict_buf, dict_size, 3);
	f->ddict = ZSTD_createDDict (dict_buf, dict_size);
	g_free (dict_buf);

	g_message ("zstd-vfs: trained %d-byte dictionary from %d pages for %s",
	           (int)dict_size, sample_count, f->outer_path);
}

/* ------------------------------------------------------------------ */
/*  Outer transaction helpers                                          */
/* ------------------------------------------------------------------ */

/* Every page write must land inside an outer transaction — a crash
 * between autocommitted page writes would leave the inner DB torn with
 * no journal to roll back (the inner runs journal_mode=MEMORY). */
static int
begin_outer (zstd_vfs_file *f)
{
	int rc;

	if (f->in_transaction)
	{
		if (!sqlite3_get_autocommit (f->outer_db))
			return SQLITE_OK;

		/* SQLite auto-rolls-back a transaction when a statement step
		 * returns SQLITE_FULL/IOERR/BUSY/NOMEM/INTERRUPT — our flag is
		 * stale: the transaction is gone even though we never called
		 * COMMIT/ROLLBACK.  sqlite3_get_autocommit() is the documented
		 * way to detect this.  Clear the flag and fall through to open
		 * a fresh transaction; never let a subsequent write believe
		 * it's already covered and autocommit on its own. */
		f->in_transaction = 0;
	}
	rc = sqlite3_exec (f->outer_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
	if (rc != SQLITE_OK)
	{
		g_warning ("zstd-vfs: BEGIN IMMEDIATE failed (%d): %s",
		           rc, sqlite3_errmsg (f->outer_db));
		return rc;
	}
	f->in_transaction = 1;
	return SQLITE_OK;
}

static int
commit_outer (zstd_vfs_file *f)
{
	int rc;

	if (!f->in_transaction)
		return SQLITE_OK;

	if (sqlite3_get_autocommit (f->outer_db))
	{
		/* Mirrors the check in begin_outer: the transaction was already
		 * auto-rolled-back by SQLite before we got a chance to commit
		 * it, so our flag was stale.  There is nothing to COMMIT — the
		 * outer DB is already back at its pre-transaction state, and
		 * every page write issued since the last successful commit
		 * either never happened or was undone.  Report this as a
		 * failure (not SQLITE_OK): callers must not be told the commit
		 * succeeded when in fact no commit happened at all. */
		g_warning ("zstd-vfs: outer transaction was rolled back by SQLite "
		           "before commit (server-side abort) — writes since the "
		           "last successful commit did not land");
		f->in_transaction = 0;
		return SQLITE_ERROR;
	}

	/* Keep meta.page_count fresh as of every commit (diagnostic; the
	 * open path derives the real value from MAX(pgno)). */
	if (f->page_count != f->meta_page_count_saved)
	{
		outer_db_save_meta_int (f, "page_count", f->page_count);
		f->meta_page_count_saved = f->page_count;
	}

	rc = sqlite3_exec (f->outer_db, "COMMIT", NULL, NULL, NULL);
	f->in_transaction = 0;
	if (rc != SQLITE_OK)
	{
		g_warning ("zstd-vfs: COMMIT failed (%d): %s — rolling back",
		           rc, sqlite3_errmsg (f->outer_db));
		sqlite3_exec (f->outer_db, "ROLLBACK", NULL, NULL, NULL);
		return rc;
	}
	return SQLITE_OK;
}

/* ------------------------------------------------------------------ */
/*  sqlite3_io_methods — compressed main database file                 */
/* ------------------------------------------------------------------ */

static int
zvfs_close (sqlite3_file *file)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;

	/* Commit any open transaction */
	commit_outer (f);

	/* Train dictionary if we don't have one yet */
	train_dictionary (f);

	outer_db_close (f);
	return SQLITE_OK;
}

static int
zvfs_read (sqlite3_file *file, void *buf, int iAmt, sqlite3_int64 iOfst)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;
	int pgno, rc;

	/* Before page_size is known (empty DB), any read returns short */
	if (f->page_size == 0)
	{
		memset (buf, 0, iAmt);
		return SQLITE_IOERR_SHORT_READ;
	}

	pgno = (int)(iOfst / f->page_size) + 1;

	/* Past EOF */
	if (pgno > f->page_count)
	{
		memset (buf, 0, iAmt);
		return SQLITE_IOERR_SHORT_READ;
	}

	sqlite3_reset (f->stmt_read);
	sqlite3_bind_int (f->stmt_read, 1, pgno);

	rc = sqlite3_step (f->stmt_read);
	if (rc == SQLITE_DONE)
	{
		/* Page genuinely absent — legitimate sparse/EOF read */
		memset (buf, 0, iAmt);
		return SQLITE_IOERR_SHORT_READ;
	}
	if (rc != SQLITE_ROW)
	{
		/* BUSY / I/O error / etc. — never fake zeroed data, it reads
		 * as a truncated DB and triggers corruption recovery upstream */
		g_warning ("zstd-vfs: read page %d failed (%d): %s",
		           pgno, rc, sqlite3_errmsg (f->outer_db));
		memset (buf, 0, iAmt);
		return SQLITE_IOERR_READ;
	}

	{
		const void *data = sqlite3_column_blob (f->stmt_read, 0);
		int data_len = sqlite3_column_bytes (f->stmt_read, 0);
		int is_compressed = sqlite3_column_int (f->stmt_read, 1);
		int sub_page_offset = (int)(iOfst % f->page_size);
		int ret = SQLITE_OK;

		if (is_compressed == COMPRESS_RAW)
		{
			/* Uncompressed — handle sub-page reads */
			if (sub_page_offset == 0 && iAmt == data_len)
				memcpy (buf, data, iAmt);
			else if (sub_page_offset + iAmt <= data_len)
				memcpy (buf, (const char *)data + sub_page_offset, iAmt);
			else
			{
				/* Partial read at/past end of stored data */
				int avail = data_len - sub_page_offset;
				if (avail > 0)
					memcpy (buf, (const char *)data + sub_page_offset, avail);
				memset ((char *)buf + (avail > 0 ? avail : 0), 0,
				        iAmt - (avail > 0 ? avail : 0));
				ret = SQLITE_IOERR_SHORT_READ;
			}
		}
		else
		{
			/* Compressed — must decompress full page, then copy portion */
			if (sub_page_offset == 0 && iAmt == f->page_size)
			{
				/* Common case: full page read */
				ret = decompress_page (f, data, data_len, buf, iAmt,
				                       is_compressed);
			}
			else
			{
				/* Sub-page read of compressed data — decompress to temp */
				void *tmp = g_malloc (f->page_size);
				ret = decompress_page (f, data, data_len, tmp,
				                       f->page_size, is_compressed);
				if (ret == SQLITE_OK)
					memcpy (buf, (char *)tmp + sub_page_offset, iAmt);
				g_free (tmp);
			}
		}

		/* Reset NOW, not at the next call's reset-before-use: a stepped
		 * SELECT left in ROW state keeps an implicit read transaction open
		 * on the outer connection, and its pinned snapshot caps every WAL
		 * auto-checkpoint at zero pages backfilled — the WAL then grows
		 * unbounded for the whole session.  Must come after the copies
		 * above: the column blob pointer dies at reset. */
		sqlite3_reset (f->stmt_read);
		return ret;
	}
}

static int
zvfs_write (sqlite3_file *file, const void *buf, int iAmt, sqlite3_int64 iOfst)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;
	int pgno, rc;
	void *compressed = NULL;
	int compressed_len = 0;
	int method = COMPRESS_RAW;

	if (begin_outer (f) != SQLITE_OK)
		return SQLITE_IOERR_WRITE;

	/* Page size detection: first write reveals it */
	if (f->page_size == 0)
	{
		f->page_size = iAmt;
		outer_db_save_meta_int (f, "page_size", f->page_size);
	}

	pgno = (int)(iOfst / f->page_size) + 1;

	/* Page 1: always store raw (SQLite needs uncompressed header) */
	if (pgno > 1)
	{
		compressed = compress_page (f, buf, iAmt, &compressed_len, &method);
	}

	sqlite3_reset (f->stmt_write);
	sqlite3_bind_int (f->stmt_write, 1, pgno);

	if (compressed)
	{
		sqlite3_bind_blob (f->stmt_write, 2, compressed,
		                   compressed_len, SQLITE_TRANSIENT);
		sqlite3_bind_int (f->stmt_write, 3, method);
		g_free (compressed);
	}
	else
	{
		sqlite3_bind_blob (f->stmt_write, 2, buf, iAmt, SQLITE_TRANSIENT);
		sqlite3_bind_int (f->stmt_write, 3, COMPRESS_RAW);
	}

	rc = sqlite3_step (f->stmt_write);
	if (rc != SQLITE_DONE)
	{
		g_warning ("zstd-vfs: write page %d failed: %s",
		           pgno, sqlite3_errmsg (f->outer_db));
		return SQLITE_IOERR_WRITE;
	}

	/* Update geometry */
	if (pgno > f->page_count)
		f->page_count = pgno;
	f->file_size = (sqlite3_int64)f->page_count * f->page_size;

	return SQLITE_OK;
}

static int
zvfs_truncate (sqlite3_file *file, sqlite3_int64 nByte)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;
	int new_count;

	if (f->page_size == 0)
		return SQLITE_OK;

	new_count = (int)(nByte / f->page_size);

	if (begin_outer (f) != SQLITE_OK)
		return SQLITE_IOERR_TRUNCATE;

	sqlite3_reset (f->stmt_delete_above);
	sqlite3_bind_int (f->stmt_delete_above, 1, new_count);
	if (sqlite3_step (f->stmt_delete_above) != SQLITE_DONE)
	{
		g_warning ("zstd-vfs: truncate to %d pages failed: %s",
		           new_count, sqlite3_errmsg (f->outer_db));
		return SQLITE_IOERR_TRUNCATE;
	}

	f->page_count = new_count;
	f->file_size = nByte;

	return SQLITE_OK;
}

static int
zvfs_sync (sqlite3_file *file, int flags)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;

	if (commit_outer (f) != SQLITE_OK)
		return SQLITE_IOERR_FSYNC;

	/* SQLite may keep writing under the same lock hold after a sync
	 * (e.g. multi-step commits); reopen the envelope immediately so no
	 * write ever lands outside a transaction.  Best-effort: on failure
	 * the begin_outer in zvfs_write retries and reports properly. */
	if (f->lock_level >= 2)
		begin_outer (f);

	return SQLITE_OK;
}

static int
zvfs_file_size (sqlite3_file *file, sqlite3_int64 *pSize)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;
	*pSize = f->file_size;
	return SQLITE_OK;
}

static int
zvfs_lock (sqlite3_file *file, int level)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;

	if (level >= 2 && begin_outer (f) != SQLITE_OK) /* RESERVED or higher */
		return SQLITE_BUSY;

	f->lock_level = level;
	return SQLITE_OK;
}

static int
zvfs_unlock (sqlite3_file *file, int level)
{
	zstd_vfs_file *f = (zstd_vfs_file *)file;

	/* When dropping below RESERVED, the inner SQLite's implicit
	 * transaction is done — commit the outer DB.  With journal_mode=MEMORY,
	 * xSync may never be called, so this is our commit point. */
	if (level < 2 && f->lock_level >= 2)
		commit_outer (f);

	f->lock_level = level;
	return SQLITE_OK;
}

static int
zvfs_check_reserved_lock (sqlite3_file *file, int *pResOut)
{
	*pResOut = 0;
	return SQLITE_OK;
}

static int
zvfs_file_control (sqlite3_file *file, int op, void *pArg)
{
	return SQLITE_NOTFOUND;
}

static int
zvfs_sector_size (sqlite3_file *file)
{
	return 4096;
}

static int
zvfs_device_characteristics (sqlite3_file *file)
{
	return 0;
}

/* ------------------------------------------------------------------ */
/*  sqlite3_io_methods — passthrough for journal/temp files            */
/* ------------------------------------------------------------------ */

static int pt_close (sqlite3_file *file)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	int rc = SQLITE_OK;
	if (p->real_file && p->real_file->pMethods)
		rc = p->real_file->pMethods->xClose (p->real_file);
	g_free (p->real_file);
	return rc;
}

static int pt_read (sqlite3_file *file, void *buf, int iAmt, sqlite3_int64 iOfst)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xRead (p->real_file, buf, iAmt, iOfst);
}

static int pt_write (sqlite3_file *file, const void *buf, int iAmt, sqlite3_int64 iOfst)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xWrite (p->real_file, buf, iAmt, iOfst);
}

static int pt_truncate (sqlite3_file *file, sqlite3_int64 sz)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xTruncate (p->real_file, sz);
}

static int pt_sync (sqlite3_file *file, int flags)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xSync (p->real_file, flags);
}

static int pt_file_size (sqlite3_file *file, sqlite3_int64 *pSize)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xFileSize (p->real_file, pSize);
}

static int pt_lock (sqlite3_file *file, int level)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xLock (p->real_file, level);
}

static int pt_unlock (sqlite3_file *file, int level)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xUnlock (p->real_file, level);
}

static int pt_check_reserved (sqlite3_file *file, int *pResOut)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xCheckReservedLock (p->real_file, pResOut);
}

static int pt_file_control (sqlite3_file *file, int op, void *pArg)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xFileControl (p->real_file, op, pArg);
}

static int pt_sector_size (sqlite3_file *file)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xSectorSize (p->real_file);
}

static int pt_device_chars (sqlite3_file *file)
{
	zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
	return p->real_file->pMethods->xDeviceCharacteristics (p->real_file);
}

/* ------------------------------------------------------------------ */
/*  sqlite3_vfs methods                                                */
/* ------------------------------------------------------------------ */

/* The inner logical DB shares its on-disk path with the outer container.
 * Sidecar files at that path (-wal/-journal/-shm) therefore belong to the
 * OUTER connection, never to the inner pager: the inner is pinned to
 * journal_mode=MEMORY and must not detect, open, or delete sidecars that
 * are really the outer's (its unconditional hot-WAL/hot-journal probes
 * would otherwise fight the outer's live, exclusively-locked WAL). */
static int
is_sidecar_name (const char *zName)
{
	return g_str_has_suffix (zName, "-wal")
	    || g_str_has_suffix (zName, "-journal")
	    || g_str_has_suffix (zName, "-shm");
}

static int
zvfs_open (sqlite3_vfs *vfs, const char *zName,
           sqlite3_file *file, int flags, int *pOutFlags)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	int rc;

	if (flags & SQLITE_OPEN_MAIN_DB)
	{
		/* Compressed main database */
		zstd_vfs_file *f = (zstd_vfs_file *)file;
		memset (f, 0, sizeof (*f));

		f->cctx = ZSTD_createCCtx ();
		f->dctx = ZSTD_createDCtx ();

		rc = outer_db_init (f, zName, flags);
		if (rc != SQLITE_OK)
		{
			outer_db_close (f);
			return rc;
		}

		f->base.pMethods = &zstd_vfs_io_methods;
		if (pOutFlags)
			*pOutFlags = flags;
		return SQLITE_OK;
	}
	else
	{
		/* Passthrough for temp/subjournal files.  The inner DB's own
		 * journal or WAL must never materialize on disk: the inner is
		 * pinned to journal_mode=MEMORY, and a real file here would sit
		 * at the outer DB's sidecar path.  Refuse loudly rather than
		 * corrupt. */
		zstd_vfs_passthru *p = (zstd_vfs_passthru *)file;
		memset (p, 0, sizeof (*p));

		if (flags & (SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_WAL))
		{
			g_warning ("zstd-vfs: refusing to open inner sidecar %s (flags 0x%x)",
			           zName ? zName : "(null)", flags);
			return SQLITE_CANTOPEN;
		}

		p->real_file = g_malloc0 (zvfs->real_vfs->szOsFile);
		rc = zvfs->real_vfs->xOpen (zvfs->real_vfs, zName,
		                            p->real_file, flags, pOutFlags);
		if (rc != SQLITE_OK)
		{
			g_free (p->real_file);
			p->real_file = NULL;
			return rc;
		}

		p->base.pMethods = &zstd_vfs_passthru_methods;
		return SQLITE_OK;
	}
}

static int
zvfs_delete (sqlite3_vfs *vfs, const char *zName, int syncDir)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;

	/* e.g. the inner's journal_mode=MEMORY transition deletes
	 * "<db>-journal" unconditionally; under WAL such names are the outer's
	 * live sidecars.  Report success without touching disk — there is
	 * never an inner sidecar to delete. */
	if (zName && is_sidecar_name (zName))
		return SQLITE_OK;
	return zvfs->real_vfs->xDelete (zvfs->real_vfs, zName, syncDir);
}

static int
zvfs_access (sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;

	/* Hide outer-DB sidecars from the inner pager's hot-WAL/hot-journal
	 * probes: to the inner logical database these files do not exist. */
	if (zName && is_sidecar_name (zName))
	{
		*pResOut = 0;
		return SQLITE_OK;
	}
	return zvfs->real_vfs->xAccess (zvfs->real_vfs, zName, flags, pResOut);
}

static int
zvfs_full_pathname (sqlite3_vfs *vfs, const char *zName,
                    int nOut, char *zOut)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	return zvfs->real_vfs->xFullPathname (zvfs->real_vfs, zName, nOut, zOut);
}

static int
zvfs_randomness (sqlite3_vfs *vfs, int nByte, char *zOut)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	return zvfs->real_vfs->xRandomness (zvfs->real_vfs, nByte, zOut);
}

static int
zvfs_sleep (sqlite3_vfs *vfs, int microseconds)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	return zvfs->real_vfs->xSleep (zvfs->real_vfs, microseconds);
}

static int
zvfs_current_time (sqlite3_vfs *vfs, double *pTime)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	return zvfs->real_vfs->xCurrentTime (zvfs->real_vfs, pTime);
}

static int
zvfs_get_last_error (sqlite3_vfs *vfs, int nBuf, char *zBuf)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	return zvfs->real_vfs->xGetLastError (zvfs->real_vfs, nBuf, zBuf);
}

static int
zvfs_current_time_int64 (sqlite3_vfs *vfs, sqlite3_int64 *pTime)
{
	zstd_vfs *zvfs = (zstd_vfs *)vfs;
	if (zvfs->real_vfs->xCurrentTimeInt64)
		return zvfs->real_vfs->xCurrentTimeInt64 (zvfs->real_vfs, pTime);
	else
	{
		double t;
		int rc = zvfs->real_vfs->xCurrentTime (zvfs->real_vfs, &t);
		*pTime = (sqlite3_int64)(t * 86400000.0);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/*  io_methods tables (populated at init)                              */
/* ------------------------------------------------------------------ */

static sqlite3_io_methods zstd_vfs_io_methods = {
	1,                          /* iVersion */
	zvfs_close,
	zvfs_read,
	zvfs_write,
	zvfs_truncate,
	zvfs_sync,
	zvfs_file_size,
	zvfs_lock,
	zvfs_unlock,
	zvfs_check_reserved_lock,
	zvfs_file_control,
	zvfs_sector_size,
	zvfs_device_characteristics
};

static sqlite3_io_methods zstd_vfs_passthru_methods = {
	1,                          /* iVersion */
	pt_close,
	pt_read,
	pt_write,
	pt_truncate,
	pt_sync,
	pt_file_size,
	pt_lock,
	pt_unlock,
	pt_check_reserved,
	pt_file_control,
	pt_sector_size,
	pt_device_chars
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int
zstd_vfs_backup_db (const char *path, const char *backup_path)
{
	char *wal_path, *shm_path;
	int ret = 0;

	if (g_rename (path, backup_path) != 0)
		return -1;

	wal_path = g_strdup_printf ("%s-wal", path);
	if (g_file_test (wal_path, G_FILE_TEST_EXISTS))
	{
		char *wal_backup = g_strdup_printf ("%s-wal", backup_path);
		if (g_rename (wal_path, wal_backup) != 0)
		{
			/* Losing the un-checkpointed tail is recoverable; letting the
			 * next DB created at this path replay a stale WAL is not. */
			g_warning ("zstd-vfs: could not move %s with its database (%s); deleting it",
			           wal_path, g_strerror (errno));
			if (g_unlink (wal_path) != 0)
			{
				/* Neither move nor delete worked: the stale WAL is
				 * stranded where SQLite would replay it into whatever
				 * database is created at this path next.  Undo the main
				 * rename so the pair at least stays together, and report
				 * failure — the caller must not create a DB here. */
				g_warning ("zstd-vfs: -wal stranded at %s (%s); backing out backup rename",
				           wal_path, g_strerror (errno));
				g_rename (backup_path, path);
				ret = -1;
			}
		}
		g_free (wal_backup);
	}
	g_free (wal_path);

	if (ret == 0)
	{
		shm_path = g_strdup_printf ("%s-shm", path);
		if (g_file_test (shm_path, G_FILE_TEST_EXISTS))
			g_unlink (shm_path);
		g_free (shm_path);
	}

	return ret;
}

int
zstd_vfs_register (const char *vfs_name)
{
	sqlite3_vfs *real;
	int sz;

	if (g_vfs)
		return SQLITE_OK;  /* already registered */

	real = sqlite3_vfs_find (NULL);  /* default OS VFS */
	if (!real)
		return SQLITE_ERROR;

	g_vfs = g_new0 (zstd_vfs, 1);
	g_vfs->real_vfs = real;

	/* Compute szOsFile: max of our two file structs */
	sz = sizeof (zstd_vfs_file);
	if ((int)sizeof (zstd_vfs_passthru) > sz)
		sz = sizeof (zstd_vfs_passthru);

	g_vfs->base.iVersion = 2;
	g_vfs->base.szOsFile = sz;
	g_vfs->base.mxPathname = real->mxPathname;
	g_vfs->base.zName = vfs_name;
	g_vfs->base.pAppData = g_vfs;

	g_vfs->base.xOpen = zvfs_open;
	g_vfs->base.xDelete = zvfs_delete;
	g_vfs->base.xAccess = zvfs_access;
	g_vfs->base.xFullPathname = zvfs_full_pathname;
	g_vfs->base.xDlOpen = NULL;
	g_vfs->base.xDlError = NULL;
	g_vfs->base.xDlSym = NULL;
	g_vfs->base.xDlClose = NULL;
	g_vfs->base.xRandomness = zvfs_randomness;
	g_vfs->base.xSleep = zvfs_sleep;
	g_vfs->base.xCurrentTime = zvfs_current_time;
	g_vfs->base.xGetLastError = zvfs_get_last_error;
	g_vfs->base.xCurrentTimeInt64 = zvfs_current_time_int64;

	return sqlite3_vfs_register (&g_vfs->base, 0);  /* 0 = not default */
}

void
zstd_vfs_shutdown (void)
{
	if (g_vfs)
	{
		sqlite3_vfs_unregister (&g_vfs->base);
		g_free (g_vfs);
		g_vfs = NULL;
	}
}
