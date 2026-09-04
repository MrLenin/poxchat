/* PoxChat
 * Copyright (C) 2026 PoxChat Contributors
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * IRCv3 draft/persistence — client side (Nefarious persistent sessions).
 * Spec (work in progress):
 *   https://gist.github.com/MrLenin/491f87ea4d95625a90ce525a804dbddb
 */

#ifndef POXCHAT_PERSISTENCE_H
#define POXCHAT_PERSISTENCE_H

#include "poxchat.h"

/* Namespace-tolerant match of an IRCv3 name — cap, batch type, metadata
 * key, ISUPPORT token — against this extension.  Accepts the draft name
 * ("draft/persistence"), the ratified name ("persistence") and any
 * vendor-scoped form ("evilnet.github.io/persistence"): the ratified
 * spec drops the draft/ prefix, and Nefarious is moving everything that
 * is not in the upstream spec into its vendor namespace.  Match through
 * this everywhere so that move is a no-op for the client.
 *
 * Returns the sub-path after the persistence stem: "" for the bare name,
 * "hold" for ".../persistence/hold"; NULL when the name is unrelated. */
const char *persistence_match (const char *name);

/* TRUE for the channel-state restoration batch type. */
gboolean persistence_is_batch_type (const char *type);

/* draft/metadata-2 notification for a server-managed persistence key.
 * key is the sub-path from persistence_match(); value NULL = key unset.
 * Returns TRUE when consumed (nothing further to show). */
gboolean persistence_handle_metadata (server *serv, const char *target,
                                      const char *key, const char *value,
                                      time_t stamp);

/* Clear per-connection state; called from the server disconnect path. */
void persistence_reset (server *serv);

#endif
