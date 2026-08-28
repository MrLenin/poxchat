# Chathistory Gap Fill — Design

**Date:** 2026-08-14
**Status:** Approved design, not yet implemented
**Depends on:** virtual scrollback (merged), chathistory catchup (merged), per-channel msgid uniqueness `idx_channel_msgid` (merged 34276cdf)

## 1. Problem

The scrollback DB now spans months (post-salvage: ~187k rows back to May), but it has
holes: periods the client was offline, catchup runs that were capped or aborted, and
spans lost to the old DB corruption. Today those holes are invisible and unfillable:

- The only scroll-driven server fetch is `CHATHISTORY BEFORE` anchored at the **oldest
  stored message** (`chathistory_request_older`, chathistory.c:433), fired when the
  scrollbar reaches the absolute top. It extends history before-the-beginning; it can
  never repair a mid-history hole.
- Join-time catchup (`LATEST` + `BEFORE` pagination) does try to bridge back to the
  newest stored message, but the `BEFORE` loop runs **on the active tab only**
  (chathistory.c:1983). Background channels get one `LATEST` batch; their residual gap
  is silently abandoned.
- The bridge target (`sess->catchup_lower_bound`) and all catchup progress are RAM-only.
  Nothing about a gap survives the session.

User-approved behavior (2026-08-14):

1. **Scope:** fill server-side holes; the local-DB materialization path is untouched.
2. **Feel:** visible gap markers in the buffer; auto-fill as the viewport approaches;
   the marker shrinks as messages splice in.
3. **Pre-existing holes:** a one-time heuristic scan produces *candidate* markers,
   verified against the server on approach; verified-empty silences are remembered.
4. **Reconnect:** eagerly close the offline gap with bounded, throttled requests after
   the `LATEST` catchup; anything past the budget becomes a recorded gap for lazy fill.

Scroll-to-top keeps its current meaning (before-the-beginning) unchanged.

## 2. Feasibility facts (from code audit, 2026-08-14)

These make the design cheap; they are why no ordinal migration is needed:

- The DB orders everything by `(timestamp, id)`: `scrollback_load_range`
  (scrollback.c:552), `scrollback_get_index_of_rowid` (scrollback.c:567), and search
  agree. A row inserted *now* with an old timestamp lands at the correct chronological
  ordinal by construction; `OFFSET`-based loads and ordinal math stay consistent.
- The FE already classifies the three splice cases in `gtk_xtext_virt_should_materialize`
  (xtext.c:12226): older than the materialized window → DB-only + `mat_first_index++`;
  inside the window's time span → materialize via sorted insert; below the window past a
  hole → DB-only (`skip_below`). Gap-fill replay reuses these paths as-is.
- `CHATHISTORY BETWEEN` is fully wired on the send side (`chreq.end_ref`,
  dispatch at chathistory.c:183) with zero callers. The response path is
  subcommand-agnostic (`chathistory_process_batch`), so only callers are needed.
- Dedup is three-layered and already correct for refill: `known_msgids` hash →
  `scrollback_session_has_msgid` DB fallback → `INSERT OR IGNORE` against
  `idx_channel_msgid`. Refetching an already-stored span is harmless (all dupes).
- `entry_id == DB rowid` for DB-backed entries, so an exact `mat_first_index`
  re-derivation exists: `scrollback_get_index_of_rowid (db, channel, text_first->entry_id)`.

## 3. Schema: the gap ledger

Per-network scrollback DB, alongside `messages`/`channels`:

```sql
CREATE TABLE IF NOT EXISTS gaps (
    id INTEGER PRIMARY KEY,
    channel_id INTEGER NOT NULL REFERENCES channels(id),
    start_ts INTEGER NOT NULL,   /* newest stored msg BEFORE the hole (exclusive bound) */
    start_msgid TEXT,            /* its msgid, when it has one                          */
    end_ts INTEGER NOT NULL,     /* oldest stored msg AFTER the hole (exclusive bound)  */
    end_msgid TEXT,
    state INTEGER NOT NULL DEFAULT 0,   /* 0=witnessed 1=candidate 2=dead */
    attempts INTEGER NOT NULL DEFAULT 0,
    last_attempt INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_gaps_channel ON gaps(channel_id, start_ts);
```

- **Bounds are anchored to real stored rows** (the flanking messages), exclusive on both
  ends. Both msgid and timestamp are kept: msgid is the preferred CHATHISTORY reference;
  timestamp is the fallback (event-only edges have no msgid; Nefarious msgid-counter
  resets are why the dedup key is already timestamp-qualified) and drives the marker's
  "~3h missing" label and the bootstrap scan.
- **States:** `witnessed` — created from an observed disconnect or a capped/aborted
  catchup; the hole definitely existed. `candidate` — produced by the bootstrap
  heuristic; may be a genuine silence. `dead` — verified empty (or unservable); kept
  forever so the server is never asked again.
- **Invariant:** gaps within a channel are disjoint. `scrollback_gap_record` merges on
  overlap/adjacency at insert time.
- The reconnect flow only ever shrinks a gap from one edge, so **no split operation is
  needed**; fills anchor at an edge (§6).

New scrollback.c API (mirroring existing helpers, all channel-name based, resolved via
`scrollback_get_channel_id`):

```
gint64 scrollback_gap_record (db, channel, start_ts, start_msgid, end_ts, end_msgid, state);
GList *scrollback_gap_list   (db, channel);              /* ordered by start_ts */
void   scrollback_gap_shrink (db, gap_id, new_start_ts, new_start_msgid,
                                          new_end_ts, new_end_msgid);
void   scrollback_gap_set_state (db, gap_id, state);
void   scrollback_gap_touch  (db, gap_id);               /* attempts++, last_attempt=now */
void   scrollback_gap_delete (db, gap_id);
int    scrollback_gap_ordinal (db, channel, gap end bound);  /* COUNT of rows sorting
                                  before (end_ts, end-row) — the gap's position in the
                                  same ordinal space load_range uses */
```

Shrinks/deletes triggered by replay run inside the same transaction as the chunk's
message inserts (`scrollback_begin_transaction` wrapper already exists at
chathistory.c:1407), so a crash can't leave the ledger claiming a hole that is filled
(the safe failure direction — a stale *open* gap merely causes one redundant fetch).

Bootstrap bookkeeping: `ALTER TABLE channels ADD COLUMN gap_bootstrap_done INTEGER
NOT NULL DEFAULT 0`, following the existing in-place migration pattern
(scrollback.c:234).

## 4. Gap lifecycle (owned by chathistory.c)

### Creation — reconnect witness

When the join-time `LATEST` batch for a session completes (`finish_batch_processing`,
LATEST phase):

- `LATEST` returned nothing → no gap (nothing was said while offline).
- The batch's oldest timestamp ≤ `catchup_lower_bound` (i.e. overlaps stored history,
  the existing bridge test at chathistory.c:1253) → contiguous, no gap.
- Otherwise → `scrollback_gap_record (witnessed)` with start = previous
  `scrollback_newest_time`/`scrollback_newest_msgid` (snapshotted before the batch
  lands), end = the batch's oldest message. Recording happens **before** eager close
  starts, so an interrupted catchup leaves the truth in the ledger.

This subsumes `catchup_lower_bound` as persistent state; the in-memory field stays as
the live loop's stop condition.

### Creation — bootstrap scan

Once per channel (`gap_bootstrap_done` latch), on first buffer attach for that channel
(scrollback already open, off the connect hot path). One window-function pass:

```sql
SELECT prev_ts, prev_msgid, timestamp, msgid FROM (
    SELECT timestamp, msgid,
           LAG(timestamp) OVER w AS prev_ts,
           LAG(msgid)     OVER w AS prev_msgid
    FROM messages WHERE channel_id = ?
    WINDOW w AS (ORDER BY timestamp, id))
WHERE prev_ts IS NOT NULL AND timestamp - prev_ts >= ?;
```

Every silence ≥ the threshold (`hex_irc_gapfill_bootstrap_hours`, default 12, 0 =
disabled) becomes a `candidate` gap, merged against existing records. Candidates only
exist between the DB's oldest and newest rows — before-the-beginning remains
scroll-to-top's territory.

### Shrinking and closing

Every replayed batch attributed to a gap fill (§6) updates the record from the edge the
batch attached to, using the batch's actual returned bounds (not the requested ones —
robust against server direction quirks). Closure:

- Remaining span bridged (batch overlaps the far bound, or returned fewer than the
  effective limit while anchored at an edge) → **delete** the record; the marker
  disappears.
- Batch empty → **dead**. For a candidate this is the expected "genuine silence" verify;
  for a witnessed gap it means server retention no longer covers it. Either way the
  marker is removed and the record is kept so the server is never asked about that span
  again. (A permanently-visible "messages lost" marker for dead witnessed gaps was
  considered and rejected as noise; revisit if wanted.)
- `FAIL` handling: §7.

### Eager close on reconnect

Generalizes `chathistory_check_before_catchup` (chathistory.c:1957): after the LATEST
phase (`chathistory_latest_pending == 0`), walk all sessions on the server that have an
open `witnessed` gap ending at the reconnect edge — **active tab first** (preserving
current behavior), then background sessions, one at a time on the existing 3 s spacing
(`CHATHISTORY_BEFORE_INTERVAL`). Each session's loop is the existing BEFORE pagination
with its existing termination ladder (bridge test, stale count, `history_exhausted`),
plus one new rung: a per-channel budget, `hex_irc_gapfill_catchup_budget` (default 500
messages, reusing the `history_catchup_retrieved` counter; `CHATHISTORY_SANITY_LIMIT`
5000 stays as the outer backstop). Budget exhausted → the shrunken record stays in the
ledger for lazy fill; the tab-switch pause/re-dispatch logic is unchanged.

## 5. FE: gap markers

A gap marker is an **ephemeral entry**, exactly the day-separator pattern (Stage 4a):
zero DB row, `TEXTENTRY_FLAG_GAP_MARKER`, ephemeral local id, carrying its ledger
`gap_id` and rendering a labeled rule:

- `witnessed`: `── ~3h 12m missing ── (scroll to load)` → `── loading… ──` while a
  request is in flight.
- `candidate`: subdued styling, `── possible gap (~14h quiet) ──`.
- `dead`: no marker.

Mechanics (all mirrored from day separators):

- **Stamp** = gap `end_ts`; the shared comparator gets the tie-break rule already
  sketched for the day-sep midnight tie: at equal stamp, ephemeral separators sort
  before real entries. (This lands the `entry_stamp_cmp` change that fix needs anyway;
  the day-sep `<= midnight` relaxation itself stays out of scope.)
- **Insertion** via `gtk_xtext_maybe_insert_gap_marker`, consulted at the same points as
  `gtk_xtext_maybe_insert_day_sep` (every real link: `link_entry`, plus the
  `ensure_range`/`recenter` manual-link materialize paths): when two consecutively
  linked entries straddle a live gap record — prev sorts ≤ the gap's start bound and
  next sorts ≥ its end bound — insert the marker between them.
- **Edge invariants:** never `text_first`/`text_last`; dropped at window edges by the
  same sweep as `gtk_xtext_drop_edge_day_sep` (head drops credit `lines_before_mat`,
  never bump `mat_first_index`); recreated on rematerialization. Content consumers
  (selection copy, save, foreach) skip it like day separators.
- **Label refresh:** when chathistory shrinks/closes a record it calls a new
  `fe_gap_updated (sess, gap_id)`; xtext updates the marker's text and estimated line
  span, or unlinks it (record deleted/dead). In-window fills also splice real entries
  around it via the normal sorted-insert path, so the marker visibly "gives way" to
  messages.

The session's gap list is cached in memory (loaded when the buffer attaches, mutated
only through the chathistory-side helpers, which keep DB and cache in step).

## 6. The scroll trigger and fill requests

### Trigger (xtext)

In `gtk_xtext_adjustment_changed`, in the post-`ensure_range` region (xtext.c:1418)
where the DB-exhausted scroll-to-top re-check lives today: compute the viewport's entry
ordinal range (already available from `mat_first_index` + the visible span) and check
the cached gap list — each gap's ordinal from `scrollback_gap_ordinal`, cached and
recomputed only when `total_entries` changes (the batch-end resync point, §8). If a
live (non-dead) gap's ordinal is within **two pages' worth of entries** of the viewport
(wider than `ensure_range`'s one-page margin, because the network is slower than
SQLite), fire a new callback:

```c
xtext->gap_fill_cb (xtext, gap_id, approach_dir, userdata);
```

Debounced per gap: 500 ms arm like the scroll-to-top debounce, plus ledger-side rate
limiting (`last_attempt` + growing backoff on `attempts`, reset on a batch that shrinks
the gap). The ordinal-space check works whether or not the marker entry is currently
materialized — a scrollbar jump into unmaterialized territory still triggers.

Registration mirrors `set_scroll_to_top_callback`; `mg_gap_fill_cb` (maingui.c) resolves
the buffer's own session — **not** `current_sess`, so background-tab gap fill works from
day one — and calls into chathistory. Gated on `prefs.hex_irc_gapfill` and
`serv->have_chathistory`.

### Requests (chathistory)

`chathistory_request_gap_fill (sess, gap, approach_dir)`:

- Primary: `CHATHISTORY BETWEEN <target> <near-side ref> <far-side ref> <limit>`,
  refs msgid-preferred, timestamp fallback; near side = the side the viewport
  approached from, so adjacent content arrives first (per spec, the limit applies from
  the first reference). Limit via the existing `get_effective_limit`.
- Priority `CHREQ_PRI_USER` (same as scroll-to-top — it is user-driven), so it preempts
  background catchup.
- Attribution: the request's gap id is carried on the `chreq` (new field) so batch
  completion knows which record to shrink — no guessing from timestamps. Shrink uses
  the batch's returned bounds and always clamps to the edge it attached to.
- Fallback when the server lacks `BETWEEN` (§7 latch): `BEFORE <gap end ref>` with the
  gap's start as the stop condition — the exact `catchup_lower_bound` pagination pattern,
  per-gap.

`chreq_is_dup` (chathistory.c:113) learns to compare `end_ref`, fixing the latent bug
where two BETWEENs sharing a start reference dedup against each other.

## 7. FAIL handling hardening

Today `proto-irc.c:1357` drops the FAIL code (`word[4]`) and passes only the context;
`chathistory_handle_fail` cannot tell auth failures from bad targets, has no fallback
for non-catchup requests, and re-asks forever. Changes:

- Pass both code and context: `chathistory_handle_fail (serv, code, context)`.
- Attribution: prefer matching `context` against the pending request's target;
  `find_session_with_pending_history` stays as last resort.
- Policy by code:
  - `INVALID_PARAMS` / `UNKNOWN_COMMAND`-ish on a BETWEEN → set a per-server
    `no_chathistory_between` latch; retry the same gap once via the BEFORE fallback.
  - `INVALID_TARGET` / `MESSAGE_ERROR` → per-gap backoff (`attempts`/`last_attempt`);
    after N failures (3) mark the gap `dead`.
  - Auth-required style rejections → per-server session latch: stop all gap-fill and
    scroll-to-top requests until reconnect (addresses the standing persistence-specs
    auth note for the chathistory leg).
- A FAIL on a scroll-to-top BEFORE also stops being silent: same backoff, and
  `history_exhausted` set on repeated failure so the debounce stops re-arming.

## 8. Ordinal integrity (prerequisite fix)

`mat_first_index` is maintained by ±1 increments with no correction path; the `++`
heuristic assumes an out-of-window insert lands adjacent to the window head, which
mid-gap splices violate (a batch landing entirely above the window bumps it by 1 per
entry via repeated `should_materialize` skips — correct — but sorted inserts that land
*at* the head edge and eviction interleavings have known drift potential, audit M-item
history). Fix, independent of everything else and landable first:

- At the existing batch-end resync (`gtk_xtext_calc_lines_virtual_ex`, xtext.c:7232,
  where `total_entries = scrollback_count(...)` already runs): re-derive
  `mat_first_index = scrollback_get_index_of_rowid (db, channel, first DB-backed
  entry's entry_id)` — one indexed COUNT, self-healing, same trust model as
  `total_entries`.
- Debug build: assert old vs re-derived value to surface any remaining incremental
  bugs while testing (behind `XTEXT_VIRT_PERF_LOG` or a sibling define).

## 9. Preferences

| Pref | Default | Meaning |
|---|---|---|
| `hex_irc_gapfill` | 1 | master toggle: markers + scroll fill + eager close |
| `hex_irc_gapfill_bootstrap_hours` | 12 | candidate threshold; 0 disables the bootstrap scan |
| `hex_irc_gapfill_catchup_budget` | 500 | per-channel eager-close budget per reconnect |

Per-request limit reuses `hex_irc_chathistory_lines` / server ISUPPORT via
`get_effective_limit`. No other knobs (YAGNI).

## 10. Implementation order

1. **Ordinal resync** (§8) — standalone, de-risks everything after it.
2. **Gap ledger** (§3): schema, API, unit coverage in the tools/ scrollback suite
   (record/merge/shrink/dead; bootstrap scan SQL against a fixture DB).
3. **Reconnect witness + eager close** (§4): records created/shrunk/closed with the
   budget; background sessions included. Testable server-side before any FE work.
4. **FAIL hardening** (§7) — small, unblocks confident automated requests.
5. **FE markers** (§5): comparator tie-break, marker entry, insertion/drop/refresh.
6. **Scroll trigger + BETWEEN fill** (§6): callback, request path, attribution,
   shrink-on-batch; BEFORE fallback.
7. **Bootstrap scan** (§4) last — it only produces more of what 5/6 already handle.

Each stage is independently mergeable; 1–4 change no rendering behavior.

## 11. Testing

Against Nefarious/X3 (AfterNET):

- **First:** verify Nefarious serves `BETWEEN` (manual `/QUOTE CHATHISTORY BETWEEN …`);
  if not, the BEFORE fallback becomes the primary path — design unchanged.
- Witnessed gap: disconnect while a second client generates traffic; reconnect →
  record created; small gap → eager close deletes it (no marker ever visible on the
  active tab); gap > budget → marker present, scroll-approach fills, marker shrinks
  and disappears.
- Background channel: same, on a non-active tab — closes without ever focusing it.
- Candidate flow: bootstrap on the real May DB → subdued markers; approaching one the
  server can't fill → dead, marker gone, stays gone after restart (no re-query —
  verify with a raw-log).
- Dedup/splice: fill a span partially overlapping stored history → no duplicates,
  ordering intact (`(timestamp, id)` walk), `mat_first_index` assert quiet.
- FAIL legs: nonexistent target, and (if testable) auth-required rejection → latch
  behavior, no request storms in the raw log.
- Restart persistence: open gaps survive; markers reappear; `dead` records suppress
  refetch.
- Perf: `XTEXT_VIRT_PERF_LOG` while scrolling across a filling gap — no synchronous
  stall beyond the known ~0.75 ms/entry materialization cost; trigger fires ahead of
  arrival (two-page margin).

## 12. Out of scope

- Local-DB materialization/trigger changes (`ensure_range` margins, radius, eviction).
- draft/read-marker, draft/multiline interactions.
- The day-sep midnight-tie *skip* relaxation (only the shared comparator tie-break
  lands here).
- A background sweeper that fills all recorded gaps without scrolling — possible later
  add-on on the same ledger.

## 13. Addendum (2026-08-28): Nefarious "seamless sessions" capability map

Source: https://gist.github.com/MrLenin/8d644eb37878d7bcaa91d1a68ae23d94 (fork
`ircv3.2-upgrade`, commits `9bc57d4` attach-cursor, `414b147`/`9fbcb3b` webpush,
`8336ec4` TLS resumption). Server-side facts that bear on this design:

- **One msgid per event across every delivery path** (live, replay, chathistory,
  playback), HLC-seeded and totally ordered. Our three-layer dedup already assumes
  this; the ledger may prefer msgid refs over timestamps with confidence.
- **`draft/persistence` with `attach-cursor`:** `PERSISTENCE ATTACH <profile> [<msgid>]`
  in the registration flight makes the server push *unsolicited* standard
  `chathistory`-type batches (channels, then PMs) from that single global anchor,
  wrapped in an `evilnet.github.io/bouncer-replay` batch. Per-buffer truncation is
  newest-biased and signalled only by msgid discontinuity. Unknown cursor →
  `FAIL PERSISTENCE CURSOR_UNKNOWN`, server falls back to last-activity replay.
- **Simple-client auto-replay** is gated on the client *lacking* `draft/chathistory`,
  so it never fires for us. Without `draft/persistence` we are on the gist's
  "smart client, no cursor" path — which is exactly what §4 models today.

### Impact on this design

1. **Unsolicited replay must be classified as the LATEST phase.** Today
   `chathistory_process_batch` derives `is_catchup` from `sess->catchup_in_progress`,
   which is FALSE for a batch we never requested: the §4 reconnect witness would never
   record, and `chathistory_request_complete` would pop an unrelated pending request.
   Rule: a `chathistory` batch whose `outer_batch` is a `bouncer-replay` wrapper (or
   that arrives with no matching `ch_pending`/in-flight request) is treated as a
   LATEST result for its target — witness check, `catchup_lower_bound` bridge test,
   and gap recording all apply; it must not consume the request queue.
   `inbound_batch_end` needs a `bouncer-replay` case that is a pure container
   (children handled on their own END; wrapper END = "replay complete" → run the
   `chathistory_latest_pending == 0` eager-close entry point once).
2. **Truncation is a witnessed gap by construction.** Discontinuity between stored
   newest and replay oldest is §4's existing test; the gist's per-buffer
   `CHATHISTORY AFTER` fallback is our eager-close/lazy-fill. No new mechanism, but
   the gap's `start_msgid` is the correct AFTER anchor if we ever add AFTER as a
   third request shape (BETWEEN/BEFORE remain sufficient).
3. **Future (not this design): adopt `draft/persistence` + `attach-cursor`.** Add the
   cap; on reconnect send `PERSISTENCE ATTACH <profile> <global newest stored msgid>`
   (global across buffers — `history_msgid_to_timestamp` is target-independent) and
   **suppress** `chathistory_schedule_deferred` + `chathistory_request_targets_on_reconnect`
   when the server advertises `attach-cursor`, since the server-driven replay
   supersedes both. Handle `FAIL PERSISTENCE CURSOR_UNKNOWN` by falling back to the
   current deferred-LATEST path. The gap ledger's per-channel newest msgid is the
   right source for the anchor (min over channels is *not* wanted — the anchor must
   be the globally newest; per-channel holes are then found by discontinuity → gaps).
4. Already aligned, no action: `draft/event-playback`, `batch`, `labeled-response`,
   `server-time`, msgid dedup. `draft/read-marker` stays out of scope (§12);
   `draft/webpush` is mobile-only.

### 13.1 Server-side `draft/persistence` unification plan (context added 2026-08-28)

From the Nefarious "draft/persistence unification" plan (Phases 1–4 shipped;
base protocol is ircv3-specifications#503 + MrLenin gist 814a674c):

- **CAP value tokens** on `draft/persistence`: `replay-control`, `list`, `attach`,
  `attach-cursor`. Feature-detect by token, not by cap presence.
- **Registration timing:** `PERSISTENCE ATTACH <profile> [<msgid>]` is accepted
  **pre-CAP-END only** (UNREG handler slot, SASL-complete window). In our flow that
  means it must be sent from the cap-negotiation path in `inbound.c` after SASL
  success and *before* the `CAP END` at inbound.c:3182/3217 (and the SASL-success
  `CAP END` at proto-irc.c:1117) — not from the 001/376 hooks.
- **Unsolicited `PERSISTENCE STATUS <client> <effective>`** arrives after the last 005
  and before MOTD-end for authenticated clients that negotiated the cap. We have no
  handler; it would currently surface as an unknown-command line. Needs a
  `process_named_msg` case that records `serv->persistence_effective` (this is the
  authoritative "am I on a held bouncer session" signal — better than the
  JOIN-timestamp inference at inbound.c:787 / `bouncer_inferred`).
- **Two wrapper batches on revive**, in order: `draft/persistence` (JOIN/TOPIC/NAMES
  channel-state burst) then `evilnet.github.io/bouncer-replay` (nested per-target
  `chathistory` batches; suppressed when empty). `inbound_batch_add_message`
  (inbound.c:2465) already passes unknown-type batch contents through, so the
  channel-state burst is processed live — correct. But those JOINs will trigger
  `chathistory_schedule_deferred` per channel; once we send an attach cursor, that
  scheduling must be suppressed when inside a `draft/persistence` batch and
  `attach-cursor` was advertised, otherwise every buffer gets LATEST-fetched *and*
  server-replayed (dedup absorbs it, but it doubles reconnect traffic).
- **`PERSISTENCE REPLAY SET OFF`** exists but is moot for us: server auto-replay is
  gated on the client *lacking* `draft/chathistory`; the only replay a
  chathistory client receives is the explicit-cursor one it asked for.
- **Profiles:** a session is a named configuration profile with its own channel
  list; `/JOIN`/`/PART` grow/shrink the active profile; delivery is filtered by the
  profile's effective channel list. Client-side implication: the network config's
  `persistent_server` flag should eventually become "attach profile *name*", and
  our auto-join list is redundant with the profile's list when attached. Out of
  scope here; note for the cap-adoption task in §13 item 3.
