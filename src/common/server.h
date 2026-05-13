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

#ifndef POXCHAT_SERVER_H
#define POXCHAT_SERVER_H

extern GSList *serv_list;

/* eventually need to keep the tcp_* functions isolated to server.c */
int tcp_send_len (server *serv, char *buf, int len);
void tcp_sendf (server *serv, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
char *tcp_generate_label (server *serv);
char *tcp_sendf_labeled (server *serv, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
char *tcp_sendf_labeled_tracked (server *serv, const char *command,
                                 const char *target, const char *fmt, ...) G_GNUC_PRINTF (4, 5);
char *tcp_sendf_with_tags (server *serv, const char *command, const char *target,
                           const char *tag_key, const char *tag_value,
                           const char *fmt, ...) G_GNUC_PRINTF (6, 7);
char *tcp_sendf_with_raw_tags (server *serv, const char *command, const char *target,
                               const char *extra_tags, const char *fmt, ...) G_GNUC_PRINTF (5, 6);
int tcp_send_real (void *ssl, int sok, GIConv write_converter, char *buf, int len);

void server_ensure_stale_sweep_timer (server *serv);
char *tcp_batch_open_multiline (server *serv, const char *target);
void tcp_batch_privmsg (server *serv, const char *batch_tag,
                        const char *target, const char *text);
void tcp_batch_close (server *serv, const char *batch_tag);
server *server_new (void);
int is_server (server *serv);
void server_fill_her_up (server *serv);
void server_set_encoding (server *serv, char *new_encoding);
void server_set_defaults (server *serv);
char *server_get_network (server *serv, gboolean fallback);
void server_set_name (server *serv, char *name);
void server_free (server *serv);

void server_away_save_message (server *serv, char *nick, char *msg);
struct away_msg *server_away_find_message (server *serv, char *nick);

void base64_encode (char *to, char *from, unsigned int len);

#endif
