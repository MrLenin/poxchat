<p align="center">
  <img src="data/icons/poxchat-256.png" width="160" alt="PoxChat">
</p>

# PoxChat

PoxChat is an Internet Relay Chat (IRC) app for Linux, Windows, Mac & more.  It is based around GTK4 and supports the  expanded IRCv3 features including replies, history, reactions, etc. **This is not release-ready** — expect bugs, rough edges, and the occasional crash. It was originally forked from the now archived/unmaintained app [HexChat](https://hexchat.github.io/)

Upstream HexChat [documentation](https://hexchat.readthedocs.org/en/latest/index.html) still applies for most basics ([FAQ](https://hexchat.readthedocs.org/en/latest/faq.html), [Python API](https://hexchat.readthedocs.org/en/latest/script_python.html), [Perl API](https://hexchat.readthedocs.org/en/latest/script_perl.html)).

## What's new in this fork
- **gtk4** many UI/UX improvements
- **IRCv3 3.1/3.2 core:** multi-prefix, away-notify, account-notify, extended-join, server-time, userhost-in-names, cap-notify, chghost, setname, invite-notify, account-tag.
- **IRCv3 message framework:** `message-tags`, `batch`, `echo-message`, `labeled-response`.
- **SASL:** PLAIN, EXTERNAL, SCRAM-SHA-1/256/512, OAUTHBEARER.
- **IRCv3 drafts** (wired up, some with caveats — see below): `chathistory`, `multiline`, `event-playback`, `read-marker`, `message-redaction`, `metadata-2`, `channel-rename`, `pre-away`, `account-registration`, `extended-isupport`.

## Known rough edges

- Persistence features (`chathistory` / `read-marker` / `metadata-2`) don't yet surface auth-required failures gracefully — on servers that require login for history, requests just silently fail.
- Intermittent crashes: opening the quit dialog; closing the find bar.
- Pane collapse can clip on fast drags.
- Chanview tree dotted-line indentation is currently broken.
- Display lists in compact mode/Chanview smaller text is incomplete.
- macOS integration is designed but not yet implemented.

## Building

### Windows (Visual Studio)
Open `win32/poxchat.sln` in Visual Studio 2022. Build dependencies come from [gvsbuild](https://github.com/wingtk/gvsbuild) built for GTK4, plus a separately installed OpenSSL. Optional: libwebsockets, jansson, libcurl for OAuth2.

`win32/poxchat.props` contains environment-specific paths and is not committed.

### Linux (Meson)
```bash
meson setup builddir
meson compile -C builddir
```

Debian-family dependencies:
```
sudo apt install build-essential meson ninja-build gettext libgtk-4-dev libssl-dev \
  libdbus-glib-1-dev libcanberra-dev libnotify-dev liblua5.4-dev libpython3-dev \
  libperl-dev libx11-dev python3-cffi libluajit-5.1-dev libpci-dev python3-dev \
  desktop-file-utils libsqlite3-dev libjansson-dev libsoup-3.0-dev libwebsockets-dev \
  libsecret-1-dev
```

---

<sub>
X-Chat ("xchat") Copyright (c) 1998-2010 By Peter Zelezny.  
HexChat ("hexchat") Copyright (c) 2009-2014 By Berke Viktor.  
PoxChat fork Copyright (c) 2026- Evilnet contributors.
</sub>

<sub>
This program is released under the GPL v2 with the additional exemption
that compiling, linking, and/or using OpenSSL is allowed. You may
provide binary packages linked to the OpenSSL libraries, provided that
all other requirements of the GPL are met.
See file COPYING for details.
</sub>
