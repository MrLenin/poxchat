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

/* Strip "draft/" and vendor ("host.tld/") prefixes from an IRCv3 name;
 * loops so "vendor.tld/draft/x" resolves to "x".  Returns a pointer into
 * name. */
const char *persistence_strip_namespace (const char *name);

/* TRUE when name is the bare persistence name in any namespace
 * ("draft/persistence", "persistence", "evilnet.github.io/persistence").
 * Used for the cap name and for the channel-state restoration batch
 * type, which the spec gives the same name. */
gboolean persistence_is_bare_name (const char *name);

/* CAP LS value of the cap: comma-separated optional-verb tokens, each
 * possibly namespaced.  Sets serv->persistence_tok_*; unknown tokens are
 * ignored (the spec calls the value a hint, not an inventory). */
void persistence_parse_cap_value (server *serv, const char *value);

/* Send "PERSISTENCE ATTACH <profile> [<msgid>]" for serv->persist_profile,
 * with the newest scrollback msgid as the cursor when the server
 * advertised attach-cursor.  Valid only between SASL completion and
 * CAP END; a no-op without the cap or without a profile, and at most
 * once per connection.  The attach token is not required — the CAP
 * value is a hint, not an inventory — so a server that lacks ATTACH
 * answers FAIL, which restores legacy behaviour.  No reply is awaited. */
void persistence_send_attach (server *serv);

/* TRUE when we handed the server a cursor and it is therefore expected
 * to replay every buffer itself: our own catch-up fetches would be
 * redundant.  Provisional — it goes FALSE once a bouncer-replay wrapper
 * actually opens (the replay is real and needs no help) and stays TRUE
 * while nothing has arrived, because a server may silently replay
 * nothing (REPLAY OFF, policy, no gap).  Callers must therefore treat it
 * as "hold off for now", not "never". */
gboolean persistence_server_drives_replay (server *serv);

/* Registration completed (001): close the ATTACH reply window, so a
 * later FAIL is not mistaken for the answer to our ATTACH. */
void persistence_registration_complete (server *serv);

/* ":server PERSISTENCE <args>" — args is everything after the verb, e.g.
 * "STATUS DEFAULT ON" or "PROFILE mobile channels :#a,#b".  Consumes
 * every reply shape in the spec; unknown shapes print the raw args. */
void persistence_handle_reply (server *serv, const char *args, time_t stamp);

/* ":server FAIL PERSISTENCE <code> [<context>] :<text>" */
void persistence_handle_fail (server *serv, const char *code, const char *context,
                              const char *text, time_t stamp);

/* draft/metadata-2 notification for a server-managed persistence key.
 * key is the sub-path from persistence_match(); value NULL = key unset.
 * Returns TRUE when consumed (nothing further to show). */
gboolean persistence_handle_metadata (server *serv, const char *target,
                                      const char *key, const char *value,
                                      time_t stamp);

/* Clear per-connection state; called from the server disconnect path. */
void persistence_reset (server *serv);

#endif
