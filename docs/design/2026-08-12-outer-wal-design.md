# Outer-DB WAL — Design

**Date:** 2026-08-12
**Status:** Implemented 2026-08-14 (with §1b added during implementation). Follow-up to `2026-08-10-zstd-vfs-resilience-design.md`.

That document's "no WAL migration" note rejected *replacing the compression
VFS with a plain SQLite database in WAL mode* — a storage-engine swap. This
proposal keeps the VFS and the nested-database design unchanged and flips only
the **outer container's** journal mode. The two are different decisions at
different layers.

## Motivation

The upstream project we based the VFS on (sqlite_zstd_vfs, Mike Lin) documents
WAL mode as unsupported, and we inherited that stance wholesale. But the
restriction only genuinely applies to the **inner** (client-visible) database.
Our architecture has a second database — the outer container that stores the
compressed pages — and nothing in the design prevents *it* from running WAL.
That is where WAL's benefits actually land:

- Every inner commit currently runs `BEGIN IMMEDIATE … COMMIT` on the outer DB
  under `journal_mode=DELETE` (`sqlite-zstd-vfs.c` `outer_db_init`): create the
  journal file, fsync, commit, delete the journal — per scrollback write batch,
  on every message append. Under WAL a commit is a sequential append to one
  `-wal` file.
- Crash resilience improves. A torn outer commit becomes impossible by
  construction; recovery after a kill is standard WAL replay on the next open.

This does **not** address the long-drag scrollbar sluggishness
(`virt-scroll-jump-perf`) — that is a read-path problem.

## Background: the two layers

```
inner DB  (scrollback schema; journal_mode=MEMORY; all I/O through the VFS)
   │  page reads/writes intercepted, zstd-compressed
   ▼
outer DB  (pages/meta tables; plain SQLite on the OS VFS)
            currently: journal_mode=DELETE, locking_mode=EXCLUSIVE,
                       synchronous=NORMAL
```

The inner DB delegates atomicity and durability entirely to the outer DB's
transaction: `begin_outer` fires when the inner takes RESERVED (`zvfs_lock`),
`commit_outer` when it drops below RESERVED (`zvfs_unlock`) or at `zvfs_sync`.

### Why inner WAL stays off the table

1. **No shared memory.** WAL needs the `xShm*` io_methods for the wal-index;
   ours are `iVersion = 1` and don't implement them. (Workaround exists —
   heap wal-index under `locking_mode=EXCLUSIVE` — but see below.)
2. **The `-wal` file bypasses compression.** Journal/WAL/temp files take the
   passthrough path in `zvfs_open`, so every committed page would sit
   *uncompressed* on disk until checkpoint — defeating the VFS for exactly the
   hottest data.
3. **It breaks the commit-point contract.** The resilience design keys the
   outer transaction envelope to the inner DB's rollback-mode lock protocol
   (`xLock`/`xUnlock` transitions through RESERVED). WAL uses a different
   locking protocol and only writes the main file at checkpoint, so the
   envelope hooks stop firing where the design expects.

Inner WAL is also pointless: its benefits (reader/writer concurrency, cheaper
commits) are already provided by `journal_mode=MEMORY` + the outer
transaction. Nothing to gain, three things to lose.

## Design

### 1. The change itself

In `outer_db_init` (`sqlite-zstd-vfs.c`), replace:

```c
sqlite3_exec (f->outer_db, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
```

with `journal_mode=WAL`. `locking_mode=EXCLUSIVE` is already set on the line
below and **must stay ordered before first file access**: EXCLUSIVE + WAL is
the configuration SQLite supports without shared memory (heap wal-index, no
`-shm` file) — required since we open the outer DB with the default OS VFS and
single-process access is already our model.

`synchronous=NORMAL` stays. Optionally set `wal_autocheckpoint` explicitly;
the default (1000 pages ≈ 4 MB of outer WAL) is reasonable, and with
EXCLUSIVE locking checkpoints can never be blocked by readers.

Everything else in the VFS is untouched. `begin_outer`/`commit_outer`,
the `sqlite3_get_autocommit()` stale-flag detection, and the geometry
derivation are all expressed against the outer *connection*, which sees WAL
state transparently. `MAX(pgno)` at open reads through WAL replay exactly as
it reads through journal rollback today.

### 1b. Sidecar virtualization in the VFS (discovered during implementation)

The pragma flip above is not sufficient on its own. The inner logical DB and
the outer container share one on-disk path — `zvfs_open` is handed the same
`zName` for both, and `outer_db_init` opens the outer connection at that
literal path. Every SQLite pager, on its first shared-lock acquisition, runs
an unconditional "does a leftover `-wal`/hot-journal exist for this file"
probe — this happens regardless of the connection's own `journal_mode`
pragma (the inner is pinned to `MEMORY`, but that pragma hasn't even been
applied yet the first time this probe fires), because journal mode is a
per-file, header-encoded property a fresh connection must rediscover.

Once the outer DB is in WAL mode, that probe finds something: the outer
connection's own live `-wal`, sitting at the exact path the inner pager is
checking. The inner core has no way to know this `-wal` isn't its own —
it judges the file stale relative to what looks like an empty/fresh inner
database and tries to delete it. That delete lands in `zvfs_delete`, which
(pre-fix) forwarded straight to the real OS `xDelete` with no filtering,
racing the outer connection's exclusive lock on the same file:

- **On Windows**, deleting an open, locked file fails outright —
  `SQLITE_IOERR_DELETE`, surfaced to the application as a generic "disk I/O
  error" on the very first write. 100% reproducible, not a timing issue:
  confirmed via instrumented tracing of `zvfs_open`/`zvfs_access`/
  `zvfs_delete`, and via bisection to `fill 1` on a brand-new file.
- **On POSIX**, this is worse, not better: `unlink()` on an open file
  normally *succeeds* silently — the directory entry disappears while the
  outer connection's open file descriptor keeps working. No error surfaces,
  but the outer's live WAL sidecar has been unlinked out from under it by an
  unrelated code path, which is a correctness hazard in its own right (e.g.
  a third process or a later by-name lookup no longer sees it).

**Fix:** the VFS now treats `-wal`/`-journal`/`-shm` suffixed names as
belonging exclusively to the outer connection, never visible to the inner
pager:
- `zvfs_access` reports these names as not existing, so the inner pager's
  hot-WAL/hot-journal probe never finds anything to act on.
- `zvfs_delete` no-ops on these names (returns `SQLITE_OK` without touching
  disk) as a second line of defense — e.g. the inner's transition to
  `journal_mode=MEMORY` itself unconditionally deletes `<db>-journal` as
  part of settling into that mode.
- The passthrough branch of `zvfs_open` refuses `SQLITE_OPEN_MAIN_JOURNAL`
  and `SQLITE_OPEN_WAL` opens outright (`SQLITE_CANTOPEN`, logged), so even
  if some other SQLite code path tries to materialize an inner sidecar file,
  it fails loudly instead of silently colliding with the outer's file.

This also retires a narrower, latent version of the same hazard that existed
before this change, unnoticed because it never had an observable effect: the
outer DB's own transient `-journal` (present only during an open outer
transaction, under the old `journal_mode=DELETE`) was, for that brief
window, equally visible to the inner pager's hot-journal probe via the same
unfiltered `xAccess`/`xDelete` passthrough. It never manifested as a bug
because DELETE-mode journals are usually gone by the time a *new* connection
does its first-open probe. WAL made the sidecar a standing, near-permanent
fixture instead of a fleeting one, turning a theoretical race into a
guaranteed collision on every open.

### 2. Durability semantics (deliberate change, strictly no worse)

| Failure | DELETE + NORMAL (today) | WAL + NORMAL (proposed) |
|---|---|---|
| Process kill (the observed failure mode — all 11 historical incidents) | Safe: hot journal rolled back at next open | Safe: `-wal` contents survive and replay at next open; **zero loss** |
| OS crash / power loss | Journal synced before commit; rare corruption windows are the reason `synchronous=FULL` exists | May lose commits since the last checkpoint (un-fsynced WAL tail), but **never corrupts** |

WAL+NORMAL trades a small power-loss durability window for the elimination of
corruption windows. For scrollback data that is the right trade.

### 3. Companion fixes (required, all in the sibling-file class)

The rule: **`db`, `db-wal`, `db-shm` are one unit in every rename, copy, and
delete.** Three sites violate it today:

1. **`scrollback_backup_corrupt` (`scrollback.c:531`) deletes the tail it
   should preserve.** It renames the DB to `X.corrupt.<ts>` but *unlinks* any
   `-wal`/`-shm` (the comment says "move"; the code deletes). Under WAL, an
   unclean exit is precisely when a `-wal` holds un-checkpointed pages — and
   quick_check failing is precisely when we take a backup for later salvage.
   Fix: rename the WAL to `X.corrupt.<ts>-wal` so a later SQLite open of the
   backup replays it (SQLite pairs WAL to DB by name). Deleting `-shm` stays
   correct (it's reconstructible, and EXCLUSIVE mode never creates one).
   Also unlink any stale `-wal`/`-shm` *at the original path* after the
   rename? No — the rename moved the db; a leftover `-wal` at the original
   path next to the freshly *recreated* db would be replayed into it,
   corrupting the new file. The rename above removes exactly that hazard by
   moving the WAL with its database. Nothing may remain at `path-wal` when
   the new DB is created.
2. **Salvage tool copies (`tools/scrollback-salvage.py:363-367`).**
   `copy_live_db` copies the DB and a `-journal` sibling but not `-wal`.
   Both the `.pre-salvage.<ts>` copies and the working copies need a `-wal`
   leg (and the tool's later opens already go through `sqlite3.connect`, which
   replays it). Same for interpreting `.corrupt.*` backups once fix 1 lands.
3. **Legacy-migration rename (`scrollback.c:1121`)** renames a pre-VFS text
   file — no SQLite siblings, no change needed. Listed for completeness.

### 4. In-place conversion and rollback

`journal_mode=WAL` is persistent (recorded in the DB header), so the nine live
outer DBs convert on first open after the change — no migration step. Rollback
is equally cheap: `journal_mode=DELETE` converts back (requires
checkpointing, which our EXCLUSIVE single-connection model always permits).
A one-off `PRAGMA journal_mode=DELETE` sweep, or temporarily reverting the
pragma line, fully undoes the flip. Downgraded builds (old binary, WAL DB)
still open fine — WAL is supported by every SQLite we ship; the old binary
would simply switch the header back to DELETE on open.

Note: on *clean* close, SQLite checkpoints and removes the `-wal`
automatically (last connection, no `SQLITE_FCNTL_PERSIST_WAL`). A lingering
`-wal` on disk is therefore itself a signal of an unclean previous exit —
worth a `g_message` at open for diagnosability, mirroring the geometry
self-heal message.

## Verification plan

- Re-run the crash harness (`tools/build-vfs-test.ps1`, `tools/run-vfs-tests.ps1`):
  kill, stale-meta, and busy legs must stay green under WAL.
- New harness leg: kill mid-write, assert `-wal` exists, reopen, assert
  quick_check passes and row count includes the pre-kill commits (WAL replay).
- New harness leg: force quick_check failure (corrupt a page blob), assert the
  `.corrupt.*` backup carries its `-wal` sibling and that opening the backup
  with stock sqlite3 sees the tail rows.
- Manual: run the app, kill it mid-traffic, relaunch, confirm no corruption
  banner and no message loss; confirm clean exit leaves no `-wal` behind.
- Salvage dry-run (`--dry-run`) against a WAL-mode DB with a live `-wal`.

## Non-goals

- Inner-DB WAL (rejected above).
- Any change to the inner `journal_mode=MEMORY` or the lock-transition commit
  envelope.
- Scroll/drag read-path performance (tracked separately).
