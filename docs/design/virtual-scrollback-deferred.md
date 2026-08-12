# Virtual Scrollback: Deferred Items — Scoping

**Status (2026-08-12):** items 5, 6, 7a, and 8 stage 1 landed
(f1d8c885, 19a6df91, 78afee72, bb6f2366).  Remaining: 1 (per-channel
msgid uniqueness), 2 (pending-msgid race), 3 (group_id persistence),
4 (save-file ephemerals), 7b (SQL prefilter — only if needed), and
8 stage 2 (anchor_to_bottom semantics — scroll-model roadmap).

Date: 2026-08-12
Companion to [virtual-scrollback-audit.md](virtual-scrollback-audit.md)
(which tracks what is already fixed) and
[scroll-model-comparison.md](scroll-model-comparison.md) (the Stage 2-4
scroll-model rearchitecture, a separate track not re-scoped here).

Each item: symptom → mechanism (verified in code) → fix options with a
recommendation → effort (S ≈ <1h, M ≈ half day, L ≈ multi-day) → risk.

---

## 1. Global `UNIQUE(msgid)` drops multi-target copies

**Symptom.** A single PRIVMSG delivered to two targets you're in
(multi-target message, STATUSMSG variant like `@#chan`) shares one
msgid; the second channel's `scrollback_db_save` hits
`UNIQUE INDEX idx_msgid ON messages(msgid)` (scrollback.c:181) and
returns −1.  The message displays live but has no DB row in that
channel: it vanishes on eviction and after restart.

**Fix options.**

- **(a) Per-channel uniqueness** — replace `idx_msgid` with
  `UNIQUE (channel_id, msgid)`.  Migration is a one-off
  `DROP INDEX` + `CREATE UNIQUE INDEX` at `init_database` (cheap; the
  already-dropped duplicates are unrecoverable, nothing to backfill).
  Consequences to audit — every *global* msgid consumer becomes
  ambiguous and must be channel-scoped:
  - `scrollback_has_msgid`, `stmt_update_pending` (already takes
    channel — verify it binds it), redaction-by-msgid,
    `scrollback_get_rowid_by_msgid` (already channel-scoped ✓).
  - `reactions` has its own global `UNIQUE(target_msgid,
    reaction_text, nick)` (scrollback.c:191) and `replies` is keyed by
    msgid alone (scrollback.c:196) — both need `channel_id` in their
    keys for full correctness, or the second channel's copy shares
    reaction/reply state with the first (mostly harmless but wrong).
  - FE side: `entries_by_msgid` is per-buffer, so no change.
- **(b) Keep global uniqueness, tolerate the drop** — document that
  multi-target copies are display-only.  Zero work, current state.

**Recommendation.** (a), but as its own commit series with a test plan
around STATUSMSG (`/msg @#chan`) and multi-target PRIVMSG; do the
`messages` index first and treat reactions/replies keys as a follow-up.
**Effort:** M.  **Risk:** medium — every msgid lookup path must be
re-checked for channel scoping; a miss reintroduces cross-channel
aliasing quietly.

---

## 2. Pending-msgid vs chathistory race (duplicate own message)

**Symptom.** Your own message can appear twice in a session and lose
its DB copy on restart.

**Mechanism (verified).** Sent messages are saved with msgid
`pending:<label>` and confirmed on echo via
`scrollback_confirm_pending` → `scrollback_update_pending_msgid`
(text.c:139-156).  If a chathistory replay stored the *real* msgid
before the echo confirm arrives, the UPDATE hits the unique index and
fails (currently just a `g_warning`).  The pending row survives
(purged only at next session's load, scrollback.c stmt at load time),
so this session has two DB rows — pending + chathistory — and after
eviction both materialize: duplicate display.  The FE entry keyed by
the pending rowid also diverges from the chathistory row's rowid.

**Fix shape (recommended).** In `scrollback_update_pending_msgid`'s
failure path (or its caller):
1. Look up the existing row's rowid for the real msgid.
2. Delete the pending row.
3. Tell the FE: if an entry with the pending rowid is materialized,
   re-key it to the surviving rowid (del234 → change entry_id →
   add234, fix `entries_by_id`, display cache) **or** simply kill it —
   the chathistory copy is authoritative.  Killing is simpler and the
   chathistory dedup usually means the visible entry *is* the pending
   one, so re-keying preserves the user's view better.  Start with
   re-key; fall back to kill if the entry isn't materialized.

Also worth checking: `chathistory_is_duplicate_msgid` seeding
(text.c:326-328 seeds only from the ≤500 initially-loaded messages) —
seeding from `scrollback_has_msgid` per message would close the
upstream overlap window generally.

**Effort:** M (the re-key helper is the meat; a kill-based v1 is S).
**Risk:** medium — touches the echo-confirm path; needs a server that
races chathistory against echo to test (Nefarious + slow echo works).

---

## 3. Multiline `group_id` persistence

**Symptom.** Multiline groups (draft/multiline batches) lose their
grouping — collapse/expand, group flash/hover — after evict +
re-materialize or across restarts.

**Mechanism.** `group_id` lives only on the textentry; the DB schema
has no column.  Group ids come from the *local* id namespace
(≥ 2^62, xtext.c:11768+), so persisting them verbatim would collide
with the next session's freshly-allocated local ids.

**Fix shape (recommended).** Persist as **leader rowid**: add a
nullable `group_leader INTEGER` column (ALTER TABLE, cheap migration);
when saving a grouped line, store the DB rowid of the group's first
line (the leader's own row stores its own rowid).  On materialize,
`ent->group_id = LOCAL_GROUP_BASE_FOR_DB | leader_rowid` — or simpler,
use the leader rowid directly as the group id value (rowids and group
ids only need to be *unique among groups*, and the FE only compares
group ids for equality; verify no code assumes group ids ≥ 2^62).
Save path: `scrollback_db_save` gains a parameter (or a
`scrollback_set_pending_group` setter to avoid touching every caller).

**Effort:** M.  **Risk:** low-medium — schema migration is trivial;
the subtlety is the id-namespace choice and the save-path plumbing
through text.c.

---

## 4. `gtk_xtext_save` omits ephemerals in virtual mode

**Symptom.** "Save text" in a DB-backed buffer writes only DB rows
(xtext.c:6765-6798); on-screen ephemeral lines (notices without DB
rows, redaction notices) are missing from the file.

**Fix shape (recommended).** Merge-walk: while streaming DB batches,
interleave the buffer's materialized ephemeral entries by
`(stamp, id)` — ephemerals are never evicted, so the in-memory list
is the complete set.  Collect `ephemerals = [ent for ent in list if
!ent->has_db_row]` once (they're already sorted), then during the
stream advance a cursor and emit any ephemeral whose stamp precedes
the next DB row.  Emit the remainder at the end.

**Effort:** S-M.  **Risk:** low.  **Priority:** low — cosmetic
completeness of saved logs.

---

## 5. Session-reuse buffer mixing (`found_unused`)

**Symptom (theoretical, now verified plausible).** Joining a channel
can reuse a `<none>` placeholder tab (`find_unused_session`,
inbound.c:663-672: `SESS_CHANNEL` with empty channel name).  The
`found_unused` path (inbound.c:736-742) then runs `scrollback_load`,
which appends DB history and calls `set_virtual` — **without clearing
whatever the placeholder buffer already holds** (server notices,
anything printed to that tab).  `set_virtual` computes
`mat_first_index = total − (mat_count − ephemerals)` over the mixed
contents, and pre-virt local entries aren't flagged EPHEMERAL
(they're created before `HAS_VIRT_DB`), so the window accounting
starts misaligned; old-tab entries also sit in `entries_by_id`.

**Fix shape (recommended).** In the `found_unused` branch, clear the
session's xtext buffer (the `/clear`-all path, which since 21ee6f1e
does full pointer/hash hygiene — but *without* `scrollback_clear`,
so factor the in-memory part out or call `gtk_xtext_clear` on a
buffer that has no `virt_db` yet, which already skips the DB) before
`scrollback_load`.  One call site, no behavior change for the normal
new-tab path.

**Effort:** S.  **Risk:** low.  Verify with: connect, let the
placeholder tab accumulate a line or two, join a channel with DB
history, check the top of the buffer and scroll behavior.

---

## 6. Lastlog mutates `stamp` after `add234`

**Symptom (theoretical).** Lastlog result entries are inserted into
the lastlog buffer's B-tree with the append-time stamp, then
`out->text_last->stamp = msg->timestamp` (xtext.c:10467+ virtual
branch; the non-virtual branch does the same with `ent->stamp`)
mutates the tree's sort key in place.  Later `del234` (pruning a huge
result set) can fail to find the entry → stale tree pointer.

**Fix (recommended).** `gtk_xtext_append` / `append_indent` already
take a stamp parameter (currently passed `0`); pass `msg->timestamp`
(virtual) / the source entry's stamp (non-virtual) and delete both
post-hoc mutations.  Check nothing relied on "0 means now" for the
lastlog display path.

**Effort:** S.  **Risk:** low.

---

## 7. Per-keystroke whole-DB search scan

**Symptom.** In virtual mode every keystroke in the find bar re-runs
`search_virt_scan` (xtext.c:9062): strip-colors + regex/text match
over the entire DB (50k rows ≈ noticeable lag per keystroke).

**Fix options.**

- **(a) Debounce** the `changed` handler (~200 ms one-shot timer in
  `search_handle_change`, reset per keystroke; fire the search on
  expiry).  Kills the per-keystroke cost entirely; orthogonal to the
  scan cost itself.
- **(b) SQL prefilter for non-regex searches** — the dead
  `stmt_search_text` (`text LIKE ?`) can produce a candidate rowid
  set in SQLite (fast, indexed table scan in C) that the precise
  matcher then confirms per-row.  Needs LIKE-escaping of the needle
  and only applies to plain-text, case-insensitive-ASCII searches;
  regex/case-match still full-scan.
- **(c) FTS5** — real index, real complexity (schema, tokenizer,
  sync on redaction/clear).  Not warranted at IRC scrollback sizes.

**Recommendation.** (a) now — it's small and fixes the felt problem;
(b) only if large-DB searches still feel slow after debouncing;
skip (c).  **Effort:** S for (a), M for (b).  **Risk:** low.

---

## 8. `fire_signal` is dead; unblocked `adjustment_set` call sites

**Symptom.** `gtk_xtext_adjustment_set (buf, fire_signal)` never
reads its second argument (~30 call sites choose TRUE/FALSE for
nothing).  Sites that call it **without** blocking `vc_signal_tag`
(separator release ~3388; status-strip paths ~7824/7884/7928/7986;
~9287; ~11887; badge-growth paths ~12097/12160/12226 — line numbers
drift) synchronously re-enter `adjustment_changed` → `ensure_range`
whenever the clamp moves the value.  Concrete misbehavior: separator
drag while scrolled up — if re-wrap shrinks `num_lines` below
`value + page`, the clamp force-sets `anchor_to_bottom = TRUE`
(conflating "upper shrank past the stale value" with "user is at the
bottom") and the view teleports to the bottom.

**Fix shape (recommended, two stages).**
1. Mechanical: drop the parameter everywhere (or repurpose it as
   `block_value_changed` and make TRUE actually block).  Audit the
   ten unblocked sites; wrap the ones where synchronous re-entry is
   wrong (most of them — ensure_range mid-badge-growth is never
   intended).
2. Semantic: make the clamp *not* set `anchor_to_bottom` when the
   value move is caused by `upper` shrinking rather than user intent —
   or defer entirely to Stage 2 of the scroll-model roadmap, where
   `anchor_to_bottom` becomes semantic state and the tolerance-derived
   flag writes disappear.

**Effort:** S-M for stage 1; stage 2 belongs to the roadmap.
**Risk:** medium — blocking changes subtle event ordering; needs the
usual scroll regression pass (wheel, drag, resize, separator drag,
badge growth at various scroll positions).

---

## Suggested order

| # | Item | Effort | Payoff |
|---|------|--------|--------|
| 1 | (6) lastlog stamp | S | closes the last known tree-invariant hole |
| 2 | (5) session-reuse clear | S | closes the last known index-misalignment hole |
| 3 | (7a) search debounce | S | biggest *felt* win of the list |
| 4 | (8.1) fire_signal / blocking audit | S-M | kills the separator-drag teleport |
| 5 | (2) pending-msgid race | M | user-visible duplicate own-messages |
| 6 | (3) group_id persistence | M | multiline UX survives eviction |
| 7 | (1) per-channel msgid uniqueness | M | correctness for multi-target; schema |
| 8 | (4) save-file ephemerals | S-M | cosmetic |

Items 1-4 are a natural next tranche (all S/S-M, no schema changes).
Items 5-7 each want their own branch + test plan; 7 (schema) should
land before or with 2 if both are done, since both touch the msgid
uniqueness semantics.
