# draft/persistence Client Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PoxChat a first-class "seamless session" client against Nefarious: negotiate `draft/persistence`, attach to a named profile with a catch-up cursor in the registration flight, consume the server-driven `draft/persistence` + `evilnet.github.io/bouncer-replay` batches correctly, and stop double-fetching history the server already replays.

**Architecture:** Three independent layers, each mergeable alone. (1) *Ingress*: handlers for the unsolicited `PERSISTENCE STATUS` line, `FAIL PERSISTENCE`, and the two wrapper batch types, plus a classification rule so an unsolicited nested `chathistory` batch is processed as a LATEST-phase catch-up instead of corrupting the request queue. (2) *Egress*: the cap request with token parsing, a per-network profile name, a whole-DB newest-msgid query, and `PERSISTENCE ATTACH <profile> [<msgid>]` written immediately before `CAP END` on SASL success. (3) *Suppression*: when the server has accepted a cursor, skip the deferred per-channel `CHATHISTORY LATEST` fan-out and `CHATHISTORY TARGETS`, falling back to them on `FAIL PERSISTENCE CURSOR_UNKNOWN`.

**Tech Stack:** C11, GLib, SQLite (scrollback), IRCv3 caps/batches. Windows MSBuild (Release|x64 only). A standalone `cl`-built harness exists only for scrollback-layer code; protocol code is verified in-app against Nefarious/X3 (AfterNET).

**Spec:** `docs/design/2026-08-14-chathistory-gap-fill.md` §13 + §13.1 (client-side reading of the Nefarious "seamless sessions" gist and the server's draft/persistence unification plan). Server wire contract: gist 8d644eb37878d7bcaa91d1a68ae23d94 (§2, §5, residual gap 1) and gist 814a674c (PERSISTENCE verbs). Read both before starting.

## Global Constraints

- C11, tabs for indentation, `module_action()` naming, GLib allocation (`g_new0`/`g_free`/`g_strdup`), `:1` bitfields for server flags (CLAUDE.md).
- Build: Release|x64 via the 64-bit MSBuild host (32-bit host OOMs on resources.c). From Git-Bash at repo root:
  ```bash
  MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe"
  "$MSBUILD" win32/poxchat.sln //p:Configuration=Release //p:Platform=x64 //p:PreferredToolArchitecture=x64 //v:minimal //nologo 2>&1 \
    | grep -E "error C[0-9]|error [A-Z]" | grep -v "jansson\|lua.h\|cffi\|MSB3073\|MSB3027\|MSB3021"
  ```
  Empty output = success. Exe: `C:\Users\johne\source\repos\poxchat-build-gtk4\x64\rel\poxchat.exe`.
- Wire facts (from the server plan, verbatim): CAP name `draft/persistence`; value tokens `replay-control`, `list`, `attach`, `attach-cursor`; `PERSISTENCE ATTACH <profile> [<msgid>]` is accepted **pre-CAP-END only**; unsolicited `PERSISTENCE STATUS <client-setting> <effective-setting>` arrives after the last 005 and before MOTD-end; errors are `FAIL PERSISTENCE <CODE> [<context>] :<text>` with codes `ACCOUNT_REQUIRED`, `INVALID_PARAMETERS`, `INTERNAL_ERROR`, `CURSOR_UNKNOWN`, `CANNOT_DETACH`; batch types `draft/persistence` (JOIN/TOPIC/NAMES burst) then `evilnet.github.io/bouncer-replay` (outer wrapper; inner per-target `chathistory` batches carry the wrapper's ref as `outer_batch`).
- Server auto-replay never fires for a client that negotiated `draft/chathistory` **unless** that client supplied an ATTACH cursor. So the bouncer-replay batch appears only after Task 5 goes live, but Tasks 1–2 must already tolerate it.
- Every `CAP END` guard site (inbound.c:3159-3183, 3199-3218, 3444-3448; proto-irc.c:1113-1118) stays consistent: we do **not** add a new `waiting_on_*` gate. ATTACH is pipelined immediately before the SASL-success `CAP END`; the server holds registration until it has processed the flight, so no reply is awaited.
- Do not commit `win32/poxchat.props`. Commit after each task with the message given.

---

### Task 1: Cap negotiation, token flags, `PERSISTENCE STATUS` / `FAIL PERSISTENCE` ingress

Request the cap, record which value tokens the server offers, handle the unsolicited STATUS line (authoritative "I am on a held bouncer session" signal — replaces the JOIN-timestamp heuristic for autojoin suppression), and route `FAIL PERSISTENCE` to a handler. Nothing is *sent* beyond the cap request yet.

**Files:**
- Modify: `src/common/poxchat.h:713-750` (server `have_*` bitfield block)
- Modify: `src/common/inbound.c:2887-2934` (`supported_caps[]`), `2777-2865` (`inbound_toggle_caps`), `3098-3136` (token parse in `inbound_cap_ls`), `1455-1468` (autojoin suppression)
- Modify: `src/common/server.c:1493` (disconnect reset)
- Modify: `src/common/proto-irc.c:1357-1378` (FAIL), `1988` (before the `garbage:` label), `1995-2024` (`process_named_servermsg`)
- Create: `src/common/persistence.c`, `src/common/persistence.h`
- Modify: `win32/common/common.vcxproj` + `common.vcxproj.filters` (add the two files next to `chathistory.c`/`.h`), `src/common/meson.build` (add `persistence.c` to the sources list next to `chathistory.c`)

**Interfaces:**
- Produces (poxchat.h server struct):
  ```c
  unsigned int have_persistence:1;          /* draft/persistence ACKed */
  unsigned int persistence_attach:1;        /* CAP value had "attach" */
  unsigned int persistence_attach_cursor:1; /* CAP value had "attach-cursor" */
  unsigned int persistence_replay_control:1;/* CAP value had "replay-control" */
  unsigned int persistence_effective:1;     /* last STATUS effective-setting == ON */
  unsigned int persistence_attached:1;      /* we sent PERSISTENCE ATTACH this connection (Task 5) */
  unsigned int persistence_cursor_sent:1;   /* ATTACH carried a msgid cursor (Task 5) */
  ```
- Produces (persistence.h):
  ```c
  void persistence_handle_status (server *serv, const char *client_setting, const char *effective_setting);
  void persistence_handle_fail (server *serv, const char *code, const char *context, const char *text);
  void persistence_reset (server *serv);   /* clear per-connection state on disconnect */
  ```
- Consumes: `EMIT_SIGNAL_TIMESTAMP`, `XP_TE_SERVTEXT`.

- [ ] **Step 1: Add server fields**

In `src/common/poxchat.h`, directly after the `bouncer_inferred:1;` line (~750):

```c
	unsigned int have_persistence:1;		/* draft/persistence ACKed */
	unsigned int persistence_attach:1;		/* CAP value token "attach" */
	unsigned int persistence_attach_cursor:1;	/* CAP value token "attach-cursor" */
	unsigned int persistence_replay_control:1;	/* CAP value token "replay-control" */
	unsigned int persistence_effective:1;		/* unsolicited PERSISTENCE STATUS said effective=ON */
	unsigned int persistence_attached:1;		/* sent PERSISTENCE ATTACH on this connection */
	unsigned int persistence_cursor_sent:1;	/* that ATTACH carried a msgid cursor */
```

- [ ] **Step 2: Create persistence.h / persistence.c**

`src/common/persistence.h`:

```c
/* PoxChat — IRCv3 draft/persistence client side (Nefarious bouncer sessions) */
#ifndef POXCHAT_PERSISTENCE_H
#define POXCHAT_PERSISTENCE_H

#include "poxchat.h"

/* :server PERSISTENCE STATUS <client-setting> <effective-setting> */
void persistence_handle_status (server *serv, const char *client_setting,
                                const char *effective_setting);

/* :server FAIL PERSISTENCE <code> [<context>] :<text> */
void persistence_handle_fail (server *serv, const char *code,
                              const char *context, const char *text);

/* Clear per-connection state; called from the server disconnect path. */
void persistence_reset (server *serv);

#endif
```

`src/common/persistence.c`:

```c
/* PoxChat — IRCv3 draft/persistence client side */
#include "config.h"
#include <string.h>
#include <glib.h>
#include "poxchat.h"
#include "poxchatc.h"
#include "text.h"
#include "server.h"
#include "persistence.h"

void
persistence_handle_status (server *serv, const char *client_setting,
                           const char *effective_setting)
{
	char *msg;

	if (!serv || !client_setting || !effective_setting)
		return;

	serv->persistence_effective =
		(g_ascii_strcasecmp (effective_setting, "ON") == 0);

	msg = g_strdup_printf ("Persistence: preference %s, effective %s",
	                       client_setting, effective_setting);
	EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session, msg,
	                       serv->servername, NULL, NULL, 0, 0);
	g_free (msg);
}

void
persistence_handle_fail (server *serv, const char *code,
                         const char *context, const char *text)
{
	char *msg;

	if (!serv || !code)
		return;

	msg = g_strdup_printf ("Persistence error %s%s%s: %s", code,
	                       context ? " " : "", context ? context : "",
	                       text ? text : "");
	EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session, msg,
	                       serv->servername, NULL, NULL, 0, 0);
	g_free (msg);

	/* Task 5 adds CURSOR_UNKNOWN → fall back to deferred LATEST here. */
}

void
persistence_reset (server *serv)
{
	if (!serv)
		return;
	serv->have_persistence = FALSE;
	serv->persistence_attach = FALSE;
	serv->persistence_attach_cursor = FALSE;
	serv->persistence_replay_control = FALSE;
	serv->persistence_effective = FALSE;
	serv->persistence_attached = FALSE;
	serv->persistence_cursor_sent = FALSE;
}
```

Add the files to `win32/common/common.vcxproj` (both `<ClCompile Include="..\..\src\common\persistence.c" />` and `<ClInclude ... persistence.h />`), the `.filters` file, and `src/common/meson.build`.

- [ ] **Step 3: Request the cap and parse its value tokens**

`src/common/inbound.c` `supported_caps[]` (~2912, next to `"draft/chathistory"`):

```c
	"draft/persistence",
```

`inbound_toggle_caps` (~2831, after the `draft/pre-away` branch):

```c
		else if (!strcmp (extension, "draft/persistence"))
			serv->have_persistence = enable;
```

`inbound_cap_ls`, after the `draft/multiline` token block (~3136), following the `draft/account-registration` pattern exactly (no `continue` — the cap must still be requested):

```c
		/* IRCv3 draft/persistence — value tokens advertise optional verbs.
		 * Format: draft/persistence=replay-control,list,attach,attach-cursor */
		if (!g_strcmp0 (extension, "draft/persistence") && value)
		{
			char **tokens = g_strsplit (value, ",", 0);
			int j;

			for (j = 0; tokens[j]; j++)
			{
				if (!strcmp (tokens[j], "attach"))
					serv->persistence_attach = TRUE;
				else if (!strcmp (tokens[j], "attach-cursor"))
					serv->persistence_attach_cursor = TRUE;
				else if (!strcmp (tokens[j], "replay-control"))
					serv->persistence_replay_control = TRUE;
			}
			g_strfreev (tokens);
			/* Don't continue - still need to request the capability */
		}
```

- [ ] **Step 4: Route the STATUS line (both prefixed and unprefixed forms)**

`src/common/proto-irc.c`, `#include "persistence.h"` next to the `chathistory.h` include (line 33).

In `process_named_msg`, immediately **before** the `garbage:` label (~1988), add a length-11 check (there is no `len == 11` block):

```c
	/* IRCv3 draft/persistence: ":server PERSISTENCE STATUS <client> <effective>"
	 * (11-char verb — no WORDL block exists for that length). */
	if (len == 11 && g_ascii_strcasecmp (type, "PERSISTENCE") == 0)
	{
		if (word[3] && g_ascii_strcasecmp (word[3], "STATUS") == 0 &&
		    word[4] && word[4][0] && word[5] && word[5][0])
		{
			persistence_handle_status (serv, word[4], word[5]);
			return;
		}
		/* PROFILE / other subcommands: show raw until we consume them */
		EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session,
		                       word_eol[3], word[1], NULL, NULL, 0,
		                       tags_data->timestamp);
		return;
	}
```

In `process_named_servermsg` (~2010, after the `NOTICE ` branch), the unprefixed form:

```c
	if (!strncasecmp (buf, "PERSISTENCE STATUS ", 19))
	{
		/* word_eol[] here is relative to the raw line: word_eol[2] = "STATUS ..." */
		char **parts = g_strsplit (buf + 19, " ", 3);
		if (parts[0] && parts[1])
			persistence_handle_status (sess->server, parts[0], parts[1]);
		g_strfreev (parts);
		return;
	}
```

`FAIL` routing (~1362, inside the existing `if (g_ascii_strcasecmp (word[3], "CHATHISTORY") == 0)` chain — add a sibling):

```c
			else if (g_ascii_strcasecmp (word[3], "PERSISTENCE") == 0)
			{
				/* :server FAIL PERSISTENCE <code> [<context>] :<text>.
				 * With no context the text starts at word[5]. */
				const char *ctx = NULL;
				const char *txt = word_eol[5];
				if (word[5] && word[5][0] != ':' && word[6] && word[6][0])
				{
					ctx = word[5];
					txt = word_eol[6];
				}
				if (txt && *txt == ':')
					txt++;
				persistence_handle_fail (serv, word[4], ctx, txt);
				return;
			}
```

Keep the existing generic FAIL display for other verbs untouched.

- [ ] **Step 5: Autojoin suppression + disconnect reset**

`src/common/inbound.c` ~1467, extend the existing condition:

```c
	if (serv->persistent_server || serv->bouncer_inferred || serv->persistence_effective)
		list = NULL;
```

Note the ordering caveat: STATUS arrives after 005 but the autojoin runs at 376/422 (MOTD end), so `persistence_effective` is already set by then. Verify in Step 7 with a raw log.

In `src/common/server.c:1493` (the disconnect path line `serv->bouncer_inferred = FALSE;	/* re-evaluate next reconnect */`), add directly after it:

```c
	persistence_reset (serv);
```

with `#include "persistence.h"` at the top of server.c.

- [ ] **Step 6: Build**

Run the Global Constraints build command. Expected: empty output.

- [ ] **Step 7: Manual verification against AfterNET**

1. Connect with SASL to AfterNET (a Nefarious `ircv3.2-upgrade` server). In the server tab raw log (`/rawlog` or `/set irc_raw_modes`… use the Raw Log window) confirm `CAP * LS` contains `draft/persistence=` and `CAP * ACK` contains `draft/persistence`.
2. Confirm a `Persistence: preference X, effective Y` line appears in the server tab before the MOTD.
3. If `effective ON`: disconnect (pull network / `/discon`) and `/reconnect`; confirm PoxChat sends **no** `JOIN` lines of its own in the raw log (server-side JOINs still appear as inbound).
4. `/quote PERSISTENCE BOGUS` → a `Persistence error INVALID_PARAMETERS …` line, no `GARBAGE:` line.

Record the results in the commit message body.

- [ ] **Step 8: Commit**

```bash
git add src/common/persistence.c src/common/persistence.h src/common/poxchat.h src/common/inbound.c src/common/proto-irc.c src/common/server.c src/common/meson.build win32/common/common.vcxproj win32/common/common.vcxproj.filters
git commit -m "persistence: negotiate draft/persistence, parse value tokens, handle STATUS/FAIL

Adds the cap to supported_caps, records attach/attach-cursor/replay-control
tokens, routes unsolicited PERSISTENCE STATUS (prefixed and bare) and
FAIL PERSISTENCE, and lets STATUS effective=ON suppress reconnect autojoin
alongside the existing persistent_server / bouncer_inferred gates.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Wrapper batches and unsolicited-chathistory classification

Make `draft/persistence` and `evilnet.github.io/bouncer-replay` batches first-class containers, and make `chathistory_process_batch` treat a `chathistory` batch we did not request as a LATEST-phase catch-up for its target (witness/bridge logic runs; request queue untouched). This is what gap-fill plan Task 3's amendment consumes.

**Files:**
- Modify: `src/common/inbound.c:2356-2401` (`inbound_batch_end` type dispatch)
- Modify: `src/common/chathistory.c:1451-1475` (`chathistory_process_batch` head), `236-255` (`chathistory_request_complete`), `1217-1230` (`finish_batch_processing` head), `1839-1879` (`send_deferred_latest` — factor the lower-bound computation)
- Modify: `src/common/chathistory.h` (new declarations)
- Modify: `src/common/poxchat.h:526-539` (`batch_info`: no change) and the `chathistory_chunk_state` struct in chathistory.c (add `unsolicited:1`)

**Interfaces:**
- Produces (chathistory.h):
  ```c
  /* TRUE if this chathistory batch was not requested by us (server-driven replay). */
  gboolean chathistory_batch_is_unsolicited (server *serv, batch_info *batch, session *sess);
  /* Put sess into LATEST-phase catch-up state without sending anything. */
  void chathistory_begin_unsolicited_catchup (session *sess);
  /* Called at the END of an evilnet.github.io/bouncer-replay wrapper. */
  void chathistory_replay_wrapper_end (server *serv);
  ```
- Consumes: `batch_info.outer_batch`, `serv->active_batches`, `sess->ch_active` (the in-flight `chreq`), `chathistory_check_before_catchup (serv)` (static in chathistory.c — same file, fine).

- [ ] **Step 1: Factor the lower-bound computation out of `send_deferred_latest`**

In `chathistory.c`, above `send_deferred_latest` (~1839):

```c
/* Enter LATEST-phase catch-up state for sess: sets the bridge target the
 * BEFORE loop stops at.  Shared by our own deferred LATEST and by
 * server-driven replay batches (draft/persistence attach-cursor). */
static void
catchup_enter_latest_phase (session *sess)
{
	sess->catchup_in_progress = TRUE;
	sess->catchup_is_before = FALSE;
	if (sess->scrollback_newest_time > CHATHISTORY_FUZZ_INTERVAL)
		sess->catchup_lower_bound = sess->scrollback_newest_time - CHATHISTORY_FUZZ_INTERVAL;
	else if (prefs.hex_irc_chathistory_background_max_age > 0)
		sess->catchup_lower_bound = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
	else
		sess->catchup_lower_bound = 0;
}
```

and replace the seven corresponding lines in `send_deferred_latest` with `catchup_enter_latest_phase (sess);`. Behavior identical.

- [ ] **Step 2: Add the unsolicited classification + entry helpers**

`chathistory.c`, next to `find_session_for_target` (~478):

```c
/* A chathistory batch is unsolicited when it is nested inside a
 * bouncer-replay wrapper, or when no request is in flight for that
 * session.  Nefarious sends these after PERSISTENCE ATTACH <profile> <msgid>. */
gboolean
chathistory_batch_is_unsolicited (server *serv, batch_info *batch, session *sess)
{
	if (batch->outer_batch && serv->active_batches)
	{
		batch_info *outer = g_hash_table_lookup (serv->active_batches, batch->outer_batch);
		if (outer && outer->type &&
		    g_ascii_strcasecmp (outer->type, "evilnet.github.io/bouncer-replay") == 0)
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
	/* Every nested chathistory batch has already run its LATEST-phase
	 * completion (decrementing latest_pending).  If the wrapper was empty
	 * of chathistory children nothing is pending; kick the eager BEFORE
	 * pass exactly once either way. */
	if (serv->chathistory_latest_pending == 0)
		chathistory_check_before_catchup (serv);
}
```

Declare all three in `chathistory.h` next to `chathistory_process_batch`. `catchup_enter_latest_phase` must be defined above these (move the helper from Step 1 up, or forward-declare it at the top of chathistory.c).

- [ ] **Step 3: Use it in `chathistory_process_batch` and skip request-complete for unsolicited batches**

`chathistory_process_batch` (~1470), replace `is_catchup = sess->catchup_in_progress;` with:

```c
	gboolean unsolicited = chathistory_batch_is_unsolicited (serv, batch, sess);
	if (unsolicited)
		chathistory_begin_unsolicited_catchup (sess);
	is_catchup = sess->catchup_in_progress;
```

Add `unsigned int unsolicited:1;` to `chathistory_chunk_state`; set `sync_state.unsolicited = unsolicited;` and `chunk->unsolicited = unsolicited;` in both branches (~1540 and ~1565).

In the **empty-batch** branch (~1476), change `chathistory_request_complete (sess);` to:

```c
		if (!unsolicited)
			chathistory_request_complete (sess);
```

In `finish_batch_processing` (~1226) change `chathistory_request_complete (sess);` to:

```c
	if (!chunk->unsolicited)
		chathistory_request_complete (sess);
```

`chathistory_request_complete` frees `ch_active` and dispatches `ch_pending`; for an unsolicited batch there is no `ch_active`, and dispatching a pending *user* request mid-replay would interleave — hence the skip. The LATEST-phase branch that follows (`chathistory_latest_pending--` → `chathistory_check_before_catchup`) is what we want to run, and it does because `is_catchup` is TRUE.

- [ ] **Step 4: Container cases in `inbound_batch_end`**

`inbound.c` ~2398, replace the `/* TODO: Handle other batch types` comment block with:

```c
		else if (g_ascii_strcasecmp (batch->type, "draft/persistence") == 0)
		{
			/* Channel-state burst (JOIN/TOPIC/NAMES) on bouncer revive.
			 * Contents were processed live (inbound_batch_add_message only
			 * collects chathistory/multiline types); nothing to flush. */
		}
		else if (g_ascii_strcasecmp (batch->type, "evilnet.github.io/bouncer-replay") == 0)
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

- [ ] **Step 6: Regression check (no server-driven replay exists yet for us)**

Because we do not send a cursor until Task 5, the only observable change is that requested batches still work. Verify:

1. Fresh connect to AfterNET, join a channel with recent traffic → the deferred `CHATHISTORY LATEST` still lands and renders (raw log shows `BATCH +… chathistory #chan` and messages appear).
2. Scroll to top → `CHATHISTORY BEFORE` fires and older lines splice in.
3. `/quote CHATHISTORY LATEST #chan * 5` by hand (this *is* a batch with no matching `ch_active`): it must render, not be dropped, and must not leave `catchup_in_progress` stuck — a subsequent scroll-to-top must still fetch. Check with the raw log that exactly one `BEFORE` follows.

- [ ] **Step 7: Commit**

```bash
git add src/common/inbound.c src/common/chathistory.c src/common/chathistory.h
git commit -m "chathistory: classify unsolicited batches as LATEST-phase; handle persistence wrapper batches

A chathistory batch nested in evilnet.github.io/bouncer-replay (or with
no request in flight) now enters catch-up state instead of popping an
unrelated pending request. draft/persistence and bouncer-replay batch
types become explicit containers; wrapper END kicks the BEFORE pass once.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Whole-DB newest msgid query (the attach cursor) + harness

The server wants ONE global anchor: the newest msgid across every buffer of the network (msgids are HLC-ordered and target-independent). Scrollback only has a per-channel query today.

**Files:**
- Modify: `src/common/scrollback.c:38-60` (struct: new stmt), `436-462` (prepare), `595-601` (finalize), after `scrollback_get_newest_msgid` (~1000) (new function)
- Modify: `src/common/scrollback.h:93` (declaration)
- Create: `tools/scrollback-query-test.c`, `tools/build-scrollback-query-test.ps1`

**Interfaces:**
- Produces: `char *scrollback_get_global_newest_msgid (scrollback_db *db);` — newest non-pending msgid across all channels, `g_strdup`ed, NULL if none. Task 5 consumes it.
- Consumes: `scrollback_open (network)`, `scrollback_db_save (db, channel, timestamp, msgid, text, is_user_msg)`, `scrollback_db_close`.

- [ ] **Step 1: Write the failing harness test**

`tools/scrollback-query-test.c`:

```c
/* scrollback-query-test.c — standalone check for scrollback query helpers.
 * Exit 0 = pass, 1 = fail.  Build: tools\build-scrollback-query-test.ps1
 * Run:   tools\out\scrollback-query-test.exe <scratch-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "../src/common/scrollback.h"

static char *xdir;
const char *get_xdir (void) { return xdir; }
void poxchat_timing_log (const char *fmt, ...) { (void) fmt; }

#define CHECK(cond, msg) do { if (!(cond)) { fprintf (stderr, "FAIL: %s\n", msg); return 1; } } while (0)

int
main (int argc, char **argv)
{
	scrollback_db *db;
	char *got;

	if (argc < 2) { fprintf (stderr, "usage: %s <scratch-dir>\n", argv[0]); return 3; }
	xdir = argv[1];
	g_mkdir_with_parents (xdir, 0700);
	scrollback_init ();

	db = scrollback_open ("testnet");
	CHECK (db != NULL, "open");

	got = scrollback_get_global_newest_msgid (db);
	CHECK (got == NULL, "empty db → NULL");

	/* Two channels; newest by (timestamp, id) is in #b, and a newer
	 * 'pending:' row must be ignored. */
	scrollback_db_save (db, "#a", 1000, "A1", "hello", FALSE);
	scrollback_db_save (db, "#a", 1005, "A2", "world", FALSE);
	scrollback_db_save (db, "#b", 1003, "B1", "x", FALSE);
	scrollback_db_save (db, "#b", 1009, "B2", "y", FALSE);
	scrollback_db_save (db, "#a", 1020, "pending:zzz", "unconfirmed", TRUE);
	scrollback_db_save (db, "#a", 1010, NULL, "no msgid, newest ts", FALSE);

	got = scrollback_get_global_newest_msgid (db);
	CHECK (got && strcmp (got, "B2") == 0, "global newest msgid == B2");
	g_free (got);

	/* Per-channel helper still works and disagrees for #a */
	got = scrollback_get_newest_msgid (db, "#a");
	CHECK (got && strcmp (got, "A2") == 0, "#a newest == A2");
	g_free (got);

	scrollback_shutdown ();
	printf ("ok\n");
	return 0;
}
```

`tools/build-scrollback-query-test.ps1` (clone of `build-vfs-test.ps1` with different inputs):

```powershell
# Builds tools\out\scrollback-query-test.exe (x64). Requires VS 2022 + gvsbuild deps
# and a prior solution build (for the generated config.h in win32\).
$ErrorActionPreference = 'Stop'
$devShell = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' | Select-Object -First 1
if (-not $devShell) { throw 'VS 2022 Launch-VsDevShell.ps1 not found' }
$repo = Split-Path $PSScriptRoot -Parent
& $devShell.FullName -Arch amd64 -SkipAutomaticLocation | Out-Null
$deps = 'c:\gtk-build\gtk4\x64\release'
$out = Join-Path $repo 'tools\out'
New-Item -ItemType Directory -Force $out | Out-Null
cl /nologo /O1 /W3 /MD /D_CRT_SECURE_NO_WARNINGS /DWIN32 `
  "/I$repo\win32" "/I$repo\src\common" "/I$repo\src\common\zstd" `
  "/I$deps\include" "/I$deps\include\glib-2.0" "/I$deps\lib\glib-2.0\include" `
  "$repo\tools\scrollback-query-test.c" "$repo\src\common\scrollback.c" `
  "$repo\src\common\sqlite-zstd-vfs.c" "$repo\src\common\zstd\zstd.c" `
  /Fo"$out\" /Fe"$out\scrollback-query-test.exe" `
  /link "/LIBPATH:$deps\lib" sqlite3.lib glib-2.0.lib intl.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Write-Host "built $out\scrollback-query-test.exe"
```

If `scrollback.c` needs additional stubs to link (the gap-fill plan verified only `get_xdir` and `poxchat_timing_log` on 2026-08-17), add them to the test file as one-line stubs and note them in the commit.

- [ ] **Step 2: Build the harness, verify it fails**

Run (PowerShell): `tools\build-scrollback-query-test.ps1`
Expected: link error — `scrollback_get_global_newest_msgid` unresolved.

- [ ] **Step 3: Implement**

`scrollback.c` struct (~49): `sqlite3_stmt *stmt_global_newest_msgid;`

Prepare (after the `stmt_newest_msgid` prepare, ~449):

```c
	/* Newest msgid across all channels — the draft/persistence attach cursor */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid FROM messages WHERE msgid IS NOT NULL "
		"AND msgid NOT LIKE 'pending:%' ORDER BY timestamp DESC, id DESC LIMIT 1",
		-1, &sdb->stmt_global_newest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;
```

Finalize (~598): `if (sdb->stmt_global_newest_msgid) sqlite3_finalize (sdb->stmt_global_newest_msgid);`

Function, after `scrollback_get_newest_msgid`:

```c
char *
scrollback_get_global_newest_msgid (scrollback_db *db)
{
	char *msgid = NULL;

	if (!db)
		return NULL;

	sqlite3_reset (db->stmt_global_newest_msgid);
	if (sqlite3_step (db->stmt_global_newest_msgid) == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (db->stmt_global_newest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}
	sqlite3_reset (db->stmt_global_newest_msgid);
	return msgid;
}
```

`scrollback.h` (~94): `char *scrollback_get_global_newest_msgid (scrollback_db *db);` with a doc comment: "Newest non-pending msgid across every channel; caller frees. Used as the draft/persistence attach cursor."

Check whether an index covers `ORDER BY timestamp DESC, id DESC` without a channel predicate (grep `CREATE INDEX` in scrollback.c). If only `(channel_id, timestamp)` exists, add `CREATE INDEX IF NOT EXISTS idx_messages_ts ON messages(timestamp, id)` in the schema-init block — a 187k-row table scan on every connect is not acceptable. Mirror any new index in `tools/scrollback-salvage.py` `migrate_image()` (~184-248) per the gap-fill plan's rule.

- [ ] **Step 4: Run the harness, verify pass**

`tools\build-scrollback-query-test.ps1; tools\out\scrollback-query-test.exe $env:TEMP\sbq-test`
Expected: `ok`, exit 0. Then the full app build (Global Constraints) — empty output.

- [ ] **Step 5: Commit**

```bash
git add src/common/scrollback.c src/common/scrollback.h tools/scrollback-query-test.c tools/build-scrollback-query-test.ps1 tools/scrollback-salvage.py
git commit -m "scrollback: add global newest-msgid query for the persistence attach cursor

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Per-network profile name (config + servlist GUI)

`PERSISTENCE ATTACH` needs a profile name. Store it per network; empty means "do not attach" (legacy behavior preserved for every existing network). Users who tick "Persistent server" get a profile field beside it.

**Files:**
- Modify: `src/common/servlist.h:39-64` (`ircnet`), `src/common/servlist.c:~478` (server fill), `~943-968` (free), `~1120-1123` (load), `~1296-1299` (save)
- Modify: `src/common/poxchat.h` (server struct: `char persist_profile[64];` near `persistent_server`)
- Modify: `src/fe-gtk/servlistgui.c:2267-2268` (checkbox) — add an entry row after it, following how the existing text entries on that page are created (look for the `servlist_create_entry` calls for `net->real` / `net->user` on the same table)

**Interfaces:**
- Produces: `ircnet.persist_profile` (`char *`, config key `PP=`), `server.persist_profile` (`char[64]`, copied at connect). Task 5 consumes `serv->persist_profile`.

- [ ] **Step 1: Struct + load/save + free**

`servlist.h` after `char *pass;`:
```c
	char *persist_profile;	/* draft/persistence profile to ATTACH; NULL = don't attach */
```

`servlist.c` load loop (~1123, next to `case 'P':` for pass — the loader switches on `buf[0]` with `buf[1] == '='`; this key is two letters, so add the check **before** the switch):
```c
			if (buf[0] == 'P' && buf[1] == 'P' && buf[2] == '=')
			{
				g_free (net->persist_profile);
				net->persist_profile = g_strdup (buf + 3);
				continue;
			}
```
The loader is `switch (buf[0])` with `buf + 2` values (servlist.c:1122 `case 'P':` reads pass), so a `PP=` line would be mis-read as `P=` — the two-letter check must run before the switch and `continue`.

Save (~1299):
```c
		if (net->persist_profile && net->persist_profile[0])
			fprintf (fp, "PP=%s\n", net->persist_profile);
```

Free (~968, next to `free_and_clear (net->pass)`): `g_free (net->persist_profile);`

Server fill (~478):
```c
	safe_strcpy (serv->persist_profile,
	             net->persist_profile ? net->persist_profile : "",
	             sizeof (serv->persist_profile));
```
and `char persist_profile[64];` in the server struct (poxchat.h, in the char-array block near `password`).

- [ ] **Step 2: GUI field**

In `servlistgui.c` near line 2268, after the persistent checkbox, add an entry bound to `persist_profile` using the same helper the page uses for `net->real` (grep `servlist_create_entry`), label `_("Persistence profile:")`, tooltip `_("draft/persistence profile name to attach on connect (leave empty to not attach)")`. Wire its changed-callback the way the sibling entries are wired (they write `selected_net-><field>` via `servlist_update_from_entry`-style callbacks — copy the `real` one and change the field).

- [ ] **Step 3: Build, verify round-trip**

Build (empty output). Run the app → Network List → edit AfterNET → set profile `desktop` → close → confirm `%APPDATA%\PoxChat\servlist.conf` has `PP=desktop` under that network; restart; field still shows `desktop`. Set it empty → `PP=` line gone.

- [ ] **Step 4: Commit**

```bash
git add src/common/servlist.c src/common/servlist.h src/common/poxchat.h src/fe-gtk/servlistgui.c
git commit -m "servlist: per-network draft/persistence profile name (PP=)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Send `PERSISTENCE ATTACH` in the registration flight; suppress redundant catch-up

The payoff. On SASL success, before `CAP END`, write `PERSISTENCE ATTACH <profile> [<msgid>]`. When a cursor was sent, the server replays every buffer itself, so our deferred `CHATHISTORY LATEST` fan-out and `CHATHISTORY TARGETS` become redundant traffic — suppress them, and restore them on `FAIL PERSISTENCE CURSOR_UNKNOWN`.

**Files:**
- Modify: `src/common/persistence.c/.h` (new `persistence_send_attach`, CURSOR_UNKNOWN handling)
- Modify: `src/common/proto-irc.c:1113-1118` (SASL success CAP END)
- Modify: `src/common/inbound.c:818-821` (JOIN → `chathistory_schedule_deferred`), `1989-1990` (TARGETS on reconnect)
- Modify: `src/common/chathistory.c:1917-1930` (`chathistory_schedule_deferred`), `2078-2100` (`chathistory_request_targets_on_reconnect`)

**Interfaces:**
- Produces: `void persistence_send_attach (server *serv);` — no-op unless `have_persistence && persistence_attach && serv->persist_profile[0]`. Sets `persistence_attached`; sets `persistence_cursor_sent` when a cursor went out.
- Produces: `gboolean persistence_server_drives_replay (server *serv);` = `persistence_attached && persistence_cursor_sent`. chathistory consumes it.
- Consumes: `scrollback_get_global_newest_msgid` (Task 3), `serv->persist_profile` (Task 4), `server_get_network`, `scrollback_open`, `tcp_sendf`.

- [ ] **Step 1: Implement the send**

`persistence.c` (add includes `"scrollback.h"`, `"chathistory.h"`):

```c
void
persistence_send_attach (server *serv)
{
	const char *network;
	scrollback_db *db;
	char *cursor = NULL;

	if (!serv || !serv->have_persistence || !serv->persistence_attach)
		return;
	if (!serv->persist_profile[0])
		return;			/* not configured — legacy behavior */
	if (serv->persistence_attached)
		return;

	if (serv->persistence_attach_cursor)
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

Declare both in `persistence.h`.

Profile-name safety: reject names containing spaces or control chars at the GUI layer (Task 4 entry) — add `if (strpbrk (serv->persist_profile, " \r\n"))
 return;` here as the belt-and-braces guard.

- [ ] **Step 2: Call it before the SASL-success CAP END**

`proto-irc.c` ~1113 (the `903/905/906/907` block; 904 falls through here after `inbound_sasl_error`):

```c
		serv->waiting_on_sasl = FALSE;
		if (!serv->sent_capend)
		{
			/* draft/persistence ATTACH must precede CAP END and requires
			 * an account — only after a *successful* SASL exchange. */
			if (n == 903)
				persistence_send_attach (serv);
			serv->sent_capend = TRUE;
			tcp_send_len (serv, "CAP END\r\n", 9);
		}
```

(`n` is `process_numeric`'s `int n` parameter — proto-irc.c:503 — switched on at line 515.) Do **not** add ATTACH to the other three CAP END sites: without SASL the server answers `ACCOUNT_REQUIRED`, and ATTACH after `CAP END` is rejected anyway.

- [ ] **Step 3: CURSOR_UNKNOWN fallback**

In `persistence_handle_fail`, replace the Task-1 comment with:

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
		/* ATTACH was rejected outright — we are a plain client this session. */
		serv->persistence_attached = FALSE;
		serv->persistence_cursor_sent = FALSE;
	}
```

Ordering note: `FAIL` for a pre-CAP-END ATTACH arrives before 001, i.e. before any JOIN; `chathistory_schedule_deferred` at that point has no channel sessions yet but is harmless (the JOIN path re-arms it because the suppression flag is now clear). `chathistory_request_targets_on_reconnect` no-ops when `last_disconnect_time == 0` (first connect), which is correct.

- [ ] **Step 4: Suppress the redundant fetches**

`chathistory.c` `chathistory_schedule_deferred` (~1917), first lines:

```c
	if (persistence_server_drives_replay (serv))
		return;		/* server replays every buffer from our ATTACH cursor */
```

and the same two lines at the top of `chathistory_request_targets_on_reconnect` (~2078). `#include "persistence.h"` in chathistory.c. Leave the inbound.c call sites alone — the gate lives in one place.

Do **not** gate the JOIN-time `bouncer_inferred` block; it is harmless and still useful when STATUS was missed.

- [ ] **Step 5: Build**

Global Constraints build. Expected: empty output.

- [ ] **Step 6: End-to-end verification against AfterNET (record raw-log excerpts in the commit body)**

Precondition: AfterNET server rebuilt at ≥ `9bc57d4` (advertises `attach-cursor`). Configure profile `desktop`, SASL enabled, "Persistent server" ticked.

1. **Flight shape.** Raw log on connect shows, in one burst after `903`: `PERSISTENCE ATTACH desktop <msgid>` then `CAP END`. `001` follows. No `FAIL PERSISTENCE`.
2. **No double fetch.** After the JOIN burst there must be **zero** outbound `CHATHISTORY LATEST` / `CHATHISTORY TARGETS` lines. Inbound: `BATCH +x draft/persistence`, then `BATCH +y evilnet.github.io/bouncer-replay` containing `BATCH +z chathistory #chan` children (only if anything happened while held).
3. **Gap closure.** With a second client, post 20 lines to a channel while PoxChat is disconnected; reconnect → the 20 lines appear once each (msgid dedup), in order, in the right buffer. Post 3 PMs while held → they land in the query tab.
4. **Truncation → BEFORE.** Post more lines than the server's per-buffer replay cap (check the server's ISUPPORT `CHATHISTORY=` value; exceed it) → after the wrapper END the raw log shows our `CHATHISTORY BEFORE` pagination for the active tab bridging the remainder (this is the existing eager BEFORE loop kicked by `chathistory_replay_wrapper_end`).
5. **Evicted cursor.** Edit the DB (or use a copy of the profile with a fake msgid via `/quote` … simpler: temporarily set `persist_profile` on a network whose DB holds msgids from a *different* network) → `FAIL PERSISTENCE CURSOR_UNKNOWN`, and the raw log then shows our per-channel `CHATHISTORY LATEST` fan-out resuming.
6. **Unauthenticated.** Disable SASL → no ATTACH is sent at all (connect is otherwise normal).
7. **No profile.** Clear the profile field → no ATTACH; deferred LATEST fan-out behaves exactly as before Task 1.
8. **Restart persistence of read state is unaffected** (read-marker is out of scope; just confirm nothing regressed: unread counts on reconnect match the replayed lines).

- [ ] **Step 7: Commit**

```bash
git add src/common/persistence.c src/common/persistence.h src/common/proto-irc.c src/common/chathistory.c
git commit -m "persistence: PERSISTENCE ATTACH <profile> <cursor> in the SASL flight; skip redundant LATEST/TARGETS

Sends the attach (with the global newest scrollback msgid when the server
advertises attach-cursor) immediately before the SASL-success CAP END.
While the server drives replay from that cursor, the deferred per-channel
LATEST fan-out and TARGETS are suppressed; CURSOR_UNKNOWN re-arms them.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Documentation and skill update

**Files:**
- Modify: `.claude/skills/ircv3-implementation.md` (new section)
- Modify: `docs/design/2026-08-14-chathistory-gap-fill.md` §13 item 3 (mark "adopt draft/persistence" as implemented by this plan; leave the gap-fill Task 3 amendment pointing at `chathistory_batch_is_unsolicited`)
- Modify: `CLAUDE.md` "Already Implemented" list (add `draft/persistence (attach, attach-cursor)`)

- [ ] **Step 1: Skill section**

Append to `.claude/skills/ircv3-implementation.md`:

```markdown
## draft/persistence (Nefarious bouncer sessions)

- Cap value tokens gate verbs: `attach`, `attach-cursor`, `replay-control`, `list`.
  Parsed in `inbound_cap_ls` into `serv->persistence_*` bits; never assume from cap presence.
- `PERSISTENCE ATTACH <profile> [<msgid>]` is pre-CAP-END only and needs an account:
  sent from the 903 handler in proto-irc.c right before `CAP END` (`persistence_send_attach`).
  Cursor = `scrollback_get_global_newest_msgid` (one global anchor; msgids are HLC-ordered).
- Unsolicited `PERSISTENCE STATUS <pref> <effective>` (after 005) → `persistence_handle_status`;
  `effective=ON` suppresses reconnect autojoin like `persistent_server`.
- Revive sends `BATCH draft/persistence` (JOIN/TOPIC/NAMES, processed live) then
  `BATCH evilnet.github.io/bouncer-replay` wrapping per-target `chathistory` batches.
  `chathistory_batch_is_unsolicited` puts those into LATEST-phase catch-up without touching
  the request queue; wrapper END → `chathistory_replay_wrapper_end`.
- While `persistence_server_drives_replay()` is TRUE, `chathistory_schedule_deferred` and
  `chathistory_request_targets_on_reconnect` no-op. `FAIL PERSISTENCE CURSOR_UNKNOWN` re-arms them.
- Profile name lives in servlist (`PP=`), empty = legacy behavior (no ATTACH).
```

- [ ] **Step 2: Design-doc and CLAUDE.md touch-ups**, then commit:

```bash
git add .claude/skills/ircv3-implementation.md docs/design/2026-08-14-chathistory-gap-fill.md CLAUDE.md
git commit -m "docs: draft/persistence client adoption notes

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Roadmap after this plan (not tasks here)

| Item | Depends on | Notes |
|---|---|---|
| Gap-fill plan Task 3 (reconnect witness) consuming the unsolicited classification | Task 2 | Truncation in server replay = witnessed gap by construction (spec §13 item 2) |
| `PERSISTENCE PROFILE LIST` → profile picker in the servlist dialog instead of a free-text field | Task 4 | Needs a pre-connect or connected round trip; UX only |
| Reconcile auto-join favourites with the profile's channel list | Task 5 | Server-side profile list is authoritative when attached; our favlist path is currently *not* gated |
| `draft/read-marker` cross-device read state | — | Separate spec; explicitly out of scope of gap-fill §12 |
| `PERSISTENCE REPLAY SET` UI | — | Moot for a chathistory-capable client; skip unless users ask |

## Self-review notes

- Spec coverage: §13 items 1 (Task 2), 2 (Task 2 + gap-fill Task 3), 3 (Tasks 3-5), 4 (none needed); §13.1 bullets: tokens (T1), pre-CAP-END timing (T5), STATUS handler (T1), wrapper batches + fan-out suppression (T2, T5), REPLAY moot (roadmap), profiles (T4 + roadmap).
- Names used across tasks: `persistence_handle_status/fail/reset` (T1) ← proto-irc/inbound; `chathistory_batch_is_unsolicited`, `chathistory_begin_unsolicited_catchup`, `chathistory_replay_wrapper_end`, `catchup_enter_latest_phase` (T2); `scrollback_get_global_newest_msgid` (T3) ← T5; `serv->persist_profile` (T4) ← T5; `persistence_send_attach`, `persistence_server_drives_replay` (T5) ← chathistory.c.
- Known unverifiable-until-server-rebuilt: Task 5 Step 6 items 1-5 need a server at ≥ `9bc57d4`; if AfterNET has not been rebuilt, complete Tasks 1-4 and Task 5 Steps 1-5, run Step 6 items 6-7, and leave the commit with a "pending server rebuild" note.
