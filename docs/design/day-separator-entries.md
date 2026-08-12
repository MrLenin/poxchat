# Day separators as first-class entries (scroll-model Stage 4a)

**Status:** implemented on branch `stage3b-bottom-anchor` (this worktree).

Executes Stage 4, option 4a, of
[scroll-model-comparison.md](scroll-model-comparison.md). Supersedes the
glued-flag model described there in §3.5.

## Why

Before this change, "the first entry of a new day carries a separator
row" was encoded redundantly in four places that had to stay in
lockstep, with no assertion and no single choke point:

1. `ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY` — read by the renderer
   and the click-zone classifier.
2. `ent->extra_lines_above` — read by `gtk_xtext_find_x` (the single
   hit-test unpack point) and the zone math.
3. `ent->display_lines` — read by ~20 walk/scroll sites via
   `ENT_DISPLAY_LINES`.
4. The B-tree weight (`update_weight234`) — read by `gtk_xtext_nth`
   and `gtk_xtext_entry_get_line`.

Plus two aggregate counters (`buf->num_lines`, `buf->lines_before_mat`)
updated inconsistently across 8 inline decision sites: `link_entry`
updated all six representations; `recalc_day_boundaries` updated two
and deferred; `virt_recenter` and the three `ensure_range` loops
updated four but not `num_lines`; `virt_evict_head` updated four and
left the counters to its caller. Two of the `ensure_range` sites were
set-only (never cleared a stale boundary); the eviction sites were
clear-only, with a `+1` `lines_before_mat` credit hand-managed at each.

Observable bugs of the old model:

- A scroll anchor saved while the bottom viewport edge sat on a day
  row restored one row off (virtual-scrollback-audit.md, known issue:
  the restore clamps `subline_offset` against text sublines only).
- Every y→column consumer had to remember to unpack
  `extra_lines_above`; `gtk_xtext_nth` never did, returning display-row
  sublines that include the separator row.

## The new model

A day separator is its own `textentry`:

- **Zero text** (`str_len == 0`), so `find_x`, word/URL lookup,
  selection, and search all naturally see "nothing here".
- **`TEXTENTRY_FLAG_DAY_SEP`** (reuses bit 0x02; the old
  `TEXTENTRY_FLAG_DAY_BOUNDARY` name is deleted so stale references
  fail to compile).
- **`stamp` = local midnight of its day** (via `xtext_day_start`).
- **Ephemeral**: no DB row, local `entry_id` from
  `LOCAL_ENTRY_ID_BASE`, `TEXTENTRY_FLAG_EPHEMERAL` set,
  `ephemeral_count` incremented — so the Stage 4.5 gating already
  excludes separators from `total_entries`, `db_mat`, and
  window-size accounting with no new code.
- **`display_lines` = 1** (the `text_len <= 0` path of `lines_taken`
  already yields this).

### Ordering does the hard work

`entry_stamp_cmp` orders by `(stamp, entry_id)`. A separator stamped
at midnight sorts:

- **after** every entry of the previous day (their stamps are smaller);
- **before** every entry of its own day (their stamps are larger —
  see the edge case below).

So a chathistory backfill that inserts an *earlier* message of the
same day lands after the separator, and a message of a *previous* day
lands before it — **no flag migration, no successor fix-up**. The old
bidirectional fix-up loop in `link_entry` and all four
`ensure_range`/`recenter` sweep loops are deleted, not rewritten.

**Edge case (accepted for now, resolve eventually):** an entry stamped
exactly 00:00:00 would tie with the separator's stamp and lose the id
tiebreak (DB rowids < `LOCAL_ENTRY_ID_BASE`), putting the separator on
the wrong side. When the successor's stamp equals midnight exactly we
skip creating the separator: a one-second window per day, cosmetic-only
(that day just shows no separator).

*Eventual fix:* teach `entry_stamp_cmp` that on a stamp tie a
`DAY_SEP` entry sorts before a non-separator entry (falling back to the
id compare only when both or neither are separators). Both structures
use the same comparator, so they stay in agreement; the ordering is
in-memory only, so nothing persisted needs migrating. With that in
place the `ent->stamp <= midnight` skip in
`gtk_xtext_maybe_insert_day_sep` becomes `ent->stamp < midnight` and
the window closes. Do it as its own change with an ordering-sanity
pass, since the comparator underpins both the B-tree and every sorted
insert.

### Creation

One helper, `gtk_xtext_maybe_insert_day_sep`, called from
`gtk_xtext_link_entry` after a **real** (non-separator) entry links.
It checks both directions:

- `ent` vs its previous real entry: different local day → insert a
  separator before `ent`.
- `ent` vs its next real entry: different local day → insert a
  separator before that next entry (this covers the prepend join in
  `ensure_range`, where older history loads above the current head).

Guards: pref `hex_gui_day_separator` on; both stamps > 0; no
separator already adjacent; successor stamp strictly greater than the
midnight boundary. Separator linking itself bypasses the helper
(recursion guard on the flag).

### Removal

A separator's lifetime is bounded by its *successor* (the first entry
of its day). To preserve the old model's behavior that the window head
never shows a stale day row:

- **Head**: whenever eviction/removal leaves a separator as
  `text_first`, it is removed too (`remove_top`, `virt_evict_head`
  callers). Its single line credits `lines_before_mat` exactly like
  any other evicted line — the hand-managed `+1` sites die.
- **Tail**: whenever removal leaves a separator as `text_last`, it is
  removed too (`remove_bottom`, `virt_evict_tail` callers). A
  separator is never the last entry.
- **Recenter** (`virt_recenter`): separators are dropped during the
  rebuild (they are synthetic) and recreated by the merge relink.
- **Pref toggle** (`recalc_day_boundaries`): OFF removes all
  separator entries; ON walks the list and inserts at boundaries;
  both end in `calc_lines` as before.
- Removal decrements `ephemeral_count` and never touches
  `mat_first_index` (separators have no DB ordinal).

### What consumers see

- `extra_lines_above` means **reply-context row only** (0 or 1) again;
  `set_reply` loses its `day_boundary ? 2 : 1` special case.
- `gtk_xtext_render_line` renders a separator entry via
  `gtk_xtext_render_day_separator` and returns 1; the old
  flag-and-decrement preamble is gone.
- `gtk_xtext_get_click_zone` returns `XTEXT_ZONE_DAY_SEP` for a
  separator entry up front. The reaction-badge catch-all now also
  requires `ent->extra_lines_below > 0`, so a transiently stale
  sublines list can no longer classify arbitrary clicks as badge
  clicks and swallow link opens (candidate cause of "links in the
  bottom row sometimes not clickable").
- Scroll anchors can land on a separator entry harmlessly — it is in
  `entries_by_id` like any entry, and its subline is 0.
- Save-to-file and selection-copy skip separator entries (synthetic
  content must not produce blank lines in output).
- Search/lastlog/URL paths need no changes: zero text never matches.

## Verification

- Hover and click text immediately above and below a separator; drag
  a selection across one; URL hover near one. Nothing lands on the
  separator, nothing misaligns by one row.
- Scroll so a separator is the bottom viewport edge, switch buffers
  away and back: position restores without the one-row jump.
- Scroll deep history in a multi-day channel: separators appear at
  boundaries during upward materialization (prepend join), disappear
  when their day scrolls out of the window, and survive re-center
  jumps.
- Toggle the "Show day separators" pref both ways with deep history.
- Copy a selection spanning a separator: no blank line in the paste.
- `/clear`, scrollback-limit pruning, and buffer close leak nothing
  (ephemeral_count returns to 0).
