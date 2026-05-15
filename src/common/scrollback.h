/* PoxChat
 * Copyright (C) 2024 PoxChat Contributors
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
 * SQLite-based scrollback storage for message history persistence
 */

#ifndef POXCHAT_SCROLLBACK_H
#define POXCHAT_SCROLLBACK_H

#include <glib.h>
#include <time.h>

/* Opaque handle to scrollback database */
typedef struct scrollback_db scrollback_db;

/* Message record from scrollback */
typedef struct {
	gint64 id;           /* Database row ID */
	char *channel;       /* Channel/query name */
	time_t timestamp;    /* Message timestamp */
	char *msgid;         /* IRCv3 msgid (may be NULL) */
	char *text;          /* Message text (formatted for display) */
	char *redacted_by;   /* Who redacted this message (NULL = not redacted) */
	char *redact_reason; /* Redaction reason (may be NULL) */
	time_t redact_time;  /* When redaction occurred */
	gboolean is_user_msg;/* True for PRIVMSG/NOTICE/ACTION; false for events.
	                      * Gates hover reply/react buttons on re-materialized entries. */
} scrollback_msg;

/**
 * Open (or create) the scrollback database for a network.
 * Database is stored in scrollback/{network}.db
 *
 * @param network Network name (used for filename)
 * @return Database handle, or NULL on error
 */
scrollback_db *scrollback_open (const char *network);

/**
 * Close the scrollback database.
 *
 * @param db Database handle
 */
void scrollback_db_close (scrollback_db *db);

/**
 * Save a message to scrollback.
 * Uses INSERT OR IGNORE for automatic deduplication by msgid.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param timestamp Message timestamp
 * @param msgid IRCv3 message ID (may be NULL)
 * @param text Message text (formatted for display)
 * @return Inserted rowid on success, -1 on failure
 */
gint64 scrollback_db_save (scrollback_db *db, const char *channel,
                          time_t timestamp, const char *msgid, const char *text,
                          gboolean is_user_msg);

/**
 * Load the most recent messages for a channel.
 * Returns messages in chronological order (oldest first).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param limit Maximum messages to load
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_db_load (scrollback_db *db, const char *channel, int limit);

/**
 * Get the newest msgid for a channel (for CHATHISTORY AFTER requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_newest_msgid (scrollback_db *db, const char *channel);

/**
 * Get the oldest msgid for a channel (for CHATHISTORY BEFORE requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Oldest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_oldest_msgid (scrollback_db *db, const char *channel);

/**
 * Get the newest timestamp for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest timestamp, or 0 if no messages
 */
time_t scrollback_get_newest_time (scrollback_db *db, const char *channel);

/**
 * Check if a msgid already exists (for deduplication).
 *
 * @param db Database handle
 * @param msgid Message ID to check
 * @return TRUE if msgid exists
 */
gboolean scrollback_has_msgid (scrollback_db *db, const char *msgid);

/**
 * Update a pending placeholder msgid to the real server-assigned msgid.
 * Used by echo-message confirmation: pending entry saved with "pending:<label>"
 * gets updated to the real msgid when the echo arrives.
 */
gboolean scrollback_update_pending_msgid (scrollback_db *db, const char *channel,
                                           const char *pending_msgid, const char *real_msgid);

/**
 * Mark a message as redacted in scrollback.
 * Preserves the original text for accountability; stores who redacted it and why.
 *
 * @param db Database handle
 * @param msgid Message ID to redact
 * @param redacted_by Nick who performed the redaction
 * @param reason Redaction reason (may be NULL)
 * @param redact_time When the redaction occurred
 * @return TRUE if a message was updated
 */
gboolean scrollback_redact_message (scrollback_db *db, const char *msgid,
                                    const char *redacted_by, const char *reason,
                                    time_t redact_time);

/**
 * Clear all messages for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 */
void scrollback_clear (scrollback_db *db, const char *channel);

/**
 * Free a scrollback message.
 *
 * @param msg Message to free
 */
void scrollback_msg_free (scrollback_msg *msg);

/**
 * Free a list of scrollback messages.
 *
 * @param list List returned by scrollback_db_load
 */
void scrollback_msg_list_free (GSList *list);

/**
 * Migrate old text-based scrollback to SQLite.
 * Called automatically when opening a database if old files exist.
 *
 * @param db Database handle
 * @param network Network name
 * @param channel Channel name
 * @return Number of messages migrated, or -1 on error
 */
int scrollback_migrate (scrollback_db *db, const char *network, const char *channel);

/* IRCv3 reaction record from scrollback */
typedef struct {
	char *target_msgid;      /* msgid of the message reacted to */
	char *reaction_text;     /* reaction content (emoji or text) */
	char *nick;              /* who reacted */
	gboolean is_self;        /* was this our own reaction? */
} scrollback_reaction;

/* IRCv3 reply record from scrollback */
typedef struct {
	char *msgid;             /* msgid of the reply message */
	char *target_msgid;      /* msgid of the message being replied to */
	char *target_nick;       /* nick of the original message */
	char *target_preview;    /* truncated preview of original message */
} scrollback_reply;

/**
 * Save a reaction to scrollback.
 */
gboolean scrollback_save_reaction (scrollback_db *db, const char *channel,
                                   const char *target_msgid, const char *reaction_text,
                                   const char *nick, gboolean is_self, time_t timestamp);

/**
 * Remove a reaction from scrollback.
 */
gboolean scrollback_remove_reaction (scrollback_db *db, const char *target_msgid,
                                     const char *reaction_text, const char *nick);

/**
 * Load all reactions for a channel.
 * @return GSList of scrollback_reaction* (caller frees with scrollback_reaction_list_free)
 */
GSList *scrollback_load_reactions (scrollback_db *db, const char *channel);

/**
 * Load reactions for a single target msgid.  Used to re-hydrate badges on
 * virt-materialized historical entries after eviction + ensure_range.
 *
 * @return GSList of scrollback_reaction* (caller frees via scrollback_reaction_list_free)
 */
GSList *scrollback_load_reactions_by_msgid (scrollback_db *db, const char *target_msgid);
void scrollback_reaction_free (scrollback_reaction *r);
void scrollback_reaction_list_free (GSList *list);

/**
 * Save reply context for a message.
 */
gboolean scrollback_save_reply (scrollback_db *db, const char *msgid,
                                const char *target_msgid, const char *target_nick,
                                const char *target_preview);

/**
 * Load all reply contexts for messages in a channel.
 * @return GSList of scrollback_reply* (caller frees with scrollback_reply_list_free)
 */
GSList *scrollback_load_replies (scrollback_db *db, const char *channel);
void scrollback_reply_free (scrollback_reply *r);
void scrollback_reply_list_free (GSList *list);

/**
 * Load the reply context for a single message by its msgid.
 * Used by virtual-scrollback re-materialization to reattach reply info
 * to entries that were evicted and reloaded from the DB.
 * @return scrollback_reply* (caller frees with scrollback_reply_free) or NULL if no reply.
 */
scrollback_reply *scrollback_load_reply_by_msgid (scrollback_db *db, const char *msgid);

/* --- Virtual scrollback query functions --- */

/**
 * Get total message count for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Total message count, or 0 on error
 */
int scrollback_count (scrollback_db *db, const char *channel);

/**
 * Load a window of messages by position (offset/limit).
 * Returns messages in chronological order (oldest first).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param offset 0-based starting position
 * @param limit Maximum messages to load
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_load_range (scrollback_db *db, const char *channel,
                               int offset, int limit);

/**
 * Get the maximum message row ID for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Max row ID, or 0 if no messages
 */
gint64 scrollback_get_max_rowid (scrollback_db *db, const char *channel);

/**
 * Get the 0-based positional index of a specific row ID in a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param rowid Row ID to look up
 * @return 0-based index of the entry
 */
int scrollback_get_index_of_rowid (scrollback_db *db, const char *channel,
                                    gint64 rowid);

/**
 * Find the DB rowid for a message by its IRCv3 msgid.
 * Returns 0 if not found.
 */
gint64 scrollback_get_rowid_by_msgid (scrollback_db *db, const char *channel,
                                       const char *msgid);

/**
 * Search message text using SQL LIKE pattern.
 * Returns matching messages in chronological order.
 * Only id, channel, and text fields are populated in the results.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param pattern SQL LIKE pattern (e.g., "%search term%")
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_search_text (scrollback_db *db, const char *channel,
                                const char *pattern);

/**
 * Begin a ref-counted transaction.  Multiple begin calls nest;
 * the actual SQL BEGIN only fires on the first.
 */
void scrollback_begin_transaction (scrollback_db *db);

/**
 * Commit a ref-counted transaction.  The actual SQL COMMIT only
 * fires when the outermost transaction ends (depth returns to 0).
 */
void scrollback_commit_transaction (scrollback_db *db);

/**
 * Initialize the scrollback subsystem.
 * Called once at startup.
 */
void scrollback_init (void);

/**
 * Shutdown the scrollback subsystem.
 * Called once at exit.
 */
void scrollback_shutdown (void);

#endif /* POXCHAT_SCROLLBACK_H */
