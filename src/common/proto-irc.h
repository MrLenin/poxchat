/* PoxChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
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
 */

#include <time.h>
#include "poxchat.h"

#ifndef POXCHAT_PROTO_H
#define POXCHAT_PROTO_H

#define MESSAGE_TAGS_DATA_INIT			\
	{									\
		NULL, /* account name */		\
		FALSE, /* identified to nick */ \
		(time_t)0, /* timestamp */		\
		NULL, /* batch_id */			\
		NULL, /* msgid */				\
		NULL, /* label */				\
		NULL, /* all_tags hash table */	\
		FALSE, /* echo_confirmed */		\
	}

#define STRIP_COLON(word, word_eol, idx) (word)[(idx)][0] == ':' ? (word_eol)[(idx)]+1 : (word)[(idx)]

/* Message tag information that might be passed along with a server message
 *
 * See https://ircv3.net/specs/extensions/message-tags
 */
typedef struct
{
	char *account;
	gboolean identified;
	time_t timestamp;
	char *batch_id;       /* batch tag - reference to active batch */
	char *msgid;          /* msgid tag - unique message identifier */
	char *label;          /* label tag - for labeled-response correlation */
	GHashTable *all_tags; /* Full tag storage for plugins and extensions */
	gboolean echo_confirmed; /* Tier 2: pending entry confirmed, suppress echo display */
} message_tags_data;

void message_tags_data_free (message_tags_data *tags_data);
char *escape_tag_value (const char *value);

void proto_fill_her_up (server *serv);

#endif
