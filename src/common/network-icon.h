/* PoxChat - IRCv3 draft/ICON Network Icon Support
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Fetches and caches network icons advertised via the draft/ICON
 * ISUPPORT token (https://ircv3.net/specs/extensions/network-icon).
 */

#ifndef POXCHAT_NETWORK_ICON_H
#define POXCHAT_NETWORK_ICON_H

#include <glib.h>

struct server;

/* Replace {size} template variable in icon URL with pixel dimension.
 * Returns newly allocated string. */
char *network_icon_resolve_url (const char *url_template, int size_px);

/* Load icon from disk cache using network name as key.
 * Call at connect time before ISUPPORT arrives. */
void network_icon_load_cached (struct server *serv);

/* Check ISUPPORT ICON URL against cache; fetch if changed.
 * On completion, calls fe_network_icon_ready() with raw image data. */
void network_icon_fetch (struct server *serv);

/* Cancel any in-flight icon fetch for this server. Safe to call if none pending. */
void network_icon_cancel (struct server *serv);

/* Remove cached icon files for this server's network. */
void network_icon_clear_cache (struct server *serv);

/* Icon target size in pixels (for {size} substitution and frontend scaling) */
#define NETWORK_ICON_SIZE 16

/* Maximum download size in bytes (512 KB) */
#define NETWORK_ICON_MAX_SIZE (512 * 1024)

#endif /* POXCHAT_NETWORK_ICON_H */
