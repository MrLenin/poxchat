# Chathistory Gap Fill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Track holes in stored IRC history (offline periods, capped catchups, salvage-era losses) in a persistent per-network gap ledger, render them as in-buffer markers, and fill them from the server via CHATHISTORY as the user scrolls near them.

**Architecture:** A new `gaps` table in the per-network scrollback SQLite DB is the single source of truth. chathistory.c owns the ledger's lifecycle (record on reconnect, shrink per replay batch, eager bounded close, dead-marking); xtext renders gap records as day-separator-style ephemeral entries and fires a proximity callback that requests `CHATHISTORY BETWEEN` fills. Mid-history splicing needs no migration: the DB already orders by `(timestamp, id)` everywhere.

**Tech Stack:** C11, GLib, SQLite (via the project's zstd VFS), GTK4 xtext widget, MSBuild (VS2022) on Windows.

**Spec:** `docs/design/2026-08-14-chathistory-gap-fill.md` — read it first; this plan argues from it. Section references (§N) below point into that spec.

## Global Constraints

- C11 everywhere — mixed declarations are fine; match the file's existing style (tabs, `module_action()` naming, GLib allocators).
- Debug tracing must not use `g_print`/`g_warning`/`g_message` (invisible on Windows GUI builds). Use the existing `XT_PERF`/`XT_DBG` macros (xtext.c) or `poxchat_timing_log` (common). `g_warning` in *error paths* of scrollback.c is the existing convention and stays.
- Scrollback DB rules: never issue `journal_mode`/`synchronous` pragmas on the inner handle (the zstd VFS owns durability); `sqlite3_reset` **before** binding, never leave a stepped SELECT un-reset; `SQLITE_TRANSIENT` for text binds; writes check `rc == SQLITE_DONE`, existence checks `rc == SQLITE_ROW`.
- **Every schema change must be mirrored in `tools/scrollback-salvage.py` `migrate_image()`** (it reimplements `init_database`'s migrations; unmirrored schema breaks salvage of old backups).
- New prefs touch three places: `poxchat.h` prefs struct, `cfgfiles.c` registration table, `cfgfiles.c` defaults block.
- New `fe_*` hooks touch three places: `fe.h` decl, `fe-gtk/fe-gtk.c` impl, `src/fe-text/fe-text.c` stub.
- `timestamp=` CHATHISTORY references use unix epoch (`%" G_GINT64_FORMAT "`), matching `send_deferred_latest` — do not "fix" to ISO8601 in this work.
- Build (Git-Bash from repo root; see `.claude/skills/windows-build.md` for interpretation — jansson/lua/cffi/fe-text failures are environmental noise):

```bash
MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSBUILD" win32/poxchat.sln //p:Configuration=Release //p:Platform=x64 //p:PreferredToolArchitecture=x64 //v:minimal //nologo 2>&1 \
  | grep -E "error C[0-9]|error [A-Z]" | grep -v "jansson\|lua.h\|cffi\|MSB3073\|MSB3027\|MSB3021"
```
  Empty output = build success. Exe lands at `C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\poxchat.exe`.
- Commits are GPG-signed; in long sessions the pinentry cache expires and `git commit` hangs on a GUI prompt — if that happens, kill stray `pinentry.exe` and ask the user before committing unsigned.
- Line numbers below are from the 2026-08-17 tree and drift; each step gives a grep anchor. Trust the anchor, not the number.

---

### Task 1: `mat_first_index` batch-end resync (spec §8)

Standalone hardening that de-risks every later task. `mat_first_index` is maintained by ±1 increments with no correction path; mid-history splices will stress it. `entry_id == DB rowid` for DB-backed entries, so an exact re-derivation is one indexed COUNT — same trust model as the `total_entries` resync that already runs at batch end.

**Files:**
- Modify: `src/fe-gtk/xtext.c` (~7232, inside `gtk_xtext_calc_lines_virtual_ex`; anchor: `DB-authoritative: total_entries`)

**Interfaces:**
- Consumes: `scrollback_get_index_of_rowid (scrollback_db *db, const char *channel, gint64 rowid)` (existing, scrollback.h:309).
- Produces: nothing new — behavioral fix only. Later tasks rely on `mat_first_index` being self-healing after every batch.

- [ ] **Step 1: Implement the re-derivation**

In `gtk_xtext_calc_lines_virtual_ex`, the existing resync block reads:

```c
	/* DB-authoritative: total_entries comes from the database, not
	 * incremental counting.  This eliminates drift from INSERT OR IGNORE
	 * rejecting duplicates while insert functions no longer increment. */
	if (HAS_VIRT_DB (buf) && buf->virt_channel)
	{
		int db_total = scrollback_count (buf->virt_db, buf->virt_channel);
		buf->total_entries = db_total;
	}
```

Extend it (inside the same `if`) to:

```c
	if (HAS_VIRT_DB (buf) && buf->virt_channel)
	{
		int db_total = scrollback_count (buf->virt_db, buf->virt_channel);
		buf->total_entries = db_total;

		/* Same trust model for mat_first_index: the ±1 incremental
		 * maintenance drifts when chathistory splices rows into the
		 * middle of history (a back-dated insert shifts every later
		 * ordinal).  entry_id == DB rowid for DB-backed entries, so an
		 * exact re-derivation is one indexed COUNT. */
		{
			textentry *first_db = buf->text_first;
			while (first_db && !first_db->has_db_row)
				first_db = first_db->next;
			if (first_db)
			{
				int derived = scrollback_get_index_of_rowid (buf->virt_db,
					buf->virt_channel, (gint64) first_db->entry_id);
				XT_PERF ("mat_first resync: incr=%d derived=%d%s",
				         buf->mat_first_index, derived,
				         buf->mat_first_index != derived ? " DRIFT" : "");
				buf->mat_first_index = derived;
			}
		}
	}
```

Notes: the ephemeral-skip walk is necessary — `text_first` can be a reply-context/notice ephemeral (day separators are barred from `text_first` but other ephemerals are not). `XT_PERF` compiles away when `XTEXT_VIRT_PERF_LOG` is 0; leave it in as the drift assert.

- [ ] **Step 2: Build**

Run the Global Constraints build command. Expected: no filtered error lines.

- [ ] **Step 3: Manual sanity note for the commit message**

This function runs at every batch end and every `ensure_range` recompute; the added cost is one indexed COUNT (~0.5 ms at 187k rows). Include in the commit message: verification plan = flip `XTEXT_VIRT_PERF_LOG` to 1, scroll through a large channel during a chathistory replay, confirm `DRIFT` never appears after the first resync.

- [ ] **Step 4: Commit**

```bash
git add src/fe-gtk/xtext.c
git commit -m "xtext: re-derive mat_first_index at batch end (self-healing vs splice drift)"
```

---

### Task 2: Gap ledger — schema, API, salvage mirror, TDD harness (spec §3)

The persistent `gaps` table plus its C API, with a standalone test harness. scrollback.c's only app dependencies are `get_xdir()` and `poxchat_timing_log()`, so it links into a test exe with two stubs (verified 2026-08-17).

**Files:**
- Modify: `src/common/scrollback.c` (schema in `init_database` ~166-259; new API section after `scrollback_get_index_of_rowid` ~1786)
- Modify: `src/common/scrollback.h` (new banner section after the virtual-scrollback section, before transactions ~332)
- Modify: `tools/scrollback-salvage.py` (`migrate_image()` ~184-248)
- Create: `tools/gap-ledger-test.c`
- Create: `tools/build-gap-test.ps1`
- Create: `tools/run-gap-tests.ps1`

**Interfaces:**
- Consumes: `scrollback_open (const char *network)`, `scrollback_db_save (db, channel, time_t timestamp, const char *msgid, const char *text, gboolean is_user_msg)` → rowid, `scrollback_get_channel_id` (static, internal), transaction helpers.
- Produces (all declared in scrollback.h; every later task consumes these exact signatures):

```c
#define SCROLLBACK_GAP_WITNESSED 0
#define SCROLLBACK_GAP_CANDIDATE 1
#define SCROLLBACK_GAP_DEAD      2

typedef struct {
	gint64 id;
	gint64 start_ts;      /* newest stored msg BEFORE the hole (exclusive bound) */
	char *start_msgid;    /* NULL when the flanking row has no msgid */
	gint64 end_ts;        /* oldest stored msg AFTER the hole (exclusive bound) */
	char *end_msgid;
	int state;            /* SCROLLBACK_GAP_* */
	int attempts;
	gint64 last_attempt;  /* unix time of last fill attempt (0 = never) */
} scrollback_gap;

gint64 scrollback_gap_record (scrollback_db *db, const char *channel,
                              gint64 start_ts, const char *start_msgid,
                              gint64 end_ts, const char *end_msgid, int state);
GList *scrollback_gap_list (scrollback_db *db, const char *channel);
void scrollback_gap_list_free (GList *gaps);
gboolean scrollback_gap_get (scrollback_db *db, gint64 gap_id, scrollback_gap *out);
void scrollback_gap_clear (scrollback_gap *gap);
void scrollback_gap_shrink (scrollback_db *db, gint64 gap_id,
                            gint64 new_start_ts, const char *new_start_msgid,
                            gint64 new_end_ts, const char *new_end_msgid);
void scrollback_gap_set_state (scrollback_db *db, gint64 gap_id, int state);
int scrollback_gap_touch (scrollback_db *db, gint64 gap_id);
void scrollback_gap_delete (scrollback_db *db, gint64 gap_id);
int scrollback_gap_ordinal (scrollback_db *db, const char *channel, gint64 end_ts);
```

Semantics contract (put in the Doxygen comments):
- `gap_record`: merges with any overlapping-or-touching **non-dead** gap of the same channel (union of bounds, msgids taken from whichever row contributes each bound, resulting state = MIN of merged states so witnessed absorbs candidate); returns the surviving row id, or -1 on failure. Whole operation inside `scrollback_begin_transaction`/`commit` (ref-counted, nests safely).
- `gap_shrink`: parameter value 0/NULL means "keep this side unchanged". Resets `attempts` and `last_attempt` to 0 (progress re-earns fast retry).
- `gap_touch`: `attempts+1`, `last_attempt = now`; returns the **new** attempts value.
- `gap_get`: fills `out` with strdup'd msgids; caller frees with `scrollback_gap_clear` (frees the msgids only, not the struct).
- `gap_list`: all states including dead (consumers filter), ordered by `start_ts`; elements freed by `scrollback_gap_list_free`.
- `gap_ordinal`: `COUNT(*) FROM messages WHERE channel_id=? AND timestamp < end_ts` — the gap's position in the same `(timestamp, id)` ordinal space `scrollback_load_range` uses (ties at `end_ts` are the end-flanking row itself and belong after the gap; off-by-tie is irrelevant for a proximity margin).

- [ ] **Step 1: Write the harness and the failing test**

`tools/gap-ledger-test.c` — standalone exe, modeled on `tools/zstd-vfs-test.c`'s command style. It links the real `scrollback.c` (schema, migrations, VFS included) and stubs the two app symbols:

```c
/* Gap-ledger test harness.  Links the real scrollback.c against a
 * scratch directory; asserts print PASS/FAIL and set the exit code. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "scrollback.h"

/* --- app stubs (scrollback.c's only outside deps) --- */
static const char *test_xdir = ".";
char *get_xdir (void) { return (char *) test_xdir; }
void poxchat_timing_log (const char *fmt, ...) { (void) fmt; }

static int failures = 0;
#define CHECK(name, cond) do { \
	if (cond) printf ("PASS: %s\n", name); \
	else { printf ("FAIL: %s\n", name); failures++; } \
} while (0)

static scrollback_gap *
nth_gap (GList *l, int n) { return (scrollback_gap *) g_list_nth_data (l, n); }

int
main (int argc, char **argv)
{
	scrollback_db *db;

	if (argc > 1)
		test_xdir = argv[1];	/* scratch dir; harness creates <dir>/scrollback/ */

	db = scrollback_open ("gaptest");
	if (!db) { printf ("FAIL: open\n"); return 1; }

	/* seed messages: 1000..1100 contiguous, hole, 90000..90100 */
	scrollback_begin_transaction (db);
	for (int i = 0; i <= 10; i++)
		scrollback_db_save (db, "#t", 1000 + i * 10, NULL, "early", TRUE);
	for (int i = 0; i <= 10; i++)
		scrollback_db_save (db, "#t", 90000 + i * 10, NULL, "late", TRUE);
	scrollback_commit_transaction (db);

	/* record + list roundtrip */
	gint64 id1 = scrollback_gap_record (db, "#t", 1100, "m-start", 90000, "m-end",
	                                    SCROLLBACK_GAP_WITNESSED);
	CHECK ("record returns id", id1 > 0);
	{
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("list count 1", g_list_length (l) == 1);
		CHECK ("list bounds", nth_gap (l, 0)->start_ts == 1100 &&
		                      nth_gap (l, 0)->end_ts == 90000);
		CHECK ("list msgids", g_strcmp0 (nth_gap (l, 0)->start_msgid, "m-start") == 0 &&
		                      g_strcmp0 (nth_gap (l, 0)->end_msgid, "m-end") == 0);
		scrollback_gap_list_free (l);
	}

	/* merge on overlap: overlapping candidate folds into the witnessed gap */
	gint64 id2 = scrollback_gap_record (db, "#t", 500, NULL, 2000, NULL,
	                                    SCROLLBACK_GAP_CANDIDATE);
	{
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("merge count 1", g_list_length (l) == 1);
		CHECK ("merge union", nth_gap (l, 0)->start_ts == 500 &&
		                      nth_gap (l, 0)->end_ts == 90000);
		CHECK ("merge keeps witnessed", nth_gap (l, 0)->state == SCROLLBACK_GAP_WITNESSED);
		CHECK ("merge keeps end msgid", g_strcmp0 (nth_gap (l, 0)->end_msgid, "m-end") == 0);
		id2 = nth_gap (l, 0)->id;
		scrollback_gap_list_free (l);
	}

	/* touch increments and returns; shrink resets */
	CHECK ("touch 1", scrollback_gap_touch (db, id2) == 1);
	CHECK ("touch 2", scrollback_gap_touch (db, id2) == 2);
	scrollback_gap_shrink (db, id2, 0, NULL, 50000, "m-newend");
	{
		scrollback_gap g;
		CHECK ("get after shrink", scrollback_gap_get (db, id2, &g));
		CHECK ("shrink end moved", g.end_ts == 50000 &&
		                           g_strcmp0 (g.end_msgid, "m-newend") == 0);
		CHECK ("shrink kept start", g.start_ts == 500);
		CHECK ("shrink reset attempts", g.attempts == 0 && g.last_attempt == 0);
		scrollback_gap_clear (&g);
	}

	/* state transitions; dead gaps don't merge */
	scrollback_gap_set_state (db, id2, SCROLLBACK_GAP_DEAD);
	{
		gint64 id3 = scrollback_gap_record (db, "#t", 400, NULL, 60000, NULL,
		                                    SCROLLBACK_GAP_WITNESSED);
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("dead not merged", g_list_length (l) == 2 && id3 > 0 && id3 != id2);
		scrollback_gap_list_free (l);
		scrollback_gap_delete (db, id3);
	}

	/* ordinal: 11 rows sort before ts 90000 */
	CHECK ("ordinal", scrollback_gap_ordinal (db, "#t", 90000) == 11);

	printf (failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n", failures);
	return failures ? 1 : 0;
}
```

`tools/build-gap-test.ps1` — clone of `build-vfs-test.ps1` with the extra sources and the generated `config.h` include dir:

```powershell
# Builds tools\out\gap-ledger-test.exe (x64). Requires VS 2022 + gvsbuild deps
# and a prior solution build (for the generated config.h).
$ErrorActionPreference = 'Stop'
$devShell = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' | Select-Object -First 1
if (-not $devShell) { throw 'VS 2022 Launch-VsDevShell.ps1 not found' }
$repo = Split-Path $PSScriptRoot -Parent
& $devShell.FullName -Arch amd64 -SkipAutomaticLocation | Out-Null
$deps = 'c:\gtk-build\gtk4\x64\release'
$cfg = Join-Path $repo '..\poxchat-build-gtk4\x64\lib'
if (-not (Test-Path (Join-Path $cfg 'config.h'))) { throw "generated config.h not found at $cfg - build the solution first" }
$out = Join-Path $repo 'tools\out'
New-Item -ItemType Directory -Force $out | Out-Null
cl /nologo /O1 /W3 /MD /D_CRT_SECURE_NO_WARNINGS `
  "/I$cfg" "/I$repo\src\common" "/I$repo\src\common\zstd" `
  "/I$deps\include" "/I$deps\include\glib-2.0" "/I$deps\lib\glib-2.0\include" "/I$deps\include\gio-win32-2.0" `
  "$repo\tools\gap-ledger-test.c" "$repo\src\common\scrollback.c" "$repo\src\common\sqlite-zstd-vfs.c" "$repo\src\common\zstd\zstd.c" `
  /Fo"$out\" /Fe"$out\gap-ledger-test.exe" `
  /link "/LIBPATH:$deps\lib" sqlite3.lib glib-2.0.lib gio-2.0.lib gobject-2.0.lib intl.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Write-Host "built $out\gap-ledger-test.exe"
```

(scrollback.c includes `<gio/gio.h>`, hence the gio include/libs. If the link stage reports other unresolved app symbols, stub them in gap-ledger-test.c next to `get_xdir` rather than pulling in more of the app — and note each in the commit message.)

`tools/run-gap-tests.ps1`:

```powershell
# Runs the gap-ledger test harness in a scratch dir.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo 'tools\out\gap-ledger-test.exe'
if (-not (Test-Path $exe)) { throw 'build first: pwsh tools\build-gap-test.ps1' }
$deps = 'c:\gtk-build\gtk4\x64\release'
$env:PATH = "$deps\bin;$env:PATH"
$scratch = Join-Path $env:TEMP ("gap-test-{0}" -f [guid]::NewGuid())
New-Item -ItemType Directory -Force $scratch | Out-Null
& $exe $scratch
$code = $LASTEXITCODE
Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "gap-ledger tests FAILED ($code)" }
Write-Host 'gap-ledger tests PASS'
```

- [ ] **Step 2: Run to verify it fails**

```
pwsh tools\build-gap-test.ps1
```
Expected: FAIL — unresolved externals / undeclared identifiers for `scrollback_gap_record` etc. (the API doesn't exist yet). That is this task's red state.

- [ ] **Step 3: Implement schema + migrations**

In `init_database` (scrollback.c), after the `channels` create (anchor: `Channel name normalization table`), add two tolerated execs following the existing pattern:

```c
	/* Gap ledger: recorded holes in stored history (chathistory gap fill).
	 * Bounds are anchored to real stored rows, exclusive on both ends. */
	sqlite3_exec (sdb->db,
		"CREATE TABLE IF NOT EXISTS gaps ("
		"    id INTEGER PRIMARY KEY,"
		"    channel_id INTEGER NOT NULL REFERENCES channels(id),"
		"    start_ts INTEGER NOT NULL,"
		"    start_msgid TEXT,"
		"    end_ts INTEGER NOT NULL,"
		"    end_msgid TEXT,"
		"    state INTEGER NOT NULL DEFAULT 0,"
		"    attempts INTEGER NOT NULL DEFAULT 0,"
		"    last_attempt INTEGER NOT NULL DEFAULT 0"
		");",
		NULL, NULL, NULL);
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_gaps_channel ON gaps(channel_id, start_ts);",
		NULL, NULL, NULL);
	/* One-shot bootstrap latch per channel (heuristic candidate scan) */
	sqlite3_exec (sdb->db,
		"ALTER TABLE channels ADD COLUMN gap_bootstrap_done INTEGER NOT NULL DEFAULT 0;",
		NULL, NULL, NULL);
```

- [ ] **Step 4: Implement the API**

New section in scrollback.c after `scrollback_get_index_of_rowid` (banner comment `/* --- Gap ledger (chathistory gap fill) --- */`). Use one-off prepared/finalized statements throughout (the `scrollback_get_rowid_by_msgid` "rare query" idiom) except `stmt_gap_list` and `stmt_gap_ordinal`, which join the prepared set in `scrollback_db` / `prepare_statements` / `finalize_statements`:

```c
	/* Gap ledger */
	sqlite3_stmt *stmt_gap_list;
	sqlite3_stmt *stmt_gap_ordinal;
```

```c
	/* Gap ledger: all gaps for a channel, chronological */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, start_ts, start_msgid, end_ts, end_msgid, state, "
		"attempts, last_attempt FROM gaps WHERE channel_id = ? "
		"ORDER BY start_ts ASC",
		-1, &sdb->stmt_gap_list, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Gap ledger: ordinal of a gap's end bound in load_range order */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?1 AND timestamp < ?2",
		-1, &sdb->stmt_gap_ordinal, NULL);
	if (rc != SQLITE_OK) goto fail;
```

Core implementations (the rest follow the same one-off shape — `sqlite3_prepare_v2`, bind, step, finalize):

```c
gint64
scrollback_gap_record (scrollback_db *db, const char *channel,
                       gint64 start_ts, const char *start_msgid,
                       gint64 end_ts, const char *end_msgid, int state)
{
	gint64 channel_id, result = -1;
	gint64 u_start = start_ts, u_end = end_ts;
	char *u_start_msgid = g_strdup (start_msgid);
	char *u_end_msgid = g_strdup (end_msgid);
	int u_state = state;
	sqlite3_stmt *stmt = NULL;

	if (!db || !channel || start_ts <= 0 || end_ts <= start_ts)
		goto out;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		goto out;

	scrollback_begin_transaction (db);

	/* Fold every overlapping-or-touching live gap into the union */
	if (sqlite3_prepare_v2 (db->db,
		"SELECT id, start_ts, start_msgid, end_ts, end_msgid, state FROM gaps "
		"WHERE channel_id = ?1 AND state != 2 AND start_ts <= ?2 AND end_ts >= ?3",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_bind_int64 (stmt, 2, end_ts);
		sqlite3_bind_int64 (stmt, 3, start_ts);
		while (sqlite3_step (stmt) == SQLITE_ROW)
		{
			gint64 o_start = sqlite3_column_int64 (stmt, 1);
			gint64 o_end = sqlite3_column_int64 (stmt, 3);
			int o_state = sqlite3_column_int (stmt, 5);
			if (o_start < u_start)
			{
				u_start = o_start;
				g_free (u_start_msgid);
				u_start_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
			}
			if (o_end > u_end)
			{
				u_end = o_end;
				g_free (u_end_msgid);
				u_end_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 4));
			}
			if (o_state < u_state)
				u_state = o_state;	/* witnessed (0) absorbs candidate (1) */
			{
				char *del = sqlite3_mprintf (
					"DELETE FROM gaps WHERE id = %lld",
					(long long) sqlite3_column_int64 (stmt, 0));
				sqlite3_exec (db->db, del, NULL, NULL, NULL);
				sqlite3_free (del);
			}
		}
		sqlite3_finalize (stmt);
		stmt = NULL;
	}

	if (sqlite3_prepare_v2 (db->db,
		"INSERT INTO gaps (channel_id, start_ts, start_msgid, end_ts, end_msgid, state) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_bind_int64 (stmt, 2, u_start);
		if (u_start_msgid)
			sqlite3_bind_text (stmt, 3, u_start_msgid, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null (stmt, 3);
		sqlite3_bind_int64 (stmt, 4, u_end);
		if (u_end_msgid)
			sqlite3_bind_text (stmt, 5, u_end_msgid, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null (stmt, 5);
		sqlite3_bind_int (stmt, 6, u_state);
		if (sqlite3_step (stmt) == SQLITE_DONE)
			result = sqlite3_last_insert_rowid (db->db);
		sqlite3_finalize (stmt);
		stmt = NULL;
	}

	scrollback_commit_transaction (db);
out:
	if (stmt)
		sqlite3_finalize (stmt);
	g_free (u_start_msgid);
	g_free (u_end_msgid);
	return result;
}
```

`scrollback_gap_shrink` (0/NULL = keep; resets backoff):

```c
void
scrollback_gap_shrink (scrollback_db *db, gint64 gap_id,
                       gint64 new_start_ts, const char *new_start_msgid,
                       gint64 new_end_ts, const char *new_end_msgid)
{
	sqlite3_stmt *stmt;
	if (!db || gap_id <= 0)
		return;
	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET "
		"start_ts = CASE WHEN ?2 > 0 THEN ?2 ELSE start_ts END, "
		"start_msgid = CASE WHEN ?2 > 0 THEN ?3 ELSE start_msgid END, "
		"end_ts = CASE WHEN ?4 > 0 THEN ?4 ELSE end_ts END, "
		"end_msgid = CASE WHEN ?4 > 0 THEN ?5 ELSE end_msgid END, "
		"attempts = 0, last_attempt = 0 "
		"WHERE id = ?1",
		-1, &stmt, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_int64 (stmt, 1, gap_id);
	sqlite3_bind_int64 (stmt, 2, new_start_ts);
	if (new_start_msgid)
		sqlite3_bind_text (stmt, 3, new_start_msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (stmt, 3);
	sqlite3_bind_int64 (stmt, 4, new_end_ts);
	if (new_end_msgid)
		sqlite3_bind_text (stmt, 5, new_end_msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (stmt, 5);
	sqlite3_step (stmt);
	sqlite3_finalize (stmt);
}
```

`scrollback_gap_touch` (one-off `UPDATE gaps SET attempts = attempts + 1, last_attempt = ?now WHERE id = ?` followed by one-off `SELECT attempts`, returning it; `now` bound from `time (NULL)` at the call). `scrollback_gap_set_state`, `scrollback_gap_delete`: single one-off UPDATE/DELETE. `scrollback_gap_get`: one-off SELECT by id filling the struct (strdup text columns; absent row → FALSE). `scrollback_gap_clear`: `g_free` both msgids, NULL them. `scrollback_gap_list`: reset/bind `stmt_gap_list`, loop rows into `g_new0 (scrollback_gap, 1)` appended to a GList; after the loop `g_warning` on `rc != SQLITE_DONE` per convention. `scrollback_gap_list_free`: `g_list_free_full` with a small free func doing `scrollback_gap_clear` + `g_free`. `scrollback_gap_ordinal`: reset/bind `stmt_gap_ordinal` (channel_id, end_ts), step, return count.

Header: new banner `/* --- Gap ledger (chathistory gap fill) --- */` in scrollback.h after line ~330 (before the transaction helpers), containing the defines, the `scrollback_gap` struct (record-struct precedent: `scrollback_reaction`/`scrollback_reply`), and Doxygen'd decls exactly as in **Produces** above.

- [ ] **Step 5: Mirror in the salvage tool**

In `tools/scrollback-salvage.py` `migrate_image()`, add after the existing index migrations (mirror the schema verbatim; the tool probes columns with `table_cols()`):

```python
    # Gap ledger (chathistory gap fill)
    cur.execute("""CREATE TABLE IF NOT EXISTS gaps (
        id INTEGER PRIMARY KEY,
        channel_id INTEGER NOT NULL REFERENCES channels(id),
        start_ts INTEGER NOT NULL,
        start_msgid TEXT,
        end_ts INTEGER NOT NULL,
        end_msgid TEXT,
        state INTEGER NOT NULL DEFAULT 0,
        attempts INTEGER NOT NULL DEFAULT 0,
        last_attempt INTEGER NOT NULL DEFAULT 0
    )""")
    cur.execute("CREATE INDEX IF NOT EXISTS idx_gaps_channel ON gaps(channel_id, start_ts)")
    if "gap_bootstrap_done" not in table_cols(cur, "channels"):
        cur.execute("ALTER TABLE channels ADD COLUMN gap_bootstrap_done INTEGER NOT NULL DEFAULT 0")
```

- [ ] **Step 6: Build the harness and run to green**

```
pwsh tools\build-gap-test.ps1
pwsh tools\run-gap-tests.ps1
```
Expected: `RESULT: ALL PASS`, script prints `gap-ledger tests PASS`.

- [ ] **Step 7: Full solution build**

Run the Global Constraints build command; expect no filtered errors (scrollback.c changed).

- [ ] **Step 8: Commit**

```bash
git add src/common/scrollback.c src/common/scrollback.h tools/scrollback-salvage.py tools/gap-ledger-test.c tools/build-gap-test.ps1 tools/run-gap-tests.ps1
git commit -m "scrollback: gap ledger table + API with standalone test harness"
```

---

### Task 3: Reconnect witness recording + prefs (spec §4 creation)

> **Amendment 2026-08-28 (spec §13):** Nefarious can also deliver reconnect
> history *unsolicited* — `chathistory`-type batches nested in an
> `evilnet.github.io/bouncer-replay` wrapper (when a `draft/persistence`
> `attach-cursor` client attaches; we do not send that yet, but the batch shape
> must not corrupt state if it ever arrives). In `chathistory_process_batch`,
> classify a `chathistory` batch whose `outer_batch` resolves to a
> `bouncer-replay` wrapper, or that matches no in-flight request for its target,
> as a LATEST-phase result: run the witness check and gap recording below, and
> do **not** call `chathistory_request_complete` for it. Add a `bouncer-replay`
> case to `inbound_batch_end` that is a pure container; its END is the single
> "replay complete" point for `chathistory_check_before_catchup`. Add a test
> line for this in the Task 3 manual checklist.

Record the offline hole as a `witnessed` gap when the join-time LATEST batch lands, and let the existing BEFORE pagination shrink it. Adds the `hex_irc_gapfill` master pref and the budget pref (used in Task 4).

**Files:**
- Modify: `src/common/poxchat.h` (session struct ~475; prefs struct `unsigned int` block ~211 and `int` block ~297)
- Modify: `src/common/cfgfiles.c` (registration ~536; defaults ~838)
- Modify: `src/common/chathistory.c` (`send_deferred_latest` ~1841; `finish_batch_processing` ~1217; `finish_catchup` ~501)
- Modify: `src/common/poxchat.c` (session free ~696)

**Interfaces:**
- Consumes: `scrollback_gap_record`, `scrollback_gap_shrink`, `scrollback_gap_delete`, `scrollback_gap_set_state` (Task 2); `scrollback_get_newest_msgid` / `scrollback_get_newest_time` (existing); `scrollback_open`, `server_get_network` (existing).
- Produces: session fields `gint64 catchup_gap_id`, `time_t catchup_prev_newest_time`, `char *catchup_prev_newest_msgid`; prefs `hex_irc_gapfill` (bool, default 1), `hex_irc_gapfill_catchup_budget` (int, default 500). Task 4 consumes the budget; Tasks 6-8 consume `hex_irc_gapfill`.

- [ ] **Step 1: Add prefs (three places)**

`poxchat.h` prefs struct — in the `unsigned int` block next to the other `hex_irc_chathistory_*` entries:
```c
	unsigned int hex_irc_gapfill;  /* gap ledger: markers + scroll fill + eager close */
```
in the `int` block (alphabetical near `hex_irc_chathistory_lines`):
```c
	int hex_irc_gapfill_catchup_budget;  /* eager-close messages per channel per reconnect */
```
`cfgfiles.c` registration table, after the `irc_chathistory_*` rows:
```c
	{"irc_gapfill", P_OFFINT (hex_irc_gapfill), TYPE_BOOL},
	{"irc_gapfill_catchup_budget", P_OFFINT (hex_irc_gapfill_catchup_budget), TYPE_INT},
```
`cfgfiles.c` defaults, after the `hex_irc_chathistory_*` defaults:
```c
	prefs.hex_irc_gapfill = 1;  /* gap ledger + fill (default: enabled) */
	prefs.hex_irc_gapfill_catchup_budget = 500; /* eager-close budget per channel */
```

- [ ] **Step 2: Session fields + lifecycle**

`poxchat.h` session struct, next to `catchup_lower_bound` (~472):
```c
	gint64 catchup_gap_id;			/* open witnessed-gap ledger row this catchup is closing (0 = none) */
	time_t catchup_prev_newest_time;	/* newest stored row when this catchup started */
	char *catchup_prev_newest_msgid;	/* its msgid (owned; may be NULL) */
```
`poxchat.c` session free (anchor: where `scrollback_oldest_msgid`/`scrollback_newest_msgid` are freed, ~696): add `g_free (sess->catchup_prev_newest_msgid);`.

- [ ] **Step 3: Refresh the newest-stored snapshot at catchup start**

In `send_deferred_latest` (chathistory.c), immediately after the early-return guards and **before** the `catchup_lower_bound` computation, insert:

```c
	/* Refresh the newest-stored snapshot from the DB.  The session's
	 * scrollback_newest_* fields are loaded once at scrollback-load time
	 * and go stale as soon as catchup or live traffic writes newer rows —
	 * a reconnect without an app restart would otherwise anchor LATEST
	 * and the gap ledger on app-start-era values. */
	{
		const char *network = server_get_network (sess->server, FALSE);
		scrollback_db *db = network ? scrollback_open (network) : NULL;
		if (db)
		{
			g_free (sess->scrollback_newest_msgid);
			sess->scrollback_newest_msgid =
				scrollback_get_newest_msgid (db, sess->channel);
			sess->scrollback_newest_time =
				scrollback_get_newest_time (db, sess->channel);
		}
	}
	sess->catchup_prev_newest_time = sess->scrollback_newest_time;
	g_free (sess->catchup_prev_newest_msgid);
	sess->catchup_prev_newest_msgid = g_strdup (sess->scrollback_newest_msgid);
	sess->catchup_gap_id = 0;
```

(chathistory.c already includes scrollback.h — it calls `scrollback_open` in `chathistory_process_batch`.)

- [ ] **Step 4: Record the witnessed gap at LATEST batch end**

In `finish_batch_processing`, the LATEST phase currently reads (anchor: `--- Initial LATEST phase ---`):

```c
		/* --- Initial LATEST phase --- */
		if (serv->chathistory_latest_pending > 0)
			serv->chathistory_latest_pending--;
```

Insert **before** the `chathistory_latest_pending` decrement:

```c
		/* --- Initial LATEST phase --- */

		/* Gap-ledger witness: the LATEST batch landed but did not reach
		 * back to our newest stored row — the span between them is a
		 * real hole.  Record it before the BEFORE loop starts shrinking
		 * it, so an interrupted catchup leaves the truth in the ledger. */
		if (prefs.hex_irc_gapfill && chunk->raw_count > 0 &&
		    sess->catchup_prev_newest_time > 0 &&
		    chunk->oldest_timestamp > 0 &&
		    sess->catchup_lower_bound > 0 &&
		    chunk->oldest_timestamp > sess->catchup_lower_bound)
		{
			const char *network = server_get_network (serv, FALSE);
			scrollback_db *gdb = network ? scrollback_open (network) : NULL;
			if (gdb)
				sess->catchup_gap_id = scrollback_gap_record (gdb,
					sess->channel,
					sess->catchup_prev_newest_time,
					sess->catchup_prev_newest_msgid,
					chunk->oldest_timestamp,
					chunk->batch_oldest_msgid,
					SCROLLBACK_GAP_WITNESSED);
		}
```

- [ ] **Step 5: Shrink/close the record in the BEFORE branch**

Three edits inside the `--- BEFORE pagination phase ---` branch of `finish_batch_processing`:

(a) After `sess->history_catchup_retrieved += chunk->msg_count;`, add the per-batch shrink:

```c
		/* Each BEFORE batch narrows the witnessed gap from its end side */
		if (sess->catchup_gap_id > 0 && chunk->raw_count > 0 &&
		    chunk->oldest_timestamp > 0)
		{
			const char *network = server_get_network (serv, FALSE);
			scrollback_db *gdb = network ? scrollback_open (network) : NULL;
			if (gdb)
				scrollback_gap_shrink (gdb, sess->catchup_gap_id,
					0, NULL,
					chunk->oldest_timestamp, chunk->batch_oldest_msgid);
		}
```

(b) The empty/exhausted termination (`raw_count == 0 || sess->history_exhausted`) and the stale-count exhaustion (`stale_count >= 3`) both mean the server cannot reach further back — before their `finish_catchup (sess);` calls, add:

```c
				if (sess->catchup_gap_id > 0)
				{
					const char *network = server_get_network (serv, FALSE);
					scrollback_db *gdb = network ? scrollback_open (network) : NULL;
					if (gdb)
						scrollback_gap_set_state (gdb, sess->catchup_gap_id,
						                          SCROLLBACK_GAP_DEAD);
				}
```

(c) The timestamp-stop termination (`chunk->oldest_timestamp < sess->catchup_lower_bound` — gap bridged) deletes the record — before its `finish_catchup (sess);`:

```c
				if (sess->catchup_gap_id > 0)
				{
					const char *network = server_get_network (serv, FALSE);
					scrollback_db *gdb = network ? scrollback_open (network) : NULL;
					if (gdb)
						scrollback_gap_delete (gdb, sess->catchup_gap_id);
				}
```

All other exits (sanity cap, the Task 4 budget, disconnect) leave the record **open** — that is the lazy-fill inventory.

- [ ] **Step 6: Reset the handle in `finish_catchup`**

Add `sess->catchup_gap_id = 0;` next to the other resets at the top of `finish_catchup`, so post-catchup batches never touch the record again.

- [ ] **Step 7: Build + commit**

Run the build command (no filtered errors). Commit message must include the manual test plan: *reconnect after traffic accumulated in a channel (second client active); confirm with `tools/out/gap-ledger-test`-style sqlite3 inspection (`SELECT * FROM gaps`) that a small offline gap is recorded at LATEST end and deleted when the BEFORE loop bridges it; kill the connection mid-catchup and confirm the shrunken record survives in the DB.*

```bash
git add src/common/poxchat.h src/common/poxchat.c src/common/cfgfiles.c src/common/chathistory.c
git commit -m "chathistory: record reconnect gaps in the ledger; shrink via BEFORE pagination"
```

---

### Task 4: Eager bounded close for background channels (spec §4 eager)

Today the BEFORE catch-up loop runs only on the active tab (`chathistory_check_before_catchup` bails when `current_sess` doesn't need it), and a hop pauses whenever the user switches tabs. Rework the scheduler: every continuation hops through `schedule_before_catchup` → `check_before_catchup`, which re-picks the target each time (active tab first, then any session on the server), and a per-channel budget bounds each session's spend.

**Files:**
- Modify: `src/common/chathistory.c` (`chathistory_check_before_catchup` ~1957; `finish_batch_processing` BEFORE branch ~1290-1318)

**Interfaces:**
- Consumes: `prefs.hex_irc_gapfill`, `prefs.hex_irc_gapfill_catchup_budget` (Task 3); `sess->history_catchup_retrieved`, `schedule_before_catchup` (existing).
- Produces: no new symbols — scheduling behavior only.

- [ ] **Step 1: Budget rung**

In the BEFORE branch of `finish_batch_processing`, immediately **before** the existing sanity-limit check (anchor: `Sanity limit`), add:

```c
			/* Per-channel eager-close budget: beyond this, the remainder
			 * stays recorded in the gap ledger for lazy scroll-fill.
			 * CHATHISTORY_SANITY_LIMIT below stays as the outer backstop. */
			if (prefs.hex_irc_gapfill &&
			    prefs.hex_irc_gapfill_catchup_budget > 0 &&
			    sess->history_catchup_retrieved >=
			    prefs.hex_irc_gapfill_catchup_budget)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}
```

(No dead-marking here — Task 3's Step 6 zeroes `catchup_gap_id` in `finish_catchup` and the record stays open.)

- [ ] **Step 2: Replace the tab-switch pause with re-pick-per-hop**

Still in the BEFORE branch, the current tail reads (anchor: `Tab switched away`):

```c
			/* Tab switched away — pause this session, check new active */
			if (serv->chathistory_before_sess != sess ||
			    sess != current_sess)
			{
				serv->chathistory_before_sess = NULL;
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Continue BEFORE pagination after a delay */
			if (chunk->batch_oldest_msgid)
			{
				schedule_before_catchup (serv);
			}
```

Replace with:

```c
			/* Continue after a delay — every hop goes through the
			 * scheduler, which re-picks the target (active tab first,
			 * then background sessions).  That re-pick is what the
			 * explicit tab-switch pause used to accomplish. */
			if (chunk->batch_oldest_msgid)
			{
				serv->chathistory_before_sess = NULL;
				schedule_before_catchup (serv);
			}
```

- [ ] **Step 3: Extend the target pick to background sessions**

In `chathistory_check_before_catchup`, after the current-sess preference block (anchor: `Prefer current_sess`), replace the bail-out:

```c
	if (!target)
	{
		/* No active tab needs catch-up — don't start on inactive tabs.
		 * BEFORE catch-up only runs on the active tab. */
		serv->chathistory_before_sess = NULL;
		return;
	}
```

with:

```c
	/* Background channels: their residual gap used to be silently
	 * abandoned here (active-tab-only).  With the gap ledger they are
	 * closed eagerly too, one session at a time, budget-bounded. */
	if (!target && prefs.hex_irc_gapfill)
	{
		for (list = sess_list; list; list = list->next)
		{
			session *s = list->data;
			if (s->server == serv && s->catchup_in_progress &&
			    !s->history_exhausted && !s->history_loading)
			{
				target = s;
				break;
			}
		}
	}

	if (!target)
	{
		serv->chathistory_before_sess = NULL;
		return;
	}
```

(The `GSList *list;` local already exists in this function, currently unused.)

- [ ] **Step 4: Review `chathistory_notify_tab_switch`**

Read `chathistory_notify_tab_switch` (chathistory.c ~2008-2029; anchor: grep `chathistory_notify_tab_switch`). It exists to nudge the scheduler on tab switch; with re-pick-per-hop it should reduce to calling `chathistory_check_before_catchup (serv)` (possibly guarded). If it manipulates `chathistory_before_sess` to force the active tab, simplify it to the nudge — the pick logic now owns priority. Keep whatever guards (`is_server`, `connected`) it already has.

- [ ] **Step 5: Build + commit**

Build (no filtered errors). Commit message test plan: *reconnect with 11 autojoin channels after >50 messages accrued in several non-active ones; confirm via raw log (`/RAWLOG`) that BEFORE requests walk every channel needing catch-up (active first, 3 s spacing), each stops at bridge or at 500 messages, and `SELECT channel_id, state FROM gaps` shows leftovers only where the budget hit.*

```bash
git add src/common/chathistory.c
git commit -m "chathistory: eager bounded BEFORE catch-up for background channels"
```

---

### Task 5: FAIL hardening (spec §7)

Pass the FAIL code through, fix the stranded `chathistory_latest_pending`, and add the per-server latches. (Per-gap FAIL policy lands in Task 7 where gap requests exist.)

**Files:**
- Modify: `src/common/proto-irc.c` (~1362, anchor: `WORDL('F','A','I','L')`)
- Modify: `src/common/chathistory.h` (`chathistory_handle_fail` decl ~180)
- Modify: `src/common/chathistory.c` (`chathistory_handle_fail` ~1141; `finish_batch_processing` head ~1220; `chathistory_dispatch_now` ~138)
- Modify: `src/common/poxchat.h` (server struct, next to `have_chathistory` ~727)
- Modify: `src/common/inbound.c` (the branch of `inbound_toggle_caps` that sets `serv->have_chathistory`; anchor: grep `have_chathistory =`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `void chathistory_handle_fail (server *serv, const char *code, const char *context);` (signature change); server fields `unsigned int chathistory_between_unsupported:1`, `unsigned int chathistory_suppressed:1`, `int chathistory_fail_streak`. Task 7 consumes both latches.

- [ ] **Step 1: Server fields + reset on capability grant**

`poxchat.h`, next to the chathistory server fields (~651-655):
```c
	unsigned int chathistory_between_unsupported:1;	/* server FAILed BETWEEN; use BEFORE/AFTER fallback */
	unsigned int chathistory_suppressed:1;	/* repeated FAILs — stop asking until reconnect */
	int chathistory_fail_streak;			/* consecutive CHATHISTORY FAILs */
```
In `inbound.c`, where `serv->have_chathistory` is set on CAP grant (grep `have_chathistory`; it's in the cap-toggle handling), reset all three in the same statement group:
```c
				serv->chathistory_between_unsupported = FALSE;
				serv->chathistory_suppressed = FALSE;
				serv->chathistory_fail_streak = 0;
```

- [ ] **Step 2: Plumb the code through the parse site**

proto-irc.c:
```c
			if (g_ascii_strcasecmp (word[3], "CHATHISTORY") == 0)
			{
				/* word[4] = FAIL code, word[5] = context (target) */
				chathistory_handle_fail (serv, word[4], word[5]);
			}
```
chathistory.h:
```c
void chathistory_handle_fail (server *serv, const char *code, const char *context);
```

- [ ] **Step 3: Rewrite `chathistory_handle_fail`**

```c
void
chathistory_handle_fail (server *serv, const char *code, const char *context)
{
	session *sess = NULL;

	/* Try to find session from context (may be the target channel) */
	if (context && context[0])
		sess = find_session_for_target (serv, context);

	/* Fall back to finding any session with a pending history request */
	if (!sess)
		sess = find_session_with_pending_history (serv);

	if (!sess)
		return;

	{
		gboolean used_msgid = sess->history_request_used_msgid;
		gboolean was_catchup_latest = sess->catchup_in_progress &&
			!sess->catchup_is_before;
		chreq_type failed_type = sess->ch_active ? sess->ch_active->type
		                                         : CHREQ_LATEST;

		/* Repeated-FAIL brake: an auth-walled or broken server would
		 * otherwise get re-asked on every join and every scroll. */
		serv->chathistory_fail_streak++;
		if (serv->chathistory_fail_streak >= 4)
			serv->chathistory_suppressed = TRUE;

		/* BETWEEN not understood → remember; the gap-fill layer falls
		 * back to BEFORE/AFTER pagination on its next trigger. */
		if (failed_type == CHREQ_BETWEEN && code && code[0] &&
		    (g_ascii_strcasecmp (code, "UNKNOWN_COMMAND") == 0 ||
		     g_ascii_strcasecmp (code, "NEED_MORE_PARAMS") == 0 ||
		     g_ascii_strcasecmp (code, "INVALID_PARAMS") == 0 ||
		     g_ascii_strcasecmp (code, "INVALID_MSGREFTYPES") == 0))
			serv->chathistory_between_unsupported = TRUE;

		/* Clear active request and advance queue */
		chathistory_request_complete (sess);

		if (sess->catchup_in_progress)
		{
			/* A failed catchup LATEST previously left latest_pending
			 * stranded, which blocked the BEFORE phase forever. */
			if (was_catchup_latest && serv->chathistory_latest_pending > 0)
				serv->chathistory_latest_pending--;

			/* Catch-up: server rejected our reference.  If we used a
			 * msgid the server doesn't recognise, retry with timestamp. */
			if (!serv->chathistory_suppressed &&
			    used_msgid && sess->scrollback_newest_time > 0)
			{
				char ref[64];
				g_snprintf (ref, sizeof (ref), "timestamp=%" G_GINT64_FORMAT,
				            (gint64) sess->scrollback_newest_time);
				chathistory_request_latest (sess, ref,
				                            prefs.hex_irc_chathistory_lines);
				return;
			}
			/* All fallbacks exhausted — finish with whatever we have */
			finish_catchup (sess);
			if (serv->chathistory_latest_pending == 0)
				chathistory_check_before_catchup (serv);
		}
	}
}
```

Note the deliberate deviation from the spec's per-session `history_exhausted`-on-FAIL idea: the server-wide streak latch is strictly stronger against request storms and simpler; record this in the commit message.

- [ ] **Step 4: Streak reset on success + suppressed gate**

Top of `finish_batch_processing`, first line of the body: `serv->chathistory_fail_streak = 0;`

In `chathistory_dispatch_now`, extend the existing capability guard:
```c
	if (!serv->have_chathistory || !serv->connected || serv->chathistory_suppressed)
	{
		chreq_free (req);
		return;
	}
```

- [ ] **Step 5: Build + commit**

Build (no filtered errors). Commit test plan: */QUOTE CHATHISTORY BEFORE #nonexistent-target msgid=x 50 four times → raw log shows no further CHATHISTORY sends this session; reconnect clears the latch.*

```bash
git add src/common/proto-irc.c src/common/chathistory.c src/common/chathistory.h src/common/poxchat.h src/common/inbound.c
git commit -m "chathistory: FAIL-code-aware handling, latest_pending fix, per-server latches"
```

---

### Task 6: FE gap markers (spec §5)

Gap records render as day-separator-style ephemeral entries. Includes the shared comparator tie-break (chrome sorts before real entries on stamp ties) and the `fe_gap_updated` hook.

**Files:**
- Modify: `src/fe-gtk/xtext.c` (flags ~184; `entry_stamp_cmp` ~315; day-sep machinery ~430, ~6172, ~6295, ~8639, ~9841, ~9858; init_entry ~9719; consumer skips 3621/4522/4552/6919/10781; buffer new/free/set_virtual ~11366/~11457)
- Modify: `src/fe-gtk/xtext.h` (`xtext_gap_info` typedef; `xtext_buffer` fields; `gtk_xtext_refresh_gap_markers` decl)
- Modify: `src/common/fe.h` (~248), `src/fe-gtk/fe-gtk.c` (~2698 area), `src/fe-text/fe-text.c` (stub block ~964)

**Interfaces:**
- Consumes: `scrollback_gap_list` / `scrollback_gap_list_free` / `scrollback_gap_ordinal`, `SCROLLBACK_GAP_*` (Task 2; xtext.c already includes scrollback.h).
- Produces:
  - `TEXTENTRY_FLAG_GAP_MARKER 0x08`, `XTEXT_ENT_IS_GAP_MARKER(ent)`, `XTEXT_ENT_IS_CHROME(ent)` (xtext.c)
  - textentry field `gint64 gap_id;`
  - xtext.h: `typedef struct { gint64 gap_id; gint64 start_ts; gint64 end_ts; int state; int ordinal; unsigned int in_flight:1; } xtext_gap_info;`; `xtext_buffer` fields `GList *gap_cache; unsigned int gap_cache_dirty:1;`
  - `void gtk_xtext_refresh_gap_markers (xtext_buffer *buf);` (public, xtext.h)
  - `void fe_gap_updated (struct session *sess, gint64 gap_id);` (fe.h; Task 7 calls it from chathistory.c)
  - Task 7 consumes `buf->gap_cache` (with `ordinal`) for the proximity trigger and sets `in_flight`.

- [ ] **Step 1: Flags, macros, textentry field**

Next to the existing flag defines (~184):
```c
#define TEXTENTRY_FLAG_GAP_MARKER    0x08  /* entry IS a gap-marker row (recorded history hole) */
```
Below `XTEXT_ENT_IS_DAY_SEP`:
```c
#define XTEXT_ENT_IS_GAP_MARKER(ent) (((ent)->flags & TEXTENTRY_FLAG_GAP_MARKER) != 0)
/* Synthetic chrome rows (day separators, gap markers): excluded from
 * selection/save/foreach, dropped at window edges, never content. */
#define XTEXT_ENT_IS_CHROME(ent) (XTEXT_ENT_IS_DAY_SEP (ent) || XTEXT_ENT_IS_GAP_MARKER (ent))
```
In `struct textentry`, next to `group_id`:
```c
	gint64 gap_id;		/* gap-marker rows: ledger row id (0 otherwise) */
```
In `gtk_xtext_init_entry`, where `entry_id`/`group_id` are assigned, add `ent->gap_id = 0;` (entries are not uniformly zero-allocated).

- [ ] **Step 2: Comparator tie-break**

Replace `entry_stamp_cmp`'s tie handling:

```c
static int
entry_stamp_cmp (void *a, void *b)
{
	textentry *ea = (textentry *)a;
	textentry *eb = (textentry *)b;
	if (ea->stamp < eb->stamp) return -1;
	if (ea->stamp > eb->stamp) return 1;
	/* Stamp tie: synthetic chrome (day separator, gap marker) sorts
	 * before real content — the separator introduces the span the
	 * equal-stamped real entry belongs to.  This is the shared
	 * tie-break the day-sep midnight fix needs; the <= midnight
	 * creation skip in maybe_insert_day_sep is unchanged for now. */
	{
		gboolean ca = XTEXT_ENT_IS_CHROME (ea);
		gboolean cb = XTEXT_ENT_IS_CHROME (eb);
		if (ca != cb)
			return ca ? -1 : 1;
	}
	/* Same timestamp — use entry_id for stable ordering */
	if (ea->entry_id < eb->entry_id) return -1;
	if (ea->entry_id > eb->entry_id) return 1;
	return 0;
}
```

Also update the now-stale ordering comments that describe the old tie rule: the block comment above `gtk_xtext_maybe_insert_day_sep` (~9858, "would tie ... and win the id tiebreak") and the insert-hint comment in `gtk_xtext_insert_sorted_entry` (~10113, "must land BEFORE the separator"). Both should now say chrome sorts first on ties.

This matters because a gap marker's stamp **equals** its end-flanking row's stamp — the tie is the common case, and chrome-first puts the marker between start row and end row in both the tree and the list.

- [ ] **Step 3: Gap cache**

xtext.h: add the `xtext_gap_info` typedef (above `xtext_buffer`) and the two buffer fields (next to the DB-backed scrollback block). xtext.c, near the other virtual-scrollback statics:

```c
static xtext_gap_info *
xtext_find_gap_info (xtext_buffer *buf, gint64 gap_id)
{
	GList *l;
	for (l = buf->gap_cache; l; l = l->next)
		if (((xtext_gap_info *) l->data)->gap_id == gap_id)
			return l->data;
	return NULL;
}

/* Reload the gap cache from the ledger.  Dead gaps are excluded — they
 * have no marker and no fill; the ledger remembers them so the server
 * is never re-asked. */
static void
gtk_xtext_refresh_gap_cache (xtext_buffer *buf)
{
	GList *rows, *l;

	g_list_free_full (buf->gap_cache, g_free);
	buf->gap_cache = NULL;
	buf->gap_cache_dirty = FALSE;

	if (!HAS_VIRT_DB (buf) || !buf->virt_channel)
		return;

	rows = scrollback_gap_list (buf->virt_db, buf->virt_channel);
	for (l = rows; l; l = l->next)
	{
		scrollback_gap *g = l->data;
		xtext_gap_info *gi;
		if (g->state == SCROLLBACK_GAP_DEAD)
			continue;
		gi = g_new0 (xtext_gap_info, 1);
		gi->gap_id = g->id;
		gi->start_ts = g->start_ts;
		gi->end_ts = g->end_ts;
		gi->state = g->state;
		gi->ordinal = scrollback_gap_ordinal (buf->virt_db, buf->virt_channel,
		                                      g->end_ts);
		buf->gap_cache = g_list_append (buf->gap_cache, gi);
	}
	scrollback_gap_list_free (rows);
}
```

Wire the lifecycle: in `gtk_xtext_calc_lines_virtual_ex`'s resync block (Task 1's edit), after the `total_entries` assignment add — ordinals shift whenever DB content changes:
```c
		if (buf->total_entries != db_total_prev || buf->gap_cache_dirty)
			gtk_xtext_refresh_gap_cache (buf);
```
where `db_total_prev` is captured before the assignment (`int db_total_prev = buf->total_entries;` at the top of the block). In buffer free (grep `entries_by_id` destroy for the site): `g_list_free_full (buf->gap_cache, g_free);`. In `gtk_xtext_buffer_set_virtual` (after `lines_before_mat` seeding): `gtk_xtext_refresh_gap_cache (buf);` followed by `gtk_xtext_refresh_gap_markers (buf);` (Step 6 — order the code so the sweep is defined first or forward-declare).

- [ ] **Step 4: Marker creation + insertion hooks**

Below `gtk_xtext_maybe_insert_day_sep` (forward-declare next to its decl ~460):

```c
/* Insert a gap-marker entry before `ent` when a recorded (live) hole
 * falls between `ent` and its nearest real predecessor.  Same contract
 * as gtk_xtext_maybe_insert_day_sep: returns display lines added. */
static int
gtk_xtext_maybe_insert_gap_marker (xtext_buffer *buf, textentry *ent)
{
	GList *l;
	textentry *sep, *real_prev;

	if (!HAS_VIRT_DB (buf) || !buf->gap_cache)
		return 0;
	if (XTEXT_ENT_IS_CHROME (ent) || !ent->prev || ent->stamp <= 0)
		return 0;

	/* Walk back over chrome (a day separator can sit inside the hole's
	 * span); an existing marker at this boundary means we're done. */
	real_prev = ent->prev;
	while (real_prev && XTEXT_ENT_IS_CHROME (real_prev))
	{
		if (XTEXT_ENT_IS_GAP_MARKER (real_prev))
			return 0;
		real_prev = real_prev->prev;
	}
	if (!real_prev || real_prev->stamp <= 0)
		return 0;

	for (l = buf->gap_cache; l; l = l->next)
	{
		xtext_gap_info *gi = l->data;
		if (real_prev->stamp <= gi->start_ts && ent->stamp >= gi->end_ts)
		{
			sep = g_malloc0 (1 + sizeof (textentry));
			sep->str = (unsigned char *) sep + sizeof (textentry);
			sep->str_len = 0;
			sep->left_len = -1;
			sep->indent = MARGIN;

			gtk_xtext_init_entry (buf, sep, (time_t) gi->end_ts);
			sep->flags |= TEXTENTRY_FLAG_GAP_MARKER;
			sep->gap_id = gi->gap_id;
			sep->group_id = 0;
			if (!(sep->flags & TEXTENTRY_FLAG_EPHEMERAL))
			{
				sep->flags |= TEXTENTRY_FLAG_EPHEMERAL;
				buf->ephemeral_count++;
			}

			sep->prev = ent->prev;
			sep->next = ent;
			return gtk_xtext_link_entry (buf, sep, LINK_BEFORE);
		}
	}
	return 0;
}
```

Hook sites — in `gtk_xtext_link_entry`'s tail, the day-sep hook becomes:

```c
	if (!XTEXT_ENT_IS_CHROME (ent))
	{
		new_lines += gtk_xtext_maybe_insert_day_sep (buf, ent);
		new_lines += gtk_xtext_maybe_insert_gap_marker (buf, ent);
		if (ent->next && !XTEXT_ENT_IS_CHROME (ent->next))
		{
			new_lines += gtk_xtext_maybe_insert_day_sep (buf, ent->next);
			new_lines += gtk_xtext_maybe_insert_gap_marker (buf, ent->next);
		}
	}
```

Then `grep -n "maybe_insert_day_sep" src/fe-gtk/xtext.c` — at every call site **outside** `gtk_xtext_link_entry` and `gtk_xtext_recalc_day_boundaries` (the manual-link materialize paths in `ensure_range`/`recenter`, ~11859/~12000), add an adjacent `gtk_xtext_maybe_insert_gap_marker` call with the same argument and the same line-count accumulation as the day-sep call beside it.

- [ ] **Step 5: Edge drops, consumer skips, render**

In `gtk_xtext_drop_edge_day_sep`, change the test `!XTEXT_ENT_IS_DAY_SEP (ent)` → `!XTEXT_ENT_IS_CHROME (ent)` and update its comment (it now drops any edge chrome; function name unchanged — all call sites already handle "returns lines dropped").

Consumer skips — change `XTEXT_ENT_IS_DAY_SEP` → `XTEXT_ENT_IS_CHROME` at exactly these sites (grep to relocate): click zone ~3621 (chrome rows own their row; returning `XTEXT_ZONE_DAY_SEP` for markers is fine), selection sizing ~4522, selection copy ~4552, save walk ~6919, foreach ~10781. Do **not** touch `gtk_xtext_recalc_day_boundaries` (its removal loop is day-sep-specific by design).

Render: in `gtk_xtext_render_line`, after the day-sep dispatch:

```c
	if (XTEXT_ENT_IS_GAP_MARKER (ent))
	{
		gtk_xtext_render_gap_marker (xtext, ent, line, win_width);
		return 1;
	}
```

`gtk_xtext_render_gap_marker` — clone `gtk_xtext_render_day_separator` (same geometry: cleared bg, two horizontal rules, centered label) with the label produced as:

```c
static void
xtext_format_gap_span (char *out, size_t out_len, gint64 span_secs)
{
	if (span_secs >= 2 * 86400)
		g_snprintf (out, out_len, _("%d days"), (int)(span_secs / 86400));
	else if (span_secs >= 2 * 3600)
		g_snprintf (out, out_len, _("%d hours"), (int)(span_secs / 3600));
	else
		g_snprintf (out, out_len, _("%d min"), (int)(span_secs / 60));
}
```

```c
	xtext_gap_info *gi = xtext_find_gap_info (xtext->buffer, ent->gap_id);
	char span_buf[64], label[160];

	if (!gi)
	{
		/* Record vanished; sweep will remove this marker — draw bg only */
		xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN,
		               xtext->fontsize);
		return;
	}
	xtext_format_gap_span (span_buf, sizeof (span_buf), gi->end_ts - gi->start_ts);
	if (gi->in_flight)
		g_snprintf (label, sizeof (label), _("loading missed messages…"));
	else if (gi->state == 1 /* SCROLLBACK_GAP_CANDIDATE */)
		g_snprintf (label, sizeof (label), _("possible gap (~%s quiet)"), span_buf);
	else
		g_snprintf (label, sizeof (label), _("~%s missing — scroll to load"), span_buf);
```

with line alpha 0.15 and text alpha 0.5 as in the day-sep renderer, except candidates use 0.10/0.35 (subdued). Use the actual `SCROLLBACK_GAP_CANDIDATE` constant, not the literal — scrollback.h is already included.

- [ ] **Step 6: The refresh sweep + `fe_gap_updated`**

Public sweep (decl in xtext.h next to `gtk_xtext_recalc_day_boundaries`):

```c
void
gtk_xtext_refresh_gap_markers (xtext_buffer *buf)
{
	textentry *ent, *next;

	gtk_xtext_refresh_gap_cache (buf);

	/* Drop markers whose record is gone or dead */
	for (ent = buf->text_first; ent; ent = next)
	{
		next = ent->next;
		if (!XTEXT_ENT_IS_GAP_MARKER (ent))
			continue;
		if (!xtext_find_gap_info (buf, ent->gap_id))
		{
			if (ent->prev)
				ent->prev->next = ent->next;
			else
				buf->text_first = ent->next;
			if (ent->next)
				ent->next->prev = ent->prev;
			else
				buf->text_last = ent->prev;
			gtk_xtext_kill_ent (buf, ent);
		}
	}

	/* Insert markers newly straddled by materialized entries */
	for (ent = buf->text_first; ent; ent = next)
	{
		next = ent->next;
		if (!XTEXT_ENT_IS_CHROME (ent))
			gtk_xtext_maybe_insert_gap_marker (buf, ent);
	}

	gtk_xtext_calc_lines (buf);
	if (buf->xtext && buf->xtext->buffer == buf)
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}
```

FE hook — fe.h (next to `fe_set_batch_mode`):
```c
/* Gap ledger changed for this session's channel (record shrunk, closed,
 * or dead-marked): refresh gap-marker entries and the scrollbar. */
void fe_gap_updated (struct session *sess, gint64 gap_id);
```
fe-gtk.c (next to `fe_set_batch_mode`):
```c
void
fe_gap_updated (session *sess, gint64 gap_id)
{
	(void) gap_id;	/* full refresh is cheap; per-gap delta not worth it */
	if (sess && sess->res && sess->res->buffer)
		gtk_xtext_refresh_gap_markers ((xtext_buffer *) sess->res->buffer);
}
```
fe-text.c stub block: `void fe_gap_updated (struct session *sess, gint64 gap_id) {}` (also add the missing `fe_set_batch_mode` stub while there — one line, noted pre-existing gap).

- [ ] **Step 7: Build + visual check + commit**

Build (no filtered errors). Manual: hand-insert a gap row into a test network DB (`INSERT INTO gaps (channel_id, start_ts, end_ts, state) VALUES (…)` spanning a known quiet stretch), launch, scroll to the span — a marker rule renders with the span label, is skipped by select-all copy, and disappears after `DELETE FROM gaps` + `/CLEAR` + reload. Include this in the commit message.

```bash
git add src/fe-gtk/xtext.c src/fe-gtk/xtext.h src/common/fe.h src/fe-gtk/fe-gtk.c src/fe-text/fe-text.c
git commit -m "xtext: gap-marker ephemeral entries + chrome tie-break comparator"
```

---

### Task 7: Scroll trigger, BETWEEN fill, attribution (spec §6 + §7 per-gap policy)

The proximity trigger in the scroll handler, the request path with gap attribution carried on the `chreq`, ledger shrink on batch completion, the BEFORE/AFTER fallback, and the `chreq_is_dup` end_ref fix.

**Files:**
- Modify: `src/fe-gtk/xtext.h` (GtkXText fields; `gtk_xtext_set_gap_fill_callback` decl)
- Modify: `src/fe-gtk/xtext.c` (init ~966; adjustment_changed post-ensure_range ~1414; setter next to `set_scroll_to_top_callback` ~10969)
- Modify: `src/fe-gtk/maingui.c` (`mg_gap_fill_cb` next to `mg_scroll_to_top_cb` ~2737; registration ~3110)
- Modify: `src/common/chathistory.h` (`chreq` struct; `chathistory_request_gap_fill` decl)
- Modify: `src/common/chathistory.c` (`chreq_is_dup` ~113; `chathistory_process_batch` ~1450; `finish_batch_processing` non-catchup section ~1332; `chathistory_handle_fail`; new `chathistory_request_gap_fill`)

**Interfaces:**
- Consumes: `buf->gap_cache` / `xtext_gap_info` (Task 6), `scrollback_gap_get/clear/touch/shrink/delete/set_state` (Task 2), `fe_gap_updated` (Task 6), `serv->chathistory_between_unsupported` / `chathistory_suppressed` (Task 5), `chathistory_request_before/after` wrappers (existing).
- Produces:
  - chreq fields `gint64 gap_id; int gap_dir;` (0 = not a gap fill)
  - `void chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir);` (chathistory.h)
  - xtext: `void gtk_xtext_set_gap_fill_callback (GtkXText *xtext, void (*callback) (GtkXText *, gint64, int, gpointer), gpointer userdata);`; GtkXText fields `guint gap_fill_debounce_tag; gint64 gap_fill_pending_id; int gap_fill_pending_dir; void (*gap_fill_cb)(GtkXText *, gint64, int, gpointer); gpointer gap_fill_userdata;`
  - chunk-state fields `gint64 gap_id; char *batch_newest_msgid;`

- [ ] **Step 1: chreq changes**

chathistory.h `chreq` struct additions:
```c
	gint64 gap_id;				/* gap-fill: ledger row this request serves (0 = none) */
	int gap_dir;				/* gap-fill: approach direction (-1 above, +1 below) */
```
(`chreq_new` is untouched — `g_new0` zeroes them; callers set after creation.)

`chreq_is_dup` gains the end_ref compare (fixes the latent two-BETWEENs-same-start bug):
```c
	if (g_strcmp0 (a->reference, b->reference) != 0)
		return FALSE;
	return g_strcmp0 (a->end_ref, b->end_ref) == 0;
```

- [ ] **Step 2: The request path**

chathistory.h decl (Doxygen'd, near `chathistory_request_between`):
```c
void chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir);
```

chathistory.c (near `chathistory_request_older`; forward-declare nothing — it only calls existing/earlier symbols):

```c
/* Gap fill: request history for a recorded hole, anchored at the edge
 * the user approached from so adjacent content arrives first.
 * approach_dir: -1 = gap is above the viewport (user scrolling up),
 * +1 = below.  Rate limiting lives in the ledger (attempts/last_attempt,
 * reset by any batch that shrinks the gap). */
void
chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir)
{
	const char *network;
	scrollback_db *db;
	scrollback_gap gap;
	char *near_ref, *far_ref;
	gboolean near_is_msgid;
	chreq *req;
	gint64 wait;

	if (!sess || !sess->server || !sess->server->have_chathistory ||
	    sess->server->chathistory_suppressed)
		return;
	if (!prefs.hex_irc_gapfill || gap_id <= 0)
		return;

	network = server_get_network (sess->server, FALSE);
	db = network ? scrollback_open (network) : NULL;
	if (!db || !scrollback_gap_get (db, gap_id, &gap))
		return;

	if (gap.state == SCROLLBACK_GAP_DEAD)
	{
		scrollback_gap_clear (&gap);
		return;
	}

	/* Ledger backoff: 5s per prior attempt, capped at 60s */
	wait = 5 * gap.attempts;
	if (wait > 60)
		wait = 60;
	if (gap.last_attempt > 0 && time (NULL) - gap.last_attempt < wait)
	{
		scrollback_gap_clear (&gap);
		return;
	}

	if (approach_dir < 0)
	{
		/* Gap above viewport: fill newest-first from its end bound */
		near_is_msgid = (gap.end_msgid && gap.end_msgid[0]);
		near_ref = near_is_msgid
			? g_strdup_printf ("msgid=%s", gap.end_msgid)
			: g_strdup_printf ("timestamp=%" G_GINT64_FORMAT, gap.end_ts);
		far_ref = (gap.start_msgid && gap.start_msgid[0])
			? g_strdup_printf ("msgid=%s", gap.start_msgid)
			: g_strdup_printf ("timestamp=%" G_GINT64_FORMAT, gap.start_ts);
	}
	else
	{
		near_is_msgid = (gap.start_msgid && gap.start_msgid[0]);
		near_ref = near_is_msgid
			? g_strdup_printf ("msgid=%s", gap.start_msgid)
			: g_strdup_printf ("timestamp=%" G_GINT64_FORMAT, gap.start_ts);
		far_ref = (gap.end_msgid && gap.end_msgid[0])
			? g_strdup_printf ("msgid=%s", gap.end_msgid)
			: g_strdup_printf ("timestamp=%" G_GINT64_FORMAT, gap.end_ts);
	}

	if (sess->server->chathistory_between_unsupported)
	{
		/* Fallback pagination anchored at the near edge; termination is
		 * the ledger shrink logic (bounds clamp per batch). */
		req = chreq_new (approach_dir < 0 ? CHREQ_BEFORE : CHREQ_AFTER,
		                 near_ref, NULL, prefs.hex_irc_chathistory_lines,
		                 CHREQ_PRI_USER, FALSE, near_is_msgid);
	}
	else
	{
		req = chreq_new (CHREQ_BETWEEN, near_ref, far_ref,
		                 prefs.hex_irc_chathistory_lines,
		                 CHREQ_PRI_USER, FALSE, near_is_msgid);
	}
	req->gap_id = gap_id;
	req->gap_dir = approach_dir;

	scrollback_gap_touch (db, gap_id);
	chathistory_submit (sess, req);

	g_free (near_ref);
	g_free (far_ref);
	scrollback_gap_clear (&gap);
}
```

- [ ] **Step 3: Batch attribution + newest-msgid capture**

`chathistory_chunk_state` additions:
```c
	gint64 gap_id;				/* gap-fill request this batch answers (0 = none) */
	char *batch_newest_msgid;	/* owned copy (chunked) / borrowed (sync) */
```

In `chathistory_process_batch`:
- After `is_catchup = sess->catchup_in_progress;` add:
  ```c
	gint64 active_gap_id = sess->ch_active ? sess->ch_active->gap_id : 0;
  ```
- **Empty-batch path**: a gap-fill probe that returns empty means the span is genuinely empty (or beyond retention) — dead-mark and do **not** poison `history_exhausted`. After the existing `chathistory_request_complete (sess);` in the empty-batch block, insert before the catchup branch:
  ```c
	if (active_gap_id > 0)
	{
		const char *gnet = server_get_network (serv, FALSE);
		scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;
		if (gdb)
		{
			scrollback_gap_set_state (gdb, active_gap_id, SCROLLBACK_GAP_DEAD);
			fe_gap_updated (sess, active_gap_id);
		}
		return;
	}
  ```
- After the sort (anchor: `g_slist_sort (batch->messages, compare_batch_msg_timestamp)`), capture the newest msgid alongside the existing oldest capture:
  ```c
	const char *batch_newest_msgid = NULL;
	{
		GSList *last = g_slist_last (batch->messages);
		batch_message *last_msg = last ? last->data : NULL;
		if (last_msg && last_msg->msgid)
			batch_newest_msgid = last_msg->msgid;
	}
  ```
- Sync path: `sync_state.gap_id = active_gap_id; sync_state.batch_newest_msgid = (char *) batch_newest_msgid;` (borrowed, like `batch_oldest_msgid`).
- Chunked path: `chunk->gap_id = active_gap_id; chunk->batch_newest_msgid = g_strdup (batch_newest_msgid);` — and free it wherever `chunk->batch_oldest_msgid` is freed (grep `batch_oldest_msgid` in the chunk-free path).

- [ ] **Step 4: Ledger shrink on completion**

In `finish_batch_processing`, in the **non-catchup** section (anchor: `--- Non-catch-up post-processing`), insert before the `fe_reset_scroll_top_backoff (sess);` line:

```c
	/* Gap fill: clamp the ledger record to the edge this batch attached
	 * to, using the batch's actual returned bounds (robust against
	 * server direction quirks).  All-duplicate batches still shrink —
	 * the span content was already stored locally. */
	if (chunk->gap_id > 0 && chunk->raw_count > 0)
	{
		const char *gnet = server_get_network (serv, FALSE);
		scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;
		scrollback_gap g;

		if (gdb && scrollback_gap_get (gdb, chunk->gap_id, &g))
		{
			gboolean bridged = FALSE;

			if (chunk->oldest_timestamp > 0 &&
			    chunk->oldest_timestamp <= g.start_ts)
				bridged = TRUE;	/* batch overlaps the start bound: covered */
			else if (chunk->raw_count <
			         get_effective_limit (serv, prefs.hex_irc_chathistory_lines))
				bridged = TRUE;	/* server returned everything in range */
			else if (chunk->newest_timestamp >= g.end_ts &&
			         chunk->oldest_timestamp > g.start_ts)
				/* attached at the end side: end bound moves down */
				scrollback_gap_shrink (gdb, chunk->gap_id, 0, NULL,
					chunk->oldest_timestamp, chunk->batch_oldest_msgid);
			else if (chunk->newest_timestamp > 0 &&
			         chunk->newest_timestamp < g.end_ts)
				/* attached at the start side: start bound moves up */
				scrollback_gap_shrink (gdb, chunk->gap_id,
					chunk->newest_timestamp, chunk->batch_newest_msgid,
					0, NULL);

			if (bridged)
				scrollback_gap_delete (gdb, chunk->gap_id);

			fe_gap_updated (sess, chunk->gap_id);
			scrollback_gap_clear (&g);
		}
	}
```

Also guard the existing `sess->history_exhausted = TRUE` consequences: a `chathistory_end` tag on a gap-fill batch refers to the requested range, not all history — in the block near the top (anchor: `chunk->chathistory_end`), change to:

```c
	if (chunk->chathistory_end && chunk->gap_id == 0)
		sess->history_exhausted = TRUE;
```

- [ ] **Step 5: Per-gap FAIL policy**

In `chathistory_handle_fail` (Task 5's version), capture before `chathistory_request_complete`:
```c
		gint64 failed_gap_id = sess->ch_active ? sess->ch_active->gap_id : 0;
```
and after `chathistory_request_complete (sess);` add:
```c
		/* Gap-fill FAIL: back off via the ledger; three strikes → dead */
		if (failed_gap_id > 0)
		{
			const char *gnet = server_get_network (serv, FALSE);
			scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;
			if (gdb)
			{
				if (scrollback_gap_touch (gdb, failed_gap_id) >= 3)
					scrollback_gap_set_state (gdb, failed_gap_id,
					                          SCROLLBACK_GAP_DEAD);
				fe_gap_updated (sess, failed_gap_id);
			}
			return;	/* not a catchup request; nothing further */
		}
```
(A BETWEEN FAIL that set `chathistory_between_unsupported` retries naturally: the next proximity trigger takes the BEFORE/AFTER fallback after the 5s backoff. Note this deviation from the spec's "retry once immediately" — simpler and avoids re-entrancy in the FAIL handler; record in the commit message.)

- [ ] **Step 6: xtext trigger**

xtext.h GtkXText fields (next to the scroll-to-top block):
```c
	/* Gap-fill (recorded history holes) support */
	guint gap_fill_debounce_tag;
	gint64 gap_fill_pending_id;
	int gap_fill_pending_dir;
	void (*gap_fill_cb) (GtkXText *xtext, gint64 gap_id, int approach_dir,
	                     gpointer userdata);
	gpointer gap_fill_userdata;
```
Decl:
```c
void gtk_xtext_set_gap_fill_callback (GtkXText *xtext,
	void (*callback) (GtkXText *, gint64, int, gpointer), gpointer userdata);
```
xtext.c: zero the five fields at the init site (~966, next to `scroll_to_top_cb = NULL`); setter next to `gtk_xtext_set_scroll_to_top_callback`; remove the debounce source in the dispose/finalize path where `scroll_top_debounce_tag` is removed (grep `scroll_top_debounce_tag` for the teardown site).

Timeout callback (near `gtk_xtext_scroll_top_timeout`):

```c
static gboolean
gtk_xtext_gap_fill_timeout (gpointer data)
{
	GtkXText *xtext = GTK_XTEXT (data);

	xtext->gap_fill_debounce_tag = 0;

	/* Re-validate: buffer switches and completed fills invalidate the
	 * pending gap. */
	if (xtext->gap_fill_cb && xtext->buffer &&
	    xtext_find_gap_info (xtext->buffer, xtext->gap_fill_pending_id))
		xtext->gap_fill_cb (xtext, xtext->gap_fill_pending_id,
		                    xtext->gap_fill_pending_dir,
		                    xtext->gap_fill_userdata);
	return G_SOURCE_REMOVE;
}
```

Proximity check — in `gtk_xtext_adjustment_changed`, inside the `HAS_VIRT_DB` block, immediately after the post-`ensure_range` scroll-to-top re-check (anchor: `Re-check scroll-to-top after ensure_range`), add:

```c
			/* Gap-fill proximity: a recorded hole within ~two pages of
			 * the viewport gets a server fill request.  Wider margin
			 * than ensure_range's one page — the network is slower than
			 * SQLite.  Line-space estimate mirrors the num_lines
			 * composition (estimate above/below, exact inside). */
			if (xtext->gap_fill_cb && xtext->buffer->gap_cache &&
			    xtext->buffer->avg_lines_per_entry > 0)
			{
				GList *gl;
				double avg = xtext->buffer->avg_lines_per_entry;
				double gmat_top = LINES_BEFORE_MAT (xtext->buffer);
				double gmat_bot = gmat_top + BUF_LINES_MAT (xtext->buffer);
				int gmat_end_idx = xtext->buffer->mat_first_index +
					(BUF_MAT_COUNT (xtext->buffer) -
					 xtext->buffer->ephemeral_count);

				for (gl = xtext->buffer->gap_cache; gl; gl = gl->next)
				{
					xtext_gap_info *gi = gl->data;
					double gap_line;

					if (gi->ordinal <= xtext->buffer->mat_first_index)
						gap_line = gi->ordinal * avg;
					else if (gi->ordinal >= gmat_end_idx)
						gap_line = gmat_bot +
							(gi->ordinal - gmat_end_idx) * avg;
					else
						gap_line = gmat_top +
							(gi->ordinal - xtext->buffer->mat_first_index) * avg;

					if (gap_line >= value - 2.0 * page_size &&
					    gap_line <= value + 3.0 * page_size)
					{
						int dir = (gap_line < value + page_size / 2) ? -1 : 1;

						if (xtext->gap_fill_debounce_tag)
							g_source_remove (xtext->gap_fill_debounce_tag);
						xtext->gap_fill_pending_id = gi->gap_id;
						xtext->gap_fill_pending_dir = dir;
						xtext->gap_fill_debounce_tag = g_timeout_add (500,
							gtk_xtext_gap_fill_timeout, xtext);
						gi->in_flight = 1;	/* display only */
						break;
					}
				}
			}
```

(The 500 ms debounce coalesces scroll ticks; the real request-rate brake is the ledger backoff in `chathistory_request_gap_fill`. `in_flight` is cleared by the next cache refresh via `fe_gap_updated`.)

- [ ] **Step 7: maingui callback**

Next to `mg_scroll_to_top_cb`:

```c
/* Callback for gap-fill proximity: request history for a recorded hole.
 * Resolves the buffer's own session — gap fill must work for whichever
 * buffer is scrolled, not just the current tab's. */
static void
mg_gap_fill_cb (GtkXText *xtext, gint64 gap_id, int approach_dir,
                gpointer userdata)
{
	GSList *list;

	(void) userdata;

	if (!prefs.hex_irc_gapfill)
		return;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->res && (void *) sess->res->buffer == (void *) xtext->buffer)
		{
			chathistory_request_gap_fill (sess, gap_id, approach_dir);
			return;
		}
	}
}
```

Registration, one line after the scroll-to-top registration (~3110):
```c
	gtk_xtext_set_gap_fill_callback (xtext, mg_gap_fill_cb, NULL);
```
(maingui.c already includes chathistory.h for `chathistory_request_older`.)

- [ ] **Step 8: Build + end-to-end manual test + commit**

Build (no filtered errors). Manual plan for the commit message: *hand-insert a witnessed gap over a span the test server can serve; scroll toward the marker → raw log shows one `CHATHISTORY BETWEEN <target> <near> <far> <N>` after ~500 ms; messages splice in chronologically with no duplicates; the marker's span label shrinks per batch and the marker disappears at bridge; `SELECT * FROM gaps` confirms delete. Repeat with `chathistory_between_unsupported` forced to 1 → BEFORE requests with the same convergence. Empty-answer gap → single request, marker gone, row state=2, no re-query after restart.*

```bash
git add src/fe-gtk/xtext.c src/fe-gtk/xtext.h src/fe-gtk/maingui.c src/common/chathistory.c src/common/chathistory.h
git commit -m "chathistory+xtext: scroll-proximity gap fill via BETWEEN with ledger attribution"
```

---

### Task 8: Bootstrap scan for pre-existing holes (spec §4 bootstrap)

One-shot per-channel candidate scan, run off the replay hot path.

**Files:**
- Modify: `src/common/scrollback.c` (new function in the gap-ledger section), `src/common/scrollback.h`
- Modify: `src/common/text.c` (scrollback session-load tail, anchor: `scrollback_get_newest_time` ~437)
- Modify: `src/common/poxchat.h` + `src/common/cfgfiles.c` (one pref)
- Modify: `tools/gap-ledger-test.c` (bootstrap test)

**Interfaces:**
- Consumes: `scrollback_gap_record` (Task 2), `prefs.hex_irc_gapfill` (Task 3), `fe_gap_updated` (Task 6), `is_session` (existing, poxchat.h).
- Produces: `int scrollback_gap_bootstrap (scrollback_db *db, const char *channel, gint64 threshold_secs);` (returns candidates recorded, -1 if already done / error); pref `hex_irc_gapfill_bootstrap_hours` (int, default 12, 0 disables).

- [ ] **Step 1: Add the failing harness test**

Append to `tools/gap-ledger-test.c` before the RESULT print (seed data from Task 2 has one big hole: 1100 → 90000, ~24.7h):

```c
	/* bootstrap: candidate over a >12h silence in a fresh channel.
	 * NOTE: do not call bootstrap before seeding — the first call sets
	 * the one-shot latch regardless of row count. */
	{
		scrollback_begin_transaction (db);
		scrollback_db_save (db, "#boot", 1000, "b1", "a", TRUE);
		scrollback_db_save (db, "#boot", 2000, "b2", "b", TRUE);
		scrollback_db_save (db, "#boot", 200000, "b3", "c", TRUE);
		scrollback_commit_transaction (db);
		CHECK ("bootstrap finds hole", scrollback_gap_bootstrap (db, "#boot", 12 * 3600) == 1);
		{
			GList *l = scrollback_gap_list (db, "#boot");
			CHECK ("bootstrap candidate", g_list_length (l) == 1 &&
				nth_gap (l, 0)->state == SCROLLBACK_GAP_CANDIDATE &&
				nth_gap (l, 0)->start_ts == 2000 &&
				nth_gap (l, 0)->end_ts == 200000 &&
				g_strcmp0 (nth_gap (l, 0)->start_msgid, "b2") == 0 &&
				g_strcmp0 (nth_gap (l, 0)->end_msgid, "b3") == 0);
			scrollback_gap_list_free (l);
		}
		CHECK ("bootstrap latched", scrollback_gap_bootstrap (db, "#boot", 12 * 3600) == -1);
	}
```

- [ ] **Step 2: Run to verify it fails**

`pwsh tools\build-gap-test.ps1` — expected: FAIL, `scrollback_gap_bootstrap` unresolved.

- [ ] **Step 3: Implement**

In the gap-ledger section of scrollback.c (one-off statements; the SELECT walk and the `gap_record` INSERTs hit different tables, so interleaving is safe; `gap_record`'s internal transaction nests under ours via the ref-counted helpers):

```c
int
scrollback_gap_bootstrap (scrollback_db *db, const char *channel,
                          gint64 threshold_secs)
{
	gint64 channel_id;
	sqlite3_stmt *stmt = NULL;
	gint64 prev_ts = 0;
	char *prev_msgid = NULL;
	int recorded = 0;
	int done = 0;

	if (!db || !channel || threshold_secs <= 0)
		return -1;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return -1;

	/* One-shot latch */
	if (sqlite3_prepare_v2 (db->db,
		"SELECT gap_bootstrap_done FROM channels WHERE id = ?1",
		-1, &stmt, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64 (stmt, 1, channel_id);
	if (sqlite3_step (stmt) == SQLITE_ROW)
		done = sqlite3_column_int (stmt, 0);
	sqlite3_finalize (stmt);
	stmt = NULL;
	if (done)
		return -1;

	scrollback_begin_transaction (db);

	if (sqlite3_prepare_v2 (db->db,
		"SELECT timestamp, msgid FROM messages WHERE channel_id = ?1 "
		"ORDER BY timestamp ASC, id ASC",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		while (sqlite3_step (stmt) == SQLITE_ROW)
		{
			gint64 ts = sqlite3_column_int64 (stmt, 0);
			const char *msgid = (const char *) sqlite3_column_text (stmt, 1);
			if (prev_ts > 0 && ts - prev_ts >= threshold_secs)
			{
				if (scrollback_gap_record (db, channel, prev_ts, prev_msgid,
				                           ts, msgid,
				                           SCROLLBACK_GAP_CANDIDATE) > 0)
					recorded++;
			}
			prev_ts = ts;
			g_free (prev_msgid);
			prev_msgid = g_strdup (msgid);
		}
		sqlite3_finalize (stmt);
	}
	g_free (prev_msgid);

	{
		char *upd = sqlite3_mprintf (
			"UPDATE channels SET gap_bootstrap_done = 1 WHERE id = %lld",
			(long long) channel_id);
		sqlite3_exec (db->db, upd, NULL, NULL, NULL);
		sqlite3_free (upd);
	}

	scrollback_commit_transaction (db);
	return recorded;
}
```

Header decl with the semantics from **Produces**.

- [ ] **Step 4: Pref + deferred call site**

Pref (three places, same pattern as Task 3): `int hex_irc_gapfill_bootstrap_hours;` / `{"irc_gapfill_bootstrap_hours", P_OFFINT (hex_irc_gapfill_bootstrap_hours), TYPE_INT},` / `prefs.hex_irc_gapfill_bootstrap_hours = 12; /* candidate threshold; 0 disables bootstrap */`.

text.c — file-scope, above the scrollback-load function that populates `scrollback_newest_msgid` (~435):

```c
/* Gap-ledger bootstrap: one-shot candidate scan per channel, deferred to
 * a low-priority idle so the 187k-row walk stays off the replay path. */
typedef struct {
	session *sess;		/* validate with is_session() before use */
	char *network;
	char *channel;
} gap_bootstrap_req;

static gboolean
gap_bootstrap_idle_cb (gpointer data)
{
	gap_bootstrap_req *req = data;
	scrollback_db *db = scrollback_open (req->network);

	if (db && scrollback_gap_bootstrap (db, req->channel,
		(gint64) prefs.hex_irc_gapfill_bootstrap_hours * 3600) > 0 &&
	    is_session (req->sess))
		fe_gap_updated (req->sess, 0);

	g_free (req->network);
	g_free (req->channel);
	g_free (req);
	return G_SOURCE_REMOVE;
}
```

At the load-tail (after the `scrollback_newest_time` line; `network` is in scope there — verify with the surrounding code):

```c
	if (prefs.hex_irc_gapfill && prefs.hex_irc_gapfill_bootstrap_hours > 0)
	{
		gap_bootstrap_req *breq = g_new0 (gap_bootstrap_req, 1);
		breq->sess = sess;
		breq->network = g_strdup (network);
		breq->channel = g_strdup (sess->channel);
		g_idle_add_full (G_PRIORITY_LOW, gap_bootstrap_idle_cb, breq, NULL);
	}
```

- [ ] **Step 5: Harness green + solution build**

```
pwsh tools\build-gap-test.ps1
pwsh tools\run-gap-tests.ps1
```
Expected: `RESULT: ALL PASS`. Then the solution build (no filtered errors).

- [ ] **Step 6: Commit**

Commit test plan: *first launch against the real May-era DB → subdued "possible gap" markers appear at long silences (after the idle scan; check `SELECT COUNT(*) FROM gaps WHERE state=1`); approaching one the server can't fill turns it dead and it never returns; second launch records nothing new (latch).*

```bash
git add src/common/scrollback.c src/common/scrollback.h src/common/text.c src/common/poxchat.h src/common/cfgfiles.c tools/gap-ledger-test.c
git commit -m "scrollback: one-shot bootstrap scan seeding candidate gaps"
```

---

## Final integration pass (after Task 8)

- [ ] Full solution build clean (filtered).
- [ ] `pwsh tools\build-vfs-test.ps1 && pwsh tools\run-vfs-tests.ps1` — the VFS suite must still pass (schema grew; salvage leg exercises `migrate_image`).
- [ ] `pwsh tools\run-gap-tests.ps1` — ledger suite green.
- [ ] Run the spec §11 manual matrix against Nefarious/X3 (AfterNET), including the **first** item: verify whether Nefarious serves `BETWEEN` (`/QUOTE CHATHISTORY BETWEEN #chan timestamp=… timestamp=… 50`); if not, confirm the fallback latch engages and the BEFORE path converges.
- [ ] Update `.claude/skills/xtext-rendering.md`: add the chrome tie-break rule, the gap-marker invariants (mirrors day-sep: never text_first/text_last, dropped at edges, recreated from the ledger), and the `mat_first_index` resync fact.
