/* sqlite-zstd-vfs.h — Transparent page-level zstd compression VFS for SQLite
 *
 * Stores compressed pages of an "inner" SQLite database as rows in an
 * "outer" SQLite database.  The outer DB is a normal SQLite file; the
 * inner DB is what the application operates on via the custom VFS.
 *
 * Usage:
 *   zstd_vfs_register("zstd");
 *   sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, "zstd");
 *   sqlite3_exec(db, "PRAGMA journal_mode=MEMORY;", ...);
 *   // ... use db normally ...
 *   sqlite3_close(db);
 *   zstd_vfs_shutdown();
 */

#ifndef SQLITE_ZSTD_VFS_H
#define SQLITE_ZSTD_VFS_H

/* Register the zstd compression VFS with the given name.
 * Returns SQLITE_OK on success. */
int zstd_vfs_register (const char *vfs_name);

/* Unregister and free the VFS.  Call after all databases are closed. */
void zstd_vfs_shutdown (void);

/* Move an outer database aside together with its -wal sidecar.  The pair
 * is one unit: a -wal left behind at the old path would be replayed by
 * SQLite into whatever database is created there next, corrupting it —
 * and a backup without its -wal silently loses the un-checkpointed tail.
 * A -shm sidecar is deleted (reconstructible cache, never valid to move).
 * Returns 0 on success, -1 if the main rename failed. */
int zstd_vfs_backup_db (const char *path, const char *backup_path);

#endif /* SQLITE_ZSTD_VFS_H */
