/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fe-gtk.h"
#include "hex-input-edit.h"

#include <gdk/gdkkeysyms.h>

#include "../common/poxchat.h"
#include "../common/util.h"
#include "../common/userlist.h"
#include "../common/modes.h"
#include "../common/text.h"
#include "../common/notify.h"
#include "../common/poxchatc.h"
#include "../common/fe.h"
#include "gtkutil.h"
#include "palette.h"
#include "maingui.h"
#include "menu.h"
#include "pixmaps.h"
#include "userlistgui.h"
#include "fkeys.h"

/*
 * GTK4 Implementation using GListStore + GtkColumnView
 *
 * In GTK4, we use a GListStore containing HcUserItem objects instead of
 * GtkListStore. Each session still has its own model (sess->res->user_model).
 */

/* GObject to hold user list row data */
#define HC_TYPE_USER_ITEM (hc_user_item_get_type())
G_DECLARE_FINAL_TYPE (HcUserItem, hc_user_item, HC, USER_ITEM, GObject)

struct _HcUserItem {
	GObject parent;
	char *nick;				/* display nick (may include prefix if icons disabled) */
	char *hostname;			/* user's hostname */
	struct User *user;		/* pointer to backend User struct (not owned) */
	GdkTexture *icon;		/* user status icon (op, voice, etc.) - may be NULL */
	int color_index;		/* color index into colors[] array, 0 = no color */
};

G_DEFINE_TYPE (HcUserItem, hc_user_item, G_TYPE_OBJECT)

static void
hc_user_item_finalize (GObject *obj)
{
	HcUserItem *item = HC_USER_ITEM (obj);
	g_free (item->nick);
	g_free (item->hostname);
	g_clear_object (&item->icon);
	/* Note: item->user is not owned by us, don't free */
	G_OBJECT_CLASS (hc_user_item_parent_class)->finalize (obj);
}

static guint hc_user_item_changed_signal = 0;

static void
hc_user_item_class_init (HcUserItemClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = hc_user_item_finalize;
	/* "changed" is emitted by fe_userlist_rehash after it mutates the
	 * item's fields in place. Bind callbacks listen to this and update
	 * their cell labels directly — no items-changed, no widget rebind,
	 * no flicker. g_signal_connect_object's auto-disconnect on target
	 * widget destruction keeps us safe from stale connections. */
	hc_user_item_changed_signal = g_signal_new (
		"changed", G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
hc_user_item_init (HcUserItem *item)
{
	item->nick = NULL;
	item->hostname = NULL;
	item->user = NULL;
	item->icon = NULL;
	item->color_index = 0;
}

static HcUserItem *
hc_user_item_new (const char *nick, const char *hostname, struct User *user,
                  GdkPixbuf *pixbuf, int color_index)
{
	HcUserItem *item = g_object_new (HC_TYPE_USER_ITEM, NULL);
	item->nick = g_strdup (nick ? nick : "");
	item->hostname = g_strdup (hostname ? hostname : "");
	item->user = user;
	item->icon = pixbuf ? hc_pixbuf_to_texture (pixbuf) : NULL;
	item->color_index = color_index;
	return item;
}

/* Forward declaration for GTK4 selection list function */
static char **userlist_selection_list_gtk4 (GtkColumnView *view, int *num_ret);
static void userlist_update_nick_color (GtkLabel *nick_label,
                                        HcUserItem *user_item,
                                        gboolean selected);

GdkPixbuf *
get_user_icon (server *serv, struct User *user)
{
	char *pre;
	int level;

	if (!user)
		return NULL;

	/* these ones are hardcoded */
	switch (user->prefix[0])
	{
		case 0: return NULL;
		case '+': return pix_ulist_voice;
		case '%': return pix_ulist_halfop;
		case '@': return pix_ulist_op;
	}

	/* find out how many levels above Op this user is */
	pre = strchr (serv->nick_prefixes, '@');
	if (pre && pre != serv->nick_prefixes)
	{
		pre--;
		level = 0;
		while (1)
		{
			if (pre[0] == user->prefix[0])
			{
				switch (level)
				{
					case 0: return pix_ulist_owner;		/* 1 level above op */
					case 1: return pix_ulist_founder;	/* 2 levels above op */
					case 2: return pix_ulist_netop;		/* 3 levels above op */
				}
				break;	/* 4+, no icons */
			}
			level++;
			if (pre == serv->nick_prefixes)
				break;
			pre--;
		}
	}

	return NULL;
}

void
fe_userlist_numbers (session *sess)
{
	char tbuf[256];

	if (sess == current_tab || !sess->gui->is_tab)
	{
		if (sess->total)
		{
			g_snprintf (tbuf, sizeof (tbuf), _("%d ops, %d total"), sess->ops, sess->total);
			tbuf[sizeof (tbuf) - 1] = 0;
			gtk_label_set_text (GTK_LABEL (sess->gui->namelistinfo), tbuf);
		} else
		{
			gtk_label_set_text (GTK_LABEL (sess->gui->namelistinfo), NULL);
		}

		if (sess->type == SESS_CHANNEL && prefs.hex_gui_win_ucount)
			fe_set_title (sess);
	}
}

/* select a row in the userlist by nick-name */

void
userlist_select (session *sess, char *name)
{
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	GListModel *model;
	guint n_items, i;
	HcUserItem *item;

	if (!sel_model)
		return;

	model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));
	n_items = g_list_model_get_n_items (model);

	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (model, i);
		if (item && item->user && sess->server->p_cmp (item->user->nick, name) == 0)
		{
			/* Toggle selection */
			if (gtk_selection_model_is_selected (sel_model, i))
				gtk_selection_model_unselect_item (sel_model, i);
			else
				gtk_selection_model_select_item (sel_model, i, FALSE);

			g_object_unref (item);
			return;
		}
		if (item)
			g_object_unref (item);
	}
}

char **
userlist_selection_list (GtkWidget *widget, int *num_ret)
{
	/* GTK4: Use the column view version */
	return userlist_selection_list_gtk4 (GTK_COLUMN_VIEW (widget), num_ret);
}

void
fe_userlist_set_selected (struct session *sess)
{
	GListStore *store = sess->res->user_model;
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	GListModel *list_model;
	guint n_items, i;
	HcUserItem *item;

	if (!sel_model)
		return;

	/* Get the underlying model - need to check if it's the same as our store */
	list_model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));

	/* If we're using a sort model, we need to get the underlying store */
	if (GTK_IS_SORT_LIST_MODEL (list_model))
		list_model = gtk_sort_list_model_get_model (GTK_SORT_LIST_MODEL (list_model));

	if (G_LIST_MODEL (store) != list_model)
		return;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (store), i);
		if (item && item->user)
		{
			item->user->selected = gtk_selection_model_is_selected (sel_model, i) ? 1 : 0;
		}
		if (item)
			g_object_unref (item);
	}
}

/*
 * GTK4: Find position of user in GListStore and return selection status
 */
static gboolean
find_row_gtk4 (GListStore *store, struct User *user, guint *position, int *selected,
               GtkSelectionModel *sel_model)
{
	guint n_items, i;
	HcUserItem *item;

	*selected = FALSE;
	n_items = g_list_model_get_n_items (G_LIST_MODEL (store));

	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (store), i);
		if (item && item->user == user)
		{
			*position = i;
			if (sel_model)
				*selected = gtk_selection_model_is_selected (sel_model, i);
			g_object_unref (item);
			return TRUE;
		}
		if (item)
			g_object_unref (item);
	}

	return FALSE;
}

void
userlist_set_value (GtkWidget *view, gfloat val)
{
	/* GTK4: GtkColumnView is inside a GtkScrolledWindow */
	GtkWidget *parent = gtk_widget_get_parent (view);
	if (GTK_IS_SCROLLED_WINDOW (parent))
	{
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (parent));
		gtk_adjustment_set_value (adj, val);
	}
}

gfloat
userlist_get_value (GtkWidget *view)
{
	GtkWidget *parent = gtk_widget_get_parent (view);
	if (GTK_IS_SCROLLED_WINDOW (parent))
	{
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (parent));
		return gtk_adjustment_get_value (adj);
	}
	return 0.0f;
}

int
fe_userlist_remove (session *sess, struct User *user)
{
	GListStore *store = sess->res->user_model;
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	guint position;
	int sel;

	if (!find_row_gtk4 (store, user, &position, &sel, sel_model))
		return 0;

	g_list_store_remove (store, position);

	mg_queue_userlist_update (sess);

	return sel;
}

void
fe_userlist_rehash (session *sess, struct User *user)
{
	GListStore *store = sess->res->user_model;
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	guint position;
	int sel;
	int nick_color = 0;
	HcUserItem *item;

	if (!find_row_gtk4 (store, user, &position, &sel, sel_model))
		return;

	if (prefs.hex_away_track && user->away)
		nick_color = COL_AWAY;
	else if (prefs.hex_gui_ulist_color)
		nick_color = text_color_of(user->nick);

	/* Mutate the item fields in place and emit "changed". Bind
	 * callbacks connected handlers via g_signal_connect_object with the
	 * cell widget as their target — GLib disconnects automatically when
	 * the widget is finalized, so there's no stale-pointer risk. The
	 * handlers also check "hc-bound-item" on the widget so a connection
	 * lingering from a previous bind (same widget, different item)
	 * doesn't clobber the current display. */
	item = g_list_model_get_item (G_LIST_MODEL (store), position);
	if (!item)
		return;

	g_free (item->hostname);
	item->hostname = user->hostname ? g_strdup (user->hostname) : NULL;
	item->color_index = nick_color;
	g_clear_object (&item->icon);
	{
		GdkPixbuf *pix = get_user_icon (sess->server, user);
		if (pix)
			item->icon = gdk_texture_new_for_pixbuf (pix);
	}

	g_signal_emit (item, hc_user_item_changed_signal, 0);

	g_object_unref (item);

	/* Do NOT kick the sorter here. fe_userlist_rehash is only called by
	 * hostname / account / away / realname updates (see userlist.c) —
	 * none of which the sort comparators examine. Access-level changes
	 * (ops/voice promotion) go through fe_userlist_remove +
	 * fe_userlist_insert instead, so the sort model handles those on
	 * insert. Calling gtk_sorter_changed here on every WHO-burst row
	 * caused GtkSortListModel to emit items-changed cascades that
	 * rebound cell widgets — the very flicker we're trying to avoid. */
	mg_queue_userlist_update (sess);
}

void
fe_userlist_insert (session *sess, struct User *newuser, gboolean sel)
{
	GListStore *store = sess->res->user_model;
	GdkPixbuf *pix = get_user_icon (sess->server, newuser);
	HcUserItem *item;
	int nick_color = 0;

	if (prefs.hex_away_track && newuser->away)
		nick_color = COL_AWAY;
	else if (prefs.hex_gui_ulist_color)
		nick_color = text_color_of(newuser->nick);

	/* Always store the clean nick — the nick column bind callback handles
	 * prepending the prefix char when the icon column is hidden. */
	item = hc_user_item_new (newuser->nick, newuser->hostname, newuser, pix, nick_color);
	g_list_store_append (store, item);
	g_object_unref (item);

	/* is it me? */
	if (newuser->me && sess->gui->nick_box)
	{
		if (!sess->gui->is_tab || sess == current_tab)
			mg_set_access_icon (sess->gui, pix, sess->server->is_away);
	}

	/* Select the new item if requested */
	if (sel)
	{
		GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
		GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
		if (sel_model)
		{
			guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
			if (n_items > 0)
				gtk_selection_model_select_item (sel_model, n_items - 1, FALSE);
		}
	}

	mg_queue_userlist_update (sess);
}

void
fe_userlist_clear (session *sess)
{
	g_list_store_remove_all (sess->res->user_model);
}

/*
 * GTK4: File drop handler for userlist - drops file on the selected user
 *
 * Note: GtkColumnView doesn't have get_path_at_pos like GtkTreeView, so we
 * use the currently selected item instead of determining the row under cursor.
 * This is a simpler UX: user selects target, then drops file.
 */
static gboolean
userlist_file_drop_cb (GtkDropTarget *target, const GValue *value,
                       double x, double y, gpointer user_data)
{
	GtkWidget *view = user_data;
	GtkColumnView *column_view = GTK_COLUMN_VIEW (view);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (column_view);
	GFile *file;
	char *uri;
	HcUserItem *item;

	(void)target;
	(void)x;
	(void)y;

	if (!G_VALUE_HOLDS (value, G_TYPE_FILE))
		return FALSE;

	file = g_value_get_object (value);
	if (!file)
		return FALSE;

	/* Get currently selected user */
	item = hc_selection_model_get_selected_item (sel_model);
	if (!item || !item->user)
	{
		if (item)
			g_object_unref (item);
		return FALSE;
	}

	uri = g_file_get_uri (file);
	if (uri)
	{
		mg_dnd_drop_file (current_sess, item->user->nick, uri);
		g_free (uri);
	}

	g_object_unref (item);
	return TRUE;
}

/* GTK4: Signal that we accept drops */
static GdkDragAction
userlist_drop_motion_cb (GtkDropTarget *target, double x, double y, gpointer user_data)
{
	(void)target;
	(void)x;
	(void)y;
	(void)user_data;

	/* Simply indicate we can accept the drop */
	return GDK_ACTION_COPY;
}

/* GTK4: Clear selection when drag leaves */
static void
userlist_drop_leave_cb (GtkDropTarget *target, gpointer user_data)
{
	(void)target;
	(void)user_data;
	/* Nothing needed for GtkColumnView - selection is handled differently */
}

/* Deferred-from-userlist_show: kick the sorter so bind callbacks get a
 * second, fully-settled pass after set_model completes. Needed because
 * the initial set_model can rebind some rows with INVALID position which
 * makes the friend-boundary helper bail before applying its CSS class. */
static gboolean
userlist_kick_sorter_idle (gpointer data)
{
	GtkSorter *sorter = data;
	gtk_sorter_changed (sorter, GTK_SORTER_CHANGE_DIFFERENT);
	g_object_unref (sorter);
	return G_SOURCE_REMOVE;
}

/* Walk a widget subtree and clear friend-boundary / friend / nonfriend
 * classes from every widget whose CSS node is named "row". Called from
 * userlist_show before swapping the model so stale classes from the
 * previous session don't carry over on row-widget reuse. */
static void
userlist_clear_row_classes (GtkWidget *w)
{
	GtkWidget *child;
	const char *name;

	if (!w)
		return;

	name = gtk_widget_get_css_name (w);
	if (name && !strcmp (name, "row"))
	{
		gtk_widget_remove_css_class (w, "poxchat-friend");
		gtk_widget_remove_css_class (w, "poxchat-nonfriend");
	}

	for (child = gtk_widget_get_first_child (w);
	     child != NULL;
	     child = gtk_widget_get_next_sibling (child))
		userlist_clear_row_classes (child);
}

/* Is `hu` a friend on `sess->server`? Dual-keyed against the notify list:
 * nick (case-insensitive, via server collation) OR account
 * (case-sensitive, per IRCv3). */
static inline gboolean
userlist_item_is_friend (session *sess, HcUserItem *hu)
{
	if (!sess || !sess->server || !hu || !hu->user)
		return FALSE;
	return notify_is_in_list (sess->server, hu->user->nick, hu->user->account)
	       ? TRUE : FALSE;
}

/* Primary sort axis: friends first. Returns -1/0/1 where 0 means the
 * friend status is equal and the caller should fall through to the
 * configured nick/ops comparator for tiebreak. Reverse-sort variants
 * still want friends at the top, so they invoke the full composed
 * comparator and negate — but the friend axis itself is NOT reversed
 * below, it's always friends-first. */
static inline int
userlist_friend_cmp (session *sess, HcUserItem *a, HcUserItem *b)
{
	gboolean af = userlist_item_is_friend (sess, a);
	gboolean bf = userlist_item_is_friend (sess, b);
	if (af && !bf) return -1;
	if (!af && bf) return 1;
	return 0;
}

/*
 * GTK4 sorting comparison functions for GtkCustomSorter
 */
static int
userlist_alpha_cmp_gtk4 (gconstpointer a, gconstpointer b, gpointer userdata)
{
	HcUserItem *item_a = HC_USER_ITEM ((gpointer)a);
	HcUserItem *item_b = HC_USER_ITEM ((gpointer)b);
	session *sess = userdata;
	int r = userlist_friend_cmp (sess, item_a, item_b);
	if (r != 0) return r;

	return nick_cmp_alpha (item_a->user, item_b->user, sess->server);
}

static int
userlist_alpha_cmp_gtk4_rev (gconstpointer a, gconstpointer b, gpointer userdata)
{
	HcUserItem *item_a = HC_USER_ITEM ((gpointer)a);
	HcUserItem *item_b = HC_USER_ITEM ((gpointer)b);
	session *sess = userdata;
	int r = userlist_friend_cmp (sess, item_a, item_b);
	if (r != 0) return r;

	/* Reverse only the tiebreak axis; friends stay on top. */
	return -nick_cmp_alpha (item_a->user, item_b->user, sess->server);
}

static int
userlist_ops_cmp_gtk4 (gconstpointer a, gconstpointer b, gpointer userdata)
{
	HcUserItem *item_a = HC_USER_ITEM ((gpointer)a);
	HcUserItem *item_b = HC_USER_ITEM ((gpointer)b);
	session *sess = userdata;
	int r = userlist_friend_cmp (sess, item_a, item_b);
	if (r != 0) return r;

	return nick_cmp_az_ops (sess->server, item_a->user, item_b->user);
}

static int
userlist_ops_cmp_gtk4_rev (gconstpointer a, gconstpointer b, gpointer userdata)
{
	HcUserItem *item_a = HC_USER_ITEM ((gpointer)a);
	HcUserItem *item_b = HC_USER_ITEM ((gpointer)b);
	session *sess = userdata;
	int r = userlist_friend_cmp (sess, item_a, item_b);
	if (r != 0) return r;

	/* "Z-A, ops last": inverts both access ordering (ops go to the
	 * bottom) and alphabetical (Z-A). Negating nick_cmp_az_ops gives
	 * exactly that — the friend-first axis above stays un-negated so
	 * friends still pin to the top regardless of sort direction. */
	return -nick_cmp_az_ops (sess->server, item_a->user, item_b->user);
}

/* Unsorted mode: no alpha or ops ordering, but friends still group at
 * the top. Non-friends preserve insertion order via GtkSortListModel's
 * stable sort on 0-return ties. */
static int
userlist_unsorted_cmp_gtk4 (gconstpointer a, gconstpointer b, gpointer userdata)
{
	HcUserItem *item_a = HC_USER_ITEM ((gpointer)a);
	HcUserItem *item_b = HC_USER_ITEM ((gpointer)b);
	session *sess = userdata;
	return userlist_friend_cmp (sess, item_a, item_b);
}

GListStore *
userlist_create_model (session *sess)
{
	GListStore *store;

	store = g_list_store_new (HC_TYPE_USER_ITEM);

	/* Sorting is handled by GtkSortListModel wrapped around this store
	 * when the view is created. The store itself is unsorted. */

	return store;
}

/*
 * GtkColumnView per-column factory callbacks
 */

/*
 * Helper to update nick label color based on selection state.
 * When selected, clears any nick color so CSS selection color applies.
 * When not selected, applies the user's nick color if set.
 */
static void
userlist_update_nick_color (GtkLabel *nick_label, HcUserItem *user_item, gboolean selected)
{
	if (!nick_label || !user_item)
		return;

	/* When selected, clear attributes so CSS selection color applies.
	 * When not selected, apply nick color if set. */
	if (selected || user_item->color_index <= 0)
	{
		gtk_label_set_attributes (nick_label, NULL);
	}
	else
	{
		GdkRGBA *color = &colors[user_item->color_index];
		PangoAttrList *attrs = pango_attr_list_new ();
		pango_attr_list_insert (attrs, pango_attr_foreground_new (
			(guint16)(color->red * 65535),
			(guint16)(color->green * 65535),
			(guint16)(color->blue * 65535)));
		gtk_label_set_attributes (nick_label, attrs);
		pango_attr_list_unref (attrs);
	}
}

/*
 * Signal handler for selection state changes on nick column items
 */
static void
userlist_nick_selection_changed_cb (GtkListItem *item, GParamSpec *pspec, gpointer user_data)
{
	GtkWidget *nick_label;
	HcUserItem *user_item;
	gboolean selected;

	nick_label = gtk_list_item_get_child (item);
	user_item = gtk_list_item_get_item (item);
	selected = gtk_list_item_get_selected (item);

	if (GTK_IS_LABEL (nick_label) && user_item)
		userlist_update_nick_color (GTK_LABEL (nick_label), user_item, selected);
}

/* "changed" signal callbacks — fired from fe_userlist_rehash after the
 * item's fields are mutated. Each target widget checks "hc-bound-item"
 * on itself to filter out connections from previous binds (same widget,
 * different item that has since been rebound). g_signal_connect_object
 * also auto-disconnects if the widget is finalized. */
static void
userlist_icon_on_item_changed (HcUserItem *item, gpointer data)
{
	GtkWidget *picture = data;
	if (g_object_get_data (G_OBJECT (picture), "hc-bound-item") != item)
		return;
	gtk_picture_set_paintable (GTK_PICTURE (picture),
		item->icon ? GDK_PAINTABLE (item->icon) : NULL);
}

static void
userlist_nick_on_item_changed (HcUserItem *item, gpointer data)
{
	GtkWidget *nick_label = data;
	if (g_object_get_data (G_OBJECT (nick_label), "hc-bound-item") != item)
		return;
	/* FALSE for selected — if actually selected, notify::selected
	 * handler restores the right color. */
	userlist_update_nick_color (GTK_LABEL (nick_label), item, FALSE);
}

static void
userlist_host_on_item_changed (HcUserItem *item, gpointer data)
{
	GtkWidget *host_label = data;
	if (g_object_get_data (G_OBJECT (host_label), "hc-bound-item") != item)
		return;
	gtk_label_set_text (GTK_LABEL (host_label),
		item->hostname ? item->hostname : "");
}

/* Walk up from a cell widget to the GtkColumnView's internal row widget
 * (CSS name "row"). GtkColumnView doesn't expose row widgets publicly,
 * but every list-view row's CSS node is named "row"; matching by css-name
 * is stable across GTK versions. Returns NULL if the row widget isn't
 * found (e.g. during setup before the row is parented). */
static GtkWidget *
userlist_row_from_cell (GtkWidget *cell)
{
	GtkWidget *w = cell;
	while (w)
	{
		const char *name = gtk_widget_get_css_name (w);
		if (name && !strcmp (name, "row"))
			return w;
		w = gtk_widget_get_parent (w);
	}
	return NULL;
}

/* Classify the row as friend or non-friend. Instead of detecting the
 * friend/non-friend boundary in C (which needed the previous row's item
 * and was fragile during session transitions), every row is just tagged
 * with one of `.poxchat-friend` or `.poxchat-nonfriend` based solely on
 * its own item. A CSS adjacent-sibling rule
 *     row.poxchat-friend + row.poxchat-nonfriend { border-top: ... }
 * then draws the rule at every friend→non-friend transition. GTK's CSS
 * engine handles the adjacency detection, so bind-time state races over
 * neighbor positions can't miss the boundary. */
static void
userlist_mark_friend_boundary (GtkWidget *view, GtkWidget *cell,
                               HcUserItem *hu)
{
	GtkWidget *row;
	session *sess;
	gboolean is_friend;

	row = userlist_row_from_cell (cell);
	if (!row)
		return;

	gtk_widget_remove_css_class (row, "poxchat-friend");
	gtk_widget_remove_css_class (row, "poxchat-nonfriend");

	if (!hu || !view)
		return;

	sess = g_object_get_data (G_OBJECT (view), "hc-userlist-sess");
	if (!sess)
		return;

	is_friend = userlist_item_is_friend (sess, hu);
	gtk_widget_add_css_class (row,
		is_friend ? "poxchat-friend" : "poxchat-nonfriend");
}

/*
 * Icon column setup: create a GtkPicture, 16px wide, scale-down
 */
static void
userlist_icon_setup_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *picture;

	picture = gtk_picture_new ();
	gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_SCALE_DOWN);
	gtk_widget_set_size_request (picture, 16, -1);
	gtk_list_item_set_child (item, picture);
}

/*
 * Icon column bind: set paintable from user_item->icon
 */
static void
userlist_icon_bind_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *picture = gtk_list_item_get_child (item);
	HcUserItem *user_item = gtk_list_item_get_item (item);

	if (!user_item)
		return;

	if (user_item->icon)
		gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (user_item->icon));
	else
		gtk_picture_set_paintable (GTK_PICTURE (picture), NULL);

	/* Track the currently-bound item so our "changed" callback can
	 * filter stale connections (same widget, previous item). */
	g_object_set_data (G_OBJECT (picture), "hc-bound-item", user_item);
	g_signal_connect_object (user_item, "changed",
		G_CALLBACK (userlist_icon_on_item_changed), picture, G_CONNECT_DEFAULT);

	userlist_mark_friend_boundary (user_data, picture, user_item);
}

/*
 * Nick column setup: create a GtkLabel, xalign=0.0, connect notify::selected
 */
static void
userlist_nick_setup_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *nick_label;

	nick_label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (nick_label), 0.0);
	/* Minimum display of 3 chars: as the pane shrinks, the label exceeds
	 * the viewport and clips (showing real letters), instead of collapsing
	 * to a column of "..." ellipses. Also ensures the column view's
	 * full_width stays > 0 so the internal listview never gets allocated
	 * 0 width (which triggers a bounds.y assertion in GTK4). */
	gtk_label_set_width_chars (GTK_LABEL (nick_label), 3);
	gtk_list_item_set_child (item, nick_label);

	/* Track nick labels so we can toggle ellipsize when host column hides */
	{
		GPtrArray *labels = g_object_get_data (G_OBJECT (user_data), "nick-labels");
		if (labels)
		{
			g_ptr_array_add (labels, nick_label);
			g_object_weak_ref (G_OBJECT (nick_label),
				(GWeakNotify) g_ptr_array_remove, labels);
		}
	}

	/* Connect to selection changes to update nick color */
	g_signal_connect (item, "notify::selected", G_CALLBACK (userlist_nick_selection_changed_cb), NULL);
}

/*
 * Nick column bind: set text (with prefix when icons hidden), attach data, update color.
 * user_data is the GtkColumnView widget.
 */
static void
userlist_nick_bind_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *view = user_data;
	GtkWidget *nick_label = gtk_list_item_get_child (item);
	HcUserItem *user_item = gtk_list_item_get_item (item);
	GtkColumnViewColumn *icon_col;

	if (!user_item)
		return;

	/* When icon column is hidden, prepend the prefix char (+/@/%) to the nick */
	icon_col = g_object_get_data (G_OBJECT (view), "icon-column");
	if (icon_col && !gtk_column_view_column_get_visible (icon_col) && user_item->user)
	{
		char prefix = user_item->user->prefix[0];
		if (prefix && prefix != ' ')
		{
			char *display = g_strdup_printf ("%c%s", prefix, user_item->nick ? user_item->nick : "");
			gtk_label_set_text (GTK_LABEL (nick_label), display);
			g_free (display);
		}
		else
			gtk_label_set_text (GTK_LABEL (nick_label), user_item->nick ? user_item->nick : "");
	}
	else
	{
		gtk_label_set_text (GTK_LABEL (nick_label), user_item->nick ? user_item->nick : "");
	}

	/* Store reference to item on the label for position lookup during click handling */
	g_object_set_data (G_OBJECT (nick_label), "hc-user-item", user_item);

	/* Track bound item + connect to the "changed" signal so rehash
	 * updates flow in without splicing the model. */
	g_object_set_data (G_OBJECT (nick_label), "hc-bound-item", user_item);
	g_signal_connect_object (user_item, "changed",
		G_CALLBACK (userlist_nick_on_item_changed), nick_label, G_CONNECT_DEFAULT);

	/* Set nick color (respects selection state) */
	userlist_update_nick_color (GTK_LABEL (nick_label), user_item, gtk_list_item_get_selected (item));

	userlist_mark_friend_boundary (view, nick_label, user_item);
}

/*
 * Host column setup: create a GtkLabel, xalign=0.0, ellipsize=END
 */
static void
userlist_host_setup_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *host_label;

	host_label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (host_label), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (host_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_size_request (host_label, 0, -1);
	gtk_list_item_set_child (item, host_label);
}

/*
 * Host column bind: set text from user_item->hostname
 */
static void
userlist_host_bind_cb (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *host_label = gtk_list_item_get_child (item);
	HcUserItem *user_item = gtk_list_item_get_item (item);

	if (!user_item)
		return;

	gtk_label_set_text (GTK_LABEL (host_label), user_item->hostname ? user_item->hostname : "");

	g_object_set_data (G_OBJECT (host_label), "hc-bound-item", user_item);
	g_signal_connect_object (user_item, "changed",
		G_CALLBACK (userlist_host_on_item_changed), host_label, G_CONNECT_DEFAULT);

	userlist_mark_friend_boundary (user_data, host_label, user_item);
}

/*
 * GTK4 version of userlist_selection_list
 * Gets selected nicks from GtkColumnView's selection model
 */
static char **
userlist_selection_list_gtk4 (GtkColumnView *view, int *num_ret)
{
	GtkSelectionModel *sel_model;
	GtkBitset *selection;
	GtkBitsetIter iter;
	guint position;
	gboolean valid;
	int num_sel, i;
	char **nicks;
	GListModel *model;
	HcUserItem *item;

	*num_ret = 0;
	sel_model = gtk_column_view_get_model (view);
	if (!sel_model)
		return NULL;

	selection = gtk_selection_model_get_selection (sel_model);
	num_sel = gtk_bitset_get_size (selection);

	if (num_sel < 1)
	{
		gtk_bitset_unref (selection);
		return NULL;
	}

	nicks = g_new (char *, num_sel + 1);

	/* Get the underlying base model (not the sort model) */
	model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));

	i = 0;
	valid = gtk_bitset_iter_init_first (&iter, selection, &position);
	while (valid && i < num_sel)
	{
		item = g_list_model_get_item (model, position);
		if (item && item->user)
		{
			nicks[i] = g_strdup (item->user->nick);
			i++;
		}
		if (item)
			g_object_unref (item);
		valid = gtk_bitset_iter_next (&iter, &position);
	}
	nicks[i] = NULL;
	gtk_bitset_unref (selection);

	*num_ret = i;
	return nicks;
}

/*
 * Helper to find position at coordinates in GtkColumnView
 * Returns the position or GTK_INVALID_LIST_POSITION if not found.
 * Uses gtk_widget_pick to find the widget at coordinates, then walks
 * up the widget tree looking for hc-user-item data attached to nick labels.
 */
static guint
userlist_get_position_at_coords (GtkColumnView *view, double x, double y)
{
	GtkWidget *child, *widget;
	GtkSelectionModel *sel_model;
	GListModel *model;
	guint n_items;
	HcUserItem *item;

	child = gtk_widget_pick (GTK_WIDGET (view), x, y, GTK_PICK_DEFAULT);
	if (!child)
		return GTK_INVALID_LIST_POSITION;

	/* Walk up from picked widget looking for hc-user-item data */
	for (widget = child; widget && widget != GTK_WIDGET (view); widget = gtk_widget_get_parent (widget))
	{
		item = g_object_get_data (G_OBJECT (widget), "hc-user-item");
		if (item)
			goto found;
	}
	return GTK_INVALID_LIST_POSITION;

found:
	sel_model = gtk_column_view_get_model (view);
	if (!sel_model)
		return GTK_INVALID_LIST_POSITION;

	model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));
	n_items = g_list_model_get_n_items (model);

	for (guint i = 0; i < n_items; i++)
	{
		HcUserItem *model_item = g_list_model_get_item (model, i);
		if (model_item == item)
		{
			g_object_unref (model_item);
			return i;
		}
		if (model_item)
			g_object_unref (model_item);
	}
	return GTK_INVALID_LIST_POSITION;
}

/*
 * Left-click handler for userlist: return focus to the input box so the
 * user can keep typing after clicking a nick.  Double-click activation is
 * handled separately via the GtkColumnView "activate" signal — rolling
 * our own n_press == 2 detection here was unreliable because the view's
 * internal selection gesture and the layout-swap GtkDragSource on the
 * same widget race for the event sequence.
 */
static void
userlist_left_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, gpointer userdata)
{
	(void)gesture; (void)n_press; (void)x; (void)y; (void)userdata;

	if (current_sess && current_sess->gui)
		gtk_widget_grab_focus (current_sess->gui->input_box);
}

/*
 * Row activation (double-click or Enter) — runs the per-nick double-click
 * command from hex_gui_ulist_doubleclick.  Using GtkColumnView's own
 * "activate" signal avoids the gesture-racing issues that made a custom
 * double-click detector unreliable.
 */
static void
userlist_activate_cb (GtkColumnView *view, guint position, gpointer user_data)
{
	char **nicks;
	int i;
	(void)position; (void)user_data;

	if (!prefs.hex_gui_ulist_doubleclick[0])
		return;

	nicks = userlist_selection_list_gtk4 (view, &i);
	if (nicks)
	{
		nick_command_parse (current_sess, prefs.hex_gui_ulist_doubleclick, nicks[0],
								  nicks[0]);
		while (i)
		{
			i--;
			g_free (nicks[i]);
		}
		g_free (nicks);
	}

	if (current_sess && current_sess->gui)
		gtk_widget_grab_focus (current_sess->gui->input_box);
}

/*
 * Right-click handler for userlist.
 * Selects the clicked item (if clicking on one), then shows context menu.
 */
static void
userlist_right_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, gpointer userdata)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	GtkColumnView *view = GTK_COLUMN_VIEW (widget);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
	char **nicks;
	int i;
	guint clicked_pos;

	/* Check for multi-selection (Ctrl held) */
	if (state & GDK_CONTROL_MASK)
	{
		/* With Ctrl held, add to selection rather than replacing */
		nicks = userlist_selection_list_gtk4 (view, &i);
		if (nicks && i > 0)
		{
			menu_nickmenu (current_sess, widget, x, y, nicks[0], i);
			while (i)
			{
				i--;
				g_free (nicks[i]);
			}
			g_free (nicks);
		}
		return;
	}

	/* Find which row was clicked */
	clicked_pos = userlist_get_position_at_coords (view, x, y);

	if (clicked_pos != GTK_INVALID_LIST_POSITION)
	{
		/* Select the clicked item (standard behavior: right-click selects) */
		gtk_selection_model_select_item (sel_model, clicked_pos, TRUE);
	}

	/* Now get the selection and show menu */
	nicks = userlist_selection_list_gtk4 (view, &i);
	if (nicks && i > 0)
	{
		menu_nickmenu (current_sess, widget, x, y, nicks[0], i);
		while (i)
		{
			i--;
			g_free (nicks[i]);
		}
		g_free (nicks);
	}
	else if (clicked_pos == GTK_INVALID_LIST_POSITION)
	{
		/* Clicked on empty area - clear selection */
		gtk_selection_model_unselect_all (sel_model);
	}
}

/*
 * Key handler for userlist - forwards printable keys to input box
 */
static gboolean
userlist_key_cb (GtkEventControllerKey *controller, guint keyval,
                 guint keycode, GdkModifierType state, gpointer userdata)
{
	if (keyval >= GDK_KEY_asterisk && keyval <= GDK_KEY_z)
	{
		/* dirty trick to avoid auto-selection */
		SPELL_ENTRY_SET_EDITABLE (current_sess->gui->input_box, FALSE);
		gtk_widget_grab_focus (current_sess->gui->input_box);
		SPELL_ENTRY_SET_EDITABLE (current_sess->gui->input_box, TRUE);
		/* GTK4: Cannot forward events directly, insert the character instead */
		if (keyval >= GDK_KEY_space && keyval <= GDK_KEY_asciitilde)
		{
			char buf[2] = { (char)keyval, 0 };
			int pos = -1;
			SPELL_ENTRY_INSERT (current_sess->gui->input_box, buf, 1, &pos);
		}
		return TRUE;
	}

	return FALSE;
}

/* Detent hint: ask GTK directly what minimum width it will enforce for the
 * column view at the current font. Char-count approximations drift at small
 * fonts because the ratio between 'n' width, Pango's approximate_char_width,
 * and actual label floor varies non-linearly with hinting. */
static int
userlist_view_detent_min (GtkWidget *view)
{
	int min_w = 0;
	gtk_widget_measure (view, GTK_ORIENTATION_HORIZONTAL, -1,
	                    &min_w, NULL, NULL, NULL);
	return min_w;
}

/*
 * GTK4 version: Create GtkColumnView with hidden headers
 */
GtkWidget *
userlist_create (GtkWidget *box)
{
	GtkWidget *sw, *view;
	GtkColumnViewColumn *col;
	GtkDropTarget *drop_target;

	sw = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand (sw, TRUE);
	gtk_widget_set_vexpand (sw, TRUE);
	/* Minimum of 1px (not 0) to avoid triggering a bounds.y assertion in
	 * gtk_list_base_update_adjustments when the list view is allocated 0
	 * width on the right pane. Visually indistinguishable from 0. */
	gtk_widget_set_size_request (sw, 1, -1);
	gtk_box_append (GTK_BOX (box), sw);

	/* Create column view - model set later in userlist_show() */
	view = hc_column_view_new_simple (NULL, GTK_SELECTION_MULTIPLE);
	gtk_widget_set_size_request (view, 1, -1);
	gtk_widget_set_name (view, "poxchat-userlist");

	/* Track nick labels for dynamic ellipsize toggling */
	g_object_set_data_full (G_OBJECT (view), "nick-labels",
		g_ptr_array_new (), (GDestroyNotify) g_ptr_array_unref);
	hc_column_view_hide_headers (GTK_COLUMN_VIEW (view));
	gtk_column_view_set_reorderable (GTK_COLUMN_VIEW (view), FALSE);
	gtk_widget_set_can_focus (view, FALSE);

	/* All bind callbacks receive the view as user_data so they don't need
	 * to gtk_widget_get_ancestor() on the cell — during set_model's
	 * widget recycling the cell isn't always parented into the view tree
	 * yet when bind fires, so the ancestor walk returns NULL and the
	 * friend-boundary helper bails. Passing the view directly works
	 * regardless of the cell's current parent state. */

	/* Icon column — always created, visibility toggled by pref */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), NULL,
		G_CALLBACK (userlist_icon_setup_cb),
		G_CALLBACK (userlist_icon_bind_cb), NULL, view);
	gtk_column_view_column_set_fixed_width (col, 20);
	gtk_column_view_column_set_visible (col, prefs.hex_gui_ulist_icons);
	g_object_set_data (G_OBJECT (view), "icon-column", col);

	/* Nick column — natural width; host is rightmost so host collapses first.
	 * Mark expandable so when the host column is hidden (progressive
	 * collapse on narrow panes), nick grows to fill the remaining
	 * ColumnView width. Otherwise rows only span icon + nick + host
	 * natural widths, leaving empty space at the right edge and
	 * causing the friend/non-friend separator line to end early. */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), NULL,
		G_CALLBACK (userlist_nick_setup_cb),
		G_CALLBACK (userlist_nick_bind_cb), NULL, view);
	gtk_column_view_column_set_expand (col, TRUE);
	g_object_set_data (G_OBJECT (view), "nick-column", col);

	/* Host column - visibility managed by pane resize callback */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), NULL,
		G_CALLBACK (userlist_host_setup_cb),
		G_CALLBACK (userlist_host_bind_cb), NULL, view);
	gtk_column_view_column_set_visible (col, prefs.hex_gui_ulist_show_hosts);
	g_object_set_data (G_OBJECT (view), "host-column", col);

	/* DND: File drops for DCC (drop file on user to send) */
	drop_target = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	g_signal_connect (drop_target, "drop", G_CALLBACK (userlist_file_drop_cb), view);
	g_signal_connect (drop_target, "motion", G_CALLBACK (userlist_drop_motion_cb), view);
	g_signal_connect (drop_target, "leave", G_CALLBACK (userlist_drop_leave_cb), view);
	gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (drop_target));

	/* Layout swapping drag source (drag userlist to reposition) */
	mg_setup_userlist_drag_source (view);

	/* Row activation (double-click or Enter) — view's own signal, immune to
	 * gesture races with the drag source / internal selection handlers. */
	g_signal_connect (view, "activate", G_CALLBACK (userlist_activate_cb), NULL);

	/* Event controllers for click and key events */
	/* Left-click gesture: return focus to the input box after a single click */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 1); /* Left-click only */
		g_signal_connect (gesture, "released", G_CALLBACK (userlist_left_click_cb), NULL);
		gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (gesture));
	}
	/* Right-click gesture for context menu (use "pressed" for immediate response) */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 3); /* Right-click only */
		g_signal_connect (gesture, "pressed", G_CALLBACK (userlist_right_click_cb), NULL);
		gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (gesture));
	}
	hc_add_key_controller (view, G_CALLBACK (userlist_key_cb), NULL, NULL);

	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), view);

	mg_set_detent_min_func (view, userlist_view_detent_min);

	return view;
}

/*
 * Toggle ellipsize on all tracked nick labels.  Called from the pane
 * resize handler so nicks only ellipsize after the host column is hidden.
 */
void
userlist_set_nick_ellipsize (GtkWidget *view, gboolean ellipsize)
{
	GPtrArray *labels = g_object_get_data (G_OBJECT (view), "nick-labels");
	guint i;

	if (!labels)
		return;

	for (i = 0; i < labels->len; i++)
	{
		GtkLabel *label = g_ptr_array_index (labels, i);
		gtk_label_set_ellipsize (label,
			ellipsize ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
	}

	/* Force column view to re-measure now that label minimums changed */
	gtk_widget_queue_resize (view);
}

/*
 * Re-apply text to every bound nick label based on current icon-column
 * visibility. GtkColumnView doesn't rebind cached row children when a
 * column's visibility toggles, so the bind_cb's prefix-prepend logic
 * only runs on rows that are later scrolled/recycled. Without this,
 * hiding the icon column leaves most nicks without the +@% prefix
 * until a channel switch, and re-showing the icon column can leave
 * the stale prefix in place next to the icon.
 */
void
userlist_refresh_nick_labels (GtkWidget *view)
{
	GPtrArray *labels = g_object_get_data (G_OBJECT (view), "nick-labels");
	GtkColumnViewColumn *icon_col;
	gboolean icons_hidden;
	guint i;

	if (!labels)
		return;

	icon_col = g_object_get_data (G_OBJECT (view), "icon-column");
	icons_hidden = icon_col && !gtk_column_view_column_get_visible (icon_col);

	for (i = 0; i < labels->len; i++)
	{
		GtkLabel *label = g_ptr_array_index (labels, i);
		HcUserItem *item = g_object_get_data (G_OBJECT (label), "hc-user-item");
		const char *nick;
		char prefix = 0;

		if (!item)
			continue;
		nick = item->nick ? item->nick : "";

		if (icons_hidden && item->user)
			prefix = item->user->prefix[0];

		if (prefix && prefix != ' ')
		{
			char *display = g_strdup_printf ("%c%s", prefix, nick);
			gtk_label_set_text (label, display);
			g_free (display);
		}
		else
		{
			gtk_label_set_text (label, nick);
		}
	}
}

/*
 * Refresh the .poxchat-friend / .poxchat-nonfriend CSS class on every
 * bound row, based on each row's current item's friend-ness. Does NOT
 * touch the selection model, so GtkColumnView keeps its current size
 * allocation — no re-measure, no paned re-clamp, no host-column flicker.
 *
 * Needed because GtkListView optimizes away bind-cb calls when only
 * positions change, leaving a row's stale friend class in place when
 * the underlying user flipped friend-ness (e.g. account-observed).
 */
void
userlist_refresh_friend_classes (GtkWidget *view)
{
	GPtrArray *labels = g_object_get_data (G_OBJECT (view), "nick-labels");
	guint i;

	if (!labels)
		return;

	for (i = 0; i < labels->len; i++)
	{
		GtkWidget *label = g_ptr_array_index (labels, i);
		HcUserItem *item = g_object_get_data (G_OBJECT (label), "hc-user-item");
		userlist_mark_friend_boundary (view, label, item);
	}
}

/*
 * GTK4: Connect the session's model to the GtkColumnView with sorting
 */
void
userlist_show (session *sess)
{
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GListStore *store = sess->res->user_model;
	GtkSortListModel *sort_model;
	GtkMultiSelection *sel_model;
	GtkCustomSorter *sorter = NULL;
	GCompareDataFunc cmp_func = NULL;

	/* Proactively drop stale friend-boundary classes on any row widgets
	 * we'll be reusing with this session's items. */
	userlist_clear_row_classes (GTK_WIDGET (view));

	/* Determine sort function based on prefs */
	switch (prefs.hex_gui_ulist_sort)
	{
	case 0: cmp_func = userlist_ops_cmp_gtk4; break;
	case 1: cmp_func = userlist_alpha_cmp_gtk4; break;
	case 2: cmp_func = userlist_ops_cmp_gtk4_rev; break;
	case 3: cmp_func = userlist_alpha_cmp_gtk4_rev; break;
	case 4: cmp_func = userlist_unsorted_cmp_gtk4; break;
	}

	if (cmp_func)
		sorter = gtk_custom_sorter_new (cmp_func, sess, NULL);

	/* Create sorted model wrapping the store */
	sort_model = gtk_sort_list_model_new (G_LIST_MODEL (g_object_ref (store)),
	                                       sorter ? GTK_SORTER (sorter) : NULL);

	/* Create multi-selection model for the column view */
	sel_model = gtk_multi_selection_new (G_LIST_MODEL (sort_model));

	/* Stash the session and sorter on the view BEFORE setting the model.
	 *   - gtk_column_view_set_model triggers synchronous bind callbacks,
	 *     which read "hc-userlist-sess" to check friend status against
	 *     the current server. Setting the model first would make those
	 *     binds read the previous session's data (stale friend check,
	 *     boundary line computed against the wrong userlist).
	 *   - fe_notify_friends_changed() reads "hc-userlist-sorter" to
	 *     signal the sorter that comparison results may have changed. */
	g_object_set_data (G_OBJECT (view), "hc-userlist-sess", sess);
	g_object_set_data (G_OBJECT (view), "hc-userlist-sorter", sorter);

	/* Clear the model first, THEN set the new one. Without this, direct
	 * channel-to-channel transitions (A → B where both have friends) let
	 * GtkColumnView reuse row widgets with mid-transition state, and the
	 * friend-boundary bind helper bails out of some rows before applying
	 * its CSS class. A null-first swap forces GTK to fully unbind every
	 * row before rebinding against the new model, so binds see settled
	 * positions and the boundary lands consistently. */
	gtk_column_view_set_model (view, NULL);
	gtk_column_view_set_model (view, GTK_SELECTION_MODEL (sel_model));

	/* Belt-and-braces second pass: kick the sorter from a fresh main-loop
	 * iteration. Handles the rare case where even the null-first swap
	 * leaves one or two rows with transient state. */
	if (sorter)
	{
		g_object_ref (sorter);
		g_idle_add (userlist_kick_sorter_idle, sorter);
	}

	/* We don't unref sel_model/sort_model - the column view takes ownership */
}

/* Synchronously measure the pixel width of the widest nick in sess's
 * user model, using the user_tree's font context. Used by
 * mg_update_userlist_columns so it can run before the ColumnView has
 * bound row labels (otherwise max-nick is 0 on the first paint after a
 * channel switch, which causes a visible column-reshuffle flicker).
 * Also avoids the virtualization bug where only on-screen labels exist. */
int
userlist_measure_max_nick_width (GtkWidget *user_tree, session *sess)
{
	PangoLayout *layout;
	GListModel *model;
	guint i, n;
	int max_w = 0;

	if (!user_tree || !sess || !sess->res || !sess->res->user_model)
		return 0;

	model = G_LIST_MODEL (sess->res->user_model);
	n = g_list_model_get_n_items (model);
	if (n == 0)
		return 0;

	layout = gtk_widget_create_pango_layout (user_tree, NULL);
	for (i = 0; i < n; i++)
	{
		HcUserItem *item = g_list_model_get_item (model, i);
		if (item)
		{
			if (item->nick)
			{
				int w;
				pango_layout_set_text (layout, item->nick, -1);
				pango_layout_get_pixel_size (layout, &w, NULL);
				if (w > max_w)
					max_w = w;
			}
			g_object_unref (item);
		}
	}
	g_object_unref (layout);
	return max_w;
}

void
fe_uselect (session *sess, char *word[], int do_clear, int scroll_to)
{
	GtkColumnView *view = GTK_COLUMN_VIEW (sess->gui->user_tree);
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	GListModel *model;
	guint n_items, i;
	HcUserItem *item;
	int thisname;
	char *name;

	(void)scroll_to; /* TODO: Implement scroll_to for GtkColumnView */

	if (!sel_model)
		return;

	model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));
	n_items = g_list_model_get_n_items (model);

	if (do_clear)
		gtk_selection_model_unselect_all (sel_model);

	for (i = 0; i < n_items; i++)
	{
		if (*word[0])
		{
			item = g_list_model_get_item (model, i);
			if (item && item->user)
			{
				thisname = 0;
				while (*(name = word[thisname++]))
				{
					if (sess->server->p_cmp (item->user->nick, name) == 0)
					{
						gtk_selection_model_select_item (sel_model, i, FALSE);
						break;
					}
				}
			}
			if (item)
				g_object_unref (item);
		}
	}
}

/*
 * Apply preference changes to the userlist without requiring restart.
 * Toggles icon/host column visibility, updates sorter in-place, refreshes counts.
 *
 * Column visibility is a property of the shared view widget, so calling this
 * for any session that shares the view is fine. The sorter is updated in-place
 * on the current model rather than rebuilding the model chain, which avoids
 * the problem of overwriting the current tab's model in tabbed mode.
 */
void
userlist_apply_prefs (session *sess)
{
	GtkWidget *view_widget;
	GtkColumnView *view;
	GtkColumnViewColumn *col;
	GtkSelectionModel *sel_model;
	GListModel *model;

	if (!sess || !sess->gui || !sess->gui->user_tree)
		return;

	view_widget = sess->gui->user_tree;
	if (!GTK_IS_COLUMN_VIEW (view_widget))
		return;

	view = GTK_COLUMN_VIEW (view_widget);

	/* Icon column visibility */
	col = g_object_get_data (G_OBJECT (view), "icon-column");
	if (col)
		gtk_column_view_column_set_visible (col, prefs.hex_gui_ulist_icons);

	/* Host column visibility */
	col = g_object_get_data (G_OBJECT (view), "host-column");
	if (col)
		gtk_column_view_column_set_visible (col, prefs.hex_gui_ulist_show_hosts);

	/* Update sorter in-place on the current model (don't rebuild model chain) */
	sel_model = gtk_column_view_get_model (view);
	if (sel_model)
	{
		model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (sel_model));
		if (GTK_IS_SORT_LIST_MODEL (model))
		{
			GtkCustomSorter *sorter = NULL;
			GCompareDataFunc cmp_func = NULL;

			switch (prefs.hex_gui_ulist_sort)
			{
			case 0: cmp_func = userlist_ops_cmp_gtk4; break;
			case 1: cmp_func = userlist_alpha_cmp_gtk4; break;
			case 2: cmp_func = userlist_ops_cmp_gtk4_rev; break;
			case 3: cmp_func = userlist_alpha_cmp_gtk4_rev; break;
			case 4: cmp_func = userlist_unsorted_cmp_gtk4; break;
			}

			if (cmp_func)
				sorter = gtk_custom_sorter_new (cmp_func, sess, NULL);

			gtk_sort_list_model_set_sorter (GTK_SORT_LIST_MODEL (model),
				sorter ? GTK_SORTER (sorter) : NULL);
		}
	}

	/* Toggle user count label visibility */
	if (sess->gui->namelistinfo)
	{
		GtkWidget *frame = gtk_widget_get_parent (sess->gui->namelistinfo);
		if (frame)
			gtk_widget_set_visible (frame, prefs.hex_gui_ulist_count);
	}
	fe_userlist_numbers (sess);
}
