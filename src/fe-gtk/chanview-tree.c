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

/* file included in chanview.c */

/*
 * =============================================================================
 * GTK4: GtkListView with GtkTreeListModel for hierarchical display
 * =============================================================================
 *
 * In GTK4, we use GtkTreeListModel which wraps a flat GListModel and adds
 * hierarchy via a callback that returns child models. This is displayed
 * in a GtkListView with GtkTreeExpander widgets for expand/collapse UI.
 *
 * The backing data is managed by chanview.c using GListStore (with HcChanItem
 * objects). The tree view uses cv->store directly as its root model.
 */

typedef struct
{
	GtkListView *view;
	GtkWidget *scrollw;		/* scrolledWindow */
	GtkTreeListModel *tree_model;	/* hierarchical model wrapper */
} treeview;

#include <gdk/gdk.h>

/* Forward declarations */
static chan *cv_tree_get_parent (chan *ch);
static void cv_tree_focus (chan *ch);

/* Width of "nn" + U+2026 HORIZONTAL ELLIPSIS — the minimum ellipsized
 * label state (2 chars + ellipsis). Used by both the detent hint and
 * the progressive-collapse thresholds so they stay coherent: a pure
 * char-count heuristic like 4*char_w("n") under-reports by a few pixels
 * at some font sizes because Pango's integer rounding doesn't track
 * glyph widths in real labels. */
static int
cv_tree_label_min_w (GtkWidget *view)
{
	PangoLayout *layout;
	int w = 0;
	layout = gtk_widget_create_pango_layout (view, "nn\xe2\x80\xa6");
	if (layout)
	{
		pango_layout_get_pixel_size (layout, &w, NULL);
		g_object_unref (layout);
	}
	if (w <= 0)
		w = 4 * hc_widget_char_width (view);
	return w;
}

/* Row chrome when fully collapsed: expander arrow + row padding +
 * label internal metrics + 1px slack for Pango rounding. Pixel-fixed
 * because expander/icons don't scale with font. Previously 43, tuned
 * visually against right-side rendering which under-allocated by the
 * paned handle width (12px); left-side and post-fix right-side both
 * render exactly `label_w + this` at detent. */
#define CV_TREE_CHROME_COLLAPSED  31
/* indent_for_depth adds one slot per depth level; hide_indent drops it. */
#define CV_TREE_INDENT_SLOT       16
/* icon slot + indent_for_icon alignment slot; hide_icons drops both. */
#define CV_TREE_ICON_SLOTS        32

/* Detent hint: the smallest useful width for the tree listview. Sits
 * just below the hide_indent threshold (see cv_tree_update_pane_size)
 * so the collapse fires first, then the detent snaps once the tree is
 * flattened to just labels.
 *
 * Binding case is the server/network row: label ellipsises to 2 chars +
 * ellipsis glyph with the expander arrow still visible.
 *
 * Two constraints: (a) detent >= content min, or content clips at snap;
 * (b) snap width (= our return + paned-child margins) < hide_indent
 * threshold, or the hide_indent collapse stage never fires —
 * cv_tree_update_pane_size compares against full pane_size which
 * includes those margins. Because both functions now derive thresholds
 * from the same measured label_w, (b) holds structurally as long as
 * the pane-child margin stays below CV_TREE_INDENT_SLOT. */
static int
cv_tree_detent_min (GtkWidget *view)
{
	return cv_tree_label_min_w (view) + CV_TREE_CHROME_COLLAPSED;
}

/*
 * GTK4: Row activated callback for GtkListView
 * Toggle expansion state of tree rows on double-click
 */
/* Re-select cv->focused after an expand adds children back to the flat
 * model.  Fires on every items-changed but bails quickly when selection
 * is already correct or nothing was added.  Does NOT call cv_tree_focus
 * because that force-expands the parent, which would undo a deliberate
 * collapse. */
static void
cv_tree_items_changed_cb (GListModel *model, guint position, guint removed,
                          guint added, chanview *cv)
{
	treeview *tv = (treeview *)cv;
	GtkSelectionModel *sel_model;
	GtkBitset *selection;
	guint i;

	if (added == 0 || !cv->focused)
		return;

	sel_model = gtk_list_view_get_model (tv->view);
	selection = gtk_selection_model_get_selection (sel_model);
	if (!gtk_bitset_is_empty (selection))
	{
		gtk_bitset_unref (selection);
		return;
	}
	gtk_bitset_unref (selection);

	/* Scan only the newly-added range for the focused channel */
	for (i = position; i < position + added; i++)
	{
		GtkTreeListRow *row = g_list_model_get_item (G_LIST_MODEL (sel_model), i);
		if (row)
		{
			HcChanItem *item = gtk_tree_list_row_get_item (row);
			if (item && item->ch == cv->focused)
			{
				gtk_selection_model_select_item (sel_model, i, TRUE);
				gtk_list_view_scroll_to (tv->view, i, GTK_LIST_SCROLL_NONE, NULL);
				g_object_unref (item);
				g_object_unref (row);
				return;
			}
			if (item)
				g_object_unref (item);
			g_object_unref (row);
		}
	}
}

static void
cv_tree_activated_cb (GtkListView *view, guint position, gpointer data)
{
	GtkTreeListRow *row;
	GtkSelectionModel *sel_model;

	sel_model = gtk_list_view_get_model (view);
	row = g_list_model_get_item (G_LIST_MODEL (sel_model), position);

	if (row && gtk_tree_list_row_is_expandable (row))
	{
		gboolean expanded = gtk_tree_list_row_get_expanded (row);
		gtk_tree_list_row_set_expanded (row, !expanded);
	}

	if (row)
		g_object_unref (row);
}

/*
 * GTK4: Selection changed callback for GtkListView
 */
static void
cv_tree_sel_cb (GtkSelectionModel *sel_model, guint position, guint n_items, chanview *cv)
{
	GtkBitset *selection;
	guint selected_pos;
	GtkTreeListRow *row;
	HcChanItem *item;

	selection = gtk_selection_model_get_selection (sel_model);
	if (gtk_bitset_is_empty (selection))
	{
		gtk_bitset_unref (selection);
		return;
	}

	selected_pos = gtk_bitset_get_nth (selection, 0);
	gtk_bitset_unref (selection);

	row = g_list_model_get_item (G_LIST_MODEL (sel_model), selected_pos);
	if (!row)
		return;

	item = gtk_tree_list_row_get_item (row);
	g_object_unref (row);

	if (item && item->ch && !cv->context_menu_active)
	{
		cv->focused = item->ch;
		cv->cb_focus (cv, item->ch, item->ch->tag, item->ch->userdata);
	}

	if (item)
		g_object_unref (item);
}

/*
 * Tree view click handler (for context menus)
 * Uses GtkGestureClick with different signature
 */

/*
 * Helper to find position at coordinates in GtkListView
 * Returns the position or GTK_INVALID_LIST_POSITION if not found.
 *
 * This uses gtk_widget_pick to find the widget at coordinates, then
 * retrieves the position from the GtkTreeExpander's list row.
 */
static guint
cv_tree_get_position_at_coords (GtkListView *view, double x, double y)
{
	GtkWidget *child;
	GtkWidget *widget;
	GtkTreeListRow *row;
	GtkSelectionModel *sel_model;
	GListModel *model;
	guint n_items, i;

	/* Pick the widget at the given coordinates */
	child = gtk_widget_pick (GTK_WIDGET (view), x, y, GTK_PICK_DEFAULT);
	if (!child)
		return GTK_INVALID_LIST_POSITION;

	/* Walk up to find the GtkTreeExpander which has our data */
	widget = child;
	while (widget != NULL && widget != GTK_WIDGET (view))
	{
		if (GTK_IS_TREE_EXPANDER (widget))
		{
			row = gtk_tree_expander_get_list_row (GTK_TREE_EXPANDER (widget));
			if (row)
			{
				/* Find this row's position in the selection model */
				sel_model = gtk_list_view_get_model (view);
				model = G_LIST_MODEL (sel_model);
				n_items = g_list_model_get_n_items (model);

				for (i = 0; i < n_items; i++)
				{
					GtkTreeListRow *model_row = g_list_model_get_item (model, i);
					if (model_row == row)
					{
						g_object_unref (model_row);
						return i;
					}
					if (model_row)
						g_object_unref (model_row);
				}
			}
		}
		widget = gtk_widget_get_parent (widget);
	}

	return GTK_INVALID_LIST_POSITION;
}

/*
 * Left-click handler - explicitly select the clicked item.
 * GtkListView with GtkTreeExpander doesn't always auto-select properly.
 * We use "released" signal to set selection after GtkListView's internal
 * handling has completed.
 */
static void
cv_tree_left_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, chanview *cv)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	GtkListView *view = GTK_LIST_VIEW (widget);
	GtkSelectionModel *sel_model;
	guint position;

	(void)gesture;
	(void)n_press;

	sel_model = gtk_list_view_get_model (view);

	/* Find which row was clicked */
	position = cv_tree_get_position_at_coords (view, x, y);

	if (position != GTK_INVALID_LIST_POSITION)
	{
		/* Explicitly select this item */
		gtk_selection_model_select_item (sel_model, position, TRUE);
	}

	/* Return focus to input box */
	if (current_sess && current_sess->gui)
		gtk_widget_grab_focus (current_sess->gui->input_box);
}

/*
 * Right-click handler - select clicked item, then show context menu
 */
static void
cv_tree_right_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, chanview *cv)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	GtkListView *view = GTK_LIST_VIEW (widget);
	GtkSelectionModel *sel_model;
	guint clicked_pos;
	GtkTreeListRow *row;
	HcChanItem *item;

	sel_model = gtk_list_view_get_model (view);

	/* Find which row was clicked; if empty space, use focused channel */
	clicked_pos = cv_tree_get_position_at_coords (view, x, y);

	if (clicked_pos != GTK_INVALID_LIST_POSITION)
	{
		row = g_list_model_get_item (G_LIST_MODEL (sel_model), clicked_pos);
		if (!row)
			return;

		item = gtk_tree_list_row_get_item (row);
		g_object_unref (row);

		if (item && item->ch)
		{
			/* Visually select the right-clicked row without switching focus */
			cv->context_menu_active = 1;
			gtk_selection_model_select_item (sel_model, clicked_pos, TRUE);
			cv->cb_contextmenu (cv, item->ch, item->ch->tag, item->ch->userdata, widget, x, y);
		}

		if (item)
			g_object_unref (item);
	}
	else if (cv->focused)
	{
		/* Right-click on empty space: show menu for focused channel */
		cv->cb_contextmenu (cv, cv->focused, cv->focused->tag,
		                    cv->focused->userdata, widget, x, y);
	}
}

/*
 * Scroll event handler for tree view
 * Uses GtkEventControllerScroll with different signature
 */
static gboolean
cv_tree_scroll_event_cb (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
	if (prefs.hex_gui_tab_scrollchans)
	{
		if (dy > 0)
			mg_switch_page (1, 1);
		else if (dy < 0)
			mg_switch_page (1, -1);

		return TRUE;
	}

	return FALSE;
}

/*
 * GTK4: Factory callbacks for GtkListView with GtkTreeExpander
 */

/* Setup callback - create the row widget structure */
static void
cv_tree_factory_setup_cb (GtkListItemFactory *factory, GtkListItem *item, chanview *cv)
{
	treeview *tv = (treeview *)cv;
	GtkWidget *expander, *content_box, *icon, *label;

	/* Tree expander for expand/collapse */
	expander = gtk_tree_expander_new ();
	gtk_widget_set_hexpand (expander, TRUE);

	/* Respect compact state for newly created items. indent_for_icon tracks
	 * the icon-hide threshold (arrow slot collapses with icons);
	 * indent_for_depth tracks the narrower indent-removal threshold. */
	if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv->view), "compact-indent")))
		gtk_tree_expander_set_indent_for_depth (GTK_TREE_EXPANDER (expander), FALSE);
	if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv->view), "compact-icons")))
		gtk_tree_expander_set_indent_for_icon (GTK_TREE_EXPANDER (expander), FALSE);

	/* Track expander for compact mode toggling */
	{
		GPtrArray *expanders = g_object_get_data (G_OBJECT (tv->view), "tree-expanders");
		if (expanders)
		{
			g_ptr_array_add (expanders, expander);
			g_object_weak_ref (G_OBJECT (expander),
				(GWeakNotify) g_ptr_array_remove, expanders);
		}
	}

	/* Create horizontal box to hold icon + label inside expander */
	content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

	/* Icon (only if icons are enabled) */
	if (cv->use_icons)
	{
		icon = gtk_picture_new ();
		gtk_picture_set_content_fit (GTK_PICTURE (icon), GTK_CONTENT_FIT_SCALE_DOWN);

		/* Respect compact icon state for newly created items. We drop the
		 * width request and the paintable together so the GtkPicture's
		 * natural width collapses to 0 — gtk_widget_set_visible(FALSE)
		 * alone doesn't reliably reflow row allocations on Linux. */
		if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv->view), "compact-icons")))
			gtk_widget_set_size_request (icon, 0, -1);
		else
			gtk_widget_set_size_request (icon, 16, -1);

		gtk_box_append (GTK_BOX (content_box), icon);
		g_object_set_data (G_OBJECT (item), "icon", icon);

		/* Track icon for compact mode toggling */
		{
			GPtrArray *icons = g_object_get_data (G_OBJECT (tv->view), "tree-icons");
			if (icons)
			{
				g_ptr_array_add (icons, icon);
				g_object_weak_ref (G_OBJECT (icon),
					(GWeakNotify) g_ptr_array_remove, icons);
			}
		}
	}

	/* Label for channel/server name */
	label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	/* Minimum display of 3 chars: as the pane shrinks, the label exceeds
	 * the viewport and clips (showing real letters), instead of collapsing
	 * to a column of "..." ellipses. Also keeps the list view's reported
	 * min width > 0 so the internal listview never gets allocated 0 width
	 * (which triggers a bounds.y assertion in GTK4). */
	gtk_label_set_width_chars (GTK_LABEL (label), 3);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_box_append (GTK_BOX (content_box), label);

	/* Put content box (icon + label) inside expander */
	gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), content_box);

	gtk_list_item_set_child (item, expander);

	/* Store references for bind callback */
	g_object_set_data (G_OBJECT (item), "expander", expander);
	g_object_set_data (G_OBJECT (item), "label", label);
}

/* Bind callback - populate row with data from HcChanItem fields */
static void
cv_tree_factory_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, chanview *cv)
{
	treeview *tv = (treeview *)cv;
	GtkWidget *expander, *icon, *label;
	GtkTreeListRow *row;
	HcChanItem *chan_item;

	expander = g_object_get_data (G_OBJECT (list_item), "expander");
	icon = g_object_get_data (G_OBJECT (list_item), "icon");
	label = g_object_get_data (G_OBJECT (list_item), "label");

	/* Safety checks for stored widget references */
	if (!expander || !label)
		return;

	row = gtk_list_item_get_item (list_item);
	if (!row)
		return;

	/* Set the tree list row on the expander for expand/collapse functionality */
	gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

	chan_item = gtk_tree_list_row_get_item (row);
	if (!chan_item || !chan_item->ch)
	{
		if (chan_item)
			g_object_unref (chan_item);
		return;
	}

	/* Read data directly from HcChanItem fields */
	gtk_label_set_text (GTK_LABEL (label), chan_item->name ? chan_item->name : "");
	gtk_label_set_attributes (GTK_LABEL (label), chan_item->attr);

	/* Track this row's live label so the typing pulse can find it. The chan is
	 * stashed on the label so unbind can clear the right one (see unbind cb). */
	if (chan_item->ch)
	{
		chan_item->ch->live_label = label;
		g_object_set_data (G_OBJECT (label), "cv-chan", chan_item->ch);
	}

	/* Set icon if we have one. While compact-icons is active, the picture's
	 * paintable must stay NULL and size_request at 0 so the label can occupy
	 * the slot; stash the real paintable under "cached-paintable" so the
	 * update_pane_size restore path can pick it up when we leave compact. */
	if (icon)
	{
		gboolean compact = GPOINTER_TO_INT (
			g_object_get_data (G_OBJECT (tv->view), "compact-icons"));

		if (chan_item->icon)
		{
			GdkTexture *texture = hc_pixbuf_to_texture (chan_item->icon);
			if (compact)
			{
				if (texture)
					g_object_set_data_full (G_OBJECT (icon), "cached-paintable",
					                        g_object_ref (texture), g_object_unref);
				else
					g_object_set_data (G_OBJECT (icon), "cached-paintable", NULL);
				gtk_picture_set_paintable (GTK_PICTURE (icon), NULL);
				gtk_widget_set_size_request (icon, 0, -1);
			}
			else
			{
				gtk_picture_set_paintable (GTK_PICTURE (icon), GDK_PAINTABLE (texture));
				g_object_set_data (G_OBJECT (icon), "cached-paintable", NULL);
				gtk_widget_set_size_request (icon, 16, -1);
			}
			if (texture)
				g_object_unref (texture);
		}
		else
		{
			gtk_picture_set_paintable (GTK_PICTURE (icon), NULL);
			g_object_set_data (G_OBJECT (icon), "cached-paintable", NULL);
			gtk_widget_set_size_request (icon, compact ? 0 : 16, -1);
		}
	}

	g_object_unref (chan_item);
}

/* Unbind callback - cleanup */
static void
cv_tree_factory_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, chanview *cv)
{
	GtkWidget *expander;
	GtkWidget *label;

	if (!list_item)
		return;

	expander = g_object_get_data (G_OBJECT (list_item), "expander");
	if (expander)
		gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);

	/* This label is being recycled to another row — stop the pulse from
	 * touching it on behalf of the chan it used to show. */
	label = g_object_get_data (G_OBJECT (list_item), "label");
	if (label)
	{
		chan *ch = g_object_get_data (G_OBJECT (label), "cv-chan");
		if (ch && ch->live_label == label)
			ch->live_label = NULL;
		g_object_set_data (G_OBJECT (label), "cv-chan", NULL);
	}
}

/*
 * GtkTreeListModel child model callback
 * Returns a GListModel for children of the given item, or NULL if no children.
 */
static GListModel *
cv_tree_create_child_model (gpointer item, gpointer user_data)
{
	HcChanItem *chan_item = HC_CHAN_ITEM (item);

	if (chan_item->is_server && chan_item->children)
	{
		/* Return a reference to the children store */
		return G_LIST_MODEL (g_object_ref (chan_item->children));
	}

	return NULL;
}

static void
cv_tree_init (chanview *cv)
{
	GtkWidget *view, *win;
	GtkListItemFactory *factory;
	GtkTreeListModel *tree_model;
	GtkSingleSelection *sel_model;
	treeview *tv = (treeview *)cv;

	win = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (win),
	                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand (win, TRUE);
	gtk_widget_set_vexpand (win, TRUE);
	/* Minimum of 1px (not 0) to avoid triggering a bounds.y assertion in
	 * gtk_list_base_update_adjustments when the list view is allocated 0
	 * width on the right pane. Visually indistinguishable from 0. */
	gtk_widget_set_size_request (win, 1, -1);
	gtk_box_append (GTK_BOX (cv->box), win);

	/* Use cv->store directly as the root model for GtkTreeListModel */
	tree_model = gtk_tree_list_model_new (
		G_LIST_MODEL (g_object_ref (cv->store)),
		FALSE,	/* passthrough - FALSE means we get GtkTreeListRow items */
		TRUE,	/* autoexpand */
		cv_tree_create_child_model,
		cv,
		NULL);	/* destroy notify */
	tv->tree_model = tree_model;

	/* Create selection model */
	sel_model = gtk_single_selection_new (G_LIST_MODEL (tree_model));
	gtk_single_selection_set_autoselect (sel_model, FALSE);

	/* Create factory for list items */
	factory = gtk_signal_list_item_factory_new ();
	g_signal_connect (factory, "setup", G_CALLBACK (cv_tree_factory_setup_cb), cv);
	g_signal_connect (factory, "bind", G_CALLBACK (cv_tree_factory_bind_cb), cv);
	g_signal_connect (factory, "unbind", G_CALLBACK (cv_tree_factory_unbind_cb), cv);

	/* Create list view */
	view = gtk_list_view_new (GTK_SELECTION_MODEL (sel_model), factory);
	gtk_widget_set_name (view, "poxchat-tree");
	gtk_widget_set_can_focus (view, FALSE);
	gtk_widget_set_size_request (view, 1, -1);

	g_object_set_data (G_OBJECT (view), "chanview-cv", cv);
	mg_set_detent_min_func (view, cv_tree_detent_min);

	/* Connect signals — sel_cb is deferred to cv_tree_postinit so that
	 * chanview_populate doesn't trigger it and override cv->focused. */
	g_signal_connect (view, "activate",
	                  G_CALLBACK (cv_tree_activated_cb), cv);
	g_signal_connect (G_LIST_MODEL (tree_model), "items-changed",
	                  G_CALLBACK (cv_tree_items_changed_cb), cv);

	/* Event controllers */
	/* Left-click gesture for explicit selection (GtkListView doesn't auto-select reliably with GtkTreeExpander)
	 * Use "released" signal so we set selection after GtkListView's internal handling completes */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 1); /* Left-click only */
		g_signal_connect (gesture, "released", G_CALLBACK (cv_tree_left_click_cb), cv);
		gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (gesture));
	}
	/* Right-click gesture for context menu */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 3); /* Right-click only */
		g_signal_connect (gesture, "pressed", G_CALLBACK (cv_tree_right_click_cb), cv);
		gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (gesture));
	}
	hc_add_scroll_controller (view, G_CALLBACK (cv_tree_scroll_event_cb), NULL);

	/* DND - drag source for layout swapping */
	mg_setup_chanview_drag_source (view);

	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (win), view);
	tv->view = GTK_LIST_VIEW (view);
	tv->scrollw = win;

	/* Tracking arrays for progressive collapse (icon hiding, indent removal) */
	if (cv->use_icons)
		g_object_set_data_full (G_OBJECT (view), "tree-icons",
			g_ptr_array_new (), (GDestroyNotify) g_ptr_array_unref);
	g_object_set_data_full (G_OBJECT (view), "tree-expanders",
		g_ptr_array_new (), (GDestroyNotify) g_ptr_array_unref);
}

/* Progressive collapse as the chanview pane shrinks. Thresholds are
 * derived from the measured "nn…" label width plus the chrome for each
 * stage, so they stay coherent with cv_tree_detent_min and with the
 * font's actual pixel metrics (a pure char-count formula like
 * `40 + 4*char_w` misses by a handful of pixels at small fonts, which
 * lets the label clip before hide_indent fires).
 *   - hide_indent fires when the pane can no longer fit the minimum
 *     label + chrome with indent_for_depth still visible.
 *   - hide_icons fires one indent-slot earlier: same budget plus the
 *     icon column + indent_for_icon alignment slot. */
static void
cv_tree_update_pane_size (chanview *cv, int pane_size)
{
	treeview *tv = (treeview *)cv;
	GPtrArray *icons, *expanders;
	gboolean hide_icons, hide_indent;
	gboolean was_hiding_icons, was_hiding_indent;
	int label_w, indent_threshold, icons_threshold;
	guint i;

	label_w = cv_tree_label_min_w (GTK_WIDGET (tv->view));
	indent_threshold = label_w + CV_TREE_CHROME_COLLAPSED + CV_TREE_INDENT_SLOT;
	icons_threshold = indent_threshold + CV_TREE_ICON_SLOTS;

	hide_icons = (pane_size >= 0 && pane_size < icons_threshold);
	hide_indent = (pane_size >= 0 && pane_size < indent_threshold);

	was_hiding_icons = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv->view), "compact-icons"));
	was_hiding_indent = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv->view), "compact-indent"));

	if (hide_icons == was_hiding_icons && hide_indent == was_hiding_indent)
		return;

	g_object_set_data (G_OBJECT (tv->view), "compact-icons", GINT_TO_POINTER (hide_icons));
	g_object_set_data (G_OBJECT (tv->view), "compact-indent", GINT_TO_POINTER (hide_indent));

	/* Collapse / restore the icon slot. set_visible(FALSE) doesn't reclaim
	 * the allocation on Linux, so we drop the paintable (which zeroes the
	 * picture's natural width) and stash it for restore. size_request goes
	 * to 0 in lock-step so both bounds are pinned to 0. GtkListView
	 * virtualises row allocation — our per-icon resize requests don't
	 * propagate through the view's cached row geometry, so we walk up
	 * each icon's parent chain and queue_resize on every ancestor. */
	if (hide_icons != was_hiding_icons && cv->use_icons)
	{
		icons = g_object_get_data (G_OBJECT (tv->view), "tree-icons");
		if (icons)
		{
			for (i = 0; i < icons->len; i++)
			{
				GtkWidget *icon = g_ptr_array_index (icons, i);
				GtkWidget *w;
				if (hide_icons)
				{
					GdkPaintable *p = gtk_picture_get_paintable (GTK_PICTURE (icon));
					if (p)
						g_object_set_data_full (G_OBJECT (icon), "cached-paintable",
						                        g_object_ref (p), g_object_unref);
					gtk_picture_set_paintable (GTK_PICTURE (icon), NULL);
					gtk_widget_set_size_request (icon, 0, -1);
				}
				else
				{
					GdkPaintable *p = g_object_get_data (G_OBJECT (icon), "cached-paintable");
					if (p)
						gtk_picture_set_paintable (GTK_PICTURE (icon), p);
					g_object_set_data (G_OBJECT (icon), "cached-paintable", NULL);
					gtk_widget_set_size_request (icon, 16, -1);
				}
				for (w = icon; w; w = gtk_widget_get_parent (w))
					gtk_widget_queue_resize (w);
			}
		}
	}

	/* Toggle expander indent knobs. indent_for_icon reserves the expand-arrow
	 * slot on leaf rows so they align with parents — it's what actually
	 * pushes the label ~16px to the right, not our GtkPicture. Collapse it
	 * in lock-step with hide_icons so the label reclaims that slot as soon
	 * as the icons go away. indent_for_depth (the per-depth indent) stays
	 * tied to hide_indent since it's a larger shift reserved for the
	 * narrowest stage. */
	if (hide_icons != was_hiding_icons || hide_indent != was_hiding_indent)
	{
		expanders = g_object_get_data (G_OBJECT (tv->view), "tree-expanders");
		if (expanders)
		{
			for (i = 0; i < expanders->len; i++)
			{
				GtkTreeExpander *exp = GTK_TREE_EXPANDER (g_ptr_array_index (expanders, i));
				gtk_tree_expander_set_indent_for_depth (exp, !hide_indent);
				gtk_tree_expander_set_indent_for_icon (exp, !hide_icons);
			}
		}
	}

	gtk_widget_queue_resize (GTK_WIDGET (tv->view));
}

static void
cv_tree_postinit (chanview *cv)
{
	treeview *tv = (treeview *)cv;
	GtkSelectionModel *sel_model = gtk_list_view_get_model (tv->view);

	g_signal_connect (sel_model, "selection-changed",
	                  G_CALLBACK (cv_tree_sel_cb), cv);
}

static void *
cv_tree_add (chanview *cv, chan *ch, char *name, gboolean has_parent)
{
	/* The item was already added to cv->store (or a parent's children store)
	 * by chanview_add_real(). Nothing more to do for the tree view. */
	return NULL;
}

static void
cv_tree_change_orientation (chanview *cv)
{
}

static void
cv_tree_focus (chan *ch)
{
	treeview *tv = (treeview *)ch->cv;
	GtkSelectionModel *sel_model;
	GListModel *model;
	guint n_items, i;
	GtkTreeListRow *row;
	HcChanItem *item;

	sel_model = gtk_list_view_get_model (tv->view);
	model = G_LIST_MODEL (sel_model);
	n_items = g_list_model_get_n_items (model);

	/* Find the row for this channel and select it */
	for (i = 0; i < n_items; i++)
	{
		row = g_list_model_get_item (model, i);
		if (!row)
			continue;

		item = gtk_tree_list_row_get_item (row);

		if (item && item->ch == ch)
		{
			/* Expand parent if this is a child */
			GtkTreeListRow *parent_row = gtk_tree_list_row_get_parent (row);
			if (parent_row)
			{
				gtk_tree_list_row_set_expanded (parent_row, TRUE);
				g_object_unref (parent_row);
			}

			/* Select this row */
			gtk_selection_model_select_item (sel_model, i, TRUE);

			/* Scroll to make it visible - GtkListView handles this automatically
			 * when using gtk_list_view_scroll_to */
			gtk_list_view_scroll_to (tv->view, i, GTK_LIST_SCROLL_SELECT, NULL);

			g_object_unref (item);
			g_object_unref (row);
			return;
		}

		if (item)
			g_object_unref (item);
		g_object_unref (row);
	}
}

static void
cv_tree_move_focus (chanview *cv, gboolean relative, int num)
{
	chan *ch;

	if (relative)
	{
		num += cv_find_number_of_chan (cv, cv->focused);
		num %= cv->size;
		/* make it wrap around at both ends */
		if (num < 0)
			num = cv->size - 1;
	}

	ch = cv_find_chan_by_number (cv, num);
	if (ch)
		cv_tree_focus (ch);
}

static void
cv_tree_remove (chan *ch)
{
	/* Removal from the GListStore is handled by chan_remove() in chanview.c.
	 * Nothing extra needed here for the tree view. */
}

static void
cv_tree_move_item_in_store (GListStore *store, guint old_pos, int delta)
{
	guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
	guint new_pos;
	HcChanItem *item;

	if (n_items < 2)
		return;

	if (delta < 0) /* move down (next) */
	{
		if (old_pos + 1 < n_items)
			new_pos = old_pos + 1;
		else
			new_pos = 0; /* wrap to top */
	}
	else /* move up (prev) */
	{
		if (old_pos > 0)
			new_pos = old_pos - 1;
		else
			new_pos = n_items - 1; /* wrap to bottom */
	}

	item = g_list_model_get_item (G_LIST_MODEL (store), old_pos);
	if (!item)
		return;

	g_object_ref (item); /* extra ref for re-insert */
	g_list_store_remove (store, old_pos);
	g_list_store_insert (store, new_pos, item);
	g_object_unref (item);
	g_object_unref (item); /* drop the get_item ref */
}

static void
cv_tree_move (chan *ch, int delta)
{
	HcChanItem *item = ch->item;
	guint pos;

	if (!item || item->is_server)
		return; /* do nothing for server rows */

	/* Find parent's children store */
	HcChanItem *parent_item = chanview_find_parent_item (ch->cv, ch->family, NULL);
	if (parent_item && parent_item->children)
	{
		if (g_list_store_find (parent_item->children, item, &pos))
			cv_tree_move_item_in_store (parent_item->children, pos, delta);
		g_object_unref (parent_item);
	}
}

static void
cv_tree_move_family (chan *ch, int delta)
{
	HcChanItem *item = ch->item;
	guint pos;

	if (!item)
		return;

	if (g_list_store_find (ch->cv->store, item, &pos))
		cv_tree_move_item_in_store (ch->cv->store, pos, delta);
}

static void
cv_tree_cleanup (chanview *cv)
{
	treeview *tv = (treeview *)cv;

	if (cv->box)
		/* kill the scrolled window */
		hc_widget_destroy_impl (GTK_WIDGET (tv->scrollw));

	tv->tree_model = NULL; /* owned by list view */
	tv->view = NULL;
}

/*
 * Helper to trigger rebind of an item in the chanview tree while preserving selection.
 * The remove/insert pattern clears GtkSingleSelection, so we save and restore it.
 */
static void
cv_tree_rebind_item (chanview *cv, GListStore *store, guint position, HcChanItem *item)
{
	treeview *tv = (treeview *)cv;
	GtkSelectionModel *sel_model;
	GtkBitset *selection;
	guint saved_selection = GTK_INVALID_LIST_POSITION;
	gboolean was_expanded = TRUE;
	gboolean is_server_rebind;

	if (!tv || !tv->view || !store || !item)
		return;

	sel_model = gtk_list_view_get_model (tv->view);
	is_server_rebind = (store == cv->store);

	/* Save current selection */
	if (sel_model)
	{
		selection = gtk_selection_model_get_selection (sel_model);
		if (!gtk_bitset_is_empty (selection))
			saved_selection = gtk_bitset_get_nth (selection, 0);
		gtk_bitset_unref (selection);
	}

	/* For server rows, save expanded state before the remove/insert
	 * cycle.  autoexpand will re-expand the row unconditionally;
	 * we collapse it again afterwards if it was deliberately collapsed. */
	if (is_server_rebind && sel_model)
	{
		guint n = g_list_model_get_n_items (G_LIST_MODEL (sel_model));
		guint j;
		for (j = 0; j < n; j++)
		{
			GtkTreeListRow *row = g_list_model_get_item (G_LIST_MODEL (sel_model), j);
			if (row)
			{
				HcChanItem *ri = gtk_tree_list_row_get_item (row);
				if (ri && ri->ch == item->ch)
				{
					was_expanded = gtk_tree_list_row_get_expanded (row);
					g_object_unref (ri);
					g_object_unref (row);
					break;
				}
				if (ri)
					g_object_unref (ri);
				g_object_unref (row);
			}
		}
	}

	/* Perform the rebind via remove/insert */
	g_list_store_remove (store, position);
	g_list_store_insert (store, position, item);

	/* Restore collapsed state if autoexpand forced it open */
	if (is_server_rebind && !was_expanded && sel_model)
	{
		guint n = g_list_model_get_n_items (G_LIST_MODEL (sel_model));
		guint j;
		for (j = 0; j < n; j++)
		{
			GtkTreeListRow *row = g_list_model_get_item (G_LIST_MODEL (sel_model), j);
			if (row)
			{
				HcChanItem *ri = gtk_tree_list_row_get_item (row);
				if (ri && ri->ch == item->ch)
				{
					gtk_tree_list_row_set_expanded (row, FALSE);
					g_object_unref (ri);
					g_object_unref (row);
					break;
				}
				if (ri)
					g_object_unref (ri);
				g_object_unref (row);
			}
		}
	}

	/* Restore selection if it was valid */
	if (sel_model && saved_selection != GTK_INVALID_LIST_POSITION)
	{
		gtk_selection_model_select_item (sel_model, saved_selection, TRUE);
	}
}

/*
 * Helper to find an HcChanItem in the tree and trigger rebind.
 * Searches both root store and children stores.
 */
static void
cv_tree_rebind_chan (chan *ch)
{
	chanview *cv = ch->cv;
	treeview *tv = (treeview *)cv;
	guint n_items, i;
	HcChanItem *item;
	GListStore *store;
	chan *parent_ch = cv_tree_get_parent (ch);

	(void)tv;

	if (parent_ch)
	{
		/* It's a channel - find in parent's children store */
		HcChanItem *parent_item = chanview_find_parent_item (cv, ch->family, NULL);
		if (parent_item && parent_item->children)
		{
			store = parent_item->children;
			n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
			for (i = 0; i < n_items; i++)
			{
				item = g_list_model_get_item (G_LIST_MODEL (store), i);
				if (item && item->ch == ch)
				{
					cv_tree_rebind_item (cv, store, i, item);
					g_object_unref (item);
					g_object_unref (parent_item);
					return;
				}
				if (item)
					g_object_unref (item);
			}
			g_object_unref (parent_item);
		}
	}
	else
	{
		/* It's a server - find in root store */
		store = cv->store;
		n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
		for (i = 0; i < n_items; i++)
		{
			item = g_list_model_get_item (G_LIST_MODEL (store), i);
			if (item && item->ch == ch)
			{
				cv_tree_rebind_item (cv, store, i, item);
				g_object_unref (item);
				return;
			}
			if (item)
				g_object_unref (item);
		}
	}
}

static void
cv_tree_set_color (chan *ch, PangoAttrList *list)
{
	/* Trigger a rebind to pick up the new color from HcChanItem.
	 * The rebind helper saves and restores selection to prevent it being cleared. */
	cv_tree_rebind_chan (ch);
}

/* Rows are virtualized/recycled, so the live label is tracked via bind/unbind
 * (see cv_tree_factory_*_cb). Returns NULL when this chan's row isn't realized. */
static GtkWidget *
cv_tree_get_label (chan *ch)
{
	return (ch->live_label && GTK_IS_LABEL (ch->live_label)) ? ch->live_label : NULL;
}

static void
cv_tree_rename (chan *ch, char *name)
{
	/* Trigger a rebind to pick up the new name from HcChanItem.
	 * The rebind helper saves and restores selection to prevent it being cleared. */
	cv_tree_rebind_chan (ch);
}

static chan *
cv_tree_get_parent (chan *ch)
{
	HcChanItem *item = ch->item;

	/* If it's a server (root item), it has no parent */
	if (!item || item->is_server)
		return NULL;

	/* Find the parent server item that contains this channel */
	HcChanItem *parent_item = chanview_find_parent_item (ch->cv, ch->family, NULL);
	if (parent_item)
	{
		chan *parent_ch = parent_item->ch;
		g_object_unref (parent_item);
		return parent_ch;
	}

	return NULL;
}

static gboolean
cv_tree_is_collapsed (chan *ch)
{
	chan *parent_ch = cv_tree_get_parent (ch);
	treeview *tv;
	GtkSelectionModel *sel_model;
	GListModel *model;
	guint n_items, i;
	GtkTreeListRow *row;
	HcChanItem *item;
	gboolean collapsed = FALSE;

	if (parent_ch == NULL)
		return FALSE;

	tv = (treeview *)parent_ch->cv;
	sel_model = gtk_list_view_get_model (tv->view);
	model = G_LIST_MODEL (sel_model);
	n_items = g_list_model_get_n_items (model);

	/* Find the row for the parent and check if it's expanded */
	for (i = 0; i < n_items; i++)
	{
		row = g_list_model_get_item (model, i);
		if (!row)
			continue;

		item = gtk_tree_list_row_get_item (row);

		if (item && item->ch == parent_ch)
		{
			collapsed = !gtk_tree_list_row_get_expanded (row);
			g_object_unref (item);
			g_object_unref (row);
			return collapsed;
		}

		if (item)
			g_object_unref (item);
		g_object_unref (row);
	}

	return FALSE;
}
