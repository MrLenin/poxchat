# Comprehensive IRCv3 Implementation Plan for PoxChat

## Overview

This plan completes IRCv3 support in PoxChat using Nefarious IRCd and X3 services as reference implementations. The plan covers all finalized IRCv3.1/3.2 specs plus widely-deployed draft specifications.

---

## Current State Analysis

### PoxChat - Already Implemented
| Capability | Status | Notes |
|------------|--------|-------|
| multi-prefix | ✅ | `serv->have_namesx` |
| away-notify | ✅ | `serv->have_awaynotify` |
| account-notify | ✅ | `serv->have_accnotify` |
| extended-join | ✅ | `serv->have_extjoin` |
| server-time | ✅ | `serv->have_server_time` |
| userhost-in-names | ✅ | `serv->have_uhnames` |
| cap-notify | ✅ | Supported (no flag) |
| chghost | ✅ | Supported (no flag) |
| setname | ✅ | Supported (no flag) |
| invite-notify | ✅ | Supported (no flag) |
| account-tag | ✅ | `serv->have_account_tag` |
| SASL | ⚠️ | PLAIN, EXTERNAL, SCRAM-SHA-* complete; OAUTHBEARER missing token refresh/re-auth |

### PoxChat - Missing (Gap Analysis vs Nefarious)
| Capability | Priority | Dependency |
|------------|----------|------------|
| batch | **Critical** | Foundation for chathistory, multiline |
| message-tags (full) | **Critical** | Foundation for msgid, client tags |
| labeled-response | High | Better UX, error correlation |
| echo-message | High | Message delivery confirmation |
| TAGMSG | Medium | Typing indicators, reactions |
| draft/chathistory | High | Missed message retrieval |
| draft/multiline | Medium | Code block pasting |
| draft/message-redaction | Medium | Message deletion |
| draft/read-marker | Medium | Multi-device sync |
| draft/event-playback | High | JOIN/PART/KICK/MODE in history |
| draft/channel-rename | Low | Channel renames |
| draft/no-implicit-names | Low | Optimization |
| standard-replies | Low | Already partially implemented |
| sts | Medium | Security hardening |
| draft/account-registration | Low | In-band registration |
| draft/metadata-2 | Low | User metadata |
| MONITOR | Low | Alternative to WATCH |
| draft/pre-away | Low | Bouncer/multi-connection away coordination |
| draft/channel-context | Medium | Channel context for private messages (whispers) |
| draft/reply | Medium | Message replies with msgid reference |
| draft/react | Low | Message reactions with emoji |
| UTF8ONLY | Low | Server enforces UTF-8 encoding |
| draft/ICON | Low | Network icon URL from ISUPPORT |
| SNI | Medium | TLS Server Name Indication |
| SASL OAUTHBEARER refresh | Medium | Token refresh & IRCv3.2 re-authentication |
| SASL ECDSA-NIST256P-CHALLENGE | Medium | Challenge-response auth with ECDSA keys |
| X3 Session Tokens | Low | Store/use session tokens from X3 services |

---

## Implementation Phases

### Phase 1: Foundation (batch + message-tags)

**Goal:** Establish the infrastructure required for all advanced features.

#### 1.1 batch Capability

**Files to modify:**
- [proto-irc.h](src/common/proto-irc.h) - Extend `message_tags_data` with batch fields
- [poxchat.h](src/common/poxchat.h) - Add `have_batch` flag, `batch_info` struct, `active_batches` hash table
- [inbound.c](src/common/inbound.c) - Add to `supported_caps[]`, implement `inbound_batch_start()`, `inbound_batch_end()`
- [proto-irc.c](src/common/proto-irc.c) - Parse `batch` tag, add BATCH command handler

**Key structures:**
```c
// In poxchat.h
typedef struct batch_info {
    char *id;
    char *type;           // "chathistory", "multiline", "netjoin", etc.
    char **params;
    char *outer_batch;    // For nested batches
    GSList *messages;     // Collected messages
    time_t started;
} batch_info;

// In server struct
unsigned int have_batch:1;
GHashTable *active_batches;  // batch_id -> batch_info
```

**Batch types to support:**
- `chathistory` - History playback
- `multiline` - Multi-line messages
- `netjoin`/`netsplit` - Network events
- `labeled-response` - ACK batches

#### 1.2 Full message-tags Support

**Files to modify:**
- [proto-irc.h](src/common/proto-irc.h) - Add `all_tags` hash table, `msgid`, `label` fields
- [poxchat.h](src/common/poxchat.h) - Add `have_message_tags` flag
- [inbound.c](src/common/inbound.c) - Add capability support
- [proto-irc.c](src/common/proto-irc.c) - Parse all tags, handle `+` client-only tags, tag escaping

**Extended message_tags_data:**
```c
typedef struct {
    char *account;
    gboolean identified;
    time_t timestamp;
    char *batch_id;
    char *msgid;           // For redaction, read-marker
    char *label;           // For labeled-response
    GHashTable *all_tags;  // Full tag storage for plugins
} message_tags_data;
```

#### 1.3 TAGMSG Command

**Files to modify:**
- [proto-irc.c](src/common/proto-irc.c) - Add TAGMSG handler
- [inbound.c](src/common/inbound.c) - Add `inbound_tagmsg()` function
- [outbound.c](src/common/outbound.c) - Add `/TAGMSG` command
- [textevents.in](src/common/textevents.in) - Add TAGMSG event

---

### Phase 2: User Experience (echo-message + labeled-response)

#### 2.1 echo-message Capability

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_echo_message` flag
- [inbound.c](src/common/inbound.c) - Add capability, modify `inbound_chanmsg()`/`inbound_privmsg()` to detect self-echoes
- [outbound.c](src/common/outbound.c) - Modify `handle_say()` to defer display when echo-message enabled

**Design considerations:**
- Add timeout for unechoed messages (fall back to local display)
- User preference to disable
- Handle network splits gracefully

#### 2.2 labeled-response Capability

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_labeled_response`, `label_counter`, `pending_labels` hash table
- [inbound.c](src/common/inbound.c) - Add capability, label management functions
- [proto-irc.c](src/common/proto-irc.c) - Parse `label` tag, handle ACK batch type
- [outbound.c](src/common/outbound.c) - Add labels to outgoing commands

---

### Phase 3: History Features

#### 3.1 draft/chathistory Capability

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_chathistory` flag, `chathistory_limit` from ISUPPORT
- [inbound.c](src/common/inbound.c) - Add capability, handle `chathistory` batch type
- [outbound.c](src/common/outbound.c) - Add `/HISTORY` command
- [textevents.in](src/common/textevents.in) - Add history events
- [text.c](src/common/text.c) - Msgid tracking in text buffer for deduplication
- [modes.c](src/common/modes.c) - Parse CHATHISTORY ISUPPORT token for server limits

**New file:** [chathistory.c](src/common/chathistory.c)
- `chathistory_request_latest(target, limit)` - Get most recent messages
- `chathistory_request_before(target, msgid_or_ts, limit)` - Get messages before reference
- `chathistory_request_after(target, msgid_or_ts, limit)` - Get messages after reference (for catch-up)
- `chathistory_request_targets(start_ts, end_ts, limit)` - Get active conversations
- `chathistory_handle_batch(batch_info)` - Process received batch
- `chathistory_deduplicate(session, messages)` - Skip messages already in buffer
- `chathistory_insert_sorted(session, messages)` - Insert at correct position (prepend for BEFORE)

**Session tracking (in session struct):**
```c
char *oldest_msgid;      /* Oldest message in buffer (for BEFORE requests) */
char *newest_msgid;      /* Newest message in buffer (for AFTER requests) */
gboolean history_exhausted;  /* Server has no more history */
gboolean history_loading;    /* Request in progress */
```

**CHATHISTORY subcommands:**
- `LATEST <target> * <limit>` - Most recent messages
- `BEFORE <target> timestamp=<time> <limit>` - Before a point (pagination)
- `AFTER <target> timestamp=<time> <limit>` - After a point (catch-up)
- `AROUND <target> timestamp=<time> <limit>` - Around a point
- `BETWEEN <target> timestamp=<start> timestamp=<end> <limit>` - Range
- `TARGETS timestamp=<start> timestamp=<end> <limit>` - Active conversations

**ISUPPORT tokens:**
- `CHATHISTORY=<limit>` - Maximum messages per request
- `MSGREFTYPES=timestamp,msgid` - Supported reference types

#### 3.2 draft/event-playback Capability

**Rationale:** Extends chathistory to include non-message events (JOIN, PART, QUIT, KICK, MODE, TOPIC). Essential for understanding channel context when reviewing history.

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_event_playback` flag
- [inbound.c](src/common/inbound.c) - Add capability to `supported_caps[]`
- [chathistory.c](src/common/chathistory.c) - Handle event messages in batch processing

**Event types stored in history:**
- `JOIN` - User joined channel (with extended-join account/realname)
- `PART` - User left channel (with reason)
- `QUIT` - User disconnected (with reason)
- `KICK` - User was kicked (with reason)
- `MODE` - Channel mode changes
- `TOPIC` - Topic changes
- `NICK` - Nick changes
- `TAGMSG` - Tag-only messages (typing indicators, reactions)

**Display considerations:**
- Events should render with appropriate text events (existing XP_TE_* events)
- Historical events need visual distinction from live events
- Consider collapsing repeated JOIN/PART (anti-flood)

#### 3.3 draft/read-marker Capability

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_read_marker`, add `last_read_msgid` to session struct
- [inbound.c](src/common/inbound.c) - Add capability, handle MARKREAD response
- [proto-irc.c](src/common/proto-irc.c) - Add MARKREAD handler
- [outbound.c](src/common/outbound.c) - Add `/MARKREAD` command
- [xtext.c](src/fe-gtk/xtext.c) - Visual read marker indicator

#### 3.4 draft/no-implicit-names Capability

**Rationale:** Optimization - prevents server from sending NAMES automatically on JOIN when client will request history anyway.

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_no_implicit_names` flag
- [inbound.c](src/common/inbound.c) - Add capability
- [inbound.c](src/common/inbound.c) - Modify JOIN handler to explicitly request NAMES when needed

---

### Phase 4: Advanced Message Features

#### 4.1 draft/multiline Capability

**Critical:** Multiline messages must be displayed as a single cohesive unit, not individual lines.

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_multiline`, `multiline_max_bytes`, `multiline_max_lines`, `multiline_message` struct
- [inbound.c](src/common/inbound.c) - Add capability, handle `multiline` batch type with line collection
- [modes.c](src/common/modes.c) - Parse MULTILINE ISUPPORT token (`max-bytes`, `max-lines`)
- [outbound.c](src/common/outbound.c) - Add `/MULTILINE` command, modify paste handling
- [xtext.c](src/fe-gtk/xtext.c) - Support embedded `\n` in single message, continuation rendering

**Receiving multiline (batch handler):**
```c
// When BATCH +ref multiline starts:
// - Create multiline_message context
// - Store in active_batches

// For each PRIVMSG with batch=ref tag:
// - Append text to multiline_message.lines

// When BATCH -ref ends:
// - Join lines with \n separator
// - Call inbound_chanmsg/privmsg ONCE with full text
// - xtext renders with embedded newlines
```

**Sending multiline:**
```c
// BATCH +clientref multiline
// PRIVMSG #chan :line 1
// PRIVMSG #chan :line 2
// BATCH -clientref
```

**ISUPPORT tokens:**
- `MULTILINE=max-bytes=<n>,max-lines=<n>` - Server limits

#### 4.2 draft/message-redaction Capability

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_redact` flag
- [inbound.c](src/common/inbound.c) - Add capability, handle REDACT (find message by msgid, mark redacted)
- [proto-irc.c](src/common/proto-irc.c) - Add REDACT handler
- [outbound.c](src/common/outbound.c) - Add `/REDACT <target> <msgid> [reason]` command
- [xtext.c](src/fe-gtk/xtext.c) - Visual handling of redacted messages

---

### Phase 5: Security and Transport

#### 5.1 sts (Strict Transport Security)

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `sts_policy` struct
- [servlist.c](src/common/servlist.c) - Persistent STS policy storage
- [inbound.c](src/common/inbound.c) - Parse `sts` capability with `port=` and `duration=` values
- [server.c](src/common/server.c) - Check STS policy before connect, redirect to TLS port

**STS policy structure:**
```c
typedef struct sts_policy {
    char *host;
    int port;
    time_t expires;
    gboolean preload;
} sts_policy;
```

#### 5.2 draft/account-registration

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_account_registration` flag
- [inbound.c](src/common/inbound.c) - Add capability, handle REGISTER responses (VERIFY, etc.)
- [outbound.c](src/common/outbound.c) - Add `/REGISTER <account> <email> <password>` command

---

### Phase 6: Additional Features

#### 6.1 MONITOR Command (IRCv3 Extension)

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add MONITOR support alongside existing WATCH
- [notify.c](src/common/notify.c) - Use MONITOR when available (730-734 numerics)
- [outbound.c](src/common/outbound.c) - `/MONITOR +nick,-nick,C,L,S`

#### 6.2 draft/metadata-2

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_metadata` flag
- [inbound.c](src/common/inbound.c) - Handle METADATA responses
- [outbound.c](src/common/outbound.c) - `/METADATA GET/SET/LIST/SUB/UNSUB` commands

**Note:** draft/metadata-2 has effectively no real-world deployment yet. ObsidianIRC is the only client with substantial metadata support. Nefarious/X3 is implementing a comprehensive metadata system that we use as our primary reference.

**Virtual Keys (Read-Only, Computed from IRCd State):**

Virtual keys are prefixed with `$` and computed dynamically - they cannot be SET.

| Key | Description | Source |
|-----|-------------|--------|
| `$secure` | TLS connection ("1"/"0") | `IsSSL(cptr)` |
| `$account` | Account name | `cli_account(cptr)` |
| `$oper` | Oper status ("1"/"0") | `IsOper(cptr)` |
| `$idle` | Idle seconds | Current time - last activity |
| `$signon` | Connection timestamp | `cli_firsttime(cptr)` |
| `$connection_count` | Connections for account | Account session count |
| `$bot` | Bot flag ("1"/"0") | `IsBot(cptr)` (+B mode) |
| `$presence` | Aggregated presence state | `present`, `away`, `away-star` |
| `$away_message` | Effective away message | (absent if none) |
| `$last_present` | Last non-away timestamp | Unix timestamp |

**User Profile Keys (ObsidianIRC-compatible, stored in LMDB/Keycloak):**

| Key | Description | Visibility | Keycloak Attr |
|-----|-------------|------------|---------------|
| `avatar` | Profile image URL | Public | `metadata.avatar` |
| `display-name` | Display name (not nick) | Public | `metadata.display-name` |
| `status` | Status message | Public | `metadata.status` |
| `url` | Homepage URL | Public | `metadata.url` |
| `location` | Geographic location | Public | `metadata.location` |
| `color` | Nick color (hex) | Public | `metadata.color` |

**X3 Services Keys:**

| Key | Description | Visibility |
|-----|-------------|------------|
| `x3.title` | User epithet/signature | Public |
| `x3.registered` | Account registration timestamp | Public |
| `x3.karma` | Reputation score | Public |
| `x3.infoline.#channel` | Per-channel role description | Per-channel |
| `x3.email` | Email address | Private (owner + opers) |
| `x3.lasthost` | Last connection host | Private (owner + opers) |
| `x3.screen_width` | Terminal width | Private (owner only) |

**Visibility Rules:**

| Key Pattern | Default | Who Can See |
|-------------|---------|-------------|
| `$*` (virtual) | Public | Anyone (read-only) |
| `avatar`, `display-name`, etc. | Public | Anyone |
| `x3.email`, `x3.lasthost` | Private | Owner + opers |
| `x3.screen_width`, etc. | Private | Owner only |
| Channel keys | Public | Anyone |

**Permission Matrix:**

| Operation | User Metadata | Channel Metadata |
|-----------|---------------|------------------|
| Read public | Anyone | Anyone |
| Read private | Owner/oper | N/A (all public) |
| Write | Owner/oper | Chan op (200+) |

**Implementation patterns:**
- Subscribe to default keys on capability ACK: `avatar`, `display-name`, `url`, `status`, `location`, `color`
- For channels, request only: `avatar`, `display-name`
- Virtual keys (`$*`) are always queryable but never writable
- Cache metadata locally for persistence across sessions
- Store metadata per-target with visibility tracking: `{ value: string, visibility: string }`
- Use METADATA numerics: 760 (WHOIS), 761 (KEYVALUE), 766 (KEYNOTSET), 770-772 (SUB/UNSUB/SUBS), 774 (SYNCLATER)
- Handle FAIL METADATA responses gracefully

**PoxChat UX for metadata:**
- Display `avatar` in user profile popup (if supported)
- Use `display-name` instead of nick where appropriate (tooltip, profile)
- Show `status` in user info/WHOIS display
- Use `color` for nick colorization (user preference to enable/disable)
- Query `$presence` for rich away status (ties into pre-away aggregation)
- Virtual keys useful for `/WHOIS`-like information without WHOIS

**Avatar upload integration (if filehost available):**
- ObsidianIRC uses EXTJWT + external filehost for avatar uploads
- Not part of metadata spec itself, but complementary feature
- Consider similar integration if server advertises filehost capability

**Related: Bot indication**
- Note: Bot indication is typically via user mode +B (`draft/bot-mode`), NOT metadata
- Virtual key `$bot` reflects the +B mode state
- If server supports `draft/bot-mode`, show bot indicator in userlist
- This is separate from writable metadata

#### 6.3 draft/channel-rename (RENAME command)

**Rationale:** Allows channels to be renamed without requiring users to rejoin.

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_channel_rename` flag
- [proto-irc.c](src/common/proto-irc.c) - Add RENAME command handler
- [inbound.c](src/common/inbound.c) - `inbound_rename()` to update session channel name
- [fe-gtk/chanview.c](src/fe-gtk/chanview.c) - Update tab/tree label on rename

#### 6.4 SASL OAUTHBEARER Token Refresh & Re-authentication

**Rationale:** OAuth2 access tokens expire. PoxChat must proactively refresh tokens and re-authenticate to maintain the session without reconnecting. Per IRCv3.2, clients can send `AUTHENTICATE` at any time to re-authenticate; servers that don't support this return 907 `ERR_SASLALREADY`.

**Current state:**
- Token storage exists (`secure_storage_store_oauth_tokens()` stores access_token, refresh_token, expires_at)
- `oauth_refresh_token()` function exists but is a stub returning "not yet implemented"
- No mechanism to trigger SASL re-authentication on established connections
- No timer to track token expiry

**Implementation:**

1. **Complete `oauth_refresh_token()`** - HTTP POST to token endpoint with refresh_token grant
   ```
   POST /token
   grant_type=refresh_token
   refresh_token=<stored_refresh_token>
   client_id=<client_id>
   ```

2. **Add token expiry timer**
   - On successful OAUTHBEARER auth, schedule GLib timeout for `expires_at - 5 minutes`
   - On timer fire: call `oauth_refresh_token()`, then attempt re-auth
   - Store timer ID in server struct to cancel on disconnect

3. **Add SASL re-authentication function**
   - `sasl_reauthenticate(server *serv)` - triggers `AUTHENTICATE OAUTHBEARER` flow on established connection
   - Reuses existing OAUTHBEARER authentication code path
   - Must handle being called post-registration (no CAP negotiation needed)

4. **Handle re-auth responses**
   - 903: Success - update stored tokens, log success, reschedule timer
   - 904/905: Auth failed - log error, maybe clear tokens
   - 907: Server doesn't support re-auth - store new token for next reconnect, log info

**Files to modify:**
- [oauth.c](src/common/oauth.c) - Complete `oauth_refresh_token()` implementation
- [oauth.h](src/common/oauth.h) - Add refresh callback types if needed
- [inbound.c](src/common/inbound.c) - Add `sasl_reauthenticate()`, handle 903/907 for re-auth case
- [poxchat.h](src/common/poxchat.h) - Add `oauth_refresh_timer` to server struct
- [server.c](src/common/server.c) - Cancel refresh timer on disconnect

**UX considerations:**
- Silent refresh when successful (maybe debug log)
- Notification on refresh failure with option to re-authorize
- If 907 received, inform user that token was refreshed but will apply on next connect

#### 6.5 SASL ECDSA-NIST256P-CHALLENGE Mechanism

**Rationale:** ECDSA-NIST256P-CHALLENGE is a challenge-response SASL mechanism using ECDSA signatures. It's supported by Atheme services and provides strong authentication without transmitting passwords. The user stores a private key; authentication involves signing a server-provided challenge.

**Mechanism flow:**
1. Client sends: `AUTHENTICATE ECDSA-NIST256P-CHALLENGE`
2. Client sends: base64(account_name)
3. Server sends: base64(challenge)
4. Client signs challenge with private key, sends: base64(signature)
5. Server verifies signature against stored public key

**Implementation:**

1. **Key management**
   - Generate ECDSA P-256 key pair (OpenSSL `EC_KEY_new_by_curve_name(NID_X9_62_prime256v1)`)
   - Store private key securely (secure storage or separate key file)
   - Export public key in format suitable for services (base64 DER or PEM)
   - UI to generate keys and display public key for registration with services

2. **Authentication flow**
   - Add `MECH_ECDSA_CHALLENGE` to `sasl_mechanism` enum
   - Add `LOGIN_SASL_ECDSA` to login methods
   - Implement challenge-response state machine similar to SCRAM

3. **Signature generation**
   - Decode base64 challenge from server
   - Sign with ECDSA using SHA-256: `ECDSA_sign(NID_sha256, challenge, challenge_len, sig, &sig_len, ec_key)`
   - Base64 encode signature and send

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `MECH_ECDSA_CHALLENGE`, `LOGIN_SASL_ECDSA`
- [inbound.c](src/common/inbound.c) - Add mechanism to SASL negotiation
- [proto-irc.c](src/common/proto-irc.c) - Handle ECDSA challenge-response state machine
- [servlist.h](src/common/servlist.h) - Add ECDSA key path to `ircnet` struct
- [servlist.c](src/common/servlist.c) - Load/save ECDSA key configuration
- [servlistgui.c](src/fe-gtk/servlistgui.c) - UI for ECDSA key generation/selection
- [secure-storage.c](src/common/secure-storage.c) - Optionally store private key securely

**Dependencies:**
- OpenSSL (already required for other SASL mechanisms)

**UI additions:**
- Network Edit dialog: ECDSA key file selector
- Button to generate new key pair
- Display public key for copying to services
- Help text explaining registration process with services

#### 6.6 X3 Session Token Support

**Rationale:** X3 Services provides session tokens as an alternative to password-based authentication. After initial AUTH with password, X3 issues a session token that can be used for subsequent SASL authentication. Benefits include: password only sent once, tokens can be revoked without password change, and enables SCRAM-SHA-256/512 for users with legacy password hashes.

**X3 flow:**
```
# First connection - authenticate with password
PRIVMSG AuthServ :AUTH myaccount mypassword
-AuthServ- Your session cookie is: xK9mN2pQ8rS3tU6vW1xY4zA7bC0dE3fG5hI8jK1lM4n=

# Client stores token securely

# Later connection - authenticate with token via SASL
CAP REQ :sasl
AUTHENTICATE PLAIN
AUTHENTICATE <base64(account\0account\0token)>
:server 903 * :SASL authentication successful
```

**Implementation:**

1. **Detect token delivery**
   - Parse NOTICE from AuthServ/NickServ
   - Match pattern: "Your session cookie is: <token>"
   - Extract token (base64-encoded, ~44 characters)

2. **Prompt user and store token**
   - When token detected, show prompt: "AuthServ provided a session token. Would you like to use it for future logins? (Your password will no longer be needed)"
   - Options: "Yes, use token" / "No, keep using password" / "Always use tokens" / "Never ask again"
   - If accepted, store via `secure_storage_store()` with key `session_token`
   - Associate with network name

3. **Configure SASL for token use**
   - Session tokens are only offered when X3 receives plaintext password (PRIVMSG AUTH or SASL PLAIN)
   - Users with SCRAM/EXTERNAL/ECDSA won't receive tokens (server never sees plaintext password)
   - If already using SASL PLAIN: token replaces password seamlessly, no config change needed
   - If using PRIVMSG AUTH (no SASL): prompt user "Using session tokens requires SASL. Enable SASL PLAIN for this network?"
   - If user accepts, enable SASL PLAIN and store token as password
   - Token replaces password field in SASL auth, account name remains the same
   - Optional upgrade: offer to switch from PLAIN to SCRAM-SHA-256 for additional security (X3 generates SCRAM credentials from token, so this just works)

4. **Use token on connect**
   - Check for stored session token before using password
   - If token exists, use it as password for SASL PLAIN
   - Also works with SCRAM-SHA-* (X3 generates SCRAM credentials from token)

5. **Handle token invalidation**
   - If SASL fails with stored token, fall back to password
   - Clear invalid token from storage
   - On `LOGOUT` command, clear stored token

6. **Token refresh**
   - After successful password AUTH, watch for new token
   - Update stored token if X3 issues a new one

**Files to modify:**
- [inbound.c](src/common/inbound.c) - Parse AuthServ NOTICE for session token pattern
- [secure-storage.c](src/common/secure-storage.c) - Store/retrieve session tokens
- [server.c](src/common/server.c) - Check for session token before password in SASL auth
- [servlist.c](src/common/servlist.c) - Track "use session token" preference per network
- [servlistgui.c](src/fe-gtk/servlistgui.c) - UI option to enable/clear session tokens

**Detection pattern:**
```c
/* Match: "Your session cookie is: <token>" from AuthServ */
if (strstr(text, "session cookie is:") && is_authserv_notice(sender))
{
    char *token = extract_token(text);
    if (token && strlen(token) >= 40)
        offer_store_session_token(serv, token);
}
```

**UX considerations:**
- First time: prompt user to confirm storing token
- Preference: "Automatically store session tokens from services"
- Network list: show indicator if session token is stored
- Option to manually clear session token

#### 6.7 draft/pre-away Capability

**Rationale:** Supports bouncer and multi-connection scenarios where a single nickname is shared across multiple client connections. Allows better coordination of away status, particularly for automated connections that shouldn't indicate user presence.

**Key features:**
- `AWAY` command accepted before registration completes
- `AWAY *` indicates "not present for unspecified reason" (e.g., automated/bouncer connection)
- Servers aggregate away status across multiple connections sharing a nickname
- Receiving `*` as away message means user is away for unspecified reason

**Use cases:**
1. **Bouncer reconnection** - When PoxChat reconnects to a bouncer, send `AWAY *` pre-registration to indicate this connection doesn't mean user is present
2. **Multi-device** - User connected on phone (active) and desktop (idle) - desktop sends `AWAY *`, phone's explicit away message takes precedence
3. **Auto-away** - Better integration with server-side auto-away systems
4. **Minimized to tray** - When PoxChat is minimized to system tray, send `AWAY *` to indicate user isn't actively present

**Implementation:**

1. **Capability negotiation**
   - Add to `supported_caps[]` in inbound.c
   - Track `have_pre_away` flag

2. **Pre-registration AWAY**
   - If capability negotiated, allow sending `AWAY` during CAP negotiation
   - New preference: "Send AWAY * on connect" for bouncer users
   - Send before `CAP END` if enabled

3. **Handle `*` away message**
   - When receiving away-notify or RPL_AWAY (301) with message `*`
   - Display as "Away" without showing the literal asterisk
   - Or use server-substituted human-readable message if provided

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `have_pre_away` flag
- [inbound.c](src/common/inbound.c) - Add capability, handle `*` away message
- [server.c](src/common/server.c) - Send `AWAY *` pre-registration if enabled
- [cfgfiles.c](src/common/cfgfiles.c) - Add preference for pre-away behavior
- [setup.c](src/fe-gtk/setup.c) - UI for bouncer/pre-away settings

**Preferences:**
- `hex_irc_pre_away` - Send `AWAY *` on connect (default: OFF, enable for bouncer users)
- `hex_irc_pre_away_tray` - Send `AWAY *` when minimized to tray (default: OFF)

#### 6.8 Client Tags: channel-context, reply, react

These are client-only tags (prefixed with `+`) that require the `message-tags` capability. They enable modern chat features without requiring server-side support beyond tag relay.

##### 6.8.1 draft/channel-context

**Spec:** https://ircv3.net/specs/client-tags/channel-context

**Purpose:** Adds channel context to private messages, enabling "whispers" - private messages that relate to a specific channel conversation.

**Tag:** `+draft/channel-context=<channel>`

**Use cases:**
- Right-click user in channel → "Send private message" → message tagged with channel context
- Recipient's client can display the whisper in the channel window or show which channel it relates to
- Useful for "Can you help me with this?" side conversations during channel discussions

**Implementation:**

1. **Sending whispers**
   - Add context menu option: "Whisper (private message about this channel)"
   - When sending from channel context, include `+draft/channel-context=#channel` tag
   - `/WHISPER <nick> <message>` command sends PRIVMSG with channel-context tag

2. **Receiving whispers**
   - Parse `+draft/channel-context` tag from incoming PRIVMSG
   - Display option: show in PM window with channel indicator, or show in channel window as whisper
   - Visual distinction for whisper messages (different color, icon, or prefix)

**Files to modify:**
- [proto-irc.c](src/common/proto-irc.c) - Parse channel-context tag
- [outbound.c](src/common/outbound.c) - Add `/WHISPER` command, include tag when sending
- [inbound.c](src/common/inbound.c) - Handle whisper display logic
- [menu.c](src/fe-gtk/menu.c) - Add "Whisper" context menu option

##### 6.8.2 draft/reply

**Spec:** https://ircv3.net/specs/client-tags/reply

**Purpose:** Links a message to a previous message it's replying to, using msgid.

**Tag:** `+draft/reply=<msgid>`

**Use cases:**
- Click "Reply" on a message → new message references the original
- Recipient sees the reply with context of what's being replied to
- Threading/conversation tracking in busy channels

**Implementation:**

1. **Sending replies**
   - Context menu on message: "Reply"
   - Store target msgid when user initiates reply
   - Visual indicator in input box showing "Replying to: <preview>"
   - Cancel button to clear reply context
   - On send, include `+draft/reply=<msgid>` tag

2. **Receiving replies**
   - Parse `+draft/reply` tag from incoming messages
   - Look up referenced message in buffer by msgid
   - Display reply with quote/preview of original message
   - Click on quoted portion scrolls to original (if still in buffer)

3. **Visual design**
   - Reply indicator: "> Replying to <nick>: <truncated message>"
   - Subtle background or left border on reply messages
   - Quoted text in smaller/muted font above the reply

**Files to modify:**
- [proto-irc.c](src/common/proto-irc.c) - Parse reply tag
- [outbound.c](src/common/outbound.c) - Track reply state, include tag when sending
- [inbound.c](src/common/inbound.c) - Handle reply display with quote
- [text.c](src/common/text.c) - Msgid lookup in buffer
- [xtext.c](src/fe-gtk/xtext.c) - Reply visual rendering, click-to-scroll
- [inputgui.c](src/fe-gtk/inputgui.c) - Reply indicator above input, cancel button
- [menu.c](src/fe-gtk/menu.c) - Add "Reply" context menu option

##### 6.8.3 draft/react

**Spec:** https://ircv3.net/specs/client-tags/react

**Purpose:** Adds emoji reactions to messages, similar to Slack/Discord.

**Tag:** `+draft/react=<emoji>` on TAGMSG with `+draft/reply=<msgid>`

**Use cases:**
- Click reaction button on message → select emoji → reaction sent
- Reactions displayed inline with message (e.g., "👍 3  ❤️ 2")
- Quick acknowledgment without sending a full message

**Implementation:**

1. **Sending reactions**
   - Context menu on message: "React" → emoji picker
   - Send as TAGMSG (not PRIVMSG) with both tags:
     - `+draft/react=👍`
     - `+draft/reply=<msgid>` (to identify target message)
   - Target is the channel/user where the message was sent

2. **Receiving reactions**
   - Parse TAGMSG with `+draft/react` and `+draft/reply` tags
   - Look up target message by msgid
   - Add reaction to message's reaction list
   - Aggregate multiple reactions (count per emoji)

3. **Visual design**
   - Reactions displayed below or beside message
   - Format: emoji + count, clickable to see who reacted
   - Compact display: "👍2 ❤️1 🎉3"
   - Hover/click shows list of users who reacted

4. **Reaction management**
   - Click own reaction to remove it
   - Send same TAGMSG again to toggle off (server/client dependent)
   - Store reactions in message struct

**Files to modify:**
- [proto-irc.c](src/common/proto-irc.c) - Parse react tag from TAGMSG
- [outbound.c](src/common/outbound.c) - Send reaction TAGMSG
- [inbound.c](src/common/inbound.c) - Handle reaction aggregation
- [poxchat.h](src/common/poxchat.h) - Add reactions field to message struct
- [xtext.c](src/fe-gtk/xtext.c) - Render reactions inline with messages
- [menu.c](src/fe-gtk/menu.c) - Add "React" context menu with emoji picker
- [emojipicker.c](src/fe-gtk/emojipicker.c) - **NEW** - Emoji picker widget

**Dependencies:**
- Requires `message-tags` capability for tag relay
- Requires TAGMSG support (Phase 1.3)
- Requires msgid tracking (Phase 1.2)

#### 6.9 UTF8ONLY ISUPPORT Token

**Spec:** https://ircv3.net/specs/extensions/utf8-only

**Purpose:** Servers advertise via ISUPPORT that they exclusively support UTF-8 encoded content. When present, clients must automatically switch to UTF-8 encoding.

**ISUPPORT token:** `UTF8ONLY`

**Requirements:**
- Server will NOT relay non-UTF-8 content (PRIVMSG, NOTICE, topics, realnames)
- Client MUST NOT send non-UTF-8 data once token is seen
- Server may send `FAIL * INVALID_UTF8` or `WARN * INVALID_UTF8` for invalid input

**Implementation:**

1. **Detect UTF8ONLY in ISUPPORT**
   - Parse `UTF8ONLY` token from 005 numeric
   - Set `serv->utf8only` flag

2. **Force UTF-8 encoding**
   - When `utf8only` flag is set, override any user encoding preference
   - Automatically switch session encoding to UTF-8
   - Notify user: "Server requires UTF-8 encoding"

3. **Validate outgoing messages**
   - Before sending, validate that text is valid UTF-8
   - Convert or reject invalid sequences
   - Ensure message truncation doesn't split multi-byte codepoints

4. **Handle INVALID_UTF8 errors**
   - Parse `FAIL * INVALID_UTF8` and `WARN * INVALID_UTF8` standard replies
   - Display user-friendly error: "Message contained invalid characters"

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `utf8only` flag to server struct
- [modes.c](src/common/modes.c) - Parse UTF8ONLY from ISUPPORT
- [server.c](src/common/server.c) - Force UTF-8 encoding when flag set
- [outbound.c](src/common/outbound.c) - Validate UTF-8 before sending, safe truncation
- [inbound.c](src/common/inbound.c) - Handle INVALID_UTF8 standard reply

**Note:** PoxChat already defaults to UTF-8 in most cases, but this ensures proper handling when server mandates it and provides correct error feedback.

#### 6.10 draft/ICON ISUPPORT Token

**Spec:** https://ircv3.net/specs/extensions/network-icon

**Purpose:** Servers advertise a network icon URL via ISUPPORT, allowing clients to display visual branding for the network.

**ISUPPORT token:** `draft/ICON=<url>`

**Example:** `draft/ICON=https://libera.chat/icon.svg`

**Implementation:**

1. **Parse ICON from ISUPPORT**
   - Extract URL from `draft/ICON` token in 005 numeric
   - Validate URL (must be valid, HTTPS preferred)
   - Store in `serv->network_icon_url`

2. **Fetch and cache icon**
   - Download icon asynchronously after connection
   - Cache locally keyed by network name
   - Support common formats: SVG, PNG, ICO
   - Respect reasonable size limits (e.g., 1MB max)

3. **Display icon**
   - Show in server list/tree next to network name
   - Show in server tab
   - Show in network properties dialog

**Files to modify:**
- [poxchat.h](src/common/poxchat.h) - Add `network_icon_url` to server struct
- [modes.c](src/common/modes.c) - Parse draft/ICON from ISUPPORT
- [fe-gtk/chanview.c](src/fe-gtk/chanview.c) - Display icon in channel tree
- [fe-gtk/servlistgui.c](src/fe-gtk/servlistgui.c) - Display icon in server list

**Caching:**
- Store downloaded icons in `~/.config/poxchat/icons/`
- Key by network name or hash of URL
- Check for updates periodically or on reconnect

#### 6.11 SNI (Server Name Indication)

**Spec:** https://ircv3.net/docs/sni

**Purpose:** TLS Server Name Indication allows clients to specify the target hostname during TLS handshake, enabling servers to present the correct certificate when hosting multiple domains.

**Requirement:** Clients MUST use SNI when connecting via TLS.

**Why it matters:**
- Server may host multiple networks (irc.example.net, server.example.net)
- Without SNI, server doesn't know which certificate to present
- Required for proper certificate validation on multi-homed servers

**Implementation:**

1. **Enable SNI in TLS connections**
   - When initiating TLS handshake, include target hostname
   - Use the hostname from server configuration (not resolved IP)
   - Requires TLS 1.1+ (TLS 1.0 lacks SNI support)

2. **OpenSSL implementation**
   ```c
   SSL_set_tlsext_host_name(ssl, hostname);
   ```

3. **Verification**
   - After handshake, verify certificate matches the SNI hostname
   - Handle certificate mismatch errors appropriately

**Files to modify:**
- [ssl.c](src/common/ssl.c) - Add SNI hostname to SSL context before connect

**Note:** PoxChat likely already supports SNI through OpenSSL defaults, but this should be verified and explicitly implemented if not present.

---

## Key Files Summary

| File | Changes |
|------|---------|
| [src/common/proto-irc.c](src/common/proto-irc.c) | BATCH, TAGMSG, MARKREAD, REDACT, RENAME handlers; full tag parsing |
| [src/common/proto-irc.h](src/common/proto-irc.h) | Extended `message_tags_data` structure |
| [src/common/inbound.c](src/common/inbound.c) | All capability negotiations; batch processing; event playback |
| [src/common/poxchat.h](src/common/poxchat.h) | Server struct flags; batch_info; sts_policy; session read marker |
| [src/common/outbound.c](src/common/outbound.c) | New commands: /HISTORY, /MARKREAD, /REDACT, /TAGMSG, /MULTILINE, /REGISTER, /MONITOR, /METADATA; typing indicator state machine |
| [src/common/modes.c](src/common/modes.c) | ISUPPORT parsing for MULTILINE, CHATHISTORY limits |
| [src/common/server.c](src/common/server.c) | STS policy enforcement |
| [src/common/servlist.c](src/common/servlist.c) | STS policy persistence |
| [src/common/cfgfiles.c](src/common/cfgfiles.c) | New IRCv3 preferences |
| [src/common/textevents.in](src/common/textevents.in) | New events: history, redaction, typing, channel rename |
| [src/common/plugin.c](src/common/plugin.c) | Plugin API for tags, chathistory, redact, markread |
| [src/common/plugin.h](src/common/plugin.h) | Plugin API declarations |
| [src/common/chathistory.c](src/common/chathistory.c) | **NEW** - Chathistory request/response/batch handling |
| [src/common/oauth.c](src/common/oauth.c) | OAUTHBEARER token refresh implementation |
| [src/common/oauth.h](src/common/oauth.h) | OAuth types and function declarations |
| [src/common/secure-storage.c](src/common/secure-storage.c) | Secure token/key storage; ECDSA key management |
| [src/fe-gtk/xtext.c](src/fe-gtk/xtext.c) | Read markers, history separators, redacted messages, scroll-to-load |
| [src/fe-gtk/chanview.c](src/fe-gtk/chanview.c) | Unread badges, channel rename updates |
| [src/fe-gtk/userlist.c](src/fe-gtk/userlist.c) | Typing indicator icons |
| [src/fe-gtk/inputgui.c](src/fe-gtk/inputgui.c) | Multiline paste detection/dialog |
| [src/fe-gtk/menu.c](src/fe-gtk/menu.c) | Message context menu (redact) |
| [src/fe-gtk/setup.c](src/fe-gtk/setup.c) | IRCv3 settings UI panel |

---

## Dependency Graph

```
Phase 1.1 (batch) ──────────────┐
                                ├──► Phase 3.1 (chathistory)
Phase 1.2 (message-tags) ───────┤         │
                                │         ▼
Phase 1.3 (TAGMSG) ─────────────┘   Phase 3.2 (read-marker)

Phase 2.1 (echo-message) ───────────────────────────────┐
                                                        │
Phase 2.2 (labeled-response) ───────────────────────────┼──► Phase 4.1 (multiline)
                                                        │
                                                        └──► Phase 4.2 (redaction)

Phase 5.1 (sts) ──► Independent
Phase 5.2 (account-registration) ──► Independent
Phase 6.x ──► Independent
```

---

## Testing Strategy

### Test Environment
- **Primary:** Nefarious IRCd (ircv3.2-upgrade branch) + X3 services (keycloak-integration branch)
- **Secondary:** Other IRCv3 servers (Ergo, InspIRCd, UnrealIRCd)
- **Bouncer:** ZNC with appropriate modules

### Test Cases per Phase

**Phase 1 (batch/message-tags):**
- [ ] CAP negotiation includes batch, message-tags
- [ ] BATCH +id type handled correctly
- [ ] Nested batches work
- [ ] All message tags preserved in `all_tags`
- [ ] Client-only tags (`+typing`) handled
- [ ] TAGMSG sent/received

**Phase 2 (echo-message/labeled-response):**
- [ ] Own messages echoed with server timestamp
- [ ] Timeout fallback works when echo not received
- [ ] Labels correlate requests to responses
- [ ] ACK batch handled

**Phase 3 (chathistory/read-marker):**
- [ ] `/HISTORY` fetches messages
- [ ] History inserted in correct order
- [ ] Read marker syncs across clients
- [ ] Visual indicator shows read position

**Phase 4 (multiline/redaction):**
- [ ] Multi-line paste sends as batch
- [ ] `/REDACT` removes message
- [ ] Redacted messages show placeholder

**Phase 5 (sts/account-registration):**
- [ ] STS policy stored and enforced
- [ ] Connection upgraded to TLS
- [ ] `/REGISTER` creates account

---

## Verification

After implementation:
1. Run PoxChat against Nefarious IRCd testnet
2. Verify all capabilities negotiated in CAP LS/ACK
3. Test each feature manually per checklist above
4. Test plugin API access to message tags
5. Test persistence (STS policies, read markers)

---

## UX Integration Design

### Design Philosophy: Sensible Defaults

**Principle:** Features should "just work" when server supports them. Users shouldn't need to manually configure everything.

**Automatic behaviors (enabled by default when capability available):**

| Feature | Automatic Behavior | User Override |
|---------|-------------------|---------------|
| chathistory | Fetch last 50 messages on JOIN | Can disable or adjust count |
| echo-message | Use server echo for accurate timestamps | Can disable for legacy feel |
| read-marker | Auto-mark read on window focus | Can disable auto-mark |
| message-tags | Preserve all tags for plugins | Always on |
| batch | Process batches transparently | Always on |
| multiline | Display as cohesive unit, truncate large | Adjust thresholds |
| typing (receive) | Show others' typing indicators | Can disable display |
| typing (send) | OFF by default (privacy) | User must opt-in |
| event-playback | Include events in history | Can hide JOIN/PART |
| labeled-response | Use labels for better error handling | Always on |
| sts | Honor TLS upgrade policies | Can clear policies |

**Smart capability detection:**
- If server advertises capability → request it automatically
- If capability has server-side limits (CHATHISTORY, MULTILINE) → respect them
- Graceful degradation: if cap unavailable, fall back to traditional behavior

**Examples of "just works" behavior:**

1. **Reconnect scenario:**
   - User disconnects, reconnects 2 hours later
   - PoxChat automatically: requests chathistory AFTER last known msgid
   - Result: missed messages appear seamlessly, no user action needed

2. **Multi-device scenario:**
   - User reads messages on mobile, opens PoxChat on desktop
   - PoxChat automatically: fetches read-marker position, shows unread line
   - Result: user sees exactly where they left off

3. **Paste code block:**
   - User pastes 15 lines of code
   - PoxChat automatically: detects multiline, sends as batch (if available)
   - Receivers see: single cohesive message (truncated if needed)
   - No dialog needed if under threshold and server supports multiline

4. **Message deleted:**
   - Someone redacts a message in channel
   - PoxChat automatically: updates display to show "[Message deleted]"
   - No user configuration needed

**Progressive enhancement model:**
```
Server supports cap?
  ├─ YES → Enable enhanced behavior automatically
  │        User can tweak via preferences if desired
  └─ NO  → Fall back to traditional behavior
           Feature preferences hidden/disabled in UI
```

**Preference philosophy:**
- Most preferences are for *tuning* behavior, not *enabling* it
- Features are ON by default when capability is available
- Only privacy-sensitive features (like sending typing indicators) might default OFF
- Preferences UI should hide irrelevant options when caps unavailable

---

### Chathistory UX

**Relationship to Local Logging/Scrollback:**

Chathistory supplements (not replaces) local logging. Key considerations:
- **Local scrollback** - PoxChat already has `hex_text_max_lines` buffer and optional disk logging
- **Server history** - Provides continuity across devices, fills gaps when client was offline
- **Coordination** - Don't duplicate: if message already in local buffer (by msgid), skip it
- **Pagination** - Never try to load years of history; always fetch in bounded pages (50-100 messages)
- **Hybrid approach** - Local buffer for immediate scrollback, server for "load more" beyond buffer

**When to fetch history:**
1. **On channel JOIN** - Fetch messages since last known msgid (or last N if first join)
   - If we have local history with msgids, use `CHATHISTORY AFTER <last_msgid>`
   - If no local history, use `CHATHISTORY LATEST * <limit>`
   - Respect server's advertised limit from ISUPPORT
2. **On scroll up** - "Load more" when user scrolls to top of buffer
   - Fetch page before oldest displayed message
   - Stop when server returns empty batch or hits retention limit
3. **On window focus** - Fetch messages missed while window was inactive
   - Only if read-marker available and we have a last-seen msgid
4. **Manual command** - `/HISTORY` for explicit requests

**Pagination strategy:**
- Default page size: 50 messages (configurable)
- Maximum single request: 200 messages (safety limit)
- Track "has more history" flag based on server response
- Show "No more history available" when server indicates end

**Deduplication:**
- Track msgids in local buffer
- When processing chathistory batch, skip messages with known msgids
- This prevents duplicates when reconnecting or loading around boundaries

**Visual design:**
- **History separator line** - Horizontal rule with "--- Earlier messages ---" label
- **Timestamp styling** - Historical messages use muted/gray timestamp color (optional)
- **Loading indicator** - "Loading history..." message while fetching
- **Gap indicator** - "--- Gap in history ---" if discontinuity detected
- **End of history** - "--- Beginning of available history ---" when server has no more

**Preferences (Settings > IRC > History):**
- `hex_irc_chathistory_auto` - Enable auto-fetch on join (default: ON)
- `hex_irc_chathistory_lines` - Lines to fetch per request (default: 50, max: 200)
- `hex_irc_chathistory_scroll` - Enable load-more on scroll (default: ON)
- `hex_irc_chathistory_dedup` - Skip messages already in buffer (default: ON)

**Files:**
- [cfgfiles.c](src/common/cfgfiles.c) - New preferences
- [setup.c](src/fe-gtk/setup.c) - Settings UI
- [xtext.c](src/fe-gtk/xtext.c) - Scroll detection, history separator rendering
- [text.c](src/common/text.c) - Msgid tracking in buffer for deduplication

### Echo-Message UX

**Behavior:**
- When echo-message enabled, don't display outgoing message immediately
- Show "sending..." indicator or muted text while waiting
- Replace with echoed message when received (with server timestamp)
- Timeout after 10 seconds - fall back to local display with warning

**Visual design:**
- **Pending message** - Italic or muted color while waiting for echo
- **Confirmed message** - Normal styling once echoed
- **Failed echo** - Normal styling + warning icon + retry option

**Failed message handling:**
- Display message locally with visual indicator (warning icon, different color)
- Show "Message may not have been sent" tooltip/status
- Right-click context menu: "Retry" / "Copy to input" / "Dismiss warning"
- "Retry" re-sends the exact message
- Track failed messages per-session; clear on successful resend or user dismiss
- Don't auto-retry (could cause duplicates if message actually went through)

**Preferences:**
- `hex_irc_echo_message` - Enable echo-message capability (default: ON)
- `hex_irc_echo_timeout` - Timeout in seconds (default: 10)

### Read Marker UX

**Behavior:**
- Track last read message per channel/query via msgid
- Marker advances when user scrolls past it (actually reading content)
- Manual update via `/MARKREAD` command or context menu
- Optional: auto-mark on window focus (opt-in, not default)
- Sync position across devices via MARKREAD command

**When marker advances (default behavior):**
- User scrolls the text buffer and the marker scrolls off-screen (upward)
- User sends a message to the channel (implies they've seen recent content)
- User explicitly marks read via command or menu

**Visual design:**
- **Read marker line** - Colored horizontal rule (e.g., red/orange) at last read position
- **Unread count** - Badge on tab showing messages since marker
- **Jump to marker** - Click marker line or keyboard shortcut to jump
- **Context menu** - "Mark as read" on channel tab or in text area

**Preferences:**
- `hex_irc_read_marker` - Enable read markers (default: ON)
- `hex_irc_read_marker_auto` - Auto-mark on focus (default: OFF - opt-in)
- `hex_irc_read_marker_color` - Marker line color

**Files:**
- [xtext.c](src/fe-gtk/xtext.c) - Render read marker line
- [chanview.c](src/fe-gtk/chanview.c) - Unread badge on tabs

### Typing Indicators (TAGMSG +typing)

**Behavior:**
- Send `+typing=active` when user starts typing (debounced, 3 second minimum between sends)
- Send `+typing=paused` after 5 seconds of no input
- Send `+typing=done` when message sent or input cleared
- Display typing status for other users

**Smart defaults:**
- **Receiving** typing indicators: ON by default (non-invasive, informative)
- **Sending** typing indicators: OFF by default (privacy-sensitive)
- User can enable sending once they understand the privacy implications

**Visual design:**
- **In nick list** - Typing icon next to nick (pencil/ellipsis)
- **In status bar** - "User1, User2 are typing..." for queries
- **Subtle** - Should not be distracting, fade after timeout

**Preferences:**
- `hex_irc_typing_send` - Send typing indicators (default: OFF - privacy)
- `hex_irc_typing_show` - Show others' typing (default: ON)

**Files:**
- [outbound.c](src/common/outbound.c) - Typing state machine, TAGMSG sending
- [userlist.c](src/fe-gtk/userlist.c) - Typing icon in nick list
- [maingui.c](src/fe-gtk/maingui.c) - Typing status in title/status bar

### Multiline UX

**Receiving multiline messages:**

Multiline batches should NOT dump lines separately into the text buffer. Instead:

1. **Batch collection** - Collect all PRIVMSG lines within the `multiline` batch
2. **Concatenate content** - Join lines with actual newlines (`\n`)
3. **Single display unit** - Render as ONE message entry with embedded line breaks
4. **Visual grouping** - The multiline message should be visually distinct

**Display options for multiline content:**
- **Inline with line breaks** - Single message entry with `\n` preserved in xtext rendering
- **Code block style** - Monospace font, slightly indented, subtle background
- **Collapsible** - For very long messages, show first N lines with "Show more..."

**Truncation for large blocks (critical for UX):**

Large multiline messages must be truncated to avoid disrupting chat flow:
- **Threshold** - Messages exceeding N lines (default: 10) or M bytes (default: 2KB) get truncated
- **Preview** - Show first 5-8 lines with visual indicator "..."
- **Expand action** - Click/hotkey to expand inline OR open in separate window/dialog
- **Collapse action** - After expanding, option to collapse back
- **Full view** - Right-click "View full message" opens in scrollable dialog

**Truncation display:**
```
<nick> [multiline - 47 lines]
| line 1 of the message
| line 2 of the message
| line 3 of the message
| ...
| [Click to expand or right-click for full view]
```

**Implementation:**
- Store full content in message metadata (not just displayed text)
- Track expanded/collapsed state per message
- Expansion can be inline (rerender with full content) or popup dialog
- Consider memory: very large messages may need lazy loading from stored content

**Implementation approach:**
```c
// In batch processing for multiline type:
typedef struct multiline_message {
    char *sender;
    char *target;
    GSList *lines;          // Collected PRIVMSG content
    message_tags_data tags; // Tags from first message (includes msgid)
} multiline_message;

// When batch ends (-reference):
// 1. Join all lines with \n
// 2. Call inbound_chanmsg/inbound_privmsg ONCE with concatenated text
// 3. xtext needs to handle embedded \n in message content
```

**xtext rendering changes:**
- Currently xtext treats each `PrintText` as a separate line
- Need to support embedded `\n` within a single message
- Render with continuation indent (no timestamp/nick prefix on continuation lines)
- Or render with subtle visual grouping (light background, border)

**Sending multiline messages:**

**Smart paste behavior (sensible defaults):**
```
User pastes multi-line content:
  │
  ├─ Server supports multiline?
  │   ├─ YES, content within limits → Send as multiline automatically (no dialog)
  │   ├─ YES, content exceeds limits → Show dialog with options
  │   └─ NO → Show dialog: split into messages or cancel
  │
  └─ Content is very large (>20 lines)?
      └─ Always show dialog for confirmation
```

- Small pastes (2-10 lines) + server supports multiline → just send it
- Medium pastes (10-20 lines) → send if within limits, else dialog
- Large pastes (>20 lines) → always confirm with user
- Server doesn't support multiline → dialog to split or cancel

**Dialog (when shown):**
- Preview of content, line count, byte count
- "Send as multiline" (if supported) / "Split into messages" / "Cancel"
- Limit warning in red if exceeds server limits
- Checkbox: "Don't ask again for small pastes"

**Preferences:**
- `hex_irc_multiline_auto_threshold` - Auto-send without dialog up to N lines (default: 10)
- `hex_irc_multiline_confirm_threshold` - Always confirm above N lines (default: 20)
- `hex_irc_multiline_style` - Display style: inline/codeblock (default: inline)
- `hex_irc_multiline_truncate_lines` - Truncate display at N lines (default: 10)
- `hex_irc_multiline_truncate_bytes` - Truncate display at N bytes (default: 2048)
- `hex_irc_multiline_preview_lines` - Lines to show in truncated preview (default: 5)

**Files:**
- [inputgui.c](src/fe-gtk/inputgui.c) - Paste detection, preview dialog
- [outbound.c](src/common/outbound.c) - Multiline batch sending
- [inbound.c](src/common/inbound.c) - Multiline batch collection and joining
- [xtext.c](src/fe-gtk/xtext.c) - Embedded newline rendering, visual grouping

### Message Redaction UX

**Behavior:**
- Context menu on own messages: "Delete message"
- Context menu for ops on channel messages: "Delete message"
- Confirmation dialog for redaction
- Redacted messages show placeholder

**Visual design:**
- **Redacted message** - "[Message deleted]" or "[Message removed by <nick>]"
- **Reason display** - Optional: "[Message deleted: <reason>]"
- **Context menu** - "Delete" / "Delete with reason..."

**Preferences:**
- `hex_irc_redact_confirm` - Confirm before redacting (default: ON)
- `hex_irc_redact_show_reason` - Show redaction reasons (default: ON)

**Files:**
- [menu.c](src/fe-gtk/menu.c) - Context menu for redaction
- [xtext.c](src/fe-gtk/xtext.c) - Redacted message rendering
- [dialog.c](src/fe-gtk/dialog.c) - Confirmation/reason dialog

### Event Playback UX

**Behavior:**
- JOIN/PART/QUIT/KICK/MODE/TOPIC from history render normally
- Consider smart collapsing for repeated events (configurable)
- Visual distinction for historical vs live events

**Visual design:**
- **Historical events** - Same as live but within history section (between separators)
- **Collapsed events** - "User1, User2, User3 joined" instead of 3 separate lines
- **Smart hide** - Option to hide JOIN/PART from history entirely

**Preferences:**
- `hex_irc_history_events` - Show events in history (default: ON)
- `hex_irc_history_collapse` - Collapse repeated events (default: ON)
- `hex_irc_history_hide_joinpart` - Hide JOIN/PART in history (default: OFF)

### Account Registration UX

**Behavior:**
- `/REGISTER` command with interactive prompts if args missing
- Handle verification flow (email codes, etc.)
- Show registration status in server tab

**Visual design:**
- **Interactive prompts** - "Enter account name:", "Enter email:", "Enter password:"
- **Verification** - "Check your email for verification code"
- **Status messages** - "Registration pending...", "Account created successfully"

**Files:**
- [servergui.c](src/fe-gtk/servergui.c) - Registration dialog (optional)
- [outbound.c](src/common/outbound.c) - REGISTER command with prompts

### Settings UI Additions

**New Settings Sections:**

**Settings > IRC > IRCv3:**
- Checkbox: Enable echo-message
- Checkbox: Enable chat history
- Spinner: History lines to fetch (10-500)
- Checkbox: Enable read markers
- Checkbox: Auto-mark read on focus
- Checkbox: Send typing indicators
- Checkbox: Show typing indicators
- Dropdown: Multiline paste behavior (Ask/Always multiline/Split)

**Settings > IRC > Security:**
- Checkbox: Honor STS policies
- Button: Clear STS policies

---

## New Text Events

Add to [textevents.in](src/common/textevents.in):

```
event HISTORY_START
pevt_histstart_help "History playback starting"
data
$1 Target name

event HISTORY_END
pevt_histend_help "History playback complete"
data
$1 Target name
$2 Message count

event HISTORY_GAP
pevt_histgap_help "Gap in message history"
data
$1 Target name

event READ_MARKER
pevt_readmarker_help "Read marker position"
data
$1 Target name

event MESSAGE_REDACTED
pevt_msgredact_help "Message was redacted"
data
$1 Nick who redacted
$2 Target
$3 Reason (or empty)

event TYPING_ACTIVE
pevt_typing_help "User is typing"
data
$1 Nick
$2 Target

event CHANNEL_RENAMED
pevt_chanrename_help "Channel was renamed"
data
$1 Old name
$2 New name
$3 Nick who renamed
```

---

## Plugin API Additions

Extend plugin API for IRCv3 features:

```c
/* Get message tag by name */
const char *poxchat_get_info_tag(poxchat_plugin *ph, const char *tag);

/* Get all message tags as list */
poxchat_list *poxchat_list_get_tags(poxchat_plugin *ph);

/* Request chat history */
void poxchat_chathistory_request(poxchat_plugin *ph, const char *target,
                                  const char *subcommand, int limit);

/* Send TAGMSG */
void poxchat_tagmsg(poxchat_plugin *ph, const char *target, const char *tags);

/* Redact message */
void poxchat_redact(poxchat_plugin *ph, const char *target, const char *msgid,
                    const char *reason);

/* Mark as read */
void poxchat_markread(poxchat_plugin *ph, const char *target, const char *msgid);
```

**Files:**
- [plugin.h](src/common/plugin.h) - API declarations
- [plugin.c](src/common/plugin.c) - API implementations

---

## Implementation Task List (Phased)

This section breaks down implementation into concrete tasks organized by dependency order. Tasks reference UX decisions from [IRCV3_UX_AUDIT.md](IRCV3_UX_AUDIT.md).

### Legend
- ✅ Complete
- 🔄 In Progress
- ⬜ Not Started
- 🔗 Dependency

---

### Phase 0: Prerequisites (Independent)

These have no dependencies and can be done anytime.

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | STS policy storage | servlist.c, servlist.h | `sts_policy_*` functions exist |
| ⬜ | STS upgrade notification | textevents.in, inbound.c | Add XP_TE_STSUPGRADE text event |
| ⬜ | `/STS` command | outbound.c | `/STS LIST`, `/STS CLEAR [host]` |
| ✅ | UTF8ONLY ISUPPORT parsing | modes.c | `serv->utf8only` flag exists |
| ⬜ | UTF8ONLY `/CHARSET` error | outbound.c | Improve error message (UX decision #6) |
| ⬜ | XP_TE_WARN text event | textevents.in, proto-irc.c | Add for WARN standard reply |
| ⬜ | XP_TE_NOTE text event | textevents.in, proto-irc.c | Add for NOTE standard reply |
| ⬜ | XP_TE_CHANRENAME text event | textevents.in | Add for channel rename |
| ✅ | Channel rename handler | proto-irc.c, inbound.c | `inbound_rename()` exists |
| ⬜ | Channel rename display | inbound.c | Emit XP_TE_CHANRENAME event |
| ⬜ | MONITOR command support | notify.c, outbound.c | Alternative to WATCH |

---

### Phase 1: Foundation (batch + message-tags)

**🔗 Required by:** chathistory, multiline, labeled-response, typing, redaction, read-marker, reply, react

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | `batch_info` struct | poxchat.h | Defined |
| ✅ | `active_batches` hash table | poxchat.h, server.c | In server struct |
| ✅ | `have_batch` capability flag | poxchat.h, inbound.c | CAP negotiation |
| ✅ | BATCH command handler | proto-irc.c | Parse +id/-id |
| ✅ | `inbound_batch_start()` | inbound.c | Create batch context |
| ✅ | `inbound_batch_end()` | inbound.c | Finalize batch |
| ✅ | `inbound_batch_add_message()` | inbound.c | Collect messages in batch |
| ✅ | Batch tag parsing | proto-irc.c | `batch=` tag in messages |
| ✅ | `message_tags_data.batch_id` | proto-irc.h | Field exists |
| ✅ | `message_tags_data.msgid` | proto-irc.h | Field exists |
| ✅ | `message_tags_data.label` | proto-irc.h | Field exists |
| ✅ | `message_tags_data.all_tags` | proto-irc.h | Hash table for all tags |
| ✅ | `have_message_tags` capability | poxchat.h, inbound.c | CAP negotiation |
| ✅ | Client-only tag parsing (`+`) | proto-irc.c | Handle `+typing` etc. |
| ✅ | TAGMSG command handler | proto-irc.c | Parse tag-only messages |
| ✅ | `inbound_tagmsg()` | inbound.c | Process TAGMSG |
| ✅ | `/TAGMSG` outbound command | outbound.c | Send TAGMSG with tags |

---

### Phase 2: Chathistory

**🔗 Requires:** batch, message-tags
**🔗 Required by:** read-marker (synergy)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | `have_chathistory` capability | poxchat.h, inbound.c | CAP negotiation |
| ✅ | `chathistory_limit` from ISUPPORT | poxchat.h, modes.c | Parse CHATHISTORY token |
| ✅ | chathistory.c module | chathistory.c | New file created |
| ✅ | `chathistory_request_latest()` | chathistory.c | LATEST subcommand |
| ✅ | `chathistory_request_before()` | chathistory.c | BEFORE subcommand |
| ✅ | `chathistory_request_after()` | chathistory.c | AFTER subcommand |
| ✅ | `chathistory_process_batch()` | chathistory.c | Handle chathistory batch |
| ✅ | Event playback (JOIN/PART/etc) | chathistory.c | Handle events in history |
| ✅ | `/HISTORY` command | outbound.c | Manual history request |
| ✅ | Auto-fetch on JOIN | inbound.c | **UX #2**: Before "You are now talking on" |
| ✅ | Hold JOIN banner for history | inbound.c, chathistory.c | Defer display until history fetched |
| ✅ | Auto-fetch on reconnect | inbound.c | Uses AFTER with scrollback_newest_msgid |
| ✅ | Scroll debouncing | fe-gtk/xtext.c | `scroll_top_debounce_tag` with backoff |
| ✅ | `history_loading` flag | poxchat.h | Prevent concurrent requests |
| ✅ | Scroll debounce timer | xtext.h | `scroll_top_debounce_tag` in GtkXText |
| ✅ | Rate limiting | chathistory.c | history_loading prevents concurrent |
| ✅ | Msgid deduplication | poxchat.h, chathistory.c, inbound.c | `known_msgids` hash table |
| ✅ | Session `oldest_msgid` | poxchat.h | For BEFORE pagination |
| ✅ | Session `newest_msgid` | poxchat.h | For AFTER catch-up |
| ✅ | `history_exhausted` flag | poxchat.h | Server has no more |
| ✅ | Chathistory preferences | cfgfiles.c | `hex_irc_chathistory_*` |
| ✅ | Background history fetching | chathistory.c | Gradual older history retrieval |
| ✅ | Background fetch time cap | chathistory.c | Max age limit (default 24h) |
| ⬜ | Chathistory settings UI | setup.c | Settings panel for preferences |

---

### Phase 3: Echo-Message + Labeled-Response

**🔗 Requires:** message-tags (for msgid correlation)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | `have_echo_message` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | Self-echo detection | inbound.c | Check if message is from self |
| ✅ | Defer outgoing display | outbound.c | Skips local display when echo-message enabled |
| ⬜ | Pending message tracking | poxchat.h | Track unechoed messages |
| ⬜ | Pending message visual | fe-gtk/xtext.c | **UX #1**: Muted color, theme-aware |
| ⬜ | Echo timeout (10s) | outbound.c | Fall back to local display |
| ⬜ | Msgid correlation | inbound.c | Match echo to pending by msgid |
| ✅ | `have_labeled_response` cap | poxchat.h, inbound.c | CAP negotiation |
| ✅ | Label counter | poxchat.h, server.c | `tcp_generate_label()` exists |
| ⬜ | `pending_labels` hash table | poxchat.h | Track pending responses |
| ✅ | Add labels to commands | server.c | `tcp_sendf_labeled()` exists |
| ⬜ | ACK batch handling | inbound.c | Handle labeled-response batch |
| ⬜ | Echo-message preference | cfgfiles.c | `hex_irc_echo_message` |

---

### Phase 4: Read Marker

**🔗 Requires:** message-tags (msgid)
**🔗 Synergy with:** chathistory (fetch from marker position)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `have_read_marker` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | MARKREAD command handler | proto-irc.c | Parse server response |
| ⬜ | `/MARKREAD` command | outbound.c | Set/query marker |
| ⬜ | Session `last_read_timestamp` | poxchat.h | Track marker position |
| ⬜ | Query marker on JOIN | inbound.c | `MARKREAD #channel` |
| ⬜ | Update on scroll past | fe-gtk/xtext.c | **UX #7**: Advance marker |
| ⬜ | Update on send message | outbound.c | **UX #7**: Implies caught up |
| ⬜ | Visual marker line | fe-gtk/xtext.c | Colored horizontal rule |
| ⬜ | Tab unread badge | fe-gtk/chanview.c | Messages since marker |
| ⬜ | Local fallback | chanopt.c | When server doesn't support |
| ⬜ | Read marker preferences | cfgfiles.c | `hex_irc_read_marker*` |

---

### Phase 5: Multiline

**🔗 Requires:** batch

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `have_multiline` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | MULTILINE ISUPPORT parsing | modes.c | `max-bytes`, `max-lines` |
| ⬜ | Multiline batch collection | inbound.c | Collect PRIVMSG lines |
| ⬜ | Join lines with `\n` | inbound.c | Single message display |
| ⬜ | xtext embedded newlines | fe-gtk/xtext.c | **UX #8**: Inline display |
| ⬜ | Truncation for large | fe-gtk/xtext.c | **UX #9**: >10 lines truncate |
| ⬜ | Expand inline | fe-gtk/xtext.c | Click to show more |
| ⬜ | Expand popup dialog | fe-gtk/dialog.c | **UX #9**: For very large |
| ⬜ | Send multiline batch | outbound.c | BATCH + multiple PRIVMSG |
| ⬜ | Paste detection | fe-gtk/inputgui.c | Detect multi-line paste |
| ⬜ | `/MULTILINE INFO` command | outbound.c | Show server limits |
| ⬜ | Multiline preferences | cfgfiles.c | `hex_irc_multiline_*` |

---

### Phase 6: Typing Indicators

**🔗 Requires:** TAGMSG, message-tags

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | Parse `+typing` tag | proto-irc.c | From TAGMSG |
| ⬜ | Typing state per user | poxchat.h | Track who's typing |
| ⬜ | Typing timeout (6s) | userlist.c | Auto-clear state |
| ⬜ | Clear on message receive | inbound.c | User sent message |
| ⬜ | Userlist typing icon | fe-gtk/userlist.c | **UX #10**: Pencil/ellipsis |
| ⬜ | Send `+typing=active` | outbound.c | On input start (debounced) |
| ⬜ | Send `+typing=paused` | outbound.c | 5s no input |
| ⬜ | Send `+typing=done` | outbound.c | On send/clear |
| ⬜ | Typing send debounce (3s) | outbound.c | Min between sends |
| ⬜ | Query typing display | TBD | **UX #10**: Needs design work |
| ⬜ | Typing preferences | cfgfiles.c | `hex_irc_typing_*` |

---

### Phase 7: Message Redaction

**🔗 Requires:** message-tags (msgid)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `have_redact` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | REDACT command handler | proto-irc.c | Parse redaction |
| ⬜ | Find message by msgid | text.c | Lookup in buffer |
| ⬜ | Mark message redacted | text.c | Update display state |
| ⬜ | Redacted visual | fe-gtk/xtext.c | "[Message deleted]" |
| ⬜ | `/REDACT` command | outbound.c | Send redaction |
| ⬜ | Context menu "Delete" | fe-gtk/menu.c | For own messages |
| ⬜ | Op context menu | fe-gtk/menu.c | Delete others' messages |
| ⬜ | Redaction preferences | cfgfiles.c | `hex_irc_redact_*` |

---

### Phase 8: Reply + React

**🔗 Requires:** TAGMSG, message-tags (msgid)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | Parse `+draft/reply` tag | proto-irc.c | From messages |
| ⬜ | Reply visual | fe-gtk/xtext.c | **UX #11**: Quote above |
| ⬜ | Click quote to scroll | fe-gtk/xtext.c | Jump to original |
| ⬜ | Reply context menu | fe-gtk/menu.c | "Reply" option |
| ⬜ | Reply input indicator | fe-gtk/inputgui.c | "Replying to..." |
| ⬜ | Send with reply tag | outbound.c | Include `+draft/reply` |
| ⬜ | Parse `+draft/react` tag | proto-irc.c | From TAGMSG |
| ⬜ | Reaction aggregation | text.c | Count per emoji per message |
| ⬜ | Reaction display | fe-gtk/xtext.c | **UX #12**: Below message |
| ⬜ | Emoji picker | fe-gtk/emojipicker.c | New widget |
| ⬜ | Send reaction | outbound.c | TAGMSG with react+reply |

---

### Phase 9: Channel-Context (Whisper)

**🔗 Requires:** message-tags

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | Parse `+draft/channel-context` | proto-irc.c | From PRIVMSG |
| ⬜ | Display context indicator | inbound.c | **UX #13**: "[via #channel]" |
| ⬜ | Context menu "Whisper" | fe-gtk/menu.c | Send PM with context |
| ⬜ | `/WHISPER` command | outbound.c | Alternative to /MSG |

---

### Phase 10: Metadata + Pre-Away

**🔗 Independent but synergistic**

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `have_metadata` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | METADATA command handlers | proto-irc.c | GET/SET/LIST/SUB/UNSUB |
| ⬜ | `/METADATA` command | outbound.c | User interface |
| ⬜ | Virtual key queries | outbound.c | `$presence`, `$idle`, etc. |
| ⬜ | Metadata caching | servlist.c | Per-target storage |
| ⬜ | `have_pre_away` capability | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | `/AWAY *` support | outbound.c | Hidden connection |
| ⬜ | Pre-away preference | cfgfiles.c | Send AWAY * on connect |

---

### Phase 11: Account Registration

**🔗 Independent**

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `have_account_registration` | poxchat.h, inbound.c | CAP negotiation |
| ⬜ | REGISTER response handler | proto-irc.c | SUCCESS/VERIFY/FAIL |
| ⬜ | `/REGISTER` command | outbound.c | **UX #15**: All args required |
| ⬜ | `/VERIFY` command | outbound.c | Verification code |
| ⬜ | Network Manager GUI | fe-gtk/servlistgui.c | Registration form |

---

### Phase 13: SASL Enhancements

**🔗 Independent, builds on existing SASL**

#### 13.1 OAUTHBEARER Token Refresh (Section 6.4)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | Token storage | secure-storage.c | `secure_storage_store_oauth_tokens()` exists |
| ⬜ | Complete `oauth_refresh_token()` | oauth.c | Currently stub, needs HTTP POST |
| ⬜ | Token expiry timer | poxchat.h, server.c | Schedule refresh before expiry |
| ⬜ | `oauth_refresh_timer` in server | poxchat.h | Timer ID storage |
| ⬜ | Cancel timer on disconnect | server.c | Cleanup |
| ⬜ | SASL re-authentication | inbound.c | `sasl_reauthenticate()` function |
| ⬜ | Handle 903 (success) | inbound.c | Update tokens, reschedule timer |
| ⬜ | Handle 907 (not supported) | inbound.c | Store for next reconnect |
| ⬜ | Token refresh notification | textevents.in | XP_TE_OAUTHREFRESH |
| ⬜ | Token failure notification | textevents.in | XP_TE_OAUTHFAIL |
| ⬜ | `/OAUTH STATUS` command | outbound.c | Check token state |
| ⬜ | `/OAUTH REAUTH` command | outbound.c | Force re-authentication |

#### 13.2 ECDSA-NIST256P-CHALLENGE (Section 6.5)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | `MECH_ECDSA_CHALLENGE` enum | poxchat.h | New SASL mechanism |
| ⬜ | `LOGIN_SASL_ECDSA` login type | poxchat.h | New login method |
| ⬜ | ECDSA key generation | secure-storage.c | P-256 via OpenSSL |
| ⬜ | Key storage (secure) | secure-storage.c | Private key storage |
| ⬜ | Public key export | secure-storage.c | For registration with services |
| ⬜ | Challenge-response flow | inbound.c, proto-irc.c | Sign server challenge |
| ⬜ | ECDSA signature | proto-irc.c | `ECDSA_sign()` with SHA-256 |
| ⬜ | Network config: key path | servlist.h, servlist.c | ECDSA key file selector |
| ⬜ | Key generation UI | fe-gtk/servlistgui.c | Generate button |
| ⬜ | Public key display | fe-gtk/servlistgui.c | Copy for services registration |

#### 13.3 X3 Session Tokens (Section 6.6)

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ⬜ | Detect token in AuthServ NOTICE | inbound.c | "session cookie is:" pattern |
| ⬜ | Token extraction | inbound.c | Parse base64 token |
| ⬜ | User prompt dialog | fe-gtk/dialog.c | "Use token for future logins?" |
| ⬜ | Store session token | secure-storage.c | Per-network storage |
| ⬜ | Use token in SASL PLAIN | server.c | Token replaces password |
| ⬜ | Token invalidation handling | inbound.c | Fall back to password |
| ⬜ | Token refresh on AUTH | inbound.c | Watch for new token |
| ⬜ | Network indicator | fe-gtk/servlistgui.c | Show if token stored |
| ⬜ | Clear token option | fe-gtk/servlistgui.c | Manual removal |
| ⬜ | Preference: auto-store | cfgfiles.c | `hex_irc_session_token_auto` |

---

### Phase 12: Network Icon

**🔗 Independent, lower priority**

| Status | Task | Files | Notes |
|--------|------|-------|-------|
| ✅ | `network_icon_url` storage | poxchat.h | Field exists |
| ⬜ | Icon URL validation | modes.c | HTTPS preferred |
| ⬜ | Async icon fetch | network.c | Download in background |
| ⬜ | Icon caching | cfgfiles.c | `~/.config/poxchat/icons/` |
| ⬜ | Channel tree icon | fe-gtk/chanview.c | **UX #3**: Replace pix_tree_server |
| ⬜ | Network list icon | fe-gtk/servlistgui.c | Show in server list |

---

## Implementation Priority Order

Based on dependencies and UX impact:

1. **Phase 1** - Foundation is complete ✅
2. **Phase 2** - Chathistory ~98% complete ✅ (remaining: settings UI panel)
3. **Phase 0** - Text events (quick wins, no dependencies)
4. **Phase 3** - Echo-message ~40% complete 🔄 (caps done, needs UX: timeout, pending visual)
5. **Phase 4** - Read marker (synergizes with chathistory)
6. **Phase 6** - Typing indicators (modern chat feel)
7. **Phase 5** - Multiline (code paste improvement)
8. **Phase 7** - Redaction (message management)
9. **Phase 8** - Reply/React (lower priority)
10. **Phase 9-12** - Remaining features

---

## Known Oversights & TODOs

This section tracks identified gaps discovered during implementation audit (January 2026).

### Missing Text Events

| Event | Purpose | Status |
|-------|---------|--------|
| `XP_TE_WARN` | WARN standard reply display | ⬜ Not created |
| `XP_TE_NOTE` | NOTE standard reply display | ⬜ Not created |
| `XP_TE_CHANRENAME` | Channel rename notification | ⬜ Not created |
| `XP_TE_STSUPGRADE` | STS TLS upgrade notification | ⬜ Not created |

### In-Code TODOs

| Location | Description | Priority |
|----------|-------------|----------|
| `inbound.c:~2101` | Typing indicator UI display | Medium |
| `inbound.c:~2124` | Reaction display handling | Medium |
| `inbound.c:~2129` | Reply quote display | Medium |

### Batch Handling Gaps

| Batch Type | CAP Negotiation | Handler Implementation |
|------------|-----------------|------------------------|
| `multiline` | ✅ | ✅ (`process_multiline_batch`) |
| `chathistory` | ✅ | ✅ (`chathistory_process_batch`) |
| `labeled-response` | ✅ (cap) | ⬜ No ACK correlation |
| `netjoin` | ⚠️ Parsed | ⬜ Not processed (falls through) |
| `netsplit` | ⚠️ Parsed | ⬜ Not processed (falls through) |

### Echo-Message UX Gaps

While the echo-message capability is negotiated and basic self-echo detection exists, UX enhancements are missing:

- ⬜ **Pending visual** - Messages should show muted/italic style while awaiting echo
- ⬜ **Timeout mechanism** - Fall back to local display if echo not received within ~10 seconds
- ⬜ **Msgid correlation** - Match received echo to pending message for confirmation
- ⬜ **Failed message handling** - Visual indicator when message delivery uncertain

### xtext Modernization Status

See [XTEXT_MODERNIZATION_PLAN.md](XTEXT_MODERNIZATION_PLAN.md) for detailed xtext buffer modernization status.

| Phase | Status |
|-------|--------|
| Phase 1: Entry Identification | ✅ Complete |
| Phase 1.5: Multiline Entry Support | ✅ Complete |
| Phase 2: Scroll Anchor System | ⬜ Not started |
| Phase 3: Insertion Modes | ⬜ Not started |
| Phase 4: Entry Modification | ⬜ Not started |
