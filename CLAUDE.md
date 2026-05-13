# PoxChat Development Guide

## Project Overview
PoxChat is an IRC client originally forked from XChat. This fork is being modernized with GTK4 support and comprehensive IRCv3 protocol implementation.

## Guiding Principle

**Simpler isn't always better. Think it through. Aim for correct.**

Don't reach for quick fixes (adding timeouts, delays, retries) when the real problem is architectural. If a plan exists, finish implementing it properly rather than patching symptoms. Ask clarifying questions instead of making assumptions.

Delegate for large/complex/difficult tasks: use agents

Whenever you learn something particularly relevant or useful for the project, add it to an appropriate skill file, or create one

When using subagent-driven development to implement plans, use a worktree unless instructed otherwise.  When using a worktree use the tool and DO NOT use the superpowers:worktrees skill.

## Build System

### Windows (Visual Studio)
- Open `win32/poxchat.sln` in Visual Studio 2022
- Configuration is in `win32/poxchat.props` - **this file contains environment-specific paths and should not be committed**
- Build dependencies come from [gvsbuild](https://github.com/wingtk/gvsbuild) for GTK4
- Additional dependencies: OpenSSL (separate install), optionally libwebsockets/jansson/libcurl for OAuth2

### Linux/macOS (Meson)
```bash
meson setup builddir
meson compile -C builddir
```

## Key Directories

- `src/common/` - Core IRC protocol handling, cross-platform code
- `src/fe-gtk/` - GTK frontend (GTK4 on this branch)
- `win32/` - Windows build files and VS solution
- `data/` - Icons, themes, default configuration

## Architecture

### Server Connection Flow
1. `server_new()` creates server struct
2. `proto_fill_her_up()` sets up IRC protocol handlers
3. `irc_login()` sends CAP LS, NICK, USER
4. CAP negotiation in `inbound.c` (`inbound_cap_ls`, `inbound_cap_ack`)
5. SASL authentication if configured
6. `CAP END` sent after negotiation complete

### Message Flow
- Incoming: `server.c` → `proto-irc.c:process_line()` → `inbound.c` handlers
- Outgoing: `outbound.c` commands → `tcp_sendf()` → socket

### Key Structures
- `server` (poxchat.h) - Server connection state, capabilities, channels
- `session` (poxchat.h) - A tab/window (channel, query, or server console)
- `message_tags_data` (proto-irc.h) - IRCv3 message tags for a single message
- `batch_info` (poxchat.h) - IRCv3 batch state for collecting related messages

## IRCv3 Implementation

### Already Implemented
- multi-prefix, away-notify, account-notify, extended-join
- server-time, userhost-in-names, cap-notify
- chghost, setname, invite-notify, account-tag
- SASL: PLAIN, EXTERNAL, SCRAM-SHA-1/256/512, OAUTHBEARER

### In Progress (this branch)
- batch capability - foundation for chathistory, multiline
- message-tags (full) - all tags preserved in hash table
- echo-message - server echoes our messages back
- labeled-response - correlate responses to commands
- TAGMSG - tag-only messages (typing indicators, reactions)

### Planned
- draft/chathistory - message history retrieval
- draft/multiline - multi-line message batches
- draft/read-marker - sync read position across clients

## Coding Conventions

### Style
- C89/C99 style with tabs for indentation
- Function names: `module_action()` (e.g., `inbound_chanmsg`, `tcp_sendf`)
- Structs use lowercase with underscores
- Boolean flags in server struct use `:1` bitfields

### Memory Management
- Use GLib functions: `g_malloc`, `g_free`, `g_strdup`, `g_new0`
- Hash tables: `g_hash_table_new_full()` with proper destroy functions
- Lists: `GSList` for singly-linked, `GList` for doubly-linked

### Adding New Capabilities
1. Add to `supported_caps[]` array in `inbound.c`
2. Add `have_xxx` flag to server struct in `poxchat.h`
3. Handle in `inbound_toggle_caps()` to set/clear flag
4. Implement protocol handling in `proto-irc.c` or `inbound.c`

### Adding New Commands
1. Add handler function in `outbound.c`: `cmd_xxx()`
2. Add to `xc_cmds[]` array with name, function, permissions
3. Use `tcp_sendf()` to send to server

## Testing

### Test Server
- Nefarious IRCd with X3 services for IRCv3.2 features
- Connect and verify CAP negotiation in server console

### Manual Testing Checklist
- [ ] CAP LS shows expected capabilities
- [ ] CAP ACK confirms requested capabilities
- [ ] Feature works as expected (messages display, etc.)
- [ ] Graceful fallback when capability unavailable

## Common Tasks

### Adding a new IRCv3 message tag
1. Add field to `message_tags_data` struct in `proto-irc.h`
2. Update `MESSAGE_TAGS_DATA_INIT` macro
3. Parse in `handle_message_tags()` in `proto-irc.c`
4. Free in `message_tags_data_free()`

### Adding a new IRC command handler
1. Find `process_named_msg()` in `proto-irc.c`
2. Add case using `WORDL()` macro for 4-char command matching
3. Call appropriate `inbound_xxx()` function

## Preprocessor Defines

- `HC_GTK4` - Building with GTK4 (vs GTK3)
- `USE_OPENSSL` - OpenSSL available for TLS/crypto
- `USE_LIBWEBSOCKETS` - libwebsockets available for OAuth2 WebSocket server
- `WIN32` - Windows build
