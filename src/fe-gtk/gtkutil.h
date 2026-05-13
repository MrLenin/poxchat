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

#ifndef POXCHAT_GTKUTIL_H
#define POXCHAT_GTKUTIL_H

#include <gtk/gtk.h>
#include "../common/fe.h"
#include "gtk-helpers.h"

typedef void (*filereqcallback) (void *, char *file);

void gtkutil_file_req (GtkWindow *parent, const char *title, void *callback, void *userdata, char *filter, char *extensions, int flags);
void gtkutil_destroy (GtkWidget * igad, GtkWidget * dgad);
void gtkutil_destroy_on_esc (GtkWidget *win);
GtkWidget *gtkutil_button (GtkWidget *box, char *stock, char *tip, void *callback,
				 void *userdata, char *labeltext);
void gtkutil_label_new (char *text, GtkWidget * box);
GtkWidget *gtkutil_entry_new (int max, GtkWidget * box, void *callback,
										gpointer userdata);
void show_and_unfocus (GtkWidget * wid);
void gtkutil_set_icon (GtkWidget *win);
GtkWidget *gtkutil_window_new (char *title, char *role, int width, int height, int flags);
gboolean gtkutil_close_request_focus_parent (GtkWindow *win, gpointer parent);
GtkApplication *fe_get_application (void);
void gtkutil_copy_to_clipboard (GtkWidget *widget, gboolean primary_only,
                                const gchar *str);
gboolean gtkutil_tray_icon_supported (GtkWindow *window);

#if defined (WIN32) || defined (__APPLE__)
gboolean gtkutil_find_font (const char *fontname);
#endif

#endif
