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
 * SQLite-based scrollback storage implementation
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>

#include "poxchat.h"
#include "poxchatc.h"
#include "scrollback.h"
#include "sqlite-zstd-vfs.h"
#include "cfgfiles.h"
#include "text.h"

struct scrollback_db {
	sqlite3 *db;
	char *network;
	int transaction_depth;	/* ref-counted transaction nesting */

	/* Prepared statements for performance */
	sqlite3_stmt *stmt_insert;
	sqlite3_stmt *stmt_load;
	sqlite3_stmt *stmt_newest_msgid;
	sqlite3_stmt *stmt_oldest_msgid;
	sqlite3_stmt *stmt_newest_time;
	sqlite3_stmt *stmt_has_msgid;
	sqlite3_stmt *stmt_clear;

	/* IRCv3 reactions and replies */
	sqlite3_stmt *stmt_save_reaction;
	sqlite3_stmt *stmt_remove_reaction;
	sqlite3_stmt *stmt_load_reactions;
	sqlite3_stmt *stmt_save_reply;
	sqlite3_stmt *stmt_load_reply;
	sqlite3_stmt *stmt_update_pending;
	sqlite3_stmt *stmt_redact;

	/* Virtual scrollback support */
	sqlite3_stmt *stmt_count;
	sqlite3_stmt *stmt_load_range;
	sqlite3_stmt *stmt_max_rowid;
	sqlite3_stmt *stmt_index_of_rowid;
	sqlite3_stmt *stmt_search_text;

	/* Channel name normalization */
	sqlite3_stmt *stmt_channel_insert;
	sqlite3_stmt *stmt_channel_lookup;
	GHashTable *channel_id_cache;    /* channel name -> GINT_TO_POINTER(channel_id) */
};

/* Hash table of open databases: network -> scrollback_db */
static GHashTable *open_dbs = NULL;

static char *
get_scrollback_dir (void)
{
	return g_build_filename (get_xdir (), "scrollback", NULL);
}

static char *
get_db_path (const char *network)
{
	char *dir = get_scrollback_dir ();
	char *safe_network = g_strdup (network);
	char *path;

	/* Sanitize network name for use as filename */
	for (char *p = safe_network; *p; p++)
	{
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
		    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
			*p = '_';
	}

	path = g_build_filename (dir, safe_network, NULL);
	g_free (dir);

	/* Add .db extension */
	char *full_path = g_strdup_printf ("%s.db", path);
	g_free (path);
	g_free (safe_network);

	return full_path;
}

/* --- Channel ID resolver --- */

static gint64
scrollback_get_channel_id (scrollback_db *sdb, const char *channel)
{
	gint64 id;
	gpointer cached;

	if (!channel || !channel[0])
		return -1;

	/* Check cache first */
	if (sdb->channel_id_cache &&
	    g_hash_table_lookup_extended (sdb->channel_id_cache, channel, NULL, &cached))
		return (gint64)GPOINTER_TO_INT (cached);

	/* Insert if new, then fetch ID */
	sqlite3_reset (sdb->stmt_channel_insert);
	sqlite3_bind_text (sdb->stmt_channel_insert, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_step (sdb->stmt_channel_insert);

	sqlite3_reset (sdb->stmt_channel_lookup);
	sqlite3_bind_text (sdb->stmt_channel_lookup, 1, channel, -1, SQLITE_TRANSIENT);
	if (sqlite3_step (sdb->stmt_channel_lookup) == SQLITE_ROW)
		id = sqlite3_column_int64 (sdb->stmt_channel_lookup, 0);
	else
		return -1;

	/* Cache it */
	if (!sdb->channel_id_cache)
		sdb->channel_id_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_insert (sdb->channel_id_cache, g_strdup (channel), GINT_TO_POINTER ((int)id));

	return id;
}

static gboolean
ensure_scrollback_dir (void)
{
	char *dir = get_scrollback_dir ();
	gboolean result = TRUE;

	if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
	{
		if (g_mkdir_with_parents (dir, 0700) != 0)
		{
			g_warning ("Failed to create scrollback directory: %s", dir);
			result = FALSE;
		}
	}

	g_free (dir);
	return result;
}

static gboolean
init_database (scrollback_db *sdb)
{
	const char *schema =
		"CREATE TABLE IF NOT EXISTS messages ("
		"    id INTEGER PRIMARY KEY,"
		"    channel TEXT NOT NULL,"
		"    timestamp INTEGER NOT NULL,"
		"    msgid TEXT,"
		"    text TEXT NOT NULL,"
		"    redacted_by TEXT,"
		"    redact_reason TEXT,"
		"    redact_time INTEGER"
		");"
		"CREATE INDEX IF NOT EXISTS idx_channel_time ON messages(channel, timestamp);"
		"CREATE UNIQUE INDEX IF NOT EXISTS idx_msgid ON messages(msgid) WHERE msgid IS NOT NULL;"
		/* IRCv3 reactions: persisted per (target_msgid, reaction_text, nick) */
		"CREATE TABLE IF NOT EXISTS reactions ("
		"    id INTEGER PRIMARY KEY,"
		"    channel TEXT NOT NULL,"
		"    target_msgid TEXT NOT NULL,"
		"    reaction_text TEXT NOT NULL,"
		"    nick TEXT NOT NULL,"
		"    is_self INTEGER NOT NULL DEFAULT 0,"
		"    timestamp INTEGER NOT NULL,"
		"    UNIQUE(target_msgid, reaction_text, nick)"
		");"
		"CREATE INDEX IF NOT EXISTS idx_reactions_target ON reactions(channel, target_msgid);"
		/* IRCv3 replies: persisted as (msgid → target_msgid) mapping */
		"CREATE TABLE IF NOT EXISTS replies ("
		"    msgid TEXT PRIMARY KEY,"
		"    target_msgid TEXT NOT NULL,"
		"    target_nick TEXT,"
		"    target_preview TEXT"
		");";

	char *errmsg = NULL;
	int rc;

	/* Inner DB uses MEMORY journal — atomicity comes from the outer
	 * (compressed) DB's own transactions via the zstd VFS. */
	sqlite3_exec (sdb->db, "PRAGMA journal_mode=MEMORY;", NULL, NULL, NULL);

	rc = sqlite3_exec (sdb->db, schema, NULL, NULL, &errmsg);

	if (rc != SQLITE_OK)
	{
		g_warning ("Failed to initialize scrollback database: %s", errmsg);
		sqlite3_free (errmsg);
		return FALSE;
	}

	/* Channel name normalization table */
	sqlite3_exec (sdb->db,
		"CREATE TABLE IF NOT EXISTS channels ("
		"    id INTEGER PRIMARY KEY,"
		"    name TEXT NOT NULL UNIQUE"
		");",
		NULL, NULL, NULL);

	/* Add channel_id column to messages (NULL = legacy, use channel TEXT) */
	sqlite3_exec (sdb->db,
		"ALTER TABLE messages ADD COLUMN channel_id INTEGER REFERENCES channels(id);",
		NULL, NULL, NULL);

	/* Add channel_id column to reactions */
	sqlite3_exec (sdb->db,
		"ALTER TABLE reactions ADD COLUMN channel_id INTEGER REFERENCES channels(id);",
		NULL, NULL, NULL);

	/* Index on channel_id for messages (replaces idx_channel_time over time) */
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_channel_id_time ON messages(channel_id, timestamp);",
		NULL, NULL, NULL);

	/* Index on channel_id for reactions */
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_reactions_channel_id ON reactions(channel_id, target_msgid);",
		NULL, NULL, NULL);

	/* Migrate existing rows: populate channels table and channel_id */
	{
		int migrated = 0;
		sqlite3_stmt *sel_ch;
		rc = sqlite3_prepare_v2 (sdb->db,
			"SELECT DISTINCT channel FROM messages WHERE channel_id IS NULL AND channel IS NOT NULL",
			-1, &sel_ch, NULL);
		if (rc == SQLITE_OK)
		{
			while (sqlite3_step (sel_ch) == SQLITE_ROW)
			{
				const char *ch_name = (const char *)sqlite3_column_text (sel_ch, 0);
				if (ch_name)
				{
					char *sql = sqlite3_mprintf (
						"INSERT OR IGNORE INTO channels (name) VALUES (%Q);"
						"UPDATE messages SET channel_id = (SELECT id FROM channels WHERE name = %Q) "
						"WHERE channel = %Q AND channel_id IS NULL;"
						"UPDATE reactions SET channel_id = (SELECT id FROM channels WHERE name = %Q) "
						"WHERE channel = %Q AND channel_id IS NULL;",
						ch_name, ch_name, ch_name, ch_name, ch_name);
					sqlite3_exec (sdb->db, sql, NULL, NULL, NULL);
					sqlite3_free (sql);
					migrated++;
				}
			}
			sqlite3_finalize (sel_ch);
			if (migrated > 0)
				g_message ("scrollback: migrated %d channels to normalized IDs for %s",
				           migrated, sdb->network);
		}
	}

	return TRUE;
}

static gboolean
prepare_statements (scrollback_db *sdb)
{
	int rc;

	/* Channel name resolution */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR IGNORE INTO channels (name) VALUES (?)",
		-1, &sdb->stmt_channel_insert, NULL);
	if (rc != SQLITE_OK) goto fail;

	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id FROM channels WHERE name = ?",
		-1, &sdb->stmt_channel_lookup, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Insert statement (channel kept for NOT NULL compat with original schema) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR IGNORE INTO messages (channel, channel_id, timestamp, msgid, text) "
		"VALUES (?, ?, ?, ?, ?)",
		-1, &sdb->stmt_insert, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Load statement - get newest N messages in chronological order */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, channel_id, timestamp, msgid, text, redacted_by, redact_reason, "
		"redact_time "
		"FROM messages WHERE channel_id = ? ORDER BY timestamp DESC LIMIT ?",
		-1, &sdb->stmt_load, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get newest msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid FROM messages WHERE channel_id = ? AND msgid IS NOT NULL "
		"AND msgid NOT LIKE 'pending:%' ORDER BY timestamp DESC LIMIT 1",
		-1, &sdb->stmt_newest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get oldest msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid FROM messages WHERE channel_id = ? AND msgid IS NOT NULL "
		"AND msgid NOT LIKE 'pending:%' ORDER BY timestamp ASC LIMIT 1",
		-1, &sdb->stmt_oldest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get newest timestamp */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT MAX(timestamp) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_newest_time, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Check msgid exists */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT 1 FROM messages WHERE msgid = ? LIMIT 1",
		-1, &sdb->stmt_has_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Clear channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"DELETE FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_clear, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: save (channel kept for NOT NULL compat) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR REPLACE INTO reactions (channel, channel_id, target_msgid, reaction_text, nick, is_self, timestamp) "
		"VALUES (?, ?, ?, ?, ?, ?, ?)",
		-1, &sdb->stmt_save_reaction, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: remove */
	rc = sqlite3_prepare_v2 (sdb->db,
		"DELETE FROM reactions WHERE target_msgid = ? AND reaction_text = ? AND nick = ?",
		-1, &sdb->stmt_remove_reaction, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: load all for a channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT target_msgid, reaction_text, nick, is_self FROM reactions "
		"WHERE channel_id = ? ORDER BY target_msgid, reaction_text",
		-1, &sdb->stmt_load_reactions, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 replies: save */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR REPLACE INTO replies (msgid, target_msgid, target_nick, target_preview) "
		"VALUES (?, ?, ?, ?)",
		-1, &sdb->stmt_save_reply, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 replies: load all for msgs in a channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT r.msgid, r.target_msgid, r.target_nick, r.target_preview "
		"FROM replies r INNER JOIN messages m ON r.msgid = m.msgid "
		"WHERE m.channel_id = ?",
		-1, &sdb->stmt_load_reply, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Echo-message: update pending placeholder msgid to real msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"UPDATE messages SET msgid = ?1 WHERE channel_id = ?2 AND msgid = ?3",
		-1, &sdb->stmt_update_pending, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Redact: mark a message as redacted by msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"UPDATE messages SET redacted_by = ?, redact_reason = ?, redact_time = ? "
		"WHERE msgid = ?",
		-1, &sdb->stmt_redact, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: total message count */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_count, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: load a window of entries by position.
	 * ORDER BY (timestamp, id) for deterministic, chronological ordering. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, timestamp, msgid, text, redacted_by, redact_reason, redact_time "
		"FROM messages WHERE channel_id = ? ORDER BY timestamp ASC, id ASC LIMIT ? OFFSET ?",
		-1, &sdb->stmt_load_range, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: maximum row ID for a channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT MAX(id) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_max_rowid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: positional index of a specific row ID.
	 * Must match load_range ordering: (timestamp, id) ASC.
	 * Counts entries strictly before the target in chronological order. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?1 AND "
		"(timestamp < (SELECT timestamp FROM messages WHERE id = ?2) OR "
		" (timestamp = (SELECT timestamp FROM messages WHERE id = ?2) AND id < ?2))",
		-1, &sdb->stmt_index_of_rowid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: search message text */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, text FROM messages WHERE channel_id = ? AND text LIKE ? "
		"ORDER BY timestamp ASC",
		-1, &sdb->stmt_search_text, NULL);
	if (rc != SQLITE_OK) goto fail;

	return TRUE;

fail:
	g_warning ("Failed to prepare scrollback statement: %s", sqlite3_errmsg (sdb->db));
	return FALSE;
}

static void
finalize_statements (scrollback_db *sdb)
{
	if (sdb->stmt_channel_insert) sqlite3_finalize (sdb->stmt_channel_insert);
	if (sdb->stmt_channel_lookup) sqlite3_finalize (sdb->stmt_channel_lookup);
	if (sdb->stmt_insert) sqlite3_finalize (sdb->stmt_insert);
	if (sdb->stmt_load) sqlite3_finalize (sdb->stmt_load);
	if (sdb->stmt_newest_msgid) sqlite3_finalize (sdb->stmt_newest_msgid);
	if (sdb->stmt_oldest_msgid) sqlite3_finalize (sdb->stmt_oldest_msgid);
	if (sdb->stmt_newest_time) sqlite3_finalize (sdb->stmt_newest_time);
	if (sdb->stmt_has_msgid) sqlite3_finalize (sdb->stmt_has_msgid);
	if (sdb->stmt_clear) sqlite3_finalize (sdb->stmt_clear);
	if (sdb->stmt_save_reaction) sqlite3_finalize (sdb->stmt_save_reaction);
	if (sdb->stmt_remove_reaction) sqlite3_finalize (sdb->stmt_remove_reaction);
	if (sdb->stmt_load_reactions) sqlite3_finalize (sdb->stmt_load_reactions);
	if (sdb->stmt_save_reply) sqlite3_finalize (sdb->stmt_save_reply);
	if (sdb->stmt_load_reply) sqlite3_finalize (sdb->stmt_load_reply);
	if (sdb->stmt_update_pending) sqlite3_finalize (sdb->stmt_update_pending);
	if (sdb->stmt_redact) sqlite3_finalize (sdb->stmt_redact);
	if (sdb->stmt_count) sqlite3_finalize (sdb->stmt_count);
	if (sdb->stmt_load_range) sqlite3_finalize (sdb->stmt_load_range);
	if (sdb->stmt_max_rowid) sqlite3_finalize (sdb->stmt_max_rowid);
	if (sdb->stmt_index_of_rowid) sqlite3_finalize (sdb->stmt_index_of_rowid);
	if (sdb->stmt_search_text) sqlite3_finalize (sdb->stmt_search_text);
	if (sdb->channel_id_cache) g_hash_table_destroy (sdb->channel_id_cache);
}

/* Sentinel value stored in open_dbs to indicate a previously-failed open.
 * Prevents retrying (and re-warning) for every channel on the same network. */
#define SCROLLBACK_FAILED_SENTINEL ((scrollback_db *)(gsize)1)

/* Check database integrity.  Returns TRUE if OK, FALSE if corrupt. */
static gboolean
scrollback_check_integrity (sqlite3 *db)
{
	sqlite3_stmt *stmt;
	int rc;
	gboolean ok = FALSE;

	rc = sqlite3_prepare_v2 (db, "PRAGMA quick_check(1)", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return FALSE;

	if (sqlite3_step (stmt) == SQLITE_ROW)
	{
		const char *result = (const char *)sqlite3_column_text (stmt, 0);
		if (result && strcmp (result, "ok") == 0)
			ok = TRUE;
	}
	sqlite3_finalize (stmt);
	return ok;
}

/* Back up a corrupt DB file by renaming it with a timestamp suffix. */
static void
scrollback_backup_corrupt (const char *path)
{
	char *backup_path;
	char *wal_path, *shm_path;

	backup_path = g_strdup_printf ("%s.corrupt.%" G_GINT64_FORMAT, path, (gint64)time (NULL));
	g_rename (path, backup_path);
	g_free (backup_path);

	/* Also move any leftover WAL/SHM files */
	wal_path = g_strdup_printf ("%s-wal", path);
	if (g_file_test (wal_path, G_FILE_TEST_EXISTS))
		g_unlink (wal_path);
	g_free (wal_path);

	shm_path = g_strdup_printf ("%s-shm", path);
	if (g_file_test (shm_path, G_FILE_TEST_EXISTS))
		g_unlink (shm_path);
	g_free (shm_path);
}

scrollback_db *
scrollback_open (const char *network)
{
	static gboolean vfs_registered = FALSE;
	scrollback_db *sdb;
	char *path;
	int rc;

	if (!network || !network[0])
		return NULL;

	if (!vfs_registered)
	{
		zstd_vfs_register ("zstd");
		vfs_registered = TRUE;
	}

	/* Ensure cache hash table exists */
	if (!open_dbs)
		open_dbs = g_hash_table_new (g_str_hash, g_str_equal);

	/* Check if already open (or previously failed) */
	{
		gpointer val;
		if (g_hash_table_lookup_extended (open_dbs, network, NULL, &val))
		{
			if (val == SCROLLBACK_FAILED_SENTINEL)
				return NULL;	/* previously failed, don't retry */
			return (scrollback_db *)val;
		}
	}

	if (!ensure_scrollback_dir ())
		return NULL;

	sdb = g_new0 (scrollback_db, 1);
	sdb->network = g_strdup (network);

	path = get_db_path (network);
	rc = sqlite3_open_v2 (path, &sdb->db,
	                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "zstd");

	if (rc != SQLITE_OK)
	{
		g_warning ("Failed to open scrollback database %s: %s", path, sqlite3_errmsg (sdb->db));
		sqlite3_close (sdb->db);
		g_free (path);
		g_free (sdb->network);
		g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
		g_free (sdb);
		return NULL;
	}

	/* Check for corruption before using the database */
	if (!scrollback_check_integrity (sdb->db))
	{
		g_warning ("Scrollback database corrupt for %s — backing up and recreating", network);
		sqlite3_close (sdb->db);
		scrollback_backup_corrupt (path);

		/* Re-open — creates a fresh empty database */
		rc = sqlite3_open_v2 (path, &sdb->db,
		                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "zstd");
		if (rc != SQLITE_OK)
		{
			g_warning ("Failed to recreate scrollback database %s: %s", path, sqlite3_errmsg (sdb->db));
			sqlite3_close (sdb->db);
			g_free (path);
			g_free (sdb->network);
			g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
			g_free (sdb);
			return NULL;
		}
	}

	g_free (path);

	if (!init_database (sdb))
	{
		sqlite3_close (sdb->db);
		g_free (sdb->network);
		g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
		g_free (sdb);
		return NULL;
	}

	if (!prepare_statements (sdb))
	{
		sqlite3_close (sdb->db);
		g_free (sdb->network);
		g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
		g_free (sdb);
		return NULL;
	}

	/* Add to open databases */
	g_hash_table_insert (open_dbs, sdb->network, sdb);

	return sdb;
}

void
scrollback_db_close (scrollback_db *db)
{
	if (!db)
		return;

	if (open_dbs)
		g_hash_table_remove (open_dbs, db->network);

	finalize_statements (db);
	sqlite3_close (db->db);
	g_free (db->network);
	g_free (db);
}

gint64
scrollback_db_save (scrollback_db *db, const char *channel,
                 time_t timestamp, const char *msgid, const char *text)
{
	int rc;

	if (!db || !channel || !text)
		return -1;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return -1;

	sqlite3_reset (db->stmt_insert);
	sqlite3_bind_text (db->stmt_insert, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_insert, 2, channel_id);
	sqlite3_bind_int64 (db->stmt_insert, 3, (sqlite3_int64)timestamp);

	if (msgid && msgid[0])
		sqlite3_bind_text (db->stmt_insert, 4, msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_insert, 4);

	sqlite3_bind_text (db->stmt_insert, 5, text, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_insert);

	if (rc != SQLITE_DONE)
	{
		/* SQLITE_CONSTRAINT is expected for duplicate msgid - not an error */
		if (rc != SQLITE_CONSTRAINT)
			g_warning ("scrollback_db_save failed: %s", sqlite3_errmsg (db->db));
		return -1;
	}

	/* INSERT OR IGNORE returns SQLITE_DONE even when the row is silently
	 * dropped due to a UNIQUE constraint conflict on msgid.  In that case
	 * sqlite3_changes() is 0 and sqlite3_last_insert_rowid() still returns
	 * the rowid of the most recent *actual* insert (not the duplicate that
	 * was ignored).  Returning that stale rowid would cause the caller to
	 * assign a stale entry_id to a brand-new entry, producing duplicate
	 * (entry_id, stamp) pairs in xtext's tree and a flood of orphans.
	 * Treat the duplicate-ignored case as "row not stored". */
	if (sqlite3_changes (db->db) == 0)
		return -1;

	return (gint64) sqlite3_last_insert_rowid (db->db);
}

GSList *
scrollback_db_load (scrollback_db *db, const char *channel, int limit)
{
	GSList *list = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	if (limit <= 0)
		limit = 500; /* Default */

	/* Purge unconfirmed echo-message entries from a previous session. */
	{
		char *errmsg = NULL;
		char *sql = sqlite3_mprintf (
			"DELETE FROM messages WHERE channel_id = %lld AND msgid LIKE 'pending:%%'",
			(long long)channel_id);
		sqlite3_exec (db->db, sql, NULL, NULL, &errmsg);
		if (errmsg)
		{
			g_warning ("Failed to purge pending entries: %s", errmsg);
			sqlite3_free (errmsg);
		}
		sqlite3_free (sql);
	}

	sqlite3_reset (db->stmt_load);
	sqlite3_bind_int64 (db->stmt_load, 1, channel_id);
	sqlite3_bind_int (db->stmt_load, 2, limit);

	while ((rc = sqlite3_step (db->stmt_load)) == SQLITE_ROW)
	{
		scrollback_msg *msg = g_new0 (scrollback_msg, 1);

		msg->id = sqlite3_column_int64 (db->stmt_load, 0);
		msg->channel = g_strdup (channel);
		msg->timestamp = (time_t)sqlite3_column_int64 (db->stmt_load, 2);

		const char *msgid_text = (const char *)sqlite3_column_text (db->stmt_load, 3);
		msg->msgid = msgid_text ? g_strdup (msgid_text) : NULL;

		msg->text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load, 4));

		{
			const char *rby = (const char *)sqlite3_column_text (db->stmt_load, 5);
			const char *rreason = (const char *)sqlite3_column_text (db->stmt_load, 6);
			msg->redacted_by = rby ? g_strdup (rby) : NULL;
			msg->redact_reason = rreason ? g_strdup (rreason) : NULL;
			msg->redact_time = (time_t)sqlite3_column_int64 (db->stmt_load, 7);
		}

		/* Prepend to get correct order (query returns DESC, we want ASC) */
		list = g_slist_prepend (list, msg);
	}

	if (rc != SQLITE_DONE)
		g_warning ("Error loading scrollback: %s", sqlite3_errmsg (db->db));

	return list;
}

char *
scrollback_get_newest_msgid (scrollback_db *db, const char *channel)
{
	char *msgid = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_newest_msgid);
	sqlite3_bind_int64 (db->stmt_newest_msgid, 1, channel_id);

	rc = sqlite3_step (db->stmt_newest_msgid);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (db->stmt_newest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}

	return msgid;
}

char *
scrollback_get_oldest_msgid (scrollback_db *db, const char *channel)
{
	char *msgid = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_oldest_msgid);
	sqlite3_bind_int64 (db->stmt_oldest_msgid, 1, channel_id);

	rc = sqlite3_step (db->stmt_oldest_msgid);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (db->stmt_oldest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}

	return msgid;
}

time_t
scrollback_get_newest_time (scrollback_db *db, const char *channel)
{
	time_t timestamp = 0;
	int rc;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_newest_time);
	sqlite3_bind_int64 (db->stmt_newest_time, 1, channel_id);

	rc = sqlite3_step (db->stmt_newest_time);
	if (rc == SQLITE_ROW)
		timestamp = (time_t)sqlite3_column_int64 (db->stmt_newest_time, 0);

	return timestamp;
}

gboolean
scrollback_has_msgid (scrollback_db *db, const char *msgid)
{
	int rc;

	if (!db || !msgid || !msgid[0])
		return FALSE;

	sqlite3_reset (db->stmt_has_msgid);
	sqlite3_bind_text (db->stmt_has_msgid, 1, msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_has_msgid);
	return (rc == SQLITE_ROW);
}

gboolean
scrollback_update_pending_msgid (scrollback_db *db, const char *channel,
                                  const char *pending_msgid, const char *real_msgid)
{
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_update_pending || !channel || !pending_msgid || !real_msgid)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_update_pending);
	sqlite3_bind_text (db->stmt_update_pending, 1, real_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_update_pending, 2, channel_id);
	sqlite3_bind_text (db->stmt_update_pending, 3, pending_msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_update_pending);
	return (rc == SQLITE_DONE && sqlite3_changes (db->db) > 0);
}

gboolean
scrollback_redact_message (scrollback_db *db, const char *msgid,
                           const char *redacted_by, const char *reason,
                           time_t redact_time)
{
	int rc;

	if (!db || !db->stmt_redact || !msgid)
		return FALSE;

	sqlite3_reset (db->stmt_redact);
	sqlite3_bind_text (db->stmt_redact, 1, redacted_by ? redacted_by : "unknown", -1, SQLITE_TRANSIENT);
	if (reason)
		sqlite3_bind_text (db->stmt_redact, 2, reason, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_redact, 2);
	sqlite3_bind_int64 (db->stmt_redact, 3, (sqlite3_int64) redact_time);
	sqlite3_bind_text (db->stmt_redact, 4, msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_redact);
	return (rc == SQLITE_DONE && sqlite3_changes (db->db) > 0);
}

void
scrollback_clear (scrollback_db *db, const char *channel)
{
	gint64 channel_id;

	if (!db || !channel)
		return;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return;

	sqlite3_reset (db->stmt_clear);
	sqlite3_bind_int64 (db->stmt_clear, 1, channel_id);
	sqlite3_step (db->stmt_clear);
}

void
scrollback_msg_free (scrollback_msg *msg)
{
	if (!msg)
		return;

	g_free (msg->channel);
	g_free (msg->msgid);
	g_free (msg->text);
	g_free (msg->redacted_by);
	g_free (msg->redact_reason);
	g_free (msg);
}

void
scrollback_msg_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_msg_free);
}

/* Migration from old text-based scrollback */

static char *
get_old_scrollback_path (const char *network, const char *channel)
{
	char *dir = get_scrollback_dir ();
	char *safe_channel = g_strdup (channel);
	char *path;

	/* Sanitize channel name for filename */
	for (char *p = safe_channel; *p; p++)
	{
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
		    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
			*p = '_';
	}

	path = g_build_filename (dir, network, safe_channel, NULL);
	g_free (dir);

	char *full_path = g_strdup_printf ("%s.txt", path);
	g_free (path);
	g_free (safe_channel);

	return full_path;
}

int
scrollback_migrate (scrollback_db *db, const char *network, const char *channel)
{
	char *old_path;
	GFile *file;
	GInputStream *stream;
	GDataInputStream *istream;
	char *line;
	int count = 0;
	char *errmsg = NULL;

	if (!db || !network || !channel)
		return -1;

	old_path = get_old_scrollback_path (network, channel);

	if (!g_file_test (old_path, G_FILE_TEST_EXISTS))
	{
		g_free (old_path);
		return 0; /* No old file to migrate */
	}

	file = g_file_new_for_path (old_path);
	stream = G_INPUT_STREAM (g_file_read (file, NULL, NULL));

	if (!stream)
	{
		g_object_unref (file);
		g_free (old_path);
		return -1;
	}

	istream = g_data_input_stream_new (stream);
	g_data_input_stream_set_newline_type (istream, G_DATA_STREAM_NEWLINE_TYPE_ANY);
	g_object_unref (stream);

	/* Begin transaction for batch insert */
	sqlite3_exec (db->db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
	if (errmsg)
	{
		g_warning ("Migration transaction begin failed: %s", errmsg);
		sqlite3_free (errmsg);
	}

	while ((line = g_data_input_stream_read_line_utf8 (istream, NULL, NULL, NULL)) != NULL)
	{
		time_t timestamp = 0;
		const char *text = line;

		/* Parse old format: T <timestamp> <text> */
		if (line[0] == 'T' && line[1] == ' ')
		{
			if (sizeof (time_t) == 4)
				timestamp = strtoul (line + 2, NULL, 10);
			else
				timestamp = g_ascii_strtoull (line + 2, NULL, 10);

			text = strchr (line + 3, ' ');
			if (text)
				text++; /* Skip the space */
			else
				text = "";
		}

		if (timestamp > 0 && text && text[0])
		{
			/* Insert without msgid (old format doesn't have them) */
			if (scrollback_db_save (db, channel, timestamp, NULL, text) >= 0)
				count++;
		}

		g_free (line);
	}

	/* Commit transaction */
	sqlite3_exec (db->db, "COMMIT", NULL, NULL, &errmsg);
	if (errmsg)
	{
		g_warning ("Migration transaction commit failed: %s", errmsg);
		sqlite3_free (errmsg);
	}

	g_object_unref (istream);
	g_object_unref (file);

	/* Rename old file to .migrated */
	if (count > 0)
	{
		char *migrated_path = g_strdup_printf ("%s.migrated", old_path);
		g_rename (old_path, migrated_path);
		g_free (migrated_path);
	}

	g_free (old_path);

	return count;
}

void
scrollback_init (void)
{
	if (!open_dbs)
		open_dbs = g_hash_table_new (g_str_hash, g_str_equal);
}

static void
close_db_callback (gpointer key, gpointer value, gpointer user_data)
{
	scrollback_db *db = value;
	if (db == SCROLLBACK_FAILED_SENTINEL)
	{
		g_free (key);	/* network name was g_strdup'd for sentinel entries */
		return;
	}
	finalize_statements (db);
	sqlite3_close (db->db);
	g_free (db->network);
	g_free (db);
}

void
scrollback_begin_transaction (scrollback_db *db)
{
	if (!db || !db->db)
		return;
	db->transaction_depth++;
	if (db->transaction_depth == 1)
		sqlite3_exec (db->db, "BEGIN TRANSACTION", NULL, NULL, NULL);
}

void
scrollback_commit_transaction (scrollback_db *db)
{
	if (!db || !db->db || db->transaction_depth <= 0)
		return;
	db->transaction_depth--;
	if (db->transaction_depth == 0)
	{
		char *errmsg = NULL;
		int rc = sqlite3_exec (db->db, "COMMIT", NULL, NULL, &errmsg);
		if (rc != SQLITE_OK)
		{
			g_warning ("Scrollback commit failed for %s: %s",
			           db->network ? db->network : "?",
			           errmsg ? errmsg : "unknown error");
			sqlite3_free (errmsg);
		}
	}
}

void
scrollback_shutdown (void)
{
	if (open_dbs)
	{
		g_hash_table_foreach (open_dbs, close_db_callback, NULL);
		g_hash_table_destroy (open_dbs);
		open_dbs = NULL;
	}
	zstd_vfs_shutdown ();
}

/* IRCv3 reactions: save a reaction to scrollback */
gboolean
scrollback_save_reaction (scrollback_db *db, const char *channel,
                          const char *target_msgid, const char *reaction_text,
                          const char *nick, gboolean is_self, time_t timestamp)
{
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_save_reaction || !channel || !target_msgid ||
	    !reaction_text || !nick)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_save_reaction);
	sqlite3_bind_text (db->stmt_save_reaction, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_save_reaction, 2, channel_id);
	sqlite3_bind_text (db->stmt_save_reaction, 3, target_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reaction, 4, reaction_text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reaction, 5, nick, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int (db->stmt_save_reaction, 6, is_self ? 1 : 0);
	sqlite3_bind_int64 (db->stmt_save_reaction, 7, (sqlite3_int64)timestamp);

	rc = sqlite3_step (db->stmt_save_reaction);
	return rc == SQLITE_DONE;
}

/* IRCv3 reactions: remove a reaction from scrollback */
gboolean
scrollback_remove_reaction (scrollback_db *db, const char *target_msgid,
                            const char *reaction_text, const char *nick)
{
	int rc;

	if (!db || !db->stmt_remove_reaction || !target_msgid ||
	    !reaction_text || !nick)
		return FALSE;

	sqlite3_reset (db->stmt_remove_reaction);
	sqlite3_bind_text (db->stmt_remove_reaction, 1, target_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_remove_reaction, 2, reaction_text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_remove_reaction, 3, nick, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_remove_reaction);
	return rc == SQLITE_DONE;
}

/* IRCv3 reactions: load all reactions for a channel.
 * Returns a GSList of scrollback_reaction* (caller frees with scrollback_reaction_list_free).
 */
GSList *
scrollback_load_reactions (scrollback_db *db, const char *channel)
{
	GSList *list = NULL;
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_load_reactions || !channel)
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reactions);
	sqlite3_bind_int64 (db->stmt_load_reactions, 1, channel_id);

	while ((rc = sqlite3_step (db->stmt_load_reactions)) == SQLITE_ROW)
	{
		scrollback_reaction *r = g_new0 (scrollback_reaction, 1);
		r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 0));
		r->reaction_text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 1));
		r->nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 2));
		r->is_self = sqlite3_column_int (db->stmt_load_reactions, 3) != 0;
		list = g_slist_prepend (list, r);
	}

	return g_slist_reverse (list);
}

void
scrollback_reaction_free (scrollback_reaction *r)
{
	if (!r)
		return;
	g_free (r->target_msgid);
	g_free (r->reaction_text);
	g_free (r->nick);
	g_free (r);
}

void
scrollback_reaction_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_reaction_free);
}

/* IRCv3 replies: save reply context to scrollback */
gboolean
scrollback_save_reply (scrollback_db *db, const char *msgid,
                       const char *target_msgid, const char *target_nick,
                       const char *target_preview)
{
	int rc;

	if (!db || !db->stmt_save_reply || !msgid || !target_msgid)
		return FALSE;

	sqlite3_reset (db->stmt_save_reply);
	sqlite3_bind_text (db->stmt_save_reply, 1, msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reply, 2, target_msgid, -1, SQLITE_TRANSIENT);
	if (target_nick)
		sqlite3_bind_text (db->stmt_save_reply, 3, target_nick, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_save_reply, 3);
	if (target_preview)
		sqlite3_bind_text (db->stmt_save_reply, 4, target_preview, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_save_reply, 4);

	rc = sqlite3_step (db->stmt_save_reply);
	return rc == SQLITE_DONE;
}

/* IRCv3 replies: load all reply contexts for messages in a channel.
 * Returns a GSList of scrollback_reply* (caller frees with scrollback_reply_list_free).
 */
GSList *
scrollback_load_replies (scrollback_db *db, const char *channel)
{
	GSList *list = NULL;
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_load_reply || !channel)
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reply);
	sqlite3_bind_int64 (db->stmt_load_reply, 1, channel_id);

	while ((rc = sqlite3_step (db->stmt_load_reply)) == SQLITE_ROW)
	{
		scrollback_reply *r = g_new0 (scrollback_reply, 1);
		r->msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 0));
		r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 1));
		r->target_nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 2));
		r->target_preview = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 3));
		list = g_slist_prepend (list, r);
	}

	return g_slist_reverse (list);
}

void
scrollback_reply_free (scrollback_reply *r)
{
	if (!r)
		return;
	g_free (r->msgid);
	g_free (r->target_msgid);
	g_free (r->target_nick);
	g_free (r->target_preview);
	g_free (r);
}

void
scrollback_reply_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_reply_free);
}

/* --- Virtual scrollback query functions --- */

int
scrollback_count (scrollback_db *db, const char *channel)
{
	int count = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_count);
	sqlite3_bind_int64 (db->stmt_count, 1, channel_id);

	if (sqlite3_step (db->stmt_count) == SQLITE_ROW)
		count = sqlite3_column_int (db->stmt_count, 0);

	return count;
}

GSList *
scrollback_load_range (scrollback_db *db, const char *channel, int offset, int limit)
{
	GSList *list = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	if (limit <= 0)
		limit = 500;

	sqlite3_reset (db->stmt_load_range);
	sqlite3_bind_int64 (db->stmt_load_range, 1, channel_id);
	sqlite3_bind_int (db->stmt_load_range, 2, limit);
	sqlite3_bind_int (db->stmt_load_range, 3, offset);

	while ((rc = sqlite3_step (db->stmt_load_range)) == SQLITE_ROW)
	{
		scrollback_msg *msg = g_new0 (scrollback_msg, 1);

		msg->id = sqlite3_column_int64 (db->stmt_load_range, 0);
		msg->channel = g_strdup (channel);
		msg->timestamp = (time_t)sqlite3_column_int64 (db->stmt_load_range, 1);

		const char *msgid_text = (const char *)sqlite3_column_text (db->stmt_load_range, 2);
		msg->msgid = msgid_text ? g_strdup (msgid_text) : NULL;

		msg->text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_range, 3));

		{
			const char *rby = (const char *)sqlite3_column_text (db->stmt_load_range, 4);
			const char *rreason = (const char *)sqlite3_column_text (db->stmt_load_range, 5);
			msg->redacted_by = rby ? g_strdup (rby) : NULL;
			msg->redact_reason = rreason ? g_strdup (rreason) : NULL;
			msg->redact_time = (time_t)sqlite3_column_int64 (db->stmt_load_range, 6);
		}

		/* ASC order — append to maintain chronological order */
		list = g_slist_prepend (list, msg);
	}

	if (rc != SQLITE_DONE)
		g_warning ("Error loading scrollback range: %s", sqlite3_errmsg (db->db));

	return g_slist_reverse (list);
}

gint64
scrollback_get_max_rowid (scrollback_db *db, const char *channel)
{
	gint64 max_id = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_max_rowid);
	sqlite3_bind_int64 (db->stmt_max_rowid, 1, channel_id);

	if (sqlite3_step (db->stmt_max_rowid) == SQLITE_ROW)
		max_id = sqlite3_column_int64 (db->stmt_max_rowid, 0);

	return max_id;
}

int
scrollback_get_index_of_rowid (scrollback_db *db, const char *channel, gint64 rowid)
{
	int index = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_index_of_rowid);
	sqlite3_bind_int64 (db->stmt_index_of_rowid, 1, channel_id);
	sqlite3_bind_int64 (db->stmt_index_of_rowid, 2, rowid);

	if (sqlite3_step (db->stmt_index_of_rowid) == SQLITE_ROW)
		index = sqlite3_column_int (db->stmt_index_of_rowid, 0);

	return index;
}

gint64
scrollback_get_rowid_by_msgid (scrollback_db *db, const char *channel, const char *msgid)
{
	gint64 rowid = 0;
	int rc;

	if (!db || !channel || !msgid || !msgid[0])
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	/* Use a one-off query — this is rare (reply click on evicted entry) */
	{
		sqlite3_stmt *stmt = NULL;
		rc = sqlite3_prepare_v2 (db->db,
			"SELECT id FROM messages WHERE channel_id = ? AND msgid = ? LIMIT 1",
			-1, &stmt, NULL);
		if (rc == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, channel_id);
			sqlite3_bind_text (stmt, 2, msgid, -1, SQLITE_TRANSIENT);
			if (sqlite3_step (stmt) == SQLITE_ROW)
				rowid = sqlite3_column_int64 (stmt, 0);
			sqlite3_finalize (stmt);
		}
	}

	return rowid;
}

GSList *
scrollback_search_text (scrollback_db *db, const char *channel, const char *pattern)
{
	GSList *list = NULL;
	int rc;

	if (!db || !channel || !pattern)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_search_text);
	sqlite3_bind_int64 (db->stmt_search_text, 1, channel_id);
	sqlite3_bind_text (db->stmt_search_text, 2, pattern, -1, SQLITE_TRANSIENT);

	while ((rc = sqlite3_step (db->stmt_search_text)) == SQLITE_ROW)
	{
		scrollback_msg *msg = g_new0 (scrollback_msg, 1);

		msg->id = sqlite3_column_int64 (db->stmt_search_text, 0);
		msg->channel = g_strdup (channel);
		msg->text = g_strdup ((const char *)sqlite3_column_text (db->stmt_search_text, 1));

		list = g_slist_prepend (list, msg);
	}

	if (rc != SQLITE_DONE)
		g_warning ("Error searching scrollback: %s", sqlite3_errmsg (db->db));

	return g_slist_reverse (list);
}
