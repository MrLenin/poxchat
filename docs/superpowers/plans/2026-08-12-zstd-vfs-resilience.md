# zstd VFS Resilience Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the compressed scrollback store from reporting phantom corruption after unclean exits, stop it discarding history on transient errors, and recover the 11 `.corrupt` backups.

**Architecture:** The scrollback store is a "database inside a database": the on-disk file is a plain SQLite DB (the *outer* DB) holding zstd-compressed 4096-byte pages of an *inner* SQLite DB in a `pages(pgno, data, is_compressed)` table plus a `meta(key, value)` table (`page_size`, `page_count`, `zstd_dict`). A custom VFS (`src/common/sqlite-zstd-vfs.c`) translates the inner DB's page I/O into SQL on the outer DB. Root cause of all observed corruption: `meta.page_count` is only persisted at clean close, so any process kill leaves it stale; on reopen the VFS reports a too-small file size, SQLite sees a truncated DB, `quick_check` says "malformed", and `scrollback_open()` nukes a healthy database. Full analysis: `docs/design/2026-08-10-zstd-vfs-resilience-design.md`.

**Tech Stack:** C (C11, GLib, SQLite, vendored zstd amalgamation), MSVC via VS 2022, Python 3 + `zstandard` (installed, 0.25.0) for the salvage tool.

## Global Constraints

- Style: tabs for indentation, `module_action()` naming, GLib memory functions (`g_malloc`/`g_free`/`g_strdup`), match surrounding code (see `CLAUDE.md`).
- Windows build: **64-bit MSBuild host only** — `"/c/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe"` with `//p:PreferredToolArchitecture=x64` (Git-Bash `//` escapes; single `/` from PowerShell). No Debug config exists; build `Release|x64`, whole solution, never `//t:<project>`. Read `.claude/skills/windows-build.md` before interpreting failures — jansson/lua/cffi/fe-text errors are pre-existing environmental noise; `LNK1104` on `poxchat.exe` means the app is running, not a code error.
- Build output: `C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\`.
- Dependency tree (gvsbuild): `c:\gtk-build\gtk4\x64\release` (`include\`, `lib\`, `bin\`). sqlite3 and glib come from there; zstd is vendored at `src/common/zstd/zstd.c`.
- Live user data: `C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\config\scrollback\` — **never modify in place without `--apply` + a `.pre-salvage` backup; test only on copies.** The app must be closed for any salvage run.
- Compression method constants (must match in C and Python): `0` = raw, `1` = zstd, `2` = zstd+dict. Page 1 is always stored raw. `meta.page_size`/`meta.page_count` are stored as TEXT; `meta.zstd_dict` as BLOB.
- Do not change the outer DB schema or the inner scrollback schema.

## File Structure

- `tools/zstd-vfs-test.c` — new: standalone harness (fill / kill / check / geom / hold) linking the real VFS.
- `tools/build-vfs-test.ps1` — new: cl.exe build script for the harness.
- `tools/run-vfs-tests.ps1` — new: regression driver, exits non-zero on any failure.
- `src/common/sqlite-zstd-vfs.c` — modify: geometry derivation, read-path error discipline, transaction helpers.
- `src/common/scrollback.c` — modify: tri-state integrity check + open-path policy.
- `tools/scrollback-salvage.py` — new: reconstruct / merge / re-compress `.corrupt` backups.

---

### Task 1: Crash-test harness that reproduces the field failure

The harness is the "failing test" for Tasks 2–4: it must demonstrate, before any fix, that *commit + process death = corruption on reopen* (exactly what the user reproduces by killing the process).

**Files:**
- Create: `tools/zstd-vfs-test.c`
- Create: `tools/build-vfs-test.ps1`
- Create: `tools/run-vfs-tests.ps1`

**Interfaces:**
- Produces: `tools\out\zstd-vfs-test.exe <db-path> <command>` with commands `fill N` (insert N rows, clean close), `kill N` (insert N rows, `COMMIT`, then `_exit(9)` — no close), `check` (open via VFS, `PRAGMA quick_check`; prints row count of test table `t` when present), `geom` (plain-open the outer DB, print `meta_page_count=X max_pgno=Y`), `hold SECONDS` (plain-open the outer DB, `BEGIN EXCLUSIVE`, sleep). Exit codes: `0` ok, `1` corrupt, `2` error (open/busy/IO), `3` usage, `9` from `kill`.
- Produces: `tools/run-vfs-tests.ps1 [-ExpectKillFail]` — full suite; `-ExpectKillFail` asserts the pre-fix behavior (kill ⇒ corrupt).

- [ ] **Step 1: Write the harness**

`tools/zstd-vfs-test.c`:

```c
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
	fprintf (stderr, "usage: %s <db> fill N | kill N | check | geom | hold SECONDS\n",
	         argv[0]);
	return 3;
}
```

- [ ] **Step 2: Write the build script**

`tools/build-vfs-test.ps1`:

```powershell
# Builds tools\out\zstd-vfs-test.exe (x64). Requires VS 2022 + gvsbuild deps.
$ErrorActionPreference = 'Stop'
$devShell = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' | Select-Object -First 1
if (-not $devShell) { throw 'VS 2022 Launch-VsDevShell.ps1 not found' }
$repo = Split-Path $PSScriptRoot -Parent
& $devShell.FullName -Arch amd64 -SkipAutomaticLocation | Out-Null
$deps = 'c:\gtk-build\gtk4\x64\release'
$out = Join-Path $repo 'tools\out'
New-Item -ItemType Directory -Force $out | Out-Null
cl /nologo /O1 /W3 /MD /D_CRT_SECURE_NO_WARNINGS `
  "/I$repo\src\common" "/I$repo\src\common\zstd" `
  "/I$deps\include" "/I$deps\include\glib-2.0" "/I$deps\lib\glib-2.0\include" `
  "$repo\tools\zstd-vfs-test.c" "$repo\src\common\sqlite-zstd-vfs.c" "$repo\src\common\zstd\zstd.c" `
  /Fo"$out\" /Fe"$out\zstd-vfs-test.exe" `
  /link "/LIBPATH:$deps\lib" sqlite3.lib glib-2.0.lib intl.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Write-Host "built $out\zstd-vfs-test.exe"
```

(If `cl` reports unresolved glib symbols, add `gobject-2.0.lib`; if intl is absent in this deps tree, drop `intl.lib`.)

- [ ] **Step 3: Write the test driver**

`tools/run-vfs-tests.ps1`:

```powershell
# Regression suite for the zstd VFS. -ExpectKillFail asserts PRE-FIX behavior.
param([switch]$ExpectKillFail)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo 'tools\out\zstd-vfs-test.exe'
if (-not (Test-Path $exe)) { throw "build first: tools\build-vfs-test.ps1" }
$env:PATH = "c:\gtk-build\gtk4\x64\release\bin;$env:PATH"
$db = Join-Path $env:TEMP ("vfs-test-{0}.db" -f [guid]::NewGuid())

function Step($name, $cmdargs, $expect) {
  & $exe $db @cmdargs | Write-Host
  if ($LASTEXITCODE -ne $expect) { throw "FAIL ${name}: exit $LASTEXITCODE, expected $expect" }
  Write-Host "PASS: $name"
}

# 1. clean lifecycle
Step 'fill-clean' @('fill','500') 0
Step 'check-clean' @('check') 0

# 2. commit + process death (the field failure: kill the app, reopen)
& $exe $db kill 500 | Write-Host
if ($LASTEXITCODE -ne 9) { throw "FAIL kill: exit $LASTEXITCODE, expected 9" }
& $exe $db geom | Write-Host
$killExpect = if ($ExpectKillFail) { 1 } else { 0 }
Step 'check-after-kill' @('check') $killExpect

# 3. busy: another process holds the outer DB -> must be error(2), never corrupt(1)
if (-not $ExpectKillFail) {
  Step 'reset-fill' @('fill','10') 0   # ensure db is healthy for the busy test
}
$hold = Start-Process $exe -ArgumentList "`"$db`"",'hold','8' -PassThru -NoNewWindow
Start-Sleep 2
& $exe $db check | Write-Host
if ($LASTEXITCODE -ne 2) { throw "FAIL busy-check: exit $LASTEXITCODE, expected 2" }
Write-Host 'PASS: busy-check'
$hold.WaitForExit()
if (-not $ExpectKillFail) { Step 'check-after-hold' @('check') 0 }

Remove-Item $db -ErrorAction SilentlyContinue
Write-Host 'ALL PASS'
```

- [ ] **Step 4: Build and run in pre-fix mode — the kill test must FAIL exactly like the field reports**

Run (PowerShell):
```powershell
powershell -File tools\build-vfs-test.ps1
powershell -File tools\run-vfs-tests.ps1 -ExpectKillFail
```
Expected: `geom` prints `meta_page_count` **smaller than** `max_pgno`; `check-after-kill` exits `1` (quick_check not "ok") so the suite prints `ALL PASS` under `-ExpectKillFail`. This confirms the harness reproduces the bug. If `check-after-kill` exits `0` here, the harness has failed to reproduce the field failure — stop and investigate before proceeding (do not proceed to Task 2 with a harness that can't see the bug).

- [ ] **Step 5: Commit**

```bash
git add tools/zstd-vfs-test.c tools/build-vfs-test.ps1 tools/run-vfs-tests.ps1
git commit -m "tools: crash-consistency harness for the zstd VFS

Reproduces the field failure: COMMIT + process death => stale
meta.page_count => phantom-truncated inner DB on reopen."
```

---

### Task 2: Geometry self-heal — derive page_count from the pages table

**Files:**
- Modify: `src/common/sqlite-zstd-vfs.c` (struct `zstd_vfs_file` ~line 44; `outer_db_init` metadata block ~lines 152–185)
- Test: `tools/run-vfs-tests.ps1` (no `-ExpectKillFail` for the kill leg — see step 3)

**Interfaces:**
- Consumes: `stmt_max_pgno` (`SELECT MAX(pgno) FROM pages`) — already prepared in `outer_db_init`, currently never executed.
- Produces: field `int meta_page_count_saved;` on `zstd_vfs_file` (used by Task 3's `commit_outer`).

- [ ] **Step 1: Add the tracking field**

In the `zstd_vfs_file` struct, after `int page_count;`:

```c
	int meta_page_count_saved;	/* last page_count written to meta */
```

- [ ] **Step 2: Replace the meta-trusting geometry block in `outer_db_init`**

Replace the existing `/* Load metadata */` block (the one that reads `page_size`/`page_count` from `meta` into `f->page_size`/`f->page_count` and computes `f->file_size`) with:

```c
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
		if (sqlite3_step (f->stmt_max_pgno) == SQLITE_ROW)
			max_pgno = sqlite3_column_int (f->stmt_max_pgno, 0);
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
				f->page_size = (ps == 1) ? 65536 : ps;
			}
			sqlite3_reset (f->stmt_read);
		}

		f->page_count = max_pgno;
		f->meta_page_count_saved = meta_page_count;
		if (f->page_size > 0)
			f->file_size = (sqlite3_int64)f->page_size * f->page_count;

		if (meta_page_count > 0 && meta_page_count != f->page_count)
			g_message ("zstd-vfs: %s: derived page_count %d (stale meta said %d) — self-healed",
			           path, f->page_count, meta_page_count);
	}
```

- [ ] **Step 3: Rebuild the harness and run the suite in post-fix mode (kill leg)**

```powershell
powershell -File tools\build-vfs-test.ps1
powershell -File tools\run-vfs-tests.ps1
```
Expected at this point: `check-after-kill` now exits `0` with `rows: 1000` (500 + 500), and the `geom` line still shows the stale meta (self-heal message appears on stderr/console from `check`). **The `busy-check` leg exits `2` already (open-time busy fails cleanly pre-Task-3), so the full suite should print `ALL PASS`.** If `check-after-kill` still exits 1, stop — the derivation is wrong; debug before touching anything else.

- [ ] **Step 4: Commit**

```bash
git add src/common/sqlite-zstd-vfs.c
git commit -m "zstd-vfs: derive geometry from MAX(pgno), not stale meta

meta.page_count was only persisted at clean close, so any process
kill left it stale and the next open saw a phantom-truncated inner
DB -> quick_check 'malformed' -> scrollback auto-nuke. All 11
.corrupt backups in the field showed exactly this signature. The
pages table is the committed truth; meta is now diagnostic only."
```

---

### Task 3: Transaction discipline and honest read errors in the VFS

**Files:**
- Modify: `src/common/sqlite-zstd-vfs.c` (`zvfs_read` ~line 434, `zvfs_write` ~line 521, `zvfs_truncate` ~line 577, `zvfs_sync` ~line 598, `zvfs_lock` ~line 620, `zvfs_unlock` ~line 635, `zvfs_close` ~line 415)
- Test: `tools/run-vfs-tests.ps1`

**Interfaces:**
- Consumes: `f->meta_page_count_saved` from Task 2.
- Produces: static helpers `begin_outer (zstd_vfs_file *f)` / `commit_outer (zstd_vfs_file *f)`, both returning a SQLite rc.

- [ ] **Step 1: Add the transaction helpers** (place above `zvfs_close`, after the dictionary-training section)

```c
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
		return SQLITE_OK;
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
```

- [ ] **Step 2: Route all transaction handling through the helpers**

- `zvfs_lock`: replace the body's `if (level >= 2 && !f->in_transaction) { sqlite3_exec(... "BEGIN IMMEDIATE" ...); f->in_transaction = 1; }` with:

```c
	if (level >= 2 && begin_outer (f) != SQLITE_OK)
		return SQLITE_BUSY;
```

- `zvfs_write`: at the top of the function, before page-size detection, add:

```c
	if (begin_outer (f) != SQLITE_OK)
		return SQLITE_IOERR_WRITE;
```

- `zvfs_truncate`: after the `page_size == 0` early return, add the same `begin_outer` guard returning `SQLITE_IOERR_TRUNCATE`, and check the delete:

```c
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
```

- `zvfs_sync`: replace the body with:

```c
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
```

- `zvfs_unlock`: replace the commit block with (return stays `SQLITE_OK`; xUnlock errors are ignored by SQLite, and `commit_outer` already rolled back on failure, keeping the outer DB consistent at the cost of that transaction):

```c
	if (level < 2 && f->lock_level >= 2)
		commit_outer (f);
```

- `zvfs_close`: replace the open-transaction commit block with `commit_outer (f);`.

- [ ] **Step 3: Make `zvfs_read` distinguish "no such page" from errors**

In `zvfs_read`, replace:

```c
	rc = sqlite3_step (f->stmt_read);
	if (rc != SQLITE_ROW)
	{
		memset (buf, 0, iAmt);
		return SQLITE_IOERR_SHORT_READ;
	}
```

with:

```c
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
```

(Do not add a `sqlite3_reset` before the blob is copied — the row must stay live until the `memcpy` below; the reset at the top of the next call handles it, as today.)

- [ ] **Step 4: Rebuild, run the full suite**

```powershell
powershell -File tools\build-vfs-test.ps1
powershell -File tools\run-vfs-tests.ps1
```
Expected: `ALL PASS` — clean lifecycle, kill-then-check with 1000 rows, busy exits 2, post-hold check ok.

- [ ] **Step 5: Commit**

```bash
git add src/common/sqlite-zstd-vfs.c
git commit -m "zstd-vfs: transaction rc discipline + honest read errors

BEGIN/COMMIT results are now checked and propagated; every page
write is guaranteed to land inside an outer transaction (defensive
begin in xWrite, immediate re-begin after the xSync commit). xRead
no longer converts SQLITE_BUSY/errors into zeroed short reads that
masquerade as a truncated database."
```

---

### Task 4: Tri-state integrity check in scrollback.c — nuke only on proven corruption

**Files:**
- Modify: `src/common/scrollback.c` (`scrollback_check_integrity` ~lines 494–514, open path ~lines 592–612)

**Interfaces:**
- Produces: `typedef enum { SB_INTEGRITY_OK, SB_INTEGRITY_CORRUPT, SB_INTEGRITY_ERROR } sb_integrity;` and `scrollback_check_integrity` returning it (both file-static).

- [ ] **Step 1: Replace `scrollback_check_integrity`**

```c
typedef enum {
	SB_INTEGRITY_OK,
	SB_INTEGRITY_CORRUPT,	/* quick_check ran and reported malformation */
	SB_INTEGRITY_ERROR	/* indeterminate (busy / I/O) — do NOT recreate */
} sb_integrity;

/* Check database integrity.  Only SB_INTEGRITY_CORRUPT may trigger the
 * backup-and-recreate path; transient errors must never cost history. */
static sb_integrity
scrollback_check_integrity (sqlite3 *db)
{
	sqlite3_stmt *stmt;
	int rc;
	sb_integrity result;

	rc = sqlite3_prepare_v2 (db, "PRAGMA quick_check(1)", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
			? SB_INTEGRITY_CORRUPT : SB_INTEGRITY_ERROR;

	rc = sqlite3_step (stmt);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (stmt, 0);
		result = (text && strcmp (text, "ok") == 0)
			? SB_INTEGRITY_OK : SB_INTEGRITY_CORRUPT;
	}
	else
		result = (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
			? SB_INTEGRITY_CORRUPT : SB_INTEGRITY_ERROR;

	sqlite3_finalize (stmt);
	return result;
}
```

- [ ] **Step 2: Update the open path in `scrollback_open`**

Replace the `if (!scrollback_check_integrity (sdb->db))` block with:

```c
	/* Check for corruption before using the database */
	{
		sb_integrity integ = scrollback_check_integrity (sdb->db);

		if (integ == SB_INTEGRITY_ERROR)
		{
			g_warning ("Scrollback database for %s unreadable right now "
			           "(busy or I/O error) — will retry next session", network);
			sqlite3_close (sdb->db);
			g_free (path);
			g_free (sdb->network);
			g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
			g_free (sdb);
			return NULL;
		}

		if (integ == SB_INTEGRITY_CORRUPT)
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
	}
```

(The recreate branch is the existing code unchanged; only the surrounding classification is new.)

- [ ] **Step 3: Build the full solution**

Run (Git-Bash from repo root):
```bash
MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSBUILD" win32/poxchat.sln //p:Configuration=Release //p:Platform=x64 //p:PreferredToolArchitecture=x64 //v:minimal //nologo 2>&1 \
  | grep -E "error C[0-9]|error [A-Z]" \
  | grep -v "jansson\|lua.h\|cffi\|MSB3073\|MSB3027\|MSB3021"
```
Expected: no output from the filter (see `.claude/skills/windows-build.md` for the known environmental noise; `LNK1104` on `poxchat.exe` = app running, close it and rebuild).

- [ ] **Step 4: Commit**

```bash
git add src/common/scrollback.c
git commit -m "scrollback: tri-state integrity check; never nuke on transient errors

backup-and-recreate now requires a genuine quick_check malformation
verdict. Busy/IO failures set the failed sentinel and retry next
session instead of discarding the user's history."
```

---

### Task 5: Salvage tool for the `.corrupt` backups

**Files:**
- Create: `tools/scrollback-salvage.py`
- Test: run against **copies** of real data in the scratchpad (never the live dir directly)

**Interfaces:**
- Consumes: outer DB format (Global Constraints), harness `check` command from Task 1 for final verification.
- Produces: CLI `python tools/scrollback-salvage.py <scrollback-dir> [--apply]`. Dry-run by default: prints per-network reconstruction verdicts and would-be merged row counts. With `--apply`: copies `X.db` to `X.db.pre-salvage.<unix-ts>`, then replaces `X.db` with the merged, re-compressed result.

- [ ] **Step 1: Write the script**

`tools/scrollback-salvage.py`:

```python
#!/usr/bin/env python3
"""Salvage poxchat compressed scrollback databases.

The on-disk scrollback file is a plain SQLite DB (the "outer" DB) holding
zstd-compressed 4096-byte pages of an "inner" SQLite DB:
    pages(pgno INTEGER PRIMARY KEY, data BLOB, is_compressed INTEGER)
    meta(key TEXT PRIMARY KEY, value BLOB)   -- page_size, page_count, zstd_dict
is_compressed: 0 = raw, 1 = zstd, 2 = zstd + dictionary.  Page 1 is raw.

Pre-fix builds only persisted meta.page_count at clean close, so any process
kill left it stale; the app then declared the DB corrupt and renamed it to
<db>.corrupt.<timestamp>.  The page data in those backups is intact.  This
tool reconstructs each backup, merges its rows into the live DB (dedup by
msgid where present, by channel/timestamp/text otherwise), and re-compresses.

Usage:
    python tools/scrollback-salvage.py <scrollback-dir>            # dry run
    python tools/scrollback-salvage.py <scrollback-dir> --apply

Run only with the app closed.  --apply keeps a <db>.pre-salvage.<ts> copy.
Requires: pip install zstandard
"""
import argparse
import glob
import os
import shutil
import sqlite3
import struct
import sys
import tempfile
import time

import zstandard

RAW, ZSTD, ZSTD_DICT = 0, 1, 2


def read_outer(path):
    """Return (page_size, header_count, {pgno: page_bytes}, dict_bytes)."""
    db = sqlite3.connect(path)  # rw open so a leftover -journal is recovered
    try:
        meta = dict(db.execute("SELECT key, value FROM meta").fetchall())
        dict_bytes = meta.get("zstd_dict")
        dctx_plain = zstandard.ZstdDecompressor()
        dctx_dict = (zstandard.ZstdDecompressor(
                        dict_data=zstandard.ZstdCompressionDict(dict_bytes))
                     if dict_bytes else None)
        rows = db.execute(
            "SELECT pgno, data, is_compressed FROM pages ORDER BY pgno").fetchall()
    finally:
        db.close()
    if not rows or rows[0][0] != 1 or rows[0][2] != RAW:
        raise ValueError("page 1 missing or not stored raw")
    hdr = rows[0][1]
    ps = (hdr[16] << 8) | hdr[17]
    page_size = 65536 if ps == 1 else ps
    header_count = struct.unpack(">I", hdr[28:32])[0]
    pages = {}
    for pgno, data, method in rows:
        if method == RAW:
            img = bytes(data)
            if len(img) < page_size:
                img += b"\0" * (page_size - len(img))
        elif method == ZSTD:
            img = dctx_plain.decompress(data, max_output_size=page_size)
        elif method == ZSTD_DICT:
            if not dctx_dict:
                raise ValueError(f"page {pgno}: dict-compressed but no dict stored")
            img = dctx_dict.decompress(data, max_output_size=page_size)
        else:
            raise ValueError(f"page {pgno}: unknown method {method}")
        if len(img) != page_size:
            raise ValueError(f"page {pgno}: {len(img)} bytes, expected {page_size}")
        pages[pgno] = img
    return page_size, header_count, pages, dict_bytes


def reconstruct(outer_path, image_path):
    """Decompress an outer DB into a plain inner SQLite image; integrity-check it."""
    page_size, header_count, pages, dict_bytes = read_outer(outer_path)
    max_pgno = max(pages)
    # The inner header count is authoritative; pages beyond it are an
    # uncommitted tail (seen in one field backup) and are dropped.
    count = min(header_count, max_pgno) if header_count else max_pgno
    missing = [p for p in range(1, count + 1) if p not in pages]
    if missing:
        raise ValueError(f"missing pages within header count: {missing[:10]}")
    with open(image_path, "wb") as fh:
        for p in range(1, count + 1):
            fh.write(pages[p])
    db = sqlite3.connect(image_path)
    try:
        verdict = db.execute("PRAGMA integrity_check").fetchone()[0]
    finally:
        db.close()
    return count, verdict, dict_bytes


def table_cols(db, table):
    return [r[1] for r in db.execute(f"PRAGMA table_info({table})")]


def bk_has_table(db, name):
    return db.execute(
        "SELECT 1 FROM bk.sqlite_master WHERE type='table' AND name=?",
        (name,)).fetchone() is not None


def merge_backup(live_image, backup_image):
    """Merge rows from backup_image into live_image.  Returns stats dict."""
    db = sqlite3.connect(live_image)
    stats = {}
    try:
        db.execute("ATTACH ? AS bk", (backup_image,))
        bcols = table_cols(db, "bk.messages")
        # Channel name of a backup row (channel_id may not exist / be NULL)
        if "channel_id" in bcols and bk_has_table(db, "channels"):
            ch = ("COALESCE((SELECT name FROM bk.channels c WHERE c.id = b.channel_id),"
                  " b.channel)")
        else:
            ch = "b.channel"
        ium = "b.is_user_msg" if "is_user_msg" in bcols else "0"

        with db:  # one transaction for the whole merge
            db.execute(f"INSERT OR IGNORE INTO channels (name) "
                       f"SELECT DISTINCT {ch} FROM bk.messages b")

            before = db.execute("SELECT COUNT(*) FROM messages").fetchone()[0]
            # msgid rows: idx_msgid (partial UNIQUE) dedupes via OR IGNORE
            db.execute(f"""
                INSERT OR IGNORE INTO messages
                    (channel, timestamp, msgid, text,
                     redacted_by, redact_reason, redact_time,
                     channel_id, is_user_msg)
                SELECT {ch}, b.timestamp, b.msgid, b.text,
                       b.redacted_by, b.redact_reason, b.redact_time,
                       (SELECT id FROM channels ch2 WHERE ch2.name = {ch}), {ium}
                FROM bk.messages b WHERE b.msgid IS NOT NULL""")
            # msgid-less rows (events): dedupe on channel/timestamp/text
            db.execute(f"""
                INSERT INTO messages
                    (channel, timestamp, msgid, text,
                     redacted_by, redact_reason, redact_time,
                     channel_id, is_user_msg)
                SELECT {ch}, b.timestamp, NULL, b.text,
                       b.redacted_by, b.redact_reason, b.redact_time,
                       (SELECT id FROM channels ch2 WHERE ch2.name = {ch}), {ium}
                FROM bk.messages b
                WHERE b.msgid IS NULL AND NOT EXISTS
                      (SELECT 1 FROM messages m
                       WHERE m.msgid IS NULL AND m.timestamp = b.timestamp
                         AND m.text = b.text AND m.channel = {ch})""")
            stats["messages"] = (
                db.execute("SELECT COUNT(*) FROM messages").fetchone()[0] - before)

            if bk_has_table(db, "reactions"):
                before = db.execute("SELECT COUNT(*) FROM reactions").fetchone()[0]
                rcols = table_cols(db, "bk.reactions")
                if "channel_id" in rcols and bk_has_table(db, "channels"):
                    rch = ("COALESCE((SELECT name FROM bk.channels c "
                           "WHERE c.id = b.channel_id), b.channel)")
                else:
                    rch = "b.channel"
                db.execute(f"""
                    INSERT OR IGNORE INTO reactions
                        (channel, target_msgid, reaction_text, nick,
                         is_self, timestamp, channel_id)
                    SELECT {rch}, b.target_msgid, b.reaction_text, b.nick,
                           b.is_self, b.timestamp,
                           (SELECT id FROM channels ch2 WHERE ch2.name = {rch})
                    FROM bk.reactions b""")
                stats["reactions"] = (
                    db.execute("SELECT COUNT(*) FROM reactions").fetchone()[0] - before)

            if bk_has_table(db, "replies"):
                before = db.execute("SELECT COUNT(*) FROM replies").fetchone()[0]
                db.execute("""
                    INSERT OR IGNORE INTO replies
                        (msgid, target_msgid, target_nick, target_preview)
                    SELECT b.msgid, b.target_msgid, b.target_nick, b.target_preview
                    FROM bk.replies b""")
                stats["replies"] = (
                    db.execute("SELECT COUNT(*) FROM replies").fetchone()[0] - before)
        db.execute("DETACH bk")
    finally:
        db.close()
    return stats


def write_outer(image_path, out_path, dict_bytes):
    """Re-compress a plain inner image into the outer DB format."""
    if os.path.exists(out_path):
        os.remove(out_path)
    with open(image_path, "rb") as fh:
        data = fh.read()
    hdr = data[:100]
    ps = (hdr[16] << 8) | hdr[17]
    page_size = 65536 if ps == 1 else ps
    if len(data) % page_size:
        raise ValueError(f"image size {len(data)} not a multiple of {page_size}")
    n = len(data) // page_size
    cd = zstandard.ZstdCompressionDict(dict_bytes) if dict_bytes else None
    cctx_dict = zstandard.ZstdCompressor(level=3, dict_data=cd) if cd else None
    cctx = zstandard.ZstdCompressor(level=3)
    db = sqlite3.connect(out_path)
    try:
        db.executescript(
            "CREATE TABLE pages (pgno INTEGER PRIMARY KEY, data BLOB NOT NULL,"
            " is_compressed INTEGER NOT NULL DEFAULT 1);"
            "CREATE TABLE meta (key TEXT PRIMARY KEY, value BLOB);")
        with db:
            for i in range(n):
                pgno = i + 1
                pg = data[i * page_size:(i + 1) * page_size]
                if pgno == 1:
                    db.execute("INSERT INTO pages VALUES (?,?,?)",
                               (pgno, pg, RAW))
                    continue
                if cctx_dict:
                    comp, method = cctx_dict.compress(pg), ZSTD_DICT
                else:
                    comp, method = cctx.compress(pg), ZSTD
                if len(comp) >= page_size:
                    comp, method = pg, RAW
                db.execute("INSERT INTO pages VALUES (?,?,?)",
                           (pgno, comp, method))
            db.execute("INSERT INTO meta VALUES ('page_size', ?)", (str(page_size),))
            db.execute("INSERT INTO meta VALUES ('page_count', ?)", (str(n),))
            if dict_bytes:
                db.execute("INSERT INTO meta VALUES ('zstd_dict', ?)",
                           (sqlite3.Binary(dict_bytes),))
    finally:
        db.close()


def salvage_network(live_path, backups, apply_changes, workdir):
    name = os.path.basename(live_path)
    print(f"\n=== {name}: {len(backups)} backup(s) ===")
    live_img = os.path.join(workdir, name + ".live.img")
    dict_bytes = None
    if os.path.exists(live_path):
        count, verdict, dict_bytes = reconstruct(live_path, live_img)
        print(f"  live: {count} pages, integrity: {verdict}")
        if verdict != "ok":
            print(f"  !! live DB failed integrity ({verdict}) — skipping network")
            return
    else:
        # No live DB: seed from the newest reconstructable backup
        for bk in sorted(backups, reverse=True):
            try:
                count, verdict, dict_bytes = reconstruct(bk, live_img)
            except ValueError as e:
                print(f"  !! seed {os.path.basename(bk)}: {e}")
                continue
            if verdict == "ok":
                print(f"  seeded from {os.path.basename(bk)} ({count} pages)")
                backups = [b for b in backups if b != bk]
                break
        else:
            print("  !! no usable backup to seed from — skipping network")
            return

    for bk in sorted(backups):  # oldest first
        bk_img = os.path.join(workdir, os.path.basename(bk) + ".img")
        try:
            count, verdict, _ = reconstruct(bk, bk_img)
        except ValueError as e:
            print(f"  !! {os.path.basename(bk)}: reconstruction failed: {e}")
            continue
        if verdict != "ok":
            print(f"  !! {os.path.basename(bk)}: integrity: {verdict} — skipped")
            continue
        stats = merge_backup(live_img, bk_img)
        print(f"  merged {os.path.basename(bk)}: +{stats}")

    db = sqlite3.connect(live_img)
    try:
        verdict = db.execute("PRAGMA integrity_check").fetchone()[0]
        total = db.execute("SELECT COUNT(*) FROM messages").fetchone()[0]
    finally:
        db.close()
    print(f"  merged result: {total} messages, integrity: {verdict}")
    if verdict != "ok":
        print("  !! merged image failed integrity — NOT applying")
        return

    if apply_changes:
        if os.path.exists(live_path):
            keep = f"{live_path}.pre-salvage.{int(time.time())}"
            shutil.copy2(live_path, keep)
            print(f"  saved {os.path.basename(keep)}")
        write_outer(live_img, live_path, dict_bytes)
        print(f"  wrote {name}")
    else:
        print("  (dry run — use --apply to write)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scrollback_dir")
    ap.add_argument("--apply", action="store_true",
                    help="write merged DBs (default: dry run)")
    args = ap.parse_args()

    by_network = {}
    for bk in glob.glob(os.path.join(args.scrollback_dir, "*.corrupt.*")):
        base = bk[:bk.index(".corrupt.")]
        by_network.setdefault(base, []).append(bk)
    if not by_network:
        print("no .corrupt backups found")
        return 0

    with tempfile.TemporaryDirectory() as workdir:
        for live_path in sorted(by_network):
            salvage_network(live_path, by_network[live_path], args.apply, workdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Test — dry run against copies of the real data**

```powershell
$scratch = "$env:LOCALAPPDATA\Temp\claude\c--Users-johne-source-repos-hexchat\7428d0ce-4298-444d-9a9b-6ede6d67f1fc\scratchpad\salvage-test"
New-Item -ItemType Directory -Force $scratch | Out-Null
Copy-Item 'C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\config\scrollback\*' $scratch
python tools\scrollback-salvage.py $scratch
```
Expected: every backup reconstructs with `integrity: ok` (the design audit predicts 11/11 — the pages are intact, only the geometry was stale); merged results report `integrity: ok` with message counts strictly greater than the live DBs alone (Fractal should gain thousands of rows). Any `!!` line = investigate before proceeding.

Note: if the app is running it holds `Fractal.db` exclusively — copy may fail or the copy may include a hot `-journal`. Close the app first; the rw open in `read_outer` recovers a copied journal.

- [ ] **Step 3: Test — apply on the copies, then verify via the C harness**

```powershell
python tools\scrollback-salvage.py $scratch --apply
$env:PATH = "c:\gtk-build\gtk4\x64\release\bin;$env:PATH"
Get-ChildItem $scratch -Filter *.db | ForEach-Object {
  & tools\out\zstd-vfs-test.exe $_.FullName check
  if ($LASTEXITCODE -ne 0) { throw "salvaged $($_.Name): check exit $LASTEXITCODE" }
}
```
Expected: every salvaged `.db` passes `quick_check` **through the real VFS** (exit 0; the `rows:` line is absent since scrollback DBs have no table `t` — that's fine). This proves the round-trip (decompress → merge → recompress) produces files the app will accept.

- [ ] **Step 4: Commit**

```bash
git add tools/scrollback-salvage.py
git commit -m "tools: scrollback salvage — recover .corrupt backups

Reconstructs the compressed outer format outside the app, merges
backup rows into the live DB (msgid dedupe, channel_id remap,
pre-ALTER schema tolerated), re-compresses, and verifies with
integrity_check. Dry-run by default; --apply keeps a .pre-salvage
copy."
```

---

### Task 6: Integration — full build, live app kill test, real salvage

**Files:** none new (verification + user-facing recovery)

- [ ] **Step 1: Full solution build** (if not already green from Task 4 step 3) — same MSBuild command and filter; expected: no real errors.

- [ ] **Step 2: App-level kill test** (needs the user or a supervised run; the app is GUI):
  1. Close PoxChat if running. Launch `C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\poxchat.exe`, connect, let some scrollback accumulate (or replay chathistory).
  2. `Stop-Process -Name poxchat -Force`.
  3. Relaunch. Expected: no new `.corrupt.*` file appears in `config\scrollback\`, history is present, and the console/debug output may show the `self-healed` message on first open.
  4. Repeat once more for confidence.

- [ ] **Step 3: Real salvage — CONFIRM WITH THE USER before `--apply` on live data.** With the app closed:

```powershell
python tools\scrollback-salvage.py 'C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\config\scrollback'          # dry run, show the user
python tools\scrollback-salvage.py 'C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\config\scrollback' --apply  # only after user OK
```
Then launch the app and confirm the recovered history renders (Fractal especially). The `.corrupt.*` and `.pre-salvage.*` files are left in place; suggest the user delete them after a few days of confidence.

- [ ] **Step 4: Record the outcome**

Add a memory note (auto-memory `project_` file) recording: the root cause, that the fix landed, the salvage tool location, and that `.corrupt`/`.pre-salvage` files can be cleaned up later. Update `MEMORY.md` index accordingly.

- [ ] **Step 5: Final commit** (if anything changed in step 4 is repo-tracked, e.g. skill-file learnings) and run the `superpowers:requesting-code-review` flow for the branch.

---

## Self-Review Notes

- **Spec coverage:** design §1 (geometry) → Task 2; §2 (read errors) → Task 3 step 3; §3 (transaction discipline) → Task 3 steps 1–2; §4 (tri-state recovery) → Task 4; §5 (salvage) → Task 5; §6 (testing: kill/stale-meta/busy/salvage) → Task 1 suite + Task 5 steps 2–3 + Task 6 step 2. The design's "stale-meta regression" test is covered by the kill leg (a kill *produces* the stale-meta DB; `geom` proves it).
- **Type consistency:** `begin_outer`/`commit_outer` defined in Task 3 and used only there; `meta_page_count_saved` defined in Task 2, consumed in Task 3; `sb_integrity` defined and consumed in Task 4; harness exit codes consistent between Task 1 and Task 5 step 3.
- **Known judgment calls:** busy-check passes even before Task 3 (open-time busy already fails cleanly); the suite's value for Task 3 is regression coverage plus the deterministic kill leg. The deep read-path BUSY race (lock taken *after* a successful open) is not deterministically reachable from a process-level harness; it is covered by code inspection and the honest-error change itself.
