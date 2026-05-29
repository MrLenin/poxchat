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

/* abstract channel view: tabs or tree or anything you like */

#include <stdlib.h>
#include <string.h>

#include "../common/poxchat.h"
#include "../common/poxchatc.h"
#include "fe-gtk.h"
#include "maingui.h"
#include "gtkutil.h"
#include "chanview.h"

/*
 * HcChanItem: GObject wrapper for chan* used in GListStore.
 * Holds all data previously stored in GtkTreeStore columns.
 * Defined here (before the #includes of chanview-tabs.c and chanview-tree.c)
 * so both implementations can see it.
 */
#define HC_TYPE_CHAN_ITEM (hc_chan_item_get_type())
G_DECLARE_FINAL_TYPE (HcChanItem, hc_chan_item, HC, CHAN_ITEM, GObject)

struct _HcChanItem {
	GObject parent;
	chan *ch;			/* pointer to channel (not owned) */
	GListStore *children;		/* child channels (for servers) */
	gboolean is_server;		/* TRUE if this is a server (can have children) */
	char *name;			/* display name (was COL_NAME) */
	PangoAttrList *attr;		/* text attributes / color (was COL_ATTR) */
	GdkPixbuf *icon;		/* icon pixbuf (was COL_PIXBUF) */
};

G_DEFINE_TYPE (HcChanItem, hc_chan_item, G_TYPE_OBJECT)

static void
hc_chan_item_finalize (GObject *obj)
{
	HcChanItem *item = HC_CHAN_ITEM (obj);
	g_clear_object (&item->children);
	g_free (item->name);
	if (item->attr)
		pango_attr_list_unref (item->attr);
	if (item->icon)
		g_object_unref (item->icon);
	/* Note: item->ch is not owned by us, don't free */
	G_OBJECT_CLASS (hc_chan_item_parent_class)->finalize (obj);
}

static void
hc_chan_item_class_init (HcChanItemClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = hc_chan_item_finalize;
}

static void
hc_chan_item_init (HcChanItem *item)
{
	item->ch = NULL;
	item->children = NULL;
	item->is_server = FALSE;
	item->name = NULL;
	item->attr = NULL;
	item->icon = NULL;
}

static HcChanItem *
hc_chan_item_new (chan *ch, gboolean is_server, const char *name, GdkPixbuf *icon)
{
	HcChanItem *item = g_object_new (HC_TYPE_CHAN_ITEM, NULL);
	item->ch = ch;
	item->is_server = is_server;
	item->name = g_strdup (name);
	item->icon = icon ? g_object_ref (icon) : NULL;
	if (is_server)
		item->children = g_list_store_new (HC_TYPE_CHAN_ITEM);
	return item;
}

struct _chanview
{
	/* impl scratch area */
	char implscratch[sizeof (void *) * 8];

	GListStore *store;	/* root-level HcChanItems */
	int size;			/* number of channels in view */

	GtkWidget *box;	/* the box we destroy when changing implementations */
	chan *focused;		/* currently focused channel */
	int trunc_len;

	/* callbacks */
	void (*cb_focus) (chanview *, chan *, int tag, void *userdata);
	void (*cb_xbutton) (chanview *, chan *, int tag, void *userdata);
	gboolean (*cb_contextmenu) (chanview *, chan *, int tag, void *userdata, GtkWidget *parent, double x, double y);
	int (*cb_compare) (void *a, void *b);

	/* impl */
	void (*func_init) (chanview *);
	void (*func_postinit) (chanview *);
	void *(*func_add) (chanview *, chan *, char *, gboolean);
	void (*func_move_focus) (chanview *, gboolean, int);
	void (*func_change_orientation) (chanview *);
	void (*func_remove) (chan *);
	void (*func_move) (chan *, int delta);
	void (*func_move_family) (chan *, int delta);
	void (*func_focus) (chan *);
	void (*func_set_color) (chan *, PangoAttrList *);
	GtkWidget *(*func_get_label) (chan *);	/* current GtkLabel for this chan, or NULL */
	void (*func_rename) (chan *, char *);
	gboolean (*func_is_collapsed) (chan *);
	chan *(*func_get_parent) (chan *);
	void (*func_cleanup) (chanview *);
	void (*func_update_pane_size) (chanview *, int);

	unsigned int sorted:1;
	unsigned int vertical:1;
	unsigned int use_icons:1;
	unsigned int context_menu_active:1; /* suppress focus during right-click selection */

	/* attention pulse: chans currently breathing their leading glyph (typing or
	 * recent activity), animated on a single frame-tick attached to cv->box
	 * (which is stable across impl/orientation switches) */
	GSList *pulse_chans;		/* (chan *) currently pulsing */
	guint pulse_tick_id;		/* frame-tick callback id on cv->box, 0 = none */
};

struct _chan
{
	chanview *cv;	/* our owner */
	HcChanItem *item;	/* our HcChanItem wrapper in the GListStore */
	void *userdata;	/* session * */
	void *family;		/* server * or null */
	void *impl;	/* togglebutton or null */
	GdkPixbuf *icon;
	short allow_closure;	/* allow it to be closed when it still has children? */
	short tag;
	unsigned int typing:1;	/* someone is typing here — pulse the leading glyph */
	gint64 pulse_until;	/* monotonic deadline for activity-driven pulse, 0 = none */
	GtkWidget *live_label;	/* realized GtkLabel in tree mode (recycled), or NULL */
};

static chan *cv_find_chan_by_number (chanview *cv, int num);
static int cv_find_number_of_chan (chanview *cv, chan *find_ch);
static HcChanItem *chanview_find_parent_item (chanview *cv, void *family, chan *avoid);
static void chanview_pulse_invalidate_labels (chanview *cv);


/* ======= TABS ======= */

#include "chanview-tabs.c"


/* ======= TREE ======= */

#include "chanview-tree.c"


/* ==== ABSTRACT CHANVIEW ==== */

static char *
truncate_tab_name (char *name, int max)
{
	char *buf;

	if (max > 2 && g_utf8_strlen (name, -1) > max)
	{
		/* truncate long channel names */
		buf = g_malloc (strlen (name) + 4);
		g_utf8_strncpy (buf, name, max);
		strcat (buf, "..");
		return buf;
	}

	return name;
}

/* iterate through the GListStore, into 1 depth of children */

static void
model_foreach_1 (GListStore *store, void (*func)(void *, HcChanItem *),
					  void *userdata)
{
	guint n_items, n_children, i, j;
	HcChanItem *item, *child;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (store), i);
		if (!item)
			continue;

		func (userdata, item);

		if (item->children)
		{
			n_children = g_list_model_get_n_items (G_LIST_MODEL (item->children));
			for (j = 0; j < n_children; j++)
			{
				child = g_list_model_get_item (G_LIST_MODEL (item->children), j);
				if (child)
				{
					func (userdata, child);
					g_object_unref (child);
				}
			}
		}

		g_object_unref (item);
	}
}

static void
chanview_pop_cb (chanview *cv, HcChanItem *item)
{
	chan *ch = item->ch;

	ch->impl = cv->func_add (cv, ch, item->name, FALSE);
	if (item->attr)
	{
		cv->func_set_color (ch, item->attr);
	}
}

static void
chanview_populate (chanview *cv)
{
	model_foreach_1 (cv->store, (void *)chanview_pop_cb, cv);
}

void
chanview_set_impl (chanview *cv, int type)
{
	/* cleanup the old one */
	if (cv->func_cleanup)
		cv->func_cleanup (cv);

	/* the old impl's label widgets are about to be destroyed; drop any cached
	 * pointers so the (cv->box-anchored) pulse tick doesn't touch freed labels
	 * during the rebuild. The pulse re-attaches to the new widgets as they bind. */
	chanview_pulse_invalidate_labels (cv);

	switch (type)
	{
	case 0:
		cv->func_init = cv_tabs_init;
		cv->func_postinit = cv_tabs_postinit;
		cv->func_add = cv_tabs_add;
		cv->func_move_focus = cv_tabs_move_focus;
		cv->func_change_orientation = cv_tabs_change_orientation;
		cv->func_remove = cv_tabs_remove;
		cv->func_move = cv_tabs_move;
		cv->func_move_family = cv_tabs_move_family;
		cv->func_focus = cv_tabs_focus;
		cv->func_set_color = cv_tabs_set_color;
		cv->func_get_label = cv_tabs_get_label;
		cv->func_rename = cv_tabs_rename;
		cv->func_is_collapsed = cv_tabs_is_collapsed;
		cv->func_get_parent = cv_tabs_get_parent;
		cv->func_cleanup = cv_tabs_cleanup;
		cv->func_update_pane_size = NULL;
		break;

	default:
		cv->func_init = cv_tree_init;
		cv->func_postinit = cv_tree_postinit;
		cv->func_add = cv_tree_add;
		cv->func_move_focus = cv_tree_move_focus;
		cv->func_change_orientation = cv_tree_change_orientation;
		cv->func_remove = cv_tree_remove;
		cv->func_move = cv_tree_move;
		cv->func_move_family = cv_tree_move_family;
		cv->func_focus = cv_tree_focus;
		cv->func_set_color = cv_tree_set_color;
		cv->func_get_label = cv_tree_get_label;
		cv->func_rename = cv_tree_rename;
		cv->func_is_collapsed = cv_tree_is_collapsed;
		cv->func_get_parent = cv_tree_get_parent;
		cv->func_cleanup = cv_tree_cleanup;
		cv->func_update_pane_size = cv_tree_update_pane_size;
		break;
	}

	/* now rebuild a new tabbar or tree */
	cv->func_init (cv);

	chanview_populate (cv);

	cv->func_postinit (cv);

	/* force re-focus */
	if (cv->focused)
		cv->func_focus (cv->focused);
}

static void
chanview_free_ch (chanview *cv, HcChanItem *item)
{
	if (item->ch)
		g_free (item->ch);
}

static void
chanview_destroy_store (chanview *cv)	/* free every (chan *) in the store */
{
	model_foreach_1 (cv->store, (void *)chanview_free_ch, cv);
	g_list_store_remove_all (cv->store);
	g_object_unref (cv->store);
}

static void
chanview_destroy (chanview *cv)
{
	if (cv->func_cleanup)
		cv->func_cleanup (cv);

	if (cv->box)
		hc_widget_destroy_impl (GTK_WIDGET (cv->box));

	/* box is gone (its tick callback with it); just drop the bookkeeping */
	g_slist_free (cv->pulse_chans);
	cv->pulse_chans = NULL;
	cv->pulse_tick_id = 0;

	chanview_destroy_store (cv);
	g_free (cv);
}

static void
chanview_box_destroy_cb (GtkWidget *box, chanview *cv)
{
	cv->box = NULL;
	chanview_destroy (cv);
}

chanview *
chanview_new (int type, int trunc_len, gboolean sort, gboolean use_icons)
{
	chanview *cv;

	cv = g_new0 (chanview, 1);
	cv->store = g_list_store_new (HC_TYPE_CHAN_ITEM);
	cv->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand (cv->box, TRUE);
	gtk_widget_set_vexpand (cv->box, TRUE);
	cv->trunc_len = trunc_len;
	cv->sorted = sort;
	cv->use_icons = use_icons;
	chanview_set_impl (cv, type);

	g_signal_connect (G_OBJECT (cv->box), "destroy",
							G_CALLBACK (chanview_box_destroy_cb), cv);

	return cv;
}

/* too lazy for signals */

void
chanview_set_callbacks (chanview *cv,
	void (*cb_focus) (chanview *, chan *, int tag, void *userdata),
	void (*cb_xbutton) (chanview *, chan *, int tag, void *userdata),
	gboolean (*cb_contextmenu) (chanview *, chan *, int tag, void *userdata, GtkWidget *parent, double x, double y),
	int (*cb_compare) (void *a, void *b))
{
	cv->cb_focus = cb_focus;
	cv->cb_xbutton = cb_xbutton;
	cv->cb_contextmenu = cb_contextmenu;
	cv->cb_compare = cb_compare;
}

/* find a place to insert this new entry in a GListStore, based on the compare function.
 * Returns the insertion position. */

static guint
chanview_find_sorted_position (chanview *cv, GListStore *store, void *ud)
{
	guint n_items, i;
	HcChanItem *item;

	if (!cv->sorted)
		return g_list_model_get_n_items (G_LIST_MODEL (store));

	n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (store), i);
		if (item)
		{
			if (item->ch && item->ch->tag == 0 && cv->cb_compare (item->ch->userdata, ud) > 0)
			{
				g_object_unref (item);
				return i;
			}
			g_object_unref (item);
		}
	}

	return n_items;
}

/* find a parent HcChanItem (server) with the same "family" pointer */

static HcChanItem *
chanview_find_parent_item (chanview *cv, void *family, chan *avoid)
{
	guint n_items, i;
	HcChanItem *item;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (cv->store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (cv->store), i);
		if (item)
		{
			if (item->ch && family == item->ch->family && item->ch != avoid)
			{
				/* Return with ref - caller must unref */
				return item;
			}
			g_object_unref (item);
		}
	}

	return NULL;
}

static chan *
chanview_add_real (chanview *cv, char *name, void *family, void *userdata,
						 gboolean allow_closure, int tag, GdkPixbuf *icon,
						 chan *ch, chan *avoid)
{
	HcChanItem *parent_item;
	HcChanItem *new_item;
	gboolean has_parent = FALSE;

	parent_item = chanview_find_parent_item (cv, family, avoid);

	if (!ch)
	{
		ch = g_new0 (chan, 1);
		ch->userdata = userdata;
		ch->family = family;
		ch->cv = cv;
		ch->allow_closure = allow_closure;
		ch->tag = tag;
		ch->icon = icon ? g_object_ref (icon) : NULL;
	}

	if (parent_item)
	{
		/* Insert as child of parent server */
		guint pos = chanview_find_sorted_position (cv, parent_item->children, userdata);
		new_item = hc_chan_item_new (ch, FALSE, name, icon);
		g_list_store_insert (parent_item->children, pos, new_item);
		has_parent = TRUE;
		g_object_unref (parent_item);
	}
	else
	{
		/* Insert at root level as a server */
		new_item = hc_chan_item_new (ch, TRUE, name, icon);
		g_list_store_append (cv->store, new_item);
	}

	ch->item = new_item;	/* store reference (item holds a ref from _new) */

	cv->size++;
	ch->impl = cv->func_add (cv, ch, name, has_parent);

	return ch;
}

chan *
chanview_add (chanview *cv, char *name, void *family, void *userdata, gboolean allow_closure, int tag, GdkPixbuf *icon)
{
	char *new_name;
	chan *ret;

	new_name = truncate_tab_name (name, cv->trunc_len);

	ret = chanview_add_real (cv, new_name, family, userdata, allow_closure, tag, icon, NULL, NULL);

	if (new_name != name)
		g_free (new_name);

	return ret;
}

int
chanview_get_size (chanview *cv)
{
	return cv->size;
}

GtkWidget *
chanview_get_box (chanview *cv)
{
	return cv->box;
}

void
chanview_move_focus (chanview *cv, gboolean relative, int num)
{
	cv->func_move_focus (cv, relative, num);
}

GtkOrientation
chanview_get_orientation (chanview *cv)
{
	return (cv->vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
}

void
chanview_set_orientation (chanview *cv, gboolean vertical)
{
	if (vertical != cv->vertical)
	{
		cv->vertical = vertical;
		cv->func_change_orientation (cv);
	}
}

int
chan_get_tag (chan *ch)
{
	return ch->tag;
}

void *
chan_get_userdata (chan *ch)
{
	return ch->userdata;
}

GtkWidget *
chan_get_impl_widget (chan *ch)
{
	return ch->impl;
}

void
chan_focus (chan *ch)
{
	if (ch->cv->focused == ch)
		return;

	ch->cv->func_focus (ch);
}

void
chanview_set_context_menu_active (chanview *cv, gboolean active)
{
	cv->context_menu_active = active ? 1 : 0;
}

void
chanview_restore_focus_selection (chanview *cv)
{
	cv->context_menu_active = 0;
	if (cv->focused)
		cv->func_focus (cv->focused);
}

void
chanview_update_pane_size (chanview *cv, int pane_size)
{
	if (cv->func_update_pane_size)
		cv->func_update_pane_size (cv, pane_size);
}

void
chan_move (chan *ch, int delta)
{
	ch->cv->func_move (ch, delta);
}

void
chan_move_family (chan *ch, int delta)
{
	ch->cv->func_move_family (ch, delta);
}

void
chan_set_color (chan *ch, PangoAttrList *list)
{
	HcChanItem *item = ch->item;
	if (item)
	{
		if (item->attr)
			pango_attr_list_unref (item->attr);
		item->attr = list ? pango_attr_list_ref (list) : NULL;
	}
	ch->cv->func_set_color (ch, list);
}

/* ---- attention pulse ----
 * A single frame-tick on cv->box softly breathes the alpha of a tab's leading
 * glyph ('#' for channels, '@' for queries) on top of whatever activity colour
 * the tab already shows. Two things arm it: someone typing here (pulses while
 * active), and fresh message activity (pulses for a few seconds after the last
 * line, then settles). The pulse rides a separate visual axis from the tab
 * colour, so it never clobbers the unread / highlight state. */

#define CV_PULSE_PERIOD_US	1300000		/* one full breath, microseconds */
#define CV_PULSE_MIN_ALPHA	0.30		/* dimmest the glyph fades to */
#define CV_PULSE_ACTIVITY_US	4000000		/* keep breathing this long after a line */

static gboolean
chan_wants_pulse (chan *ch, gint64 now)
{
	return ch->typing || (ch->pulse_until != 0 && now < ch->pulse_until);
}

/* drop the pulsing alpha, leaving just the tab's base colour */
static void
chan_pulse_restore (chan *ch)
{
	GtkWidget *lbl = ch->cv->func_get_label ? ch->cv->func_get_label (ch) : NULL;
	if (lbl && GTK_IS_LABEL (lbl))
		gtk_label_set_attributes (GTK_LABEL (lbl), ch->item ? ch->item->attr : NULL);
}

static gboolean
chanview_pulse_tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	chanview *cv = data;
	GSList *l;
	gint64 now;
	double phase, s;
	guint16 alpha;
	(void)widget;

	now = gdk_frame_clock_get_frame_time (clock);
	phase = (double)(now % CV_PULSE_PERIOD_US) / (double)CV_PULSE_PERIOD_US;	/* 0..1 */
	/* smooth triangle breathe (no libm): fold 0..1 to a 0->1->0 ramp, then
	 * smoothstep it so the brightness eases in and out at the extremes */
	s = 2.0 * phase;
	if (s > 1.0)
		s = 2.0 - s;
	s = s * s * (3.0 - 2.0 * s);
	alpha = (guint16)((CV_PULSE_MIN_ALPHA + (1.0 - CV_PULSE_MIN_ALPHA) * s) * 65535.0);

	l = cv->pulse_chans;
	while (l)
	{
		GSList *next = l->next;
		chan *ch = l->data;

		if (!chan_wants_pulse (ch, now))
		{
			/* typing stopped and activity lapsed — settle to the base colour */
			chan_pulse_restore (ch);
			ch->pulse_until = 0;
			cv->pulse_chans = g_slist_remove (cv->pulse_chans, ch);
		}
		else
		{
			GtkWidget *lbl = cv->func_get_label ? cv->func_get_label (ch) : NULL;
			const char *txt;

			if (lbl && GTK_IS_LABEL (lbl)
				&& (txt = gtk_label_get_text (GTK_LABEL (lbl))) != NULL && *txt)
			{
				/* base colour + a fading alpha on just the first UTF-8 glyph */
				PangoAttrList *comp = (ch->item && ch->item->attr)
					? pango_attr_list_copy (ch->item->attr)
					: pango_attr_list_new ();
				PangoAttribute *a = pango_attr_foreground_alpha_new (alpha);
				a->start_index = 0;
				a->end_index = (guint)(g_utf8_next_char (txt) - txt);
				pango_attr_list_insert (comp, a);
				gtk_label_set_attributes (GTK_LABEL (lbl), comp);
				pango_attr_list_unref (comp);
			}
		}
		l = next;
	}

	if (!cv->pulse_chans)
	{
		cv->pulse_tick_id = 0;
		return G_SOURCE_REMOVE;		/* nothing left to animate */
	}
	return G_SOURCE_CONTINUE;
}

/* arm the pulse for this chan and make sure the frame-tick is running */
static void
chanview_pulse_ensure (chan *ch)
{
	chanview *cv = ch->cv;
	if (!g_slist_find (cv->pulse_chans, ch))
		cv->pulse_chans = g_slist_prepend (cv->pulse_chans, ch);
	if (cv->pulse_tick_id == 0 && cv->box)
		cv->pulse_tick_id = gtk_widget_add_tick_callback (cv->box,
							chanview_pulse_tick_cb, cv, NULL);
}

/* remove a chan from the pulse entirely (e.g. its tab is being destroyed) */
static void
chanview_pulse_forget (chan *ch)
{
	chanview *cv = ch->cv;
	ch->typing = 0;
	ch->pulse_until = 0;
	cv->pulse_chans = g_slist_remove (cv->pulse_chans, ch);
	if (!cv->pulse_chans && cv->pulse_tick_id != 0 && cv->box)
	{
		gtk_widget_remove_tick_callback (cv->box, cv->pulse_tick_id);
		cv->pulse_tick_id = 0;
	}
}

/* impl/orientation switch is about to destroy the label widgets — forget the
 * cached pointers (tree mode); the pulse re-attaches as new rows bind */
static void
chanview_pulse_invalidate_labels (chanview *cv)
{
	GSList *l;
	for (l = cv->pulse_chans; l; l = l->next)
		((chan *)l->data)->live_label = NULL;
}

void
chan_set_typing (chan *ch, gboolean typing)
{
	if (!ch)
		return;
	typing = !!typing;
	if (ch->typing == (unsigned int)typing)
		return;
	ch->typing = typing;
	if (typing)
		chanview_pulse_ensure (ch);
	/* when typing stops, the tick prunes it next frame and restores the base
	 * colour — unless message activity is still keeping it alive */
}

void
chan_pulse_activity (chan *ch, gboolean on)
{
	if (!ch)
		return;
	if (on)
	{
		ch->pulse_until = g_get_monotonic_time () + CV_PULSE_ACTIVITY_US;
		chanview_pulse_ensure (ch);
	}
	else
	{
		/* tab was read — let the activity pulse lapse (tick prunes it) */
		ch->pulse_until = 0;
	}
}

void
chan_rename (chan *ch, char *name, int trunc_len)
{
	char *new_name;
	HcChanItem *item = ch->item;

	new_name = truncate_tab_name (name, trunc_len);

	if (item)
	{
		g_free (item->name);
		item->name = g_strdup (new_name);
	}
	ch->cv->func_rename (ch, new_name);
	ch->cv->trunc_len = trunc_len;

	if (new_name != name)
		g_free (new_name);
}

void
chan_set_icon (chan *ch, GdkPixbuf *icon)
{
	HcChanItem *item = ch->item;

	if (ch->icon)
		g_object_unref (ch->icon);
	ch->icon = icon ? g_object_ref (icon) : NULL;

	if (item)
	{
		if (item->icon)
			g_object_unref (item->icon);
		item->icon = icon ? g_object_ref (icon) : NULL;
	}

	if (!ch->cv->use_icons)
		return;

	/* Trigger rebind so GTK4 list view picks up the new icon */
	ch->cv->func_rename (ch, item ? item->name : "");
}

/* this thing is overly complicated */

static int
cv_find_number_of_chan (chanview *cv, chan *find_ch)
{
	guint n_items, n_children, i, j;
	HcChanItem *item, *child;
	int num = 0;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (cv->store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (cv->store), i);
		if (!item)
			continue;

		if (item->ch == find_ch)
		{
			g_object_unref (item);
			return num;
		}
		num++;

		if (item->children)
		{
			n_children = g_list_model_get_n_items (G_LIST_MODEL (item->children));
			for (j = 0; j < n_children; j++)
			{
				child = g_list_model_get_item (G_LIST_MODEL (item->children), j);
				if (child)
				{
					if (child->ch == find_ch)
					{
						g_object_unref (child);
						g_object_unref (item);
						return num;
					}
					num++;
					g_object_unref (child);
				}
			}
		}

		g_object_unref (item);
	}

	return 0;	/* WARNING */
}

/* this thing is overly complicated too */

static chan *
cv_find_chan_by_number (chanview *cv, int num)
{
	guint n_items, n_children, i, j;
	HcChanItem *item, *child;
	int count = 0;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (cv->store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (cv->store), i);
		if (!item)
			continue;

		if (count == num)
		{
			chan *ch = item->ch;
			g_object_unref (item);
			return ch;
		}
		count++;

		if (item->children)
		{
			n_children = g_list_model_get_n_items (G_LIST_MODEL (item->children));
			for (j = 0; j < n_children; j++)
			{
				child = g_list_model_get_item (G_LIST_MODEL (item->children), j);
				if (child)
				{
					if (count == num)
					{
						chan *ch = child->ch;
						g_object_unref (child);
						g_object_unref (item);
						return ch;
					}
					count++;
					g_object_unref (child);
				}
			}
		}

		g_object_unref (item);
	}

	return NULL;
}

static void
chan_emancipate_children (chan *ch)
{
	HcChanItem *item = ch->item;
	HcChanItem *child_item;
	chan *childch;
	char *name;
	PangoAttrList *attr;

	if (!item || !item->children)
		return;

	while (g_list_model_get_n_items (G_LIST_MODEL (item->children)) > 0)
	{
		child_item = g_list_model_get_item (G_LIST_MODEL (item->children), 0);
		if (!child_item)
			break;

		childch = child_item->ch;
		name = g_strdup (child_item->name);
		attr = child_item->attr ? pango_attr_list_ref (child_item->attr) : NULL;

		/* Remove from impl and from children store */
		ch->cv->func_remove (childch);
		childch->item = NULL;
		g_list_store_remove (item->children, 0);
		ch->cv->size--;

		/* Re-add, avoiding using "ch" as parent */
		chanview_add_real (childch->cv, name, childch->family, childch->userdata,
		                   childch->allow_closure, childch->tag, childch->icon, childch, ch);
		if (attr)
		{
			childch->cv->func_set_color (childch, attr);
			pango_attr_list_unref (attr);
		}
		g_free (name);
		g_object_unref (child_item);
	}
}

gboolean
chan_remove (chan *ch, gboolean force)
{
	chan *new_ch;
	extern int poxchat_is_quitting;

	if (poxchat_is_quitting)	/* avoid lots of looping on exit */
		return TRUE;

	/* is this ch allowed to be closed while still having children? */
	if (!force && ch->item && ch->item->children &&
		 g_list_model_get_n_items (G_LIST_MODEL (ch->item->children)) > 0 &&
		 !ch->allow_closure)
		return FALSE;

	/* drop it from the pulse list before the chan is freed */
	if (ch->typing || ch->pulse_until)
		chanview_pulse_forget (ch);

	chan_emancipate_children (ch);
	ch->cv->func_remove (ch);

	/* is it the focused one? */
	if (ch->cv->focused == ch)
	{
		HcChanItem *item = ch->item;
		GListStore *store;
		guint pos;
		new_ch = NULL;

		/* Figure out which store this item is in */
		if (item && !item->is_server)
		{
			/* It's a child - find parent's children store */
			HcChanItem *parent_item = chanview_find_parent_item (ch->cv, ch->family, NULL);
			if (parent_item && parent_item->children && g_list_store_find (parent_item->children, item, &pos))
			{
				store = parent_item->children;
				/* Try previous sibling */
				if (pos > 0)
				{
					HcChanItem *sib = g_list_model_get_item (G_LIST_MODEL (store), pos - 1);
					if (sib)
					{
						new_ch = sib->ch;
						g_object_unref (sib);
					}
				}
				/* Try next sibling */
				if (!new_ch && pos + 1 < g_list_model_get_n_items (G_LIST_MODEL (store)))
				{
					HcChanItem *sib = g_list_model_get_item (G_LIST_MODEL (store), pos + 1);
					if (sib)
					{
						new_ch = sib->ch;
						g_object_unref (sib);
					}
				}
				/* Try parent */
				if (!new_ch)
					new_ch = parent_item->ch;

				g_object_unref (parent_item);
			}
			else
			{
				if (parent_item)
					g_object_unref (parent_item);
			}
		}
		else if (item && g_list_store_find (ch->cv->store, item, &pos))
		{
			store = ch->cv->store;
			/* Try previous sibling */
			if (pos > 0)
			{
				HcChanItem *sib = g_list_model_get_item (G_LIST_MODEL (store), pos - 1);
				if (sib)
				{
					new_ch = sib->ch;
					g_object_unref (sib);
				}
			}
			/* Try next sibling */
			if (!new_ch && pos + 1 < g_list_model_get_n_items (G_LIST_MODEL (store)))
			{
				HcChanItem *sib = g_list_model_get_item (G_LIST_MODEL (store), pos + 1);
				if (sib)
				{
					new_ch = sib->ch;
					g_object_unref (sib);
				}
			}
		}

		if (new_ch)
			chan_focus (new_ch);
	}

	/* Remove from the GListStore */
	if (ch->item)
	{
		guint pos;
		if (!ch->item->is_server)
		{
			HcChanItem *parent_item = chanview_find_parent_item (ch->cv, ch->family, NULL);
			if (parent_item && parent_item->children)
			{
				if (g_list_store_find (parent_item->children, ch->item, &pos))
					g_list_store_remove (parent_item->children, pos);
				g_object_unref (parent_item);
			}
			else
			{
				if (parent_item)
					g_object_unref (parent_item);
				/* Fallback: try root store */
				if (g_list_store_find (ch->cv->store, ch->item, &pos))
					g_list_store_remove (ch->cv->store, pos);
			}
		}
		else
		{
			if (g_list_store_find (ch->cv->store, ch->item, &pos))
				g_list_store_remove (ch->cv->store, pos);
		}
		g_object_unref (ch->item);
	}

	ch->cv->size--;
	g_free (ch);
	return TRUE;
}

gboolean
chan_is_collapsed (chan *ch)
{
	return ch->cv->func_is_collapsed (ch);
}

chan *
chan_get_parent (chan *ch)
{
	return ch->cv->func_get_parent (ch);
}
