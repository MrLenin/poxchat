# IRCv3 UX Audit - Missing Aspects and Gaps

This document identifies UX considerations missing from the implementation plan, gaps between implementation and UX design, and additional requirements discovered during review.

---

## 1. Features Missing from UX Section

### 1.1 UTF8ONLY (ISUPPORT)

**Status:** Implemented, UX incomplete

**What we have:**
- Flag `serv->utf8only` set when server advertises UTF8ONLY
- Encoding forced to UTF-8 via `server_set_encoding()`
- `/CHARSET` command blocked with error message

**Missing UX considerations:**
- [ ] Visual indicator in server info showing encoding is locked
- [ ] Network list GUI: show UTF8ONLY status for connected servers
- [ ] `/ENCODING` or `/CHARSET` command should show "(enforced by server)" when utf8only
- [ ] Tooltip/status bar indication when connected to UTF8ONLY server
- [ ] Consider: should we hide encoding dropdown in network edit dialog when utf8only is active?

**User feedback needed:**
- When encoding cannot be changed, user should understand why
- Current message "Cannot change encoding: server requires UTF-8 (UTF8ONLY)" is good but could be more discoverable

---

### 1.2 STS (Strict Transport Security)

**Status:** Implemented, UX incomplete

**What we have:**
- STS policies stored persistently
- Connection automatically upgraded to TLS port
- Policy expiration handling

**Missing UX considerations:**
- [ ] User notification when STS upgrades connection: "Connection upgraded to TLS (port 6697) per server security policy"
- [ ] `/STS` command to list active policies
- [ ] `/STS CLEAR [host]` to remove policies (plan mentions button, no command)
- [ ] Network list GUI: indicator for servers with active STS policy
- [ ] Settings > Security panel: "View STS Policies" button → dialog listing all policies
- [ ] What happens if STS upgrade fails? User feedback for this scenario
- [ ] Log/display when STS policy is added or updated

**Preferences (plan mentions but incomplete):**
- `hex_irc_sts_honor` - Honor STS policies (default: ON)
- `hex_irc_sts_preload` - Use hardcoded preload list (default: ON, if we have one)

---

### 1.3 draft/channel-rename (RENAME)

**Status:** Implemented, UX incomplete

**What we have:**
- Capability negotiation
- RENAME command handler updates session channel name
- Tab label and title updated via `fe_set_channel()` and `fe_set_title()`

**Missing UX considerations:**
- [ ] Text event for channel rename (plan mentions XP_TE_CHANRENAME but not implemented)
- [ ] Message in channel: "*** Channel has been renamed from #old to #new by nick [reason]"
- [ ] History/logging consideration: should log entry note the rename?
- [ ] User notification in session: visual indication that rename occurred
- [ ] Consider: what if user has channel in multiple windows/layouts?

---

### 1.4 draft/pre-away and Presence Aggregation

**Status:** Implemented (cap negotiation), UX not defined

**What we have:**
- `serv->have_pre_away` flag set when capability ACKed

**How presence aggregation works (Nefarious/X3):**

*Three-State Model (not binary):*
| State | Value | Meaning |
|-------|-------|---------|
| `CONN_PRESENT` | 0 | Not away |
| `CONN_AWAY` | 1 | Away with message |
| `CONN_AWAY_STAR` | 2 | Hidden connection (`AWAY *`) |

*Precedence ("most-present-wins"):*
1. `CONN_PRESENT` beats everything
2. `CONN_AWAY` beats `CONN_AWAY_STAR`
3. `CONN_AWAY_STAR` only shows if ALL connections hidden

*Hidden Connections (`AWAY *`):*
- Special syntax `AWAY *` marks connection as hidden/background
- Useful for bouncers, mobile push listeners
- Doesn't contribute to presence - user can appear "away" even with this connection active
- Server provides fallback message (default: "Away")

*Virtual Metadata Keys (query aggregated state):*
| Key | Value |
|-----|-------|
| `$presence` | `present`, `away`, or `away-star` |
| `$away_message` | The effective away message (absent if none) |
| `$last_present` | Unix timestamp of last non-away activity |

```
METADATA <nick> GET $presence
```

*draft/pre-away capability:*
- Allows setting away state BEFORE registration completes
- Useful for bouncers to immediately signal background connection
- Away state stored and applied after registration

**UX Implementation Plan:**

*Away Indicator States (three-state + hidden):*
| Icon | Meaning |
|------|---------|
| 🟢 | Present (you or aggregated) |
| 🟡 | Away (this client), Present (network) |
| 🔴 | Away (all connections) |
| ⚫ | Hidden (`AWAY *`) |

*Commands to support:*
```
/AWAY *       → Mark this connection as hidden (won't affect network presence)
/AWAY         → Clear away
/AWAY <msg>   → Normal away with message
```

*Query own presence:*
- Use `METADATA <self> GET $presence` to show network view
- Display: "You are: Away (this client) | Network sees: Online (other connections active)"

**Missing UX considerations:**
- [ ] Implement `/AWAY *` support for hidden connections
- [ ] Query `$presence` metadata on connect to show aggregated state
- [ ] Three-state away indicator in UI (present/away-local/away-network/hidden)
- [ ] Enhanced `/AWAY` response showing both local and network state
- [ ] Tooltip on away indicator explaining multi-connection behavior
- [ ] Consider showing connection count: "Online (2 other connections)"
- [ ] `/WHOIS self` enhancement to show per-connection breakdown (if X3 provides)

---

### 1.5 draft/ICON (Network Icon)

**Status:** Implemented (storage), no UX

**What we have:**
- `serv->network_icon_url` stored from ISUPPORT

**Missing UX considerations:**
- [ ] Where to display the network icon?
  - Server tab icon
  - Network list entry
  - Window title area
- [ ] Icon caching strategy (don't fetch on every connect)
- [ ] Fallback if icon URL is invalid or unreachable
- [ ] Size/format constraints (what if server provides huge image?)
- [ ] Security: only load from HTTPS? Content-type validation?
- [ ] Preference: `hex_gui_network_icons` - Show network icons (default: ON)

---

### 1.6 Standard Replies (FAIL/WARN/NOTE)

**Status:** Implemented, UX incomplete

**What we have:**
- FAIL handled with XP_TE_FAIL and XP_TE_FAILCMD text events
- Basic message display

**Missing UX considerations:**
- [ ] Visual distinction between FAIL (error), WARN (warning), NOTE (info)
  - FAIL: Red text or error icon
  - WARN: Yellow/orange text or warning icon
  - NOTE: Blue text or info icon
- [ ] Text events for WARN and NOTE (currently only FAIL exists)
- [ ] Structured display: "FAIL [COMMAND] CODE: message" format
- [ ] Consider: should these appear in a dedicated "Server Messages" window?

---

### 1.7 MONITOR vs WATCH

**Status:** Both implemented, UX not defined

**What we have:**
- MONITOR support with numerics 730-734
- Legacy WATCH support

**Missing UX considerations:**
- [ ] Transparent to user - should "just work"
- [ ] Debug/info mode: `/NOTIFY DEBUG` to show which method is being used
- [ ] Consider: preference to force one method over another? Probably not needed.

---

### 1.8 SASL OAUTHBEARER Token Refresh

**Status:** Implemented, UX incomplete

**What we have:**
- Token storage in secure storage
- Refresh token logic in oauth.c

**Missing UX considerations:**
- [ ] User notification when token is refreshed automatically
- [ ] User notification when refresh fails (with actionable next steps)
- [ ] Token status indicator: "OAuth: Valid until [time]" or "OAuth: Token expired"
- [ ] `/OAUTH STATUS` command to check token state
- [ ] `/OAUTH REAUTH` command to force re-authentication
- [ ] Settings: OAuth token management UI
  - View connected OAuth accounts
  - Revoke/remove tokens
  - Re-authenticate button

---

### 1.9 SETNAME (Real Name Change)

**Status:** Protocol supported, no explicit UX

**What we have:**
- SETNAME command handler in proto-irc.c (via chghost handling?)

**Missing UX considerations:**
- [ ] `/SETNAME <new realname>` command for users
- [ ] Notification when own realname changes
- [ ] Notification when other user's realname changes (if displayed)
- [ ] Consider: should realname be shown somewhere in UI? Tooltip on nick?

---

### 1.10 CHGHOST

**Status:** Implemented, UX minimal

**What we have:**
- Host change handling in protocol

**Missing UX considerations:**
- [ ] User notification when own host changes
- [ ] Notification format: "Your host is now user@newhost.example.com"
- [ ] Should this be a text event? XP_TE_OWNHOSTCHANGE?

---

## 2. Gaps in Existing UX Sections

### 2.1 Chathistory - Scroll Debouncing

**CRITICAL: Missing from plan**

**Problem:** If user scrolls very fast, we could spam CHATHISTORY BEFORE requests.

**Required implementation:**
- [ ] Debounce scroll-triggered history requests (minimum 500ms between requests)
- [ ] Show "Loading..." indicator while request is pending
- [ ] Don't trigger new request while one is in flight (`history_loading` flag)
- [ ] Consider: maximum requests per time period (rate limiting)

**Implementation approach:**
```c
// In session struct
guint history_scroll_timer;    /* Debounce timer ID */
gboolean history_scroll_pending; /* Request queued */

// On scroll to top:
// 1. If history_loading, do nothing
// 2. If timer active, reset timer
// 3. Else start timer, fire request after 500ms
```

---

### 2.2 Chathistory - Non-Joined Channels

**Question raised:** Can we request history for channels we're not in?

**Answer per IRCv3 spec:** Server policy decision (not a hard rule). The spec says:
> "Given conventional expectations around channel membership, servers MAY wish to disallow clients from querying the history of channels they are not joined to. If they do not, they SHOULD disallow clients from querying channels that they are banned from, or which are private."

**Implications:**
- Some servers allow history for non-joined public channels
- Some servers restrict to members only
- Server will return FAIL if access denied

**UX approach:**
- [ ] Allow `/HISTORY #channel` regardless of membership - let server decide
- [ ] Handle FAIL response gracefully: "Access denied" or "Channel history not available"
- [ ] Don't preemptively block requests - server is the authority
- [ ] Consider: "Load history" option in channel list for non-joined channels (if server supports)

---

### 2.3 Chathistory - Automatic Fetch Timing

**Missing detail:** When exactly should auto-fetch trigger?

**Clarification needed:**
- [ ] On JOIN: fetch LATEST immediately after 353/366 (NAMES complete)?
- [ ] On reconnect: how long to wait before AFTER request?
- [ ] Rate limit between channels when joining multiple on connect
- [ ] Priority: active window first, then others?

---

### 2.4 Echo-Message - Visual Pending State

**Plan mentions but vague:**

**Specific implementation needed:**
- [ ] How to mark message as "pending" in text buffer?
  - Option A: Muted color (gray text)
  - Option B: Italic rendering
  - Option C: Small icon/indicator
- [ ] How to "replace" pending message with echoed one?
  - By msgid correlation
  - What if no msgid support?
- [ ] Timer mechanism for timeout fallback

---

### 2.5 Read Marker - Implementation Details (from Nefarious/X3)

**Architecture:**
- X3 is the **authoritative store** for read markers
- Servers maintain local LMDB caches for fast lookups
- Multi-device sync via broadcast to all servers

**Protocol:**

*Set marker:*
```
MARKREAD <target> timestamp=<ts>
```

*Query marker:*
```
MARKREAD <target>
```

*Response (from server):*
```
:server MARKREAD <target> timestamp=<ts>
```

**Key behaviors:**
- **Nefarious/X3 requires authentication** - markers stored per-account (not per-connection)
  - *Note: IRCv3 spec doesn't mandate auth - this is implementation-specific*
  - Other servers may store per-nick or per-connection
- Timestamps use Unix time with **microsecond precision** (e.g., `1705500000.123456`)
- Validation: new timestamp must be **> existing** (can't go backwards)
- Broadcasts sync all devices automatically (to all connections of the same account)

**Multi-device flow:**
1. Device A marks #channel read at timestamp T
2. Server forwards to X3 (authoritative)
3. X3 validates T > existing timestamp
4. X3 broadcasts update to all servers
5. Server 2 (with Device B) notifies Device B
6. Device B updates its unread count

**PoxChat implementation plan:**

*When to send MARKREAD:*
- [ ] On window focus (if auto-mark enabled) - use newest message timestamp
- [ ] On scroll past marker (marker moves with scroll)
- [ ] On sending a message (implies caught up)
- [ ] On explicit `/MARKREAD` command

*When to query MARKREAD:*
- [ ] On JOIN - query current position: `MARKREAD #channel`
- [ ] On reconnect - query all joined channels

*Tracking timestamps:*
- [ ] Store `server-time` tag from messages
- [ ] Use message timestamp (not local time) for MARKREAD
- [ ] Session field: `time_t last_read_timestamp` (microseconds)

*Local fallback (no server support):*
- [ ] If `have_read_marker` is FALSE, still track locally
- [ ] Store in session, persist to channel options file
- [ ] No sync across devices, but still useful for single-device

*Unread count:*
- [ ] Track count of messages after `last_read_timestamp`
- [ ] Update tab badge with unread count
- [ ] Clear count when marker advances

**Missing UX considerations:**
- [ ] Handle MARKREAD response to update local state
- [ ] Visual marker line in text buffer at read position
- [ ] Tab badge showing unread count since marker
- [ ] "Jump to first unread" command/button
- [ ] Preference: auto-mark on focus (default: OFF for privacy)
- [ ] **Auth handling** (implementation-specific, not spec-mandated):
  - Nefarious/X3 requires auth - markers are per-account
  - Other servers may work without auth
  - If MARKREAD fails with auth error, fall back to local-only tracking
  - Don't preemptively block - let server decide

---

### 2.6 Typing Indicators - State Management

**Missing details:**

- [ ] How long to show "typing" before auto-clearing? (Timeout: 6 seconds typical)
- [ ] Multiple users typing: "user1, user2 are typing..." format
- [ ] More than 3 users: "user1, user2, and 2 others are typing..."
- [ ] Clear typing state when message received from that user

---

### 2.7 Multiline - Server Limit Display

**Missing:** How to show server limits to user

- [ ] When paste exceeds limits, dialog should show:
  - "Server limit: X lines / Y bytes"
  - "Your paste: A lines / B bytes"
  - "Exceeds limit by: ..."
- [ ] `/MULTILINE INFO` command to show current server limits

---

### 2.8 Event Playback - Visual Distinction

**Vague in plan:** "Visual distinction for historical vs live events"

**Specific approach needed:**
- [ ] Option A: Different timestamp color for historical events
- [ ] Option B: Small icon or marker on historical events
- [ ] Option C: History section bracketed by separator lines (current approach)
- [ ] Preference to choose method?

---

## 3. Missing Preferences Summary

### New Preferences Needed

| Preference | Type | Default | Description |
|------------|------|---------|-------------|
| `hex_irc_utf8only_indicator` | bool | ON | Show indicator when server enforces UTF-8 |
| `hex_irc_sts_honor` | bool | ON | Honor STS security policies |
| `hex_irc_sts_notify` | bool | ON | Show notification on STS upgrade |
| `hex_gui_network_icons` | bool | ON | Download and display network icons |
| `hex_irc_history_debounce` | int | 500 | Milliseconds between scroll history requests |
| `hex_irc_oauth_status_notify` | bool | ON | Show OAuth token refresh notifications |
| `hex_irc_standard_reply_colors` | bool | ON | Color-code FAIL/WARN/NOTE messages |
| `hex_irc_typing_timeout` | int | 6000 | Milliseconds before clearing typing indicator |

---

## 4. Missing Text Events

| Event | Constant | Parameters | Description |
|-------|----------|------------|-------------|
| Channel Renamed | XP_TE_CHANRENAME | old, new, nick, reason | Channel was renamed |
| STS Upgrade | XP_TE_STSUPGRADE | host, port | Connection upgraded via STS |
| Own Host Changed | XP_TE_OWNHOSTCHANGE | newhost | Your host/cloak changed |
| Server Warning | XP_TE_WARN | command, code, message | WARN standard reply |
| Server Note | XP_TE_NOTE | command, code, message | NOTE standard reply |
| OAuth Token Refreshed | XP_TE_OAUTHREFRESH | account | OAuth token auto-refreshed |
| OAuth Token Failed | XP_TE_OAUTHFAIL | account, error | OAuth refresh failed |

---

## 5. Missing Commands

| Command | Format | Description |
|---------|--------|-------------|
| `/STS` | `/STS [LIST\|CLEAR [host]]` | Manage STS policies |
| `/SETNAME` | `/SETNAME <realname>` | Change real name (if server supports) |
| `/OAUTH` | `/OAUTH STATUS\|REAUTH` | OAuth token management |
| `/MULTILINE` | `/MULTILINE INFO` | Show server multiline limits |

---

## 6. Feature Interdependencies

Understanding how features depend on each other helps plan implementation order and reveals UX synergies.

### 6.1 Dependency Graph

```
                    ┌─────────────────┐
                    │     batch       │ ◄── Foundation
                    └────────┬────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
        ▼                    ▼                    ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐
│  chathistory  │   │   multiline   │   │labeled-response│
└───────┬───────┘   └───────────────┘   └───────┬───────┘
        │                                       │
        ▼                                       ▼
┌───────────────┐                       ┌───────────────┐
│event-playback │                       │  echo-message │
└───────────────┘                       └───────────────┘


                    ┌─────────────────┐
                    │  message-tags   │ ◄── Foundation
                    └────────┬────────┘
                             │
   ┌─────────────────────────┼─────────────────────────┐
   │              │          │          │              │
   ▼              ▼          ▼          ▼              ▼
┌──────┐   ┌───────────┐ ┌───────┐ ┌─────────┐  ┌──────────┐
│msgid │   │  +typing  │ │ label │ │ batch   │  │ account  │
│track │   │ indicator │ │  tag  │ │  tag    │  │   tag    │
└──┬───┘   └───────────┘ └───────┘ └─────────┘  └──────────┘
   │
   ├──► chathistory pagination (BEFORE msgid=xxx)
   ├──► redaction (REDACT target msgid)
   └──► read-marker (MARKREAD target msgid)


                    ┌─────────────────┐
                    │    metadata     │ ◄── Enables rich presence/profiles
                    └────────┬────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
        ▼                    ▼                    ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐
│  $presence    │   │    avatar     │   │  display-name │
│  $away_msg    │   │  (user/chan)  │   │   bot flag    │
│  $last_present│   └───────────────┘   └───────────────┘
└───────┬───────┘
        │
        ▼
┌───────────────┐
│   pre-away    │ ◄── Rich presence UX requires metadata
│  aggregation  │
└───────────────┘
```

### 6.2 Feature Synergies

| Feature Combo | Synergy |
|---------------|---------|
| **chathistory + read-marker** | On reconnect, use read-marker position to request CHATHISTORY AFTER, fetching only missed messages |
| **chathistory + event-playback** | Get full context (joins/parts/topics) not just messages |
| **echo-message + labeled-response** | Correlate sent message with server echo using label tag |
| **pre-away + metadata** | Query `$presence` to show aggregated away state across connections |
| **multiline + batch** | Multiline messages ARE batches - no multiline without batch support |
| **redaction + msgid** | Redaction requires msgid - no redaction without message-tags |
| **typing + TAGMSG** | Typing indicators use `TAGMSG` with `+typing` client tag |

### 6.3 Implementation Order Implications

**Must implement first (foundations):**
1. `batch` - Required by chathistory, multiline, labeled-response
2. `message-tags` - Required by msgid tracking, typing, labels

**Can implement in parallel (no dependencies on each other):**
- STS (independent)
- UTF8ONLY (independent)
- channel-rename (independent)
- account-registration (independent)
- MONITOR (independent)

**Requires foundations:**
- chathistory → needs batch
- multiline → needs batch
- typing indicators → needs message-tags + TAGMSG
- redaction → needs message-tags (msgid)
- read-marker → needs message-tags (msgid)
- rich presence UX → needs metadata + pre-away

### 6.4 UX Features That Work Best Together

**"Seamless Reconnect" experience:**
- chathistory + read-marker + event-playback
- On reconnect: query read-marker → CHATHISTORY AFTER → show missed messages with context

**"Modern Chat" experience:**
- typing indicators + read-marker + echo-message + multiline
- Real-time feedback on who's typing, message delivery confirmation, proper code blocks

**"Multi-Device" experience:**
- pre-away + metadata ($presence) + read-marker
- Aggregated presence, synced read position across devices

**"Rich Profiles" experience:**
- metadata (avatar, display-name, status, bot)
- User profiles with avatars and custom display names

---

## 7. Finalized UX Decisions

### Decision Summary Table

| # | Topic | Decision |
|---|-------|----------|
| 1 | Echo-message pending | Muted color (theme-aware), possibly italic. Must work with color themes. |
| 2 | Historical events (chathistory) | Render as seamless scrollback (no special visual). First join: history before "You are now talking on". Reconnect: history fills gap. Explicit `/HISTORY`: may use separators. |
| 3 | Network icon location | Channel tree (replace `pix_tree_server`) + Network list. Lower priority. |
| 4 | Standard replies (FAIL/WARN/NOTE) | Route to current session. Use text events (XP_TE_FAIL exists, add XP_TE_WARN, XP_TE_NOTE) with theme-customizable colors. |
| 5 | Chathistory auto-fetch timing | Small delay after JOIN (batch multiple joins), rate limiting essential, stagger on reconnect. |
| 6 | UTF8ONLY dropdown | Leave visible, error on change attempt (disable requires active connection). |
| 7 | Read marker auto-advance | Window focus = OFF default; scroll past marker = ON; send message = ON. |
| 8 | Multiline display style | Inline (code block style would need explicit markup - future enhancement). |
| 9 | Multiline expand behavior | Both: inline expand for medium, popup dialog for very large. |
| 10 | Typing indicators | Channels: userlist icon. Queries: needs design work - something subtle, not traditional status bar. Defer query display to implementation. |
| 11 | Reply quote style | Quote above reply with muted style, click to scroll to original. |
| 12 | Reactions | Approved design. Note: emoji support is font-limited (larger issue). |
| 13 | Whisper (channel-context) | Normal PM with "[via #channel]" context indicator. |
| 14 | Event playback collapsing | Respect existing `hex_irc_conf_mode` / `text_hidejoinpart` setting. Collapsing as general enhancement deferred. |
| 15 | Account registration | `/REGISTER <account> <email> <password>` with GUI in Network Manager. |

### Chathistory Display Flow

**First JOIN (new channel):**
```
[chathistory messages - seamless scrollback]
(23:52:04) --> You are now talking on #channel
(23:52:04) Topic for #channel is: ...
[live messages]
```

**Reconnect:**
```
[previous session messages]
(23:50:00) Disconnected (Connection reset)
[chathistory fills gap - seamless]
(23:52:04) Connected. Now logging in.
(23:52:04) --> You are now talking on #channel
[live messages]
```

**Explicit `/HISTORY` request:**
- May use separator lines to distinguish requested history
- More flexible presentation since user explicitly asked

---

## 8. Action Items by Priority

### High Priority (UX Blockers)
1. Chathistory scroll debouncing (prevent request spam)
2. Chathistory display ordering (hold JOIN banner until history fetched)
3. Echo-message pending state visual (muted/italic, theme-aware)
4. Standard replies - add XP_TE_WARN, XP_TE_NOTE text events

### Medium Priority (Polish)
5. UTF8ONLY error message on `/CHARSET` change attempt
6. Channel rename text event (XP_TE_CHANRENAME)
7. Typing indicator - userlist icon for channels
8. STS upgrade notification text event

### Lower Priority (Nice to Have)
9. Network icon fetch/cache/display
10. `/STS` command for policy management
11. Query typing indicator (design TBD)
12. Whisper "[via #channel]" indicator
13. Reply quote visual design
14. Reaction display (depends on emoji improvements)

---

## 9. Testing Checklist

- [ ] UTF8ONLY: Connect to server, try `/CHARSET latin1`, verify error message
- [ ] STS: Connect to server with STS, verify TLS upgrade (text event)
- [ ] Channel rename: Have op rename channel, verify tab updates + text event
- [ ] Chathistory (first join): Join channel, verify history appears BEFORE "You are now talking on"
- [ ] Chathistory (reconnect): Disconnect/reconnect, verify history fills gap seamlessly
- [ ] Chathistory scroll: Scroll rapidly to top, verify debouncing (no request spam)
- [ ] Echo-message: Send message, verify pending→confirmed visual transition
- [ ] Standard replies: Trigger FAIL/WARN/NOTE, verify distinct text events
- [ ] Typing (channel): Start typing, verify userlist icon appears for others
- [ ] Read marker: Change windows, verify marker doesn't auto-advance (focus=OFF)
- [ ] Read marker: Scroll past marker, verify it advances
- [ ] Multiline receive: Receive multiline batch, verify single cohesive display
- [ ] Multiline large: Receive >10 line message, verify truncation + expand option

---

## 10. Future Work: xtext Buffer Prepend Support

### Problem Statement

Currently, xtext (the text rendering widget) only supports appending text to the buffer. When loading older history via chathistory, messages are appended at the bottom rather than prepended at the top where they logically belong. This creates a suboptimal UX:

1. **Visual discontinuity**: Older messages appear below newer ones during history load
2. **Scroll position disruption**: User loses their place when history is inserted
3. **Timestamp ordering**: Messages may not appear in chronological order during loading

### Why This Is Essential

Proper chathistory UX requires the ability to:
- **Prepend messages**: Insert older history at the TOP of the buffer
- **Maintain scroll position**: Keep user's view stable when inserting above viewport
- **Seamless pagination**: Load older messages on scroll-to-top without jumping

Without prepend support, chathistory will always have compromises in how it displays historical messages.

### Required xtext Changes

**Core buffer changes:**
1. Add `gtk_xtext_prepend_text()` function (or mode flag to existing functions)
2. Modify `textentry` linked list to support efficient head insertion
3. Update line numbering/indexing for prepended content

**Scroll position handling:**
1. Before prepend: Save current scroll offset and first visible line
2. After prepend: Calculate new scroll position to keep same content visible
3. Handle edge cases: scrolled to top, partially visible lines, selection active

**Rendering updates:**
1. Invalidate cached line positions after prepend
2. Recalculate visible range
3. Handle marker line position adjustment (read marker, etc.)

### Implementation Approach

```c
/* Proposed API */
void gtk_xtext_prepend_text (GtkXText *xtext, const char *text, time_t timestamp);

/* Or mode flag */
void gtk_xtext_set_insert_mode (GtkXText *xtext, GtkXTextInsertMode mode);
/* where mode is GTK_XTEXT_INSERT_APPEND or GTK_XTEXT_INSERT_PREPEND */

/* Scroll position preservation */
typedef struct {
    int first_visible_line;
    int scroll_offset_px;
} GtkXTextScrollState;

void gtk_xtext_save_scroll_state (GtkXText *xtext, GtkXTextScrollState *state);
void gtk_xtext_restore_scroll_state (GtkXText *xtext, const GtkXTextScrollState *state, int lines_prepended);
```

### Use Cases Beyond Chathistory

This capability would also benefit:
- **Log replay**: Prepending historical logs on session start
- **Search results**: Inserting context lines around search matches
- **Backlog from bouncers**: ZNC playback could use proper ordering
- **Any "load more" pattern**: Common in modern chat UIs

### Current Workaround

Until xtext prepend is implemented, chathistory uses these workarounds:
- Background fetching is time-capped (default 24 hours) to limit out-of-order display
- Scroll-to-load shows separators to visually distinguish history batches
- Initial join history appears in correct order (fetched before any live messages)

### Priority

**HIGH** - This is foundational infrastructure that affects multiple features. The current workarounds are functional but not ideal. Proper prepend support would significantly improve the chathistory and scrollback experience.
