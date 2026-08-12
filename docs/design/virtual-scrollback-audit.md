# Virtual Scrollback Audit

**Status (2026-08-12):** C1, C2 (and C3-C6), M1, M2, M5-M11, m3 fixed —
see commits c9e8b586..231d46bb.  M3/M4/M11 landed as the contiguity
gates (47fcab38) + window re-center (231d46bb).  Still open: the minor
list in §3 (highlight-all on materialization, buffer_show page_size
reconfigure, /clear reaction/reply orphans, global UNIQUE(msgid),
pending-msgid race, group_id persistence, save-file ephemerals,
session-reuse buffer mixing, lastlog stamp mutation, initial-load
tiebreaker, per-keystroke DB search scan).

Date: 2026-08-10
Scope: the virtual scrollback subsystem — `src/fe-gtk/xtext.c` (windowing,
line accounting, render path, search/navigation) and `src/common/scrollback.c`
(SQLite layer) — audited as on disk, including the uncommitted find-bar
Msg-ID WIP.
Method: five independent audit passes (windowing/eviction, line accounting &
adjustment math, render path, DB layer & contract, user flows), findings
cross-checked against each other and against
[scroll-model-comparison.md](scroll-model-comparison.md). Findings reported
by 3+ independent passes with matching mechanisms are marked **[convergent]**.
Line numbers are for the working tree on the audit date and will drift.

Companion docs: [scroll-model-comparison.md](scroll-model-comparison.md)
(Repro A/B history, Stage 1/4.5 fixes, roadmap),
[mark-based-scroll-anchoring.md](mark-based-scroll-anchoring.md).

---

## 1. Crash-severity findings

### C1. `/clear` (delete-all) raw-frees every entry, bypassing `kill_ent` **[convergent — 4 passes]**

`gtk_xtext_clear`'s delete-all branch (xtext.c:8597-8620) walks the list
doing bare `g_free (buf->text_first)` and rebuilds the B-tree — and nothing
else. Not cleaned up:

- `entries_by_id` / `entries_by_msgid` hash tables → every later lookup for
  a pre-clear id/msgid returns a **freed pointer**. Reachable via: reply
  quote referencing a cleared message, late reaction/redaction TAGMSG for a
  pre-clear msgid, the WIP find-bar Msg-ID jump, `materialize_msg`'s dup
  guard (xtext.c:11087).
- `pagetop_ent`, `flash_ent`, `hover_ent`, `hilight_ent` — only `kill_ent`
  (xtext.c:8333-8347) nulls these. Draw code dereferences
  `flash_ent->group_id` / `hover_ent->group_id` (xtext.c:6451-6453).
- The display cache is not flushed and `hintsearch_id`/`cursearch`/`curmark`
  are not reset. `gtk_xtext_search` at xtext.c:9197-9199 does
  `ent = find_by_id (hintsearch_id); for (mark = ent->marks; ...)` with
  **no NULL check** — crashes on both NULL and dangling.
- Each entry's `msgid`, `stripped_str`, `sublines`, `fmt_spans`,
  `emoji_list`, marks, reactions, reply are leaked (contrast the full free
  walk in `buffer_free`, xtext.c:10957-10979).

DB detail that makes this *reachable*, not just latent: the `messages` table
is `INTEGER PRIMARY KEY` **without AUTOINCREMENT** (scrollback.c:171), and
`scrollback_clear` is a plain `DELETE` (scrollback.c:912-927), so SQLite
**reuses rowids** after a clear. New messages then get `entry_id`s equal to
stale hash keys: the dup guard sees the stale entry and silently refuses to
materialize (missing lines), and `init_entry`'s hash insert overwrites keys
intermittently — matching the "infrequent crash" character of the known
open crash reports.

Fix shape: run the entries through `kill_ent` (or replicate its cleanup:
`g_hash_table_remove_all` on both hashes, display-cache flush, null all
widget-level entry pointers, reset search/hint state, free per-entry
allocations), and add the missing NULL check at xtext.c:9197.

### C2. Find-bar signal callbacks capture the window-creating session forever

`mg_create_search` is called once per shared `session_gui`
(maingui.c:3976/3982) with whatever session created the window. Every
find-bar callback — entry `changed` (maingui.c:4455), Esc key controller,
close button `connect_swapped (mg_search_toggle, sess)` (maingui.c:4447),
prev/next, highlight refresh, and the WIP msgid toggle — holds that raw
`sess` pointer forever. `fe_session_callback` (maingui.c:5672) frees the
session without disconnecting or repointing these.

Close the original tab, keep using the find bar → freed-`sess` deref.
Closing the bar is the most likely crash site: `mg_search_toggle` →
`hc_entry_set_text (shentry, "")` fires `changed` →
`search_handle_change (stale sess)` → `sess->gui` UAF.

**This is the leading root-cause candidate for the known intermittent
find-bar-close crash** (memory: "Find bar crash (infrequent)") — the
intermittency matches "only crashes when the creating session was closed."

Fix shape: callbacks should resolve `current_sess` (the find bar always
operates on the visible session in a shared gui) instead of capturing a
session at creation time.

### C3. `gtk_xtext_buffer_free` leaves widget-level entry pointers dangling

`buffer_free` (xtext.c:10930-11000) frees all entries without nulling
`xtext->flash_ent` / `hover_ent` / `hover_reply_target` / `hilight_ent`.
Trigger: reply/msgid jump (1.5 s flash pending) or an active hover, then
close that tab → next draw of the remaining buffer dereferences the freed
pointer (xtext.c:6451/6453; `hilight_ent` also via `render_ents`).
Plausible contributor to the other uninvestigated intermittent crashes.

### C4. `buf->insert_hint` not cleared by `kill_ent`

`kill_ent` clears pagetop/hover/flash/hilight but not `insert_hint`
(cleared only at batch end, fe-gtk.c:2743-2746). `append_entry`'s prune
(xtext.c:9589-9594) has no batch-mode guard and can `remove_top` the hint
entry — e.g. a live PRIVMSG interleaved inside a chathistory batch — after
which `insert_sorted_entry` dereferences `insert_hint->stamp`
(xtext.c:9722-9725) on freed memory. Medium confidence (needs the
interleaving), trivially cheap to fix in `kill_ent`.

### C5. `hover_react_ent` / `redact_confirm_ent` not cleared by `kill_ent`

- `hover_react_ent` (set xtext.c:4013, used in the ~500 ms delayed-show
  timer at 3928): eviction between arming and firing (a reply-jump's
  `ensure_range` can evict the whole current window) leaves the timer
  holding a freed pointer.
- `redact_confirm_ent` (pointer-compared at xtext.c:6365): after
  evict/re-materialize churn, a recycled allocation can alias a different
  entry, letting the two-click redact confirm fire on the wrong message.

### C6. Virtual search rescan duplicates `search_found` and leaks marks

When the DB grew since the scan (`total_entries != search_virt_db_total`,
xtext.c:9116-9133), `search_virt_scan` re-runs **without** `search_fini`:
`search_textentry_add` overwrites `ent->marks` (leaking the old list) and
appends the entry to `search_found` a second time (xtext.c:8801-8804).
`kill_ent` → `search_textentry_del` removes only one instance
(`g_list_remove`), so the duplicate dangles; a later `search_fini` or
follow-toggle walk touches freed memory. Trigger: search open in a live
channel → Next (rescan) → scroll far enough to evict a match → change/close
search.

---

## 2. Major presentation / scrolling findings

### M1. `scroll_to_entry` and `moveto_marker_pos` omit `LINES_BEFORE_MAT` **[convergent — 4 passes]**

Both compute the target adjustment value by summing display lines from
`text_first` — a **materialized-window-relative** count — and write it into
the adjustment, whose coordinate space begins with `lines_before_mat`
estimated lines (`scroll_to_entry` xtext.c:10723-10729; `moveto_marker_pos`
xtext.c:10678-10685, which also compares its mat-relative value against the
absolute `adj->value`, so its early-out never fires in virtual mode).
Compare the correct convention at xtext.c:9019, 9234 (search paths) and
12438, 12558 (anchor restores).

Any off-window jump — reply-quote click, the WIP Msg-ID find-bar jump,
"go to marker" — lands `lines_before_mat` lines (thousands, in a deep
buffer) **above** the target. Worse, the bogus low value then takes the
`click_into_unmat` path in `adjustment_changed`, which treats it as a
scrollbar click into ancient history and **evicts the current window to
materialize the oldest history**. Flash highlight fires off-screen.

This is the single most user-visible bug in the audit: it breaks every
"jump to X" feature in every long-history channel. One-line fix per site
(seed with `LINES_BEFORE_MAT (buf)`).

### M2. `remove_top` evicts without crediting `lines_before_mat` **[convergent — 4 passes]**

`ensure_range`'s head eviction does `lines_before_mat += evicted_lines`
(xtext.c:11495-11503) — the absorptive contract that keeps absolute
coordinates stable. `remove_top` (xtext.c:8446-8484), which runs on **every
live append once the window is full** (append_entry xtext.c:9589-9594) and
from `enforce_mat_window`, instead does `mat_first_index++`,
`num_lines -= ent_lines`, `value -= ent_lines` — the evicted entry's lines
vanish from the coordinate space even though the entry is still in the DB.

Consequences accumulate for the whole session in any active channel:
`upper` stagnates while history grows; on later scroll-up, `ensure_range`'s
prepend subtracts *actual* lines from the never-credited counter, hits the
0 clamp (xtext.c:7042-7045) while `mat_first_index` is still thousands,
so the thumb pegs to the top while content keeps loading; the scroll-to-top
debounce (xtext.c:1098-1110 — no `mat_first_index == 0` check, unlike its
sibling at 1300-1308) fires **chathistory server fetches** while thousands
of DB rows remain unloaded; and thumb-position→index estimates operate in a
compressed space. Same omission mirrored in `prepend_entry`
(xtext.c:9653-9655, direction reversed).

### M3. Window-contiguity violations → list/tree divergence and permanent holes **[convergent — 2 passes]**

The window's DB extent is always computed as
`mat_end = mat_first_index + (BUF_MAT_COUNT − ephemeral_count) − 1`
(xtext.c:11309; near_bot trigger 1213-1216), i.e. materialized DB entries
are assumed to be one contiguous DB index range. Verified ways to break
that while the window sits mid-history:

- **(a) Count-basis mismatch:** eviction loops stop at raw
  `BUF_MAT_COUNT > VIRT_MAT_WINDOW` (ephemerals included,
  xtext.c:11486/11523) but the materialization gate uses
  `BUF_MAT_COUNT − ephemeral_count < VIRT_MAT_WINDOW` (xtext.c:11595). With
  N ephemerals in-window the gate stays open and a live message is linked
  at the list tail even though the user is scrolled up.
- **(b) `should_materialize`'s LINK_BEFORE gate** (xtext.c:11605-11612)
  only skips entries older than the window head; a sorted-insert entry
  newer than everything materialized goes to the list tail regardless of
  the unmaterialized DB gap before it.
- **(c) Thumb-drag to bottom:** `value >= upper − page − 1` sets
  `anchor_to_bottom` (xtext.c:1068-1071) even though the window is
  mid-history (see M4) — the next live message materializes at the tail
  across a gap of unloaded rows.

Once a tail entry is non-adjacent: the next `ensure_range` append loads
from `computed mat_end + 1`, permanently **skipping one DB row per spurious
tail entry** (holes — lines that never appear), and links
chronologically-older DB rows **after** the newer tail entry in the linked
list while `add234` places them mid-tree — the core invariant (list order ==
tree order) is violated, after which `nth()` and the render walk disagree:
out-of-order lines on screen, clicks/selection resolving to wrong entries,
viewport jumps in the corrupted region.

### M4. No "click below the window" handling — bottom teleport/stale tail **[convergent — 2 passes; also §7 of scroll-model-comparison.md]**

`click_into_unmat` handles only `scroll_line < mat_top` (xtext.c:1139).
After DB-only appends while scrolled up, dragging the thumb to the bottom:
near_bot extends only ~2 pages from `mat_end + 1` (xtext.c:1209-1227),
`ensure_range`'s internal anchor restore snaps the value back to the old
window, wheel-down at the pinned value is suppressed
(`new_value != adj_value` gate, xtext.c:4479; whole virtual block gated on
`old_value != value`, xtext.c:1066). Result: the bottom of the scrollbar
shows the newest *materialized* content, not the newest messages; the real
tail appears only when the next live message arrives — which then arms M3(c).

### M5. Buffer-switch restore uses the center-convention restore on a bottom-convention anchor

The persistent `buf->scroll_anchor` stores the **bottom-edge** entry
(xtext.c:1080-1091; render consumes it that way at 7342-7356), but
`buffer_show` restores through `restore_scroll_anchor` (xtext.c:10855),
which centers: `new_value = target_line − page/2` (xtext.c:12456) instead
of `target_line − page + 1`. Each A→B→A tab round-trip while scrolled up
walks the viewport **~half a page toward the bottom**, and the unblocked
`set_value` re-captures the anchor at the drifted position, compounding.

### M6. Auto-collapse sweep in `render_page` uses mat-relative positions against absolute `adj->value`

xtext.c:7544-7578: `line_pos` accumulates from `text_first` (mat-relative)
but is compared with absolute `adj_val`. With `lines_before_mat > 0` (the
steady state of a long-history channel), an expanded entry inside the
viewport satisfies `line_pos + ent_lines <= adj_val` whenever
`ent_lines <= lines_before_mat` — essentially always. Symptom: with
`hex_gui_collapse_multiline` on, clicking to expand a collapsed multiline
message flashes open and instantly re-collapses; the feature is unusable in
long-history buffers. Fix: seed `line_pos = LINES_BEFORE_MAT (buf)` (the
at_top test at 7319 already does).

### M7. The unread marker is destroyed or dragged by routine eviction **[convergent — 3 passes]**

`kill_ent` (xtext.c:8351-8356) treats every kill as deletion: it migrates
`marker_pos_id` to `ent->next` and sets `MARKER_RESET_BY_KILL`. In virtual
mode eviction is routine scrolling/pruning and `entry_id` (== DB rowid) is
stable across re-materialization, so leaving the marker alone would restore
it exactly. As-is: tail eviction (evicted tail's `next` is NULL) **zeroes**
the marker; head eviction drags it one entry forward per eviction, so live
traffic walks the red line to whatever is at the window head. Related:
`set_marker_from_timestamp` (xtext.c:10747) scans only materialized
entries, pinning the marker to the window top when the read timestamp
predates the window. Fix shape: in virtual mode, skip marker migration for
entries with `has_db_row` (id remains resolvable on re-materialization).

### M8. WIP regression: reply-quote click is a dead no-op when the target is evicted

The uncommitted `gtk_xtext_click_reply_context` (xtext.c ~3691-3706): when
`reply->target_entry_id` is set (the common case — it is resolved whenever
the target was materialized at reply time) and `find_by_id` fails (target
evicted), it returns silently. The old code fell through to the
msgid → `scrollback_get_rowid_by_msgid` → `ensure_range` materialization
path. Fix: on `find_by_id` failure, fall through to the msgid path (and/or
pass the entry_id-as-rowid into the DB materialization path directly).
Also note: replies created against a *local-id* target (ephemeral/pre-save,
id ≥ 2^62) store an id that never resolves after re-materialization.

### M9. Redaction state lost on re-materialization

`materialize_msg` (xtext.c:11073-11216) rehydrates reply context and
reactions but ignores `msg->redacted_by` / `redact_reason`
(scrollback.h:37-39; the initial load applies them, text.c:319-322).
Scroll away and back past a redacted message → the original, supposedly
redacted text is displayed again.

### M10. `mat_first_index` / `lines_before_mat` drift class — no re-derivation path

`total_entries` is periodically re-synced from `scrollback_count`
(xtext.c:7035-7039), but `mat_first_index` and `lines_before_mat` are
**never re-derived** — every bookkeeping slip is permanent and cumulative.
Verified slips, beyond M2:

- `prepend_entry` / `insert_sorted_entry` decrement or shift
  `mat_first_index` without gating on `ent->has_db_row`
  (xtext.c:9649-9655, 9786-9793) — ephemerals (dup-rejected saves,
  redaction notices) occupy no DB index.
- `virt_should_materialize`'s skip path (xtext.c:11614-11627) increments
  `total_entries`/`mat_first_index` unconditionally even when the save was
  dup-rejected (`pending_db_rowid == 0` is checkable but unused).
- `ensure_range` commits `mat_first_index = want_start` (xtext.c:11354)
  even when `scrollback_load_range` returned nothing (transient SQL/VFS
  error → silent empty list, scrollback.c:1443-1444) or `materialize_msg`
  rejected rows — the mapping can be rebased by ±radius with no content.
- Redaction notices are double-recorded (in-memory ephemeral via
  `fe_redact_message` + a separate DB row via
  `scrollback_redact_for_session`, text.c:180-197 / fe-gtk.c:1434-1455)
  with no index compensation for the DB row — and render twice after
  evict/re-materialize.

Recommended structural fix (also collapses much of M2): a periodic resync
`mat_first_index = scrollback_get_index_of_rowid (text_first's rowid)` —
the primitive already exists — plus gating every index mutation on
`has_db_row`.

### M11. `ensure_range` loads the entire gap synchronously **[convergent — 2 passes]**

Prepend/append counts are uncapped gap sizes (xtext.c:11313-11316,
11398-11401): a scrollbar click or search-navigate far from the window in a
50k-row DB materializes thousands of entries (full textentry + Pango
`lines_taken` each, xtext.c:11164) and then immediately evicts most of
them. Multi-second UI freeze + memory spike. There is no
"discontiguous re-center" path (evict all, load around target); one is
needed for far jumps.

---

## 3. Minor findings

- **Search highlight-all is incomplete in virtual mode:** marks are applied
  only to entries materialized at scan time (xtext.c:8957-8963) plus the
  navigation target; `materialize_msg` never applies marks, so matches
  materialized later show no highlight, and re-materialized entries lose
  theirs.
- **`buffer_show` never reconfigures `page_size` when widget size is
  unchanged** (xtext.c:10834-10891): switching from an undersized buffer
  (page clamped to a small `upper` by the GTK4-quirk clamp at 998-999) to a
  full buffer leaves the stale small page on the shared adjustment until
  the next append/resize — wrong thumb size and bottom-tolerance math.
  Fix: unconditional `gtk_xtext_adjustment_set` on mount.
- **`adjustment_set`'s `fire_signal` parameter is dead** (body never reads
  it; ~30 call sites choose TRUE/FALSE for nothing). Real hazard: sites
  calling it without blocking `vc_signal_tag` (xtext.c:3388, 7824, 7884,
  7928, 7986, 9287, 11887, 12097, 12160, 12226) synchronously re-enter
  `adjustment_changed` → `ensure_range` when the clamp moves the value.
  Concrete case: separator drag while scrolled up — if re-wrap shrinks
  `num_lines` below `value + page`, the clamp force-sets
  `anchor_to_bottom = TRUE` (conflating "upper shrank" with "user at
  bottom") and the view teleports to the bottom.
- **`restore_scroll_anchor` subline clamp off-by-one** (xtext.c:12443):
  `g_slist_length (sublines) + 1` allows `subline == count` (top variant at
  12562 is correct); also captured sublines are display rows including
  `extra_lines_above`, so anchors on a badge/day row restore one row off.
- **`remove_top`/`remove_bottom` ignore selection pins** (xtext.c:8443,
  8516) — `ensure_range` honors `sel_pin_*` but live-append pruning can
  free a selection endpoint; degrades to stale/short selection.
- **Adjustment mutation inside the snapshot walk** (xtext.c:7440-7453
  compensation `configure`, 7573-7577 collapse-sweep `calc_lines`):
  value-changed is blocked but `changed` still fires, poking the internal
  scrollbar mid-snapshot; both converge in one extra frame. Documented
  deviation from the "never mutate the adjustment mid-render" rule —
  bounded, but worth moving to an idle if flicker is ever traced here.
- **`/clear N` (partial) is cosmetic in virtual mode:** `remove_top`
  advances the window but DB rows survive and re-materialize on scroll-up.
  Whole-buffer semantics also inherit M2's missing credit.
- **`scrollback_clear` leaves `reactions`/`replies` rows** for the cleared
  channel (scrollback.c:912-927) — stale state re-attaches to re-fetched
  msgids; unbounded orphan growth.
- **Global `UNIQUE(msgid)`** (scrollback.c:181): the same msgid delivered
  to two targets (multi-target PRIVMSG, STATUSMSG variant) saves only once;
  the second channel's copy is display-only and vanishes after
  eviction/restart.
- **`scrollback_update_pending_msgid` vs chathistory race**
  (scrollback.c:885-886): if chathistory stored the real msgid before the
  echo confirm, the UPDATE hits the UNIQUE index and fails — own message
  duplicated this session (pending + chathistory rows), pending copy purged
  next session.
- **WIP msgid mode never clears text-search state** (maingui.c msgid
  branch returns before `gtk_xtext_search` runs): prior highlight-all marks
  stay painted while in msgid mode and after closing the bar from it.
- **Multiline `group_id` not persisted** in the DB: re-materialized group
  members lose group flash/hover/collapse behavior.
- **`gtk_xtext_save` in virtual mode streams only DB rows**
  (xtext.c:6712-6751): on-screen ephemeral notices are absent from saved
  logs.
- **Session-reuse path** (`found_unused`, inbound.c:736-742) may run
  `set_virtual` on a buffer still holding the previous channel's entries,
  mixing foreign entries into the new channel's index space (medium
  confidence — depends on whether part/kick cleared the buffer).
- **lastlog mutates `stamp` after `add234`** (xtext.c:10299/10335) —
  breaks the tree comparator invariant in the lastlog buffer; theoretical.
- **Initial-load ordering** (`ORDER BY timestamp DESC LIMIT ?`,
  scrollback.c:317) lacks the `id` tiebreaker all other queries have; ties
  at the LIMIT boundary are contractually unordered. Same in
  `stmt_newest_msgid`/`stmt_oldest_msgid`, which can hand chathistory the
  wrong tie member as the BEFORE/AFTER anchor.
- **Search-over-DB rescans strip+match the entire DB on every keystroke**
  (xtext.c:8917) — perf only.
- Cosmetic: duplicated `anchor_to_bottom = FALSE` (xtext.c:1289-1291);
  stale comment about `dontscroll` clearing `anchor_to_bottom`
  (xtext.c:10843).

---

## 4. Verified sound (checked and passed)

- **Ordering contract for contiguous loads:** `scrollback_load_range` and
  `stmt_index_of_rowid` both use `(timestamp ASC, id ASC)`, exactly
  matching the tree comparator (stamp, then entry_id, with entry_id ==
  rowid) — prepend/append splices preserve list==tree order in the
  contiguous case, including non-monotonic server-time stamps. M3 is the
  exception, not the rule.
- **Stage 4.5 fixes hold:** `scrollback_db_save` returns −1 on
  `INSERT OR IGNORE` swallow (`sqlite3_changes()==0`); the 2^62 local-id /
  rowid namespace split prevents live collisions (undermined only by
  post-`/clear` rowid reuse, C1); materialize paths gate `total_entries++`
  on `has_db_row` (the skip path of M10 is the residual).
- **Re-materialization identity:** entry_id == rowid is stable across
  evict/reload; msgid re-registered; reply context, reactions, is_user_msg
  rehydrated (redaction is the gap — M9).
- **`kill_ent` pointer hygiene** for pagetop/hover/flash/hilight/scroll
  anchor/hashes/tree/display-cache/ephemeral_count is thorough; the misses
  are `insert_hint`, `hover_react_ent`, `redact_confirm_ent` (C4/C5) and
  the paths that bypass `kill_ent` entirely (C1, C3).
- **Display cache** is keyed by `entry_id` (not pointer), copies its
  subline list, and is safe across evict/re-materialize; the only aliasing
  hole is post-`/clear` rowid reuse (C1).
- **Reentrancy discipline:** every adjustment mutation reachable from
  `adjustment_changed` runs with `vc_signal_tag` blocked; the
  window-unchanged gate in `ensure_range` blocks oscillation loops. (The
  unblocked *other* call sites are the fire_signal finding above.)
- **`gtk_xtext_nth`/`entry_get_line`** handle `LINES_BEFORE_MAT`
  correctly, as do both search-navigation paths and both anchor restores —
  which is what makes M1's two outliers stand out.
- **Selection across the window boundary** (ensure_range path): pins are
  normalized, checked at every boundary kill, cleared on both
  selection-clear paths; no unmaterialized hole can open inside a
  selection because the window is loaded as a contiguous range.
- **Absorptive `lines_before_mat`** in `ensure_range` itself (subtract
  actual on prepend, add actual + day-boundary credit on head-evict) is
  correct — the Phase 5 jitter design works where it is applied; M2 is the
  path that skipped it.
- **Threading:** all scrollback access is main-thread; chathistory
  chunking commits per chunk on the same connection; corruption recovery
  runs only at `scrollback_open`, never under a live buffer.
- **Scrollbar-down ±1.0 tolerance** cannot misfire from estimate noise:
  at the bottom, `entries_after == 0` and the tail is fully materialized,
  so the bottom edge of the range is exact.

---

## 5. Priority order

1. **C1 + C2** — the two reachable crash classes, both matching known
   "infrequent crash" reports. C2 is a small, self-contained maingui fix.
2. **M1** — one-line-per-site coordinate fix; unbreaks every jump feature
   (and the WIP Msg-ID mode depends on it).
3. **M8 + the WIP msgid-mode mark-clearing** — should land with (or
   before) the uncommitted diff.
4. **M2** (+ the M10 resync) — the everyday scrollbar-drift/thumb-pegging
   and spurious chathistory fetches.
5. **M7, M9** — marker and redaction correctness on eviction; small,
   contained.
6. **M5, M6** — tab-switch drift and multiline re-collapse; small fixes.
7. **M3, M4, M11** — the contiguity/below-window/far-jump complex; these
   interact and are best designed together (a "re-center window around
   target" primitive addresses M4 and M11 and removes M3's trigger (c)).
8. C3-C6 and the minor list opportunistically.
