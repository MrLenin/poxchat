# draft/persistence Client Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> Revised 2026-09-04 against the current spec revision and the code as of commit 7025f6d2 (the METADATA/persistence.c landing). The 2026-08-28 text assumed the restoration batch would be deferred; it is now handled in wire order, and `src/common/persistence.c` already exists.

**Goal:** Make PoxChat a first-class client of the `draft/persistence` extension against Nefarious: negotiate the cap, parse its value tokens, consume every `PERSISTENCE` reply and `FAIL PERSISTENCE`, expose a `/PERSISTENCE` command, attach to a named profile with a catch-up cursor in the registration flight, consume the server-driven `evilnet.github.io/bouncer-replay` batches correctly, and stop double-fetching history the server already replays.

**Architecture:** Three independent layers, each mergeable alone. (1) *Ingress*: cap + tokens, handlers for every `PERSISTENCE` reply shape (unsolicited `STATUS` included), `FAIL PERSISTENCE`, the `/PERSISTENCE` command, and a classification rule so an unsolicited nested `chathistory` batch is processed as a LATEST-phase catch-up instead of corrupting the request queue. (2) *Egress*: a per-network profile name, a whole-DB newest-msgid query, and `PERSISTENCE ATTACH <profile> [<msgid>]` written immediately before `CAP END` on SASL success. (3) *Suppression*: when the server has accepted a cursor, skip the deferred per-channel `CHATHISTORY LATEST` fan-out and `CHATHISTORY TARGETS`, falling back to them on `FAIL PERSISTENCE CURSOR_UNKNOWN`.

**Tech Stack:** C11, GLib, SQLite (scrollback), IRCv3 caps/batches. Windows MSBuild (Release|x64 only). Standalone `cl`-built harnesses exist for scrollback-layer code (`tools/build-gap-test.ps1`) and are the model for new ones; protocol code is verified in-app against Nefarious/X3 (AfterNET) by the user.

**Spec:** the work-in-progress extension text is snapshotted at `.superpowers/sdd/2026-08-28-draft-persistence-client/spec.md` (source: https://gist.github.com/MrLenin/491f87ea4d95625a90ce525a804dbddb). It is the binding authority; read it before any task. Background: `docs/design/2026-08-14-chathistory-gap-fill.md` §13 + §13.1.

## Already landed (do not redo)

- `src/common/persistence.c/.h` (commit 7025f6d2): `persistence_match()` — namespace-tolerant matcher that strips `draft/` and vendor (`host.tld/`) prefixes and returns the sub-path after the `persistence` stem (`""` for the bare name, `"hold"` for `.../persistence/hold`, NULL if unrelated); `persistence_is_batch_type()`; `persistence_handle_metadata()` consuming the server-managed `hold` key into `serv->persistence_hold(_known)`; `persistence_reset()` called from the disconnect path in `server.c`.
- `inbound_metadata()` parses `draft/metadata-2` `METADATA` notifications (proto-irc.c dispatches the 8-char verb in the `len >= 5` switch).
- The `draft/persistence` restoration batch is handled **in wire order** in `inbound_batch_add_message()` (commit a6017e12): numerics 332/333/353/366 never pass through the batch collector, so nothing in that batch is deferred; a self-JOIN for a channel this connection already joined is an attach echo and is consumed on the spot (msgid tracked, `ignore_names` set); everything else runs the live handlers. `inbound_ujoin()` decides "restored" from the JOIN's @time against the last disconnect and records the join as a plain JOIN row drawn as the self-join banner while current. **Do not touch that flow.**

## Global Constraints

- C11, tabs for indentation, `module_action()` naming, GLib allocation (`g_new0`/`g_free`/`g_strdup`), `:1` bitfields for server flags (CLAUDE.md). All user-visible strings wrapped in `_()`; every persistence line printed to the server tab starts with `Persistence` (e.g. `Persistence: ...`, `Persistence error ...`, `Persistence replay: ...`) via the existing `persistence_print()` helper in persistence.c.
- **Namespace rule.** Nefarious is moving everything that is not in the upstream spec into its `evilnet.github.io/` vendor namespace, and the ratified spec drops `draft/`. The literal string `"draft/persistence"` may appear in exactly one place: the `supported_caps[]` entry (the spec mandates the draft cap name on the wire while WIP). Every other comparison of a persistence-related name — cap name at ACK/NAK, cap value tokens, batch types, metadata keys, FAIL codes — goes through `persistence_match()` / `persistence_is_bare_name()` / `persistence_strip_namespace()` from persistence.h. The `evilnet.github.io/bouncer-replay` batch type is matched as `persistence_strip_namespace(type)` equal to `"bouncer-replay"`.
- Build: Release|x64 via the 64-bit MSBuild host (the 32-bit host OOMs on resources.c). From Git-Bash at the **worktree** root:
  ```bash
  MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe"
  "$MSBUILD" win32/poxchat.sln //p:Configuration=Release //p:Platform=x64 //p:PreferredToolArchitecture=x64 "//p:YourPython3Path=C:\Python314\\" //v:minimal //nologo 2>&1 \
    | grep -E "error C[0-9]|error [A-Z]" | grep -v "jansson\|lua.h\|cffi\|MSB3073\|MSB3027\|MSB3021\|python313"
  ```
  Empty output = success **and** the exe mtime must advance. In this worktree the output tree is `C:\Users\johne\source\repos\hexchat\.claude\worktrees\poxchat-build-gtk4\` (the props file's `$(SolutionDir)..\..\poxchat-build-gtk4`), exe at `...\poxchat-build-gtk4\x64\rel\poxchat.exe`, generated `config.h` at `...\poxchat-build-gtk4\x64\lib\config.h` (the harness scripts need it — build the solution once before a harness). `LNK1181 python313.lib` from the python plugin is pre-existing noise, not a failure. The `YourPython3Path` override is required: tracked `win32/poxchat.props` points at CI's `C:\Python313`, this machine has `C:\Python314`. Never commit `win32/poxchat.props`.
- Wire facts (spec, verbatim): CAP name `draft/persistence`; value tokens `replay-control`, `profile`, `attach`, `detach`, `list`, `attach-cursor` (clients MUST tolerate unknown tokens; absence of a token proves nothing); `PERSISTENCE ATTACH <profile> [<msgid>]` is accepted **between SASL completion and CAP END only**; the unsolicited `PERSISTENCE STATUS <client-setting> <effective-setting>` arrives after the last 005 and before 376/422; `FAIL PERSISTENCE <code> [<context>] :<text>` with codes `ACCOUNT_REQUIRED`, `INVALID_PARAMETERS`, `INTERNAL_ERROR`, `CANNOT_DETACH`, `NO_SUCH_SESSION`, plus `CURSOR_UNKNOWN` (attach-cursor); batch types `draft/persistence` (JOIN + 332/333/353/366 per channel, wire order) and `evilnet.github.io/bouncer-replay` (outer wrapper; inner per-target `chathistory` batches carry the wrapper's ref as `outer_batch`; an inner batch opener carrying the `draft/chathistory-end` tag means that target's replay is complete, its absence means newest-biased truncation).
- Server auto-replay never fires for a client that negotiated `draft/chathistory` **unless** that client supplied an ATTACH cursor. So the bouncer-replay batch appears only after Task 5 goes live, but Tasks 1–2 must already tolerate it.
- Every `CAP END` guard site stays consistent: we do **not** add a new `waiting_on_*` gate. ATTACH is pipelined immediately before the SASL-success `CAP END`; the server holds registration until it has processed the flight, so no reply is awaited.
- Nothing here may run the GUI or connect to a network: implementers verify by building the solution and by the `cl`-built harnesses the tasks specify. Live verification against AfterNET is listed per task for the user and is **not** an implementer step.
- Commit after each task with the message given, ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

## Roadmap after this plan (not tasks here)

| Item | Depends on | Notes |
|---|---|---|
| Gap-fill plan Task 3 (reconnect witness) consuming the unsolicited classification | Task 2 | Truncation in server replay = witnessed gap by construction (spec §13 item 2) |
| `PERSISTENCE PROFILE LIST` → profile picker in the servlist dialog instead of a free-text field | Task 4 | Needs a pre-connect or connected round trip; UX only |
| Reconcile auto-join favourites with the profile's channel list | Task 5 | Server-side profile list is authoritative when attached; our favlist path is intentionally *not* gated |
| `draft/read-marker` cross-device read state | — | Separate spec; out of scope |
| `PERSISTENCE REPLAY SET` UI | — | The `/PERSISTENCE REPLAY` passthrough (Task 1) is enough for a chathistory-capable client |

## Self-review notes

- Spec coverage: cap + tokens (T1), STATUS/SET/REPLAY/PROFILE/ATTACH/DETACH/LIST replies and FAIL codes (T1), auto-rejoin suppression on cap presence (T1), restoration batch (landed), bouncer-replay wrapper + unsolicited chathistory (T2), attach cursor (T3+T5), profile selection at registration (T4+T5), server-managed metadata keys (landed).
- Names used across tasks: `persistence_strip_namespace`, `persistence_is_bare_name`, `persistence_parse_cap_value`, `persistence_handle_reply`, `persistence_handle_fail` (T1) ← proto-irc/inbound/outbound; `chathistory_batch_is_unsolicited`, `chathistory_begin_unsolicited_catchup`, `chathistory_replay_wrapper_end`, `catchup_enter_latest_phase` (T2); `scrollback_get_global_newest_msgid` (T3) ← T5; `serv->persist_profile` (T4) ← T5; `persistence_send_attach`, `persistence_server_drives_replay` (T5) ← chathistory.c.
- Known unverifiable offline: Task 2 and Task 5 behaviour against a live server. Their briefs list the user's verification steps; the implementer's evidence is a clean build plus the harness where one exists.

---

### Task 1: Cap negotiation, value tokens, every `PERSISTENCE` reply, `FAIL PERSISTENCE`, auto-rejoin suppression, `/PERSISTENCE` command

Request the cap, record which value tokens the server offers, consume every reply shape the spec defines (the unsolicited STATUS line is the authoritative "the server holds my session" signal), route `FAIL PERSISTENCE` to a handler, suppress client-driven auto-rejoin on cap presence as the spec requires, and give the user a `/PERSISTENCE` command. A `cl`-built harness exercises the pure parsing.

**Files:**
- Modify: `src/common/poxchat.h` (server `persistence_*` bitfield block, right after `persistence_hold:1;`)
- Modify: `src/common/persistence.h`, `src/common/persistence.c`
- Modify: `src/common/inbound.c` (`supported_caps[]`, `inbound_toggle_caps`, the CAP LS value-token parse loop next to the `draft/account-registration` block, the reconnect auto-join gate `if (serv->persistent_server || serv->bouncer_inferred)`)
- Modify: `src/common/proto-irc.c` (`#include "persistence.h"`; the `len >= 5` switch in `process_named_msg`; the `FAIL` case; `process_named_servermsg`)
- Modify: `src/common/outbound.c` (`cmd_persistence`, `xc_cmds[]` entry)
- Create: `tools/persistence-test.c`, `tools/build-persistence-test.ps1`

**Interfaces:**
- Produces (poxchat.h server struct, after `persistence_hold:1;`):
  ```c
	unsigned int have_persistence:1;			/* draft/persistence ACKed */
	unsigned int persistence_tok_replay_control:1;	/* CAP value tokens (spec: hints, not an inventory) */
	unsigned int persistence_tok_profile:1;
	unsigned int persistence_tok_attach:1;
	unsigned int persistence_tok_detach:1;
	unsigned int persistence_tok_list:1;
	unsigned int persistence_tok_attach_cursor:1;
	unsigned int persistence_status_known:1;	/* saw PERSISTENCE STATUS this connection */
	unsigned int persistence_effective:1;		/* ...and effective-setting was ON */
	unsigned int persistence_attached:1;		/* sent PERSISTENCE ATTACH this connection (Task 5 sets it) */
	unsigned int persistence_cursor_sent:1;		/* that ATTACH carried a msgid cursor (Task 5 sets it) */
  ```
- Produces (persistence.h):
  ```c
  /* Strip "draft/" and vendor ("host.tld/") prefixes from an IRCv3 name; loops so
   * "vendor.tld/draft/x" resolves to "x".  Returns a pointer into name. */
  const char *persistence_strip_namespace (const char *name);
  /* TRUE when name is the bare persistence name in any namespace
   * ("draft/persistence", "persistence", "evilnet.github.io/persistence"). */
  gboolean persistence_is_bare_name (const char *name);
  /* CAP LS value of the cap: comma-separated optional-verb tokens, each possibly
   * namespaced.  Sets serv->persistence_tok_*; unknown tokens are ignored. */
  void persistence_parse_cap_value (server *serv, const char *value);
  /* ":server PERSISTENCE <args>" — args is everything after the verb, e.g.
   * "STATUS DEFAULT ON" or "PROFILE mobile channels :#a,#b".  Consumes every
   * reply shape in the spec; unknown shapes print the raw args. */
  void persistence_handle_reply (server *serv, const char *args, time_t stamp);
  /* ":server FAIL PERSISTENCE <code> [<context>] :<text>" */
  void persistence_handle_fail (server *serv, const char *code, const char *context,
                                const char *text, time_t stamp);
  ```
  `persistence_is_batch_type()` is replaced by `persistence_is_bare_name()` (rename; one call site in inbound.c). The existing static `strip_namespace()` becomes the exported `persistence_strip_namespace()`. `persistence_reset()` additionally clears every new bitfield above.
- Consumes: `EMIT_SIGNAL_TIMESTAMP`, `XP_TE_SERVTEXT` (through `persistence_print`), `tcp_sendf`.

- [ ] **Step 1: Server fields and persistence.h**

Add the bitfields above to poxchat.h. Add the declarations above to persistence.h (keep the existing ones; document each). Rename `persistence_is_batch_type` → `persistence_is_bare_name` here, in persistence.c, and at its single call site in `inbound_batch_add_message`.

- [ ] **Step 2: persistence.c — token parse, reply handler, FAIL handler**

`persistence_parse_cap_value`: `g_strsplit (value, ",", 0)`; for each non-empty token compare `persistence_strip_namespace (token)` case-insensitively against `replay-control`, `profile`, `attach`, `detach`, `list`, `attach-cursor` and set the matching `persistence_tok_*`; ignore anything else silently (spec: clients MUST tolerate unknown tokens).

`persistence_handle_reply`: tokenise `args` on single spaces into up to 8 words, skipping empty tokens; a token that begins with `:` starts the trailing parameter — everything after that colon (spaces included) is one word. Subcommand words are matched case-insensitively. Print with `persistence_print (serv, stamp, ...)` (server tab) and update state:

| Shape (after `PERSISTENCE`) | Effect |
|---|---|
| `STATUS <client> <effective>` | `persistence_status_known = TRUE; persistence_effective = (effective ≡ "ON")`; print `_("Persistence: preference %s, effective %s")` |
| `STATUS <x>` (legacy one-argument form) | same, with `<x>` as effective; print `_("Persistence: effective %s")` |
| `SET <arg>` | `_("Persistence: preference set to %s")` |
| `REPLAY STATUS <client> <effective>` | `_("Persistence replay: preference %s, effective %s")` |
| `REPLAY SET <arg>` | `_("Persistence replay: preference set to %s")` |
| `PROFILE ENDOFLIST` | `_("Persistence: end of profile list")` |
| `PROFILE CREATED <name> [parent=<p>]` | `_("Persistence: profile %s created (%s)")` with the attribute text or `default` |
| `PROFILE DELETED <name>` | `_("Persistence: profile %s deleted")` |
| `PROFILE RENAMED <old> <new>` | `_("Persistence: profile %s renamed to %s")` |
| `PROFILE <name>` or `PROFILE <name> <k=v> ...` (LIST line: no third word, or the third word contains `=`) | `_("Persistence: profile %s")` / `_("Persistence: profile %s (%s)")` with the attributes joined by spaces |
| `PROFILE <name> <key> :<value>` | `_("Persistence: profile %s: %s = %s")` |
| `PROFILE <name> <key>` (no trailing parameter) | `_("Persistence: profile %s: %s unset")` |
| `ATTACH <profile>` | `_("Persistence: attached to profile %s")` |
| `DETACH OK` | `_("Persistence: detached, the session was released")` |
| `DETACH NOSESSION` | `_("Persistence: no session to detach")` |
| anything else (`LIST`, `SESSION ...`, `ENDOFLIST`, unknown) | `_("Persistence: %s")` with the raw args |

`persistence_handle_fail`: compare `persistence_strip_namespace (code)` case-insensitively; print `_("Persistence error %s: %s")` (code, text) or `_("Persistence error %s (%s): %s")` when a context is present. Leave a clearly marked hook for Task 5: `/* Task 5: CURSOR_UNKNOWN re-arms the deferred LATEST fan-out; ACCOUNT_REQUIRED / INVALID_PARAMETERS clear persistence_attached. */` — Task 1 only prints.

`persistence_reset`: also clear `have_persistence`, every `persistence_tok_*`, `persistence_status_known`, `persistence_effective`, `persistence_attached`, `persistence_cursor_sent`.

- [ ] **Step 3: inbound.c — request the cap, parse its value, toggle, auto-rejoin gate**

`supported_caps[]`: add `"draft/persistence",` after `"draft/oper-tag",` (the only literal occurrence allowed).

`inbound_toggle_caps`: `else if (persistence_is_bare_name (extension)) serv->have_persistence = enable;`

`inbound_cap_ls` value-token loop, next to the `draft/account-registration` block and following its pattern exactly (no `continue` — the cap must still be requested):
```c
		/* IRCv3 draft/persistence — the value advertises optional verbs.
		 * Format: draft/persistence=replay-control,profile,attach,attach-cursor */
		if (persistence_is_bare_name (extension) && value)
			persistence_parse_cap_value (serv, value);
```

Reconnect auto-join gate (the `if (serv->persistent_server || serv->bouncer_inferred)` line): add `|| (serv->have_persistence && serv->persistence_effective)` and extend the comment: the spec's Client behaviour section says a client that negotiated the cap MUST NOT send JOIN for channels it remembers from a previous connection, because the server delivers them in the restoration burst. The server only restores when it holds a session, and it reports that through the unsolicited `PERSISTENCE STATUS` (after 005, before 376 — so known by the time this gate runs) with effective-setting ON; an unauthenticated connection gets no STATUS and no session, and an account with persistence OFF has no session either, so in both cases the remembered channels are rejoined exactly as before this extension (the "respect what the server delivers" half of the rule is satisfied: the server delivers nothing). Channels a restoration burst did *not* include are never rejoined — they were parted from another client or filtered by the profile's channel list. The favlist path below stays ungated.

- [ ] **Step 4: proto-irc.c — route replies (prefixed and bare) and FAIL**

`#include "persistence.h"` next to the `chathistory.h` include.

In `process_named_msg`'s `else if (len >= 5)` switch, after the `case WORDL('M','E','T','A'):` block:
```c
		case WORDL('P','E','R','S'):
			/* PERSISTENCE - draft/persistence reply (STATUS is also sent
			 * unsolicited between 005 and 376).  Everything after the verb
			 * goes to one parser so the bare form can share it. */
			if (len == 11 && g_ascii_strcasecmp (type, "PERSISTENCE") == 0)
			{
				persistence_handle_reply (serv, word_eol[3], tags_data->timestamp);
				return;
			}
			goto garbage;
```

`process_named_servermsg` (servers that omit the source prefix), after the `MARKREAD ` branch:
```c
	if (!strncasecmp (buf, "PERSISTENCE ", 12))
	{
		persistence_handle_reply (sess->server, buf + 12, tags_data->timestamp);
		return;
	}
```

`FAIL` case: add a sibling to the existing `CHATHISTORY` / `REDACT` / `BATCH` chain:
```c
			else if (g_ascii_strcasecmp (word[3], "PERSISTENCE") == 0)
			{
				/* :server FAIL PERSISTENCE <code> [<context>] :<text> — the
				 * context is present when the trailing parameter is not
				 * word[5]. */
				const char *ctx = (trailing_index (word_eol) > 5 && word[5][0]) ? word[5] : NULL;
				persistence_handle_fail (serv, word[4], ctx, text, tags_data->timestamp);
				return;
			}
```
(`text` is already the stripped trailing parameter at that point.)

- [ ] **Step 5: outbound.c — `/PERSISTENCE`**

Add `cmd_persistence` next to `cmd_metadata`, following its shape. With no argument print usage:
```
Usage: /PERSISTENCE STATUS | GET                 - show the held-session state
       /PERSISTENCE SET <ON|OFF|DEFAULT>          - account persistence preference
       /PERSISTENCE REPLAY GET | SET <ON|OFF|DEFAULT>
       /PERSISTENCE PROFILE LIST | CREATE <name> [FROM <parent>] | DELETE <name> | RENAME <old> <new> | GET <name> <key> | SET <name> <key> <value|DEFAULT>
       /PERSISTENCE ATTACH <profile>              - only valid before registration completes
       /PERSISTENCE DETACH [<session-id>]
       /PERSISTENCE LIST
```
Otherwise send `tcp_sendf (serv, "PERSISTENCE %s\r\n", word_eol[2]);` verbatim — the spec says servers MUST NOT refuse the command from clients that did not negotiate the cap, so no local gating and no warning. Register `{"PERSISTENCE", cmd_persistence, 1, 0, 1, N_("PERSISTENCE <STATUS|GET|SET|REPLAY|PROFILE|ATTACH|DETACH|LIST> [...], manage the server-held session (draft/persistence)")}` in `xc_cmds[]` **between the `PART` and `PING` entries** — the table is bsearch'd, so alphabetical placement is mandatory (see `/GAPS` history).

- [ ] **Step 6: Harness — `tools/persistence-test.c` + `tools/build-persistence-test.ps1`**

Clone `tools/build-gap-test.ps1` (keep the config.h/OpenSSL/deps discovery) but compile only `tools/persistence-test.c` and `src/common/persistence.c`, output `tools\out\persistence-test.exe`. The test file stubs whatever persistence.c links against outside itself (at minimum `text_emit`, whose signature is in `text.h` — capture the `a` argument into a static buffer; check `EMIT_SIGNAL_TIMESTAMP` in poxchat.h for the exact call), builds a `server` with `g_new0`, `p_cmp = g_ascii_strcasecmp`, `nick = "me"`, and asserts with a `CHECK(cond, msg)` macro (exit 1 on the first failure, print `ok` and exit 0 at the end):

1. `persistence_match`: `"draft/persistence"` → `""`; `"persistence"` → `""`; `"persistence/hold"` → `"hold"`; `"draft/persistence/hold"` → `"hold"`; `"evilnet.github.io/persistence/hold"` → `"hold"`; `"evilnet.github.io/draft/persistence"` → `""`; `"draft/persistence-foo"` → NULL; `"avatar"` → NULL; `""`/NULL → NULL.
2. `persistence_strip_namespace`: `"evilnet.github.io/bouncer-replay"` → `"bouncer-replay"`; `"draft/x"` → `"x"`; `"plain"` → `"plain"`; `"nodot/x"` → `"nodot/x"`.
3. `persistence_parse_cap_value ("replay-control,profile,attach,evilnet.github.io/attach-cursor,bogus")` sets exactly replay_control, profile, attach, attach_cursor.
4. `persistence_handle_reply` for each row of the Step 2 table, asserting the captured text and, for STATUS, `persistence_status_known`/`persistence_effective` (`"STATUS DEFAULT ON"` → effective TRUE; `"STATUS OFF OFF"` → FALSE).
5. `persistence_handle_metadata (serv, "me", "hold", "1", 0)` sets hold; a repeat produces no output; `"0"` clears it; NULL value clears `_known`.
6. `persistence_handle_fail (serv, "CANNOT_DETACH", "DETACH", "Connection class enforces persistence", 0)` and a context-less call print the two formats.

Build: `powershell -ExecutionPolicy Bypass -File tools/build-persistence-test.ps1` then run `tools/out/persistence-test.exe`. Expected `ok`, exit 0.

- [ ] **Step 7: Build the solution**

Run the Global Constraints build command. Expected: empty output, exe mtime advanced.

- [ ] **Step 8: Commit**

```bash
git add src/common/persistence.c src/common/persistence.h src/common/poxchat.h src/common/inbound.c src/common/proto-irc.c src/common/outbound.c tools/persistence-test.c tools/build-persistence-test.ps1
git commit -m "persistence: negotiate draft/persistence, parse value tokens, consume PERSISTENCE replies and FAIL, add /PERSISTENCE

Requests the cap; records the replay-control/profile/attach/detach/list/
attach-cursor tokens (namespace-tolerant); routes every PERSISTENCE reply
shape in the spec (unsolicited STATUS included) and FAIL PERSISTENCE to
persistence.c; suppresses reconnect auto-rejoin on cap presence as the
spec requires; adds /PERSISTENCE as a documented passthrough. cl-built
harness covers the matcher, token parse and reply parser.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

**User verification later (not an implementer step):** connect with SASL to AfterNET; the raw log shows `draft/persistence=` in CAP LS and in CAP ACK; a `Persistence: preference X, effective Y` line precedes the MOTD; with effective ON, `/discon` + `/reconnect` sends no client JOINs; `/PERSISTENCE BOGUS` yields a `Persistence error INVALID_PARAMETERS` line and no `GARBAGE:`.

---

### Task 2: Wrapper batches and unsolicited-chathistory classification

Make `evilnet.github.io/bouncer-replay` a first-class container, and make `chathistory_process_batch` treat a `chathistory` batch we did not request as a LATEST-phase catch-up for its target (witness/bridge logic runs; request queue untouched; `history_exhausted` never poisoned by a replay's `draft/chathistory-end`).

**Files:**
- Modify: `src/common/inbound.c` (`inbound_batch_end` type dispatch — the `/* TODO: Handle other batch types` block)
- Modify: `src/common/chathistory.c` (`send_deferred_latest`, `chathistory_process_batch`, `finish_batch_processing`, the `chathistory_chunk_state` struct, new helpers next to `find_session_for_target`), `src/common/chathistory.h`

**Interfaces:**
- Produces (chathistory.h):
  ```c
  /* TRUE if this chathistory batch was not requested by us (server-driven replay:
   * nested in a bouncer-replay wrapper, or no request in flight for the session). */
  gboolean chathistory_batch_is_unsolicited (server *serv, batch_info *batch, session *sess);
  /* Put sess into LATEST-phase catch-up state without sending anything. */
  void chathistory_begin_unsolicited_catchup (session *sess);
  /* Called at the END of an evilnet.github.io/bouncer-replay wrapper. */
  void chathistory_replay_wrapper_end (server *serv);
  ```
- Consumes: `batch_info.outer_batch`, `batch_info.chathistory_end`, `serv->active_batches`, `sess->ch_active`, `sess->history_loading`, `serv->chathistory_latest_pending`, `chathistory_check_before_catchup` (static, same file), `persistence_is_bare_name` / `persistence_strip_namespace` (Task 1).

- [ ] **Step 1: Factor the catch-up entry state out of `send_deferred_latest`**

Read `send_deferred_latest` completely. Everything it does *before* building and dispatching the request — the newest-stored snapshot refresh from the DB, `catchup_prev_newest_time/msgid`, `catchup_gap_id = 0`, `catchup_in_progress`, `catchup_is_before`, the lower-bound computation — moves verbatim into a new static `catchup_enter_latest_phase (session *sess)` defined above it, and `send_deferred_latest` calls it. Behaviour identical.

- [ ] **Step 2: Classification and entry helpers**

Next to `find_session_for_target`:
```c
gboolean
chathistory_batch_is_unsolicited (server *serv, batch_info *batch, session *sess)
{
	if (batch->outer_batch && serv->active_batches)
	{
		batch_info *outer = g_hash_table_lookup (serv->active_batches, batch->outer_batch);
		if (outer && outer->type &&
		    g_ascii_strcasecmp (persistence_strip_namespace (outer->type), "bouncer-replay") == 0)
			return TRUE;
	}
	return sess->ch_active == NULL && !sess->history_loading;
}

void
chathistory_begin_unsolicited_catchup (session *sess)
{
	if (!sess || !sess->server)
		return;
	if (sess->catchup_in_progress)
		return;			/* already in our own LATEST/BEFORE loop — leave it */
	catchup_enter_latest_phase (sess);
	sess->server->chathistory_latest_pending++;
}

void
chathistory_replay_wrapper_end (server *serv)
{
	/* Every nested chathistory batch already ran its LATEST-phase
	 * completion (decrementing latest_pending).  If the wrapper had no
	 * chathistory children nothing is pending; kick the eager BEFORE pass
	 * exactly once either way. */
	if (serv->chathistory_latest_pending == 0)
		chathistory_check_before_catchup (serv);
}
```
`#include "persistence.h"` in chathistory.c. Declare the three in chathistory.h next to `chathistory_process_batch`. `catchup_enter_latest_phase` must be visible above these (forward-declare at the top of chathistory.c with the other forward declarations).

- [ ] **Step 3: Use it in `chathistory_process_batch` / `finish_batch_processing`**

In `chathistory_process_batch`, right after `is_catchup` is computed:
```c
	unsolicited = chathistory_batch_is_unsolicited (serv, batch, sess);
	if (unsolicited && active_gap_id == 0)
	{
		chathistory_begin_unsolicited_catchup (sess);
		is_catchup = sess->catchup_in_progress;
	}
```
(declare `gboolean unsolicited;` with the other locals). Add `unsigned int unsolicited:1;` to `chathistory_chunk_state`; set it wherever the chunk/sync state is initialised (both the chunked and the synchronous branches — read the function to find every initialisation).

`history_exhausted` must never be set from an unsolicited batch's `draft/chathistory-end`: the tag on a replay opener means "this target's replay is complete", not "no older history exists". Guard both assignments — the empty-batch branch in `chathistory_process_batch` (`if (batch->chathistory_end && active_gap_id == 0)`) and the one in `finish_batch_processing` (`if (chunk->chathistory_end && chunk->gap_id == 0)`) — with `&& !unsolicited` / `&& !chunk->unsolicited`.

`chathistory_request_complete (sess)` frees `ch_active` and dispatches `ch_pending`; for an unsolicited batch there is no `ch_active`, and dispatching a pending *user* request mid-replay would interleave. Skip it for unsolicited batches in both places it is called for a batch (the empty-batch branch and `finish_batch_processing`). In the empty-batch branch also skip the "server may not recognize our msgid → resend LATEST on timestamps" fallback for unsolicited batches (we sent nothing to fall back from); the LATEST-phase bookkeeping that the non-empty case performs (`chathistory_latest_pending--` → `chathistory_check_before_catchup`) must still run so the counter balances — read that code path and make the empty unsolicited case reach it.

- [ ] **Step 4: Container cases in `inbound_batch_end`**

Replace the `/* TODO: Handle other batch types` comment block with:
```c
		else if (persistence_is_bare_name (batch->type))
		{
			/* draft/persistence channel-state burst.  Handled in wire order
			 * by inbound_batch_add_message (numerics never come through the
			 * collector); nothing to flush here. */
		}
		else if (g_ascii_strcasecmp (persistence_strip_namespace (batch->type), "bouncer-replay") == 0)
		{
			/* Outer wrapper around per-target chathistory batches.  Each
			 * child already ran at its own BATCH -ref; the wrapper END is
			 * the single "replay complete" point. */
			chathistory_replay_wrapper_end (serv);
		}
		/* TODO: "netjoin"/"netsplit": collapse join/quit messages */
```

- [ ] **Step 5: Build**

Global Constraints build command. Expected: empty output.

- [ ] **Step 6: Commit**

```bash
git add src/common/inbound.c src/common/chathistory.c src/common/chathistory.h
git commit -m "chathistory: classify unsolicited batches as LATEST-phase; handle the bouncer-replay wrapper

A chathistory batch nested in evilnet.github.io/bouncer-replay (or with no
request in flight) enters catch-up state instead of popping an unrelated
pending request, and its draft/chathistory-end never marks the channel's
history exhausted. draft/persistence and bouncer-replay batch types become
explicit containers; the wrapper END kicks the BEFORE pass once.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

**User verification later:** fresh connect, join a channel with recent traffic → the deferred `CHATHISTORY LATEST` still lands; scroll to top → `BEFORE` fires; `/quote CHATHISTORY LATEST #chan * 5` by hand (a batch with no `ch_active`) renders, is not dropped, and does not leave `catchup_in_progress` stuck (a later scroll-to-top still fetches; exactly one `BEFORE` follows).

---

### Task 3: Whole-DB newest msgid query (the attach cursor) + harness

The server wants ONE global anchor: the newest msgid across every buffer of the network (msgids are HLC-ordered and target-independent). Scrollback only has a per-channel query today.

**Files:**
- Modify: `src/common/scrollback.c` (statement struct next to `stmt_newest_msgid`; prepare/finalize next to it; new function after `scrollback_get_newest_msgid`)
- Modify: `src/common/scrollback.h` (declaration next to `scrollback_get_newest_msgid`)
- Create: `tools/scrollback-query-test.c`, `tools/build-scrollback-query-test.ps1`

**Interfaces:**
- Produces: `char *scrollback_get_global_newest_msgid (scrollback_db *db);` — newest non-pending msgid across all channels by `(timestamp, id)`, `g_strdup`ed, NULL if none. Task 5 consumes it.
- Consumes: `scrollback_open (network)`, `scrollback_db_save (...)`, `scrollback_get_newest_msgid`, `scrollback_shutdown` (check the exact signatures in scrollback.h — they have grown since the plan was written).

**Design ruling:** no new index. There is no `(timestamp, id)` index and adding one means a one-time build over ~190k rows on the user's zstd-backed store plus mirroring in `tools/scrollback-salvage.py`. The existing `idx_channel_id_time (channel_id, timestamp)` makes "newest per channel" cheap, and channel counts are small, so the query is one statement that takes each channel's newest qualifying row and picks the newest of those:
```sql
SELECT m.msgid FROM channels c
JOIN messages m ON m.id = (
    SELECT id FROM messages
     WHERE channel_id = c.id AND msgid IS NOT NULL AND msgid NOT LIKE 'pending:%'
     ORDER BY timestamp DESC, id DESC LIMIT 1)
ORDER BY m.timestamp DESC, m.id DESC LIMIT 1
```
Verify the `channels` table's id column name and the `messages.channel_id` column against the schema in scrollback.c before using it; adjust the SQL to the real names.

- [ ] **Step 1: Write the failing harness test**

`tools/scrollback-query-test.c`, modelled on `tools/gap-ledger-test.c` (same stubs — read it for the current list; scrollback.c's outside deps are `get_xdir` and `poxchat_timing_log`):

```c
/* scrollback-query-test.c — standalone check for scrollback query helpers.
 * Exit 0 = pass, 1 = fail.  Build: tools\build-scrollback-query-test.ps1
 * Run:   tools\out\scrollback-query-test.exe <scratch-dir>
 */
```
Body: open `testnet`; assert `scrollback_get_global_newest_msgid` is NULL on an empty DB; save `#a` (1000,"A1") (1005,"A2"), `#b` (1003,"B1") (1009,"B2"), `#a` (1020,"pending:zzz", is_user_msg TRUE), `#a` (1010, NULL msgid); assert global newest is `"B2"` (the pending row and the msgid-less row are ignored even though they are newer) and per-channel `#a` newest is still `"A2"`; `scrollback_shutdown`; print `ok`. Use whatever save function scrollback.h exposes for a plain row with an explicit msgid (read the header).

`tools/build-scrollback-query-test.ps1`: clone of `tools/build-gap-test.ps1` with the test file swapped in and the output exe renamed.

- [ ] **Step 2: Build the harness, verify it fails**

`powershell -ExecutionPolicy Bypass -File tools/build-scrollback-query-test.ps1` → expected: link error, `scrollback_get_global_newest_msgid` unresolved. (Requires the solution to have been built once in this worktree for `config.h`; Task 1 did that.)

- [ ] **Step 3: Implement**

`sqlite3_stmt *stmt_global_newest_msgid;` in the db struct; prepare it after `stmt_newest_msgid` with the SQL above (`goto fail` on error like its neighbours); finalize it next to `stmt_newest_msgid`; implement:
```c
char *
scrollback_get_global_newest_msgid (scrollback_db *db)
{
	char *msgid = NULL;

	if (!db || !db->stmt_global_newest_msgid)
		return NULL;

	sqlite3_reset (db->stmt_global_newest_msgid);
	if (sqlite3_step (db->stmt_global_newest_msgid) == SQLITE_ROW)
	{
		const char *text = (const char *) sqlite3_column_text (db->stmt_global_newest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}
	sqlite3_reset (db->stmt_global_newest_msgid);
	return msgid;
}
```
Declare in scrollback.h with the doc comment: "Newest non-pending msgid across every channel; caller frees. The draft/persistence attach cursor."

- [ ] **Step 4: Run the harness, verify pass; build the solution**

Harness → `ok`, exit 0. Then the Global Constraints build → empty output.

- [ ] **Step 5: Commit**

```bash
git add src/common/scrollback.c src/common/scrollback.h tools/scrollback-query-test.c tools/build-scrollback-query-test.ps1
git commit -m "scrollback: global newest-msgid query for the persistence attach cursor

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Per-network profile name (config + servlist GUI)

`PERSISTENCE ATTACH` needs a profile name. Store it per network; empty means "do not attach" (legacy behaviour preserved for every existing network).

**Files:**
- Modify: `src/common/servlist.h` (`ircnet`), `src/common/servlist.c` (server fill near `serv->persistent_server = ...`; free; load; save)
- Modify: `src/common/poxchat.h` (server struct: `char persist_profile[64];` in the char-array block near `password`)
- Modify: `src/fe-gtk/servlistgui.c` (entry in the same grid as the `Persistent server (bouncer keeps channel state)` checkbox; the update-from-entry block)

**Interfaces:**
- Produces: `ircnet.persist_profile` (`char *`, config key `PP=`), `server.persist_profile` (`char[64]`, copied at connect). Task 5 consumes `serv->persist_profile`.

- [ ] **Step 1: Struct + load/save/free/fill**

Mirror every place `ircnet.oauth_scopes` is handled in servlist.c (allocation/free, load, save, any copy) for `persist_profile`, with config key `PP=`. **Loader caveat:** the loader is `switch (buf[0])` reading values from `buf + 2`, and `case 'P':` is the password — a `PP=` line must be recognised **before** the switch with `if (buf[0] == 'P' && buf[1] == 'P' && buf[2] == '=') { g_free (net->persist_profile); net->persist_profile = g_strdup (buf + 3); continue; }` (check how oauth's multi-letter keys are matched and follow that pattern if one exists). Save only when non-empty: `fprintf (fp, "PP=%s\n", net->persist_profile);`.

Server fill, next to `serv->persistent_server = (net->flags & FLAG_PERSISTENT) ? TRUE : FALSE;`:
```c
	safe_strcpy (serv->persist_profile,
	             net->persist_profile ? net->persist_profile : "",
	             sizeof (serv->persist_profile));
```
and `char persist_profile[64];	/* draft/persistence profile to ATTACH; empty = don't attach */` in the server struct.

- [ ] **Step 2: GUI field**

In `servlistgui.c`, add `static GtkWidget *edit_entry_persist_profile;` with the other `edit_entry_*` statics; create it with `servlist_create_entry` in the grid that holds the persistent-server checkbox, on the row immediately after that checkbox's row (renumber the rows that follow), label `_("Persistence _profile:")`, tooltip `_("draft/persistence profile name to attach on connect (leave empty to not attach)")`; add `servlist_update_from_entry (&net->persist_profile, edit_entry_persist_profile);` in the block that updates `net->real`/`net->pass`. Profile names are 1–32 chars of `A-Za-z0-9_-` (spec) — reject anything else at update time by leaving the stored value unchanged and setting the entry text back; a belt-and-braces `strpbrk` guard lives in Task 5.

- [ ] **Step 3: Build**

Global Constraints build → empty output. (GUI round-trip — set `desktop`, close, `PP=desktop` in `servlist.conf`, restart, field shows `desktop`, clear → line gone — is the user's verification later.)

- [ ] **Step 4: Commit**

```bash
git add src/common/servlist.c src/common/servlist.h src/common/poxchat.h src/fe-gtk/servlistgui.c
git commit -m "servlist: per-network draft/persistence profile name (PP=)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Send `PERSISTENCE ATTACH` in the registration flight; suppress redundant catch-up

The payoff. On SASL success, before `CAP END`, write `PERSISTENCE ATTACH <profile> [<msgid>]`. When a cursor was sent, the server replays every buffer itself, so our deferred `CHATHISTORY LATEST` fan-out and `CHATHISTORY TARGETS` become redundant traffic — suppress them, and restore them on `FAIL PERSISTENCE CURSOR_UNKNOWN`.

**Files:**
- Modify: `src/common/persistence.c/.h` (new `persistence_send_attach`, `persistence_server_drives_replay`; the Task 1 hook in `persistence_handle_fail`)
- Modify: `src/common/proto-irc.c` (the `case 903:` block that sends `CAP END`)
- Modify: `src/common/chathistory.c` (`chathistory_schedule_deferred`, `chathistory_request_targets_on_reconnect`)

**Interfaces:**
- Produces: `void persistence_send_attach (server *serv);` — no-op unless `have_persistence && persistence_tok_attach && serv->persist_profile[0]` and not already attached. Sets `persistence_attached`; sets `persistence_cursor_sent` when a cursor went out.
- Produces: `gboolean persistence_server_drives_replay (server *serv);` = `persistence_attached && persistence_cursor_sent && !persistence_replay_seen` (the last bit set by the first bouncer-replay wrapper start — final-review ruling, see the ledger). chathistory consumes it as a *provisional* gate: grace-delayed fan-out and deferred TARGETS rather than suppression.
- Consumes: `scrollback_get_global_newest_msgid` (Task 3), `serv->persist_profile` (Task 4), `server_get_network`, `scrollback_open`, `tcp_sendf`.

- [ ] **Step 1: Implement the send**

persistence.c (add includes `"scrollback.h"`, `"chathistory.h"`, `"server.h"`):
```c
void
persistence_send_attach (server *serv)
{
	const char *network;
	scrollback_db *db;
	char *cursor = NULL;

	if (!serv || !serv->have_persistence || !serv->persistence_tok_attach)
		return;
	if (!serv->persist_profile[0])
		return;			/* not configured — legacy behaviour */
	if (strpbrk (serv->persist_profile, " \r\n"))
		return;			/* never let a bad name break the registration flight */
	if (serv->persistence_attached)
		return;

	if (serv->persistence_tok_attach_cursor)
	{
		network = server_get_network (serv, FALSE);
		db = network ? scrollback_open (network) : NULL;
		if (db)
			cursor = scrollback_get_global_newest_msgid (db);
	}

	if (cursor)
		tcp_sendf (serv, "PERSISTENCE ATTACH %s %s\r\n", serv->persist_profile, cursor);
	else
		tcp_sendf (serv, "PERSISTENCE ATTACH %s\r\n", serv->persist_profile);

	serv->persistence_attached = TRUE;
	serv->persistence_cursor_sent = (cursor != NULL);
	g_free (cursor);
}

gboolean
persistence_server_drives_replay (server *serv)
{
	return serv && serv->persistence_attached && serv->persistence_cursor_sent;
}
```
Declare both in persistence.h.

- [ ] **Step 2: Call it before the SASL-success CAP END**

proto-irc.c, the shared `case 903/905/906/907` block (904 falls into it after `inbound_sasl_error`): the ATTACH must precede `CAP END` and requires an account, so only after a *successful* exchange — send it only when the numeric is 903 (check the name of `process_numeric`'s numeric parameter and use it):
```c
		serv->waiting_on_sasl = FALSE;
		if (!serv->sent_capend)
		{
			/* draft/persistence ATTACH is accepted only between SASL
			 * completion and CAP END, and needs an account. */
			if (n == 903)
				persistence_send_attach (serv);
			serv->sent_capend = TRUE;
			tcp_send_len (serv, "CAP END\r\n", 9);
		}
```
Do **not** add ATTACH to the other `CAP END` sites: without SASL the server answers `ACCOUNT_REQUIRED`, and ATTACH after `CAP END` is rejected anyway.

- [ ] **Step 3: FAIL fallbacks**

In `persistence_handle_fail`, replace the Task 1 hook comment (code compared through `persistence_strip_namespace`, case-insensitively):
```c
	if (g_ascii_strcasecmp (code, "CURSOR_UNKNOWN") == 0)
	{
		/* Server evicted our anchor: it falls back to last-activity
		 * replay, which may be short.  Re-arm our own per-channel LATEST
		 * fan-out + TARGETS so nothing is missed. */
		serv->persistence_cursor_sent = FALSE;
		chathistory_schedule_deferred (serv);
		chathistory_request_targets_on_reconnect (serv);
	}
	else if (g_ascii_strcasecmp (code, "ACCOUNT_REQUIRED") == 0 ||
	         g_ascii_strcasecmp (code, "INVALID_PARAMETERS") == 0)
	{
		/* ATTACH was rejected outright — a plain client this session. */
		serv->persistence_attached = FALSE;
		serv->persistence_cursor_sent = FALSE;
	}
```
Ordering note: `FAIL` for a pre-CAP-END ATTACH arrives before 001, i.e. before any JOIN; `chathistory_schedule_deferred` at that point has no channel sessions yet but is harmless (the JOIN path re-arms it because the suppression flag is now clear). `chathistory_request_targets_on_reconnect` no-ops when `last_disconnect_time == 0` (first connect), which is correct.

- [ ] **Step 4: Suppress the redundant fetches**

First lines of `chathistory_schedule_deferred` and `chathistory_request_targets_on_reconnect`:
```c
	if (persistence_server_drives_replay (serv))
		return;		/* server replays every buffer from our ATTACH cursor */
```
(chathistory.c already includes persistence.h after Task 2.) Leave the inbound.c call sites alone — the gate lives in one place. Do **not** gate the JOIN-time `bouncer_inferred` block; it is harmless and still useful when STATUS was missed.

- [ ] **Step 5: Harness + build**

Extend `tools/persistence-test.c` (Task 1) with a `tcp_sendf` stub capturing the last line and cases: no profile → nothing sent; profile `desktop` with `have_persistence`+`tok_attach` and no cursor token → `PERSISTENCE ATTACH desktop`; with `tok_attach_cursor` but no scrollback (stub `server_get_network` → NULL) → same line, `persistence_cursor_sent` FALSE; a second call sends nothing; `persistence_handle_fail (..., "CURSOR_UNKNOWN", ...)` clears `persistence_cursor_sent` (stub `chathistory_schedule_deferred` / `chathistory_request_targets_on_reconnect` as no-ops and assert they were called). Rebuild and run → `ok`. Then the Global Constraints build → empty output.

- [ ] **Step 6: Commit**

```bash
git add src/common/persistence.c src/common/persistence.h src/common/proto-irc.c src/common/chathistory.c tools/persistence-test.c
git commit -m "persistence: PERSISTENCE ATTACH <profile> <cursor> in the SASL flight; skip redundant LATEST/TARGETS

Sends the attach (with the global newest scrollback msgid when the server
advertises attach-cursor) immediately before the SASL-success CAP END.
While the server drives replay from that cursor, the deferred per-channel
LATEST fan-out and TARGETS are suppressed; CURSOR_UNKNOWN re-arms them.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

**User verification later (server at ≥ the attach-cursor build):** raw log on connect shows `PERSISTENCE ATTACH desktop <msgid>` then `CAP END` after 903, `001` follows, no `FAIL PERSISTENCE`; zero outbound `CHATHISTORY LATEST`/`TARGETS` after the JOIN burst; inbound `BATCH +x draft/persistence` then `BATCH +y evilnet.github.io/bouncer-replay` with `chathistory` children; lines posted while held appear once each in the right buffers; more lines than the per-buffer cap → our `CHATHISTORY BEFORE` bridges after the wrapper END; a bogus cursor → `FAIL PERSISTENCE CURSOR_UNKNOWN` and the LATEST fan-out resumes; SASL off → no ATTACH; empty profile → no ATTACH and the old fan-out.

---

### Task 6: Documentation and skill update

**Files:**
- Modify: `.claude/skills/ircv3-implementation.md` (extend the "draft/persistence names" section added 2026-09-04)
- Modify: `docs/design/2026-08-14-chathistory-gap-fill.md` §13 item 3 (mark "adopt draft/persistence" as implemented by this plan; leave the gap-fill Task 3 amendment pointing at `chathistory_batch_is_unsolicited`)
- Modify: `CLAUDE.md` "Already Implemented" list (add `draft/persistence (status, profiles, attach, attach-cursor)`)

- [ ] **Step 1: Skill section**

Append to the existing persistence section of `.claude/skills/ircv3-implementation.md`:
```markdown
- Cap value tokens gate verbs: `replay-control`, `profile`, `attach`, `detach`, `list`, `attach-cursor`.
  Parsed by `persistence_parse_cap_value` into `serv->persistence_tok_*`; never assume from cap presence.
- Every `:server PERSISTENCE ...` reply (prefixed or bare) goes to `persistence_handle_reply`;
  `FAIL PERSISTENCE` to `persistence_handle_fail`. `/PERSISTENCE` is a documented passthrough.
- Reconnect auto-rejoin is suppressed when the cap is ACKed *and* the unsolicited STATUS said effective ON
  (the server holds a session and restores it); no STATUS (unauthenticated) or effective OFF rejoins as before.
  Channels the restoration burst omitted are never rejoined. Favourites are never gated.
- `PERSISTENCE ATTACH <profile> [<msgid>]` is pre-CAP-END only and needs an account:
  sent from the 903 handler right before `CAP END` (`persistence_send_attach`) whenever the cap is
  ACKed and a profile is configured — regardless of the `attach` token (absence proves nothing);
  the cursor goes only when `attach-cursor` is advertised and is dropped if longer than 256 bytes.
  Cursor = `scrollback_get_global_newest_msgid` (one global anchor; msgids are HLC-ordered).
- Revive sends `BATCH draft/persistence` (wire order) then `BATCH evilnet.github.io/bouncer-replay`
  wrapping per-target `chathistory` batches. `chathistory_batch_is_unsolicited` puts those into
  LATEST-phase catch-up without touching the request queue; a replay's `draft/chathistory-end`
  never sets `history_exhausted`; wrapper END → `chathistory_replay_wrapper_end`.
- While `persistence_server_drives_replay()` is TRUE (attached, cursor sent, no replay wrapper
  seen yet) the gate is *provisional*: `chathistory_schedule_deferred` arms its timer with a
  10× grace delay instead of 2 s and `chathistory_request_targets_on_reconnect` defers TARGETS to
  that timer; the first `bouncer-replay` wrapper START (`chathistory_replay_wrapper_begin`) sets
  `persistence_replay_seen`, cancels the timer and drops the deferred TARGETS. A server that
  replays nothing (REPLAY OFF, policy, fresh session) therefore gets the old fan-out late, not
  never. The FAIL answering our ATTACH (tracked by
  `persistence_attach_pending`, cleared by the ATTACH ack) is the only one that changes state:
  `CURSOR_UNKNOWN` clears the cursor flag so the regular 366 / login-end call sites re-arm the
  fan-out themselves (the FAIL precedes 001 — never drive catch-up from it); `ACCOUNT_REQUIRED` /
  `INVALID_PARAMETERS` clear `persistence_attached`. Any other `FAIL PERSISTENCE` only prints.
- Profile name lives in servlist (`PP=`), empty = legacy behaviour (no ATTACH).
```

- [ ] **Step 2: Design-doc and CLAUDE.md touch-ups**, then commit:

```bash
git add .claude/skills/ircv3-implementation.md docs/design/2026-08-14-chathistory-gap-fill.md CLAUDE.md
git commit -m "docs: draft/persistence client adoption notes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```
