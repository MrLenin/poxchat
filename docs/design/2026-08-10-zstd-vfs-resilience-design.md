# zstd VFS Resilience — Design

**Date:** 2026-08-10
**Status:** Approved direction: fix the compression VFS (keep compression); no WAL migration, no storage-engine swap.

## Problem

The compressed scrollback store (`src/common/sqlite-zstd-vfs.c` + `src/common/scrollback.c`)
repeatedly reports corruption after unclean exits, and the auto-recovery path then
backs up and recreates the database, discarding history. Evidence from
`config/scrollback/`: 11 `.corrupt.*` backups over ~3 months; `Fractal.db` was
recreated 4 times, dropping from 7.6 MB of history to under 1 MB. Killing the
process is reliably enough to trigger "corruption" on the next launch.

## Root cause (confirmed against all 11 backups)

The databases were never corrupt. In every `.corrupt` file, the page data is
complete and internally consistent (`MAX(pgno)` matches the page count in the
inner SQLite header on page 1), but `meta.page_count` — which the VFS trusts at
open to report file size — is stale, because it is only persisted in
`outer_db_close()`. After any unclean exit:

1. VFS reports a file size smaller than reality (stale `meta.page_count`).
2. Reads past the phantom EOF return zeroed `SQLITE_IOERR_SHORT_READ`.
3. `PRAGMA quick_check` sees a truncated file → "malformed".
4. `scrollback_open()` backs up and recreates a healthy database.

Secondary hazards found during review (not implicated in the 11 incidents but
real):

- `zvfs_read` maps **every** non-`SQLITE_ROW` step result (including
  `SQLITE_BUSY` from a second instance or AV lock) to zeros + `SHORT_READ`,
  which feeds the same nuke-on-open path.
- `BEGIN IMMEDIATE` / `COMMIT` return codes are ignored in
  `zvfs_lock`/`zvfs_sync`/`zvfs_unlock`. A failed BEGIN lets page writes run in
  autocommit (one outer transaction per page — torn state if killed mid-way);
  a failed COMMIT is silent data loss.
- After `zvfs_sync` commits mid-hold, `in_transaction` is 0 while the inner
  lock is still ≥ RESERVED; any subsequent write in that window is unprotected.

## Design

### 1. Geometry: derive, never trust (the fix for the observed corruption)

At open (`outer_db_init`):

- `page_count` = `SELECT MAX(pgno) FROM pages` (the already-prepared,
  currently-unused `stmt_max_pgno`). The pages table after the outer DB's own
  journal recovery is the committed truth; a stale meta row can no longer
  shrink the file.
- `page_size` from meta as today, with fallback: parse big-endian bytes 16–17
  of page 1 (always stored raw) if meta is absent.
- `file_size` = `page_count * page_size`.
- Keep writing the meta rows, but update `page_count` at each commit point
  (inside the outer transaction, only when it changed) instead of only at
  close. Meta becomes diagnostic/back-compat only.
- `g_message` when derived count differs from meta (visible self-heal).

Reporting *more* pages than the inner header claims is safe (SQLite treats the
header count as authoritative and ignores the tail); reporting fewer is what
corrupts. One backup showed a 3-page uncommitted tail beyond the header count —
harmless under this rule.

### 2. Honest errors in the read path

`zvfs_read` step-result handling becomes:

- `SQLITE_ROW` → return data (as today).
- `SQLITE_DONE` → page genuinely absent → zeros + `SQLITE_IOERR_SHORT_READ`
  (legitimate sparse/EOF read).
- anything else (`SQLITE_BUSY`, I/O errors, …) → `SQLITE_IOERR_READ`. Never
  fake zeros for an error.

### 3. Transaction discipline

- `zvfs_lock`: check `BEGIN IMMEDIATE` rc; on failure return `SQLITE_BUSY`
  and do not set `in_transaction`.
- `zvfs_write`: if `lock_level >= RESERVED && !in_transaction`, defensively
  `BEGIN IMMEDIATE` first (closes the post-sync unprotected window). Failure →
  `SQLITE_IOERR_WRITE`.
- `zvfs_sync`: update `meta.page_count` if changed, then `COMMIT`; on failure
  return `SQLITE_IOERR_FSYNC` (and `ROLLBACK` to keep the outer DB
  consistent). If the commit succeeds and `lock_level >= RESERVED`,
  immediately `BEGIN IMMEDIATE` again so the envelope stays closed.
- `zvfs_unlock`: on commit failure, log + `ROLLBACK` (xUnlock errors are
  largely ignored by SQLite; consistency over durability here).
- Net invariant: **no page write ever lands outside an outer transaction.**

### 4. Recovery policy: nuke only on proven corruption

`scrollback_check_integrity()` becomes tri-state:

- `OK` — proceed.
- `CORRUPT` — `quick_check` ran and returned a malformed verdict → backup and
  recreate (as today; now expected to be rare and genuine).
- `ERROR` — prepare/step failed (busy, I/O) → set the failed sentinel for this
  session and retry next launch. **Never** back up / recreate on `ERROR`.

### 5. Salvage the existing backups (one-off script)

Python script (outer format is plain SQLite + zstd, fully readable outside the
app), run with the app closed, for each network with `.corrupt.*` files:

1. Reconstruct each backup and the current live DB into plain inner-image
   SQLite files: read `pages` in pgno order, decompress per `is_compressed`
   method using `meta.zstd_dict` when present, concatenate.
2. Merge old rows into the live image at SQL level, oldest backup first:
   - `messages` with msgid: `INSERT OR IGNORE` (unique `idx_msgid`).
   - `messages` without msgid (events): dedupe on
     `(channel-name, timestamp, text)` (the schema has no separate nick
     column; `text` is the full stored line).
   - `channel_id` remapped through `channels.name` (ids diverged between
     backup and live); insert missing channels first.
   - `reactions`: `INSERT OR IGNORE` (unique `(target_msgid, reaction_text,
     nick)`), with `channel_id` remap.
   - `replies`: `INSERT OR IGNORE` (msgid PK).
   - New rowids for merged rows are fine — runtime queries order by
     channel/timestamp, not rowid.
   - Backups may predate the `channel_id` / `is_user_msg` ALTERs — read
     columns by name and default missing ones (`is_user_msg` → 0,
     `channel_id` → resolve from `channel` text).
3. Re-compress the merged image back into outer format (page 1 raw, zstd per
   page, dict reused, meta rows written), replacing the live DB after a
   `.pre-salvage` backup copy.
4. Verify: `PRAGMA integrity_check` on the merged inner image before
   re-compression; app-side `quick_check` on first open after.

Script lives in `tools/` (checked in — it documents the format and may be
needed again by other users of affected builds).

### 6. Testing

- **Kill test (primary, reproduces the field failure):** run the app, generate
  scrollback growth, kill the process. Relaunch: DB must open clean, no
  `.corrupt` file, history intact. Repeat several times.
- **Stale-meta regression:** hand-craft an outer DB with
  `meta.page_count < MAX(pgno)` (that is: any pre-fix DB killed mid-session);
  open must self-heal and log the mismatch.
- **Busy test:** hold the outer file open/locked from a second process; app
  open must fail gracefully with the sentinel — no `.corrupt` backup created.
- **Salvage verification:** post-merge DBs open in the app, `quick_check`
  clean, old and new history interleaved correctly by timestamp.

## Out of scope

- WAL migration / dropping compression (revisit only if the VFS misbehaves
  again after these fixes).
- RocksDB or any storage-engine swap.
- Schema changes.

## Files touched

- `src/common/sqlite-zstd-vfs.c` — items 1–3.
- `src/common/scrollback.c` — item 4 (integrity tri-state + open-path policy).
- `tools/scrollback-salvage.py` — item 5 (new).
