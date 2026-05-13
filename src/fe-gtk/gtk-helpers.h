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

/*
 * GTK4 Helper Functions for PoxChat
 *
 * Convenience functions for common GTK4 patterns. These are NOT compatibility
 * wrappers — they provide genuine utility for patterns that would otherwise
 * require several lines of boilerplate at each call site.
 */

#ifndef POXCHAT_GTK_HELPERS_H
#define POXCHAT_GTK_HELPERS_H

#include "config.h"
#include <gtk/gtk.h>

/* =============================================================================
 * Box Packing Helpers
 * =============================================================================
 * GTK4 uses gtk_box_append() with widget expand properties. These helpers
 * set the correct expand property based on box orientation.
 */

static inline void
hc_box_pack_start_impl (GtkBox *box, GtkWidget *child, gboolean expand)
{
	if (expand)
	{
		GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_hexpand (child, TRUE);
		else
			gtk_widget_set_vexpand (child, TRUE);
	}
	gtk_box_append (box, child);
}

static inline void
hc_box_pack_end_impl (GtkBox *box, GtkWidget *child, gboolean expand)
{
	GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));

	if (expand)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_hexpand (child, TRUE);
		else
			gtk_widget_set_vexpand (child, TRUE);
	}
	else
	{
		if (orient == GTK_ORIENTATION_VERTICAL)
			gtk_widget_set_valign (child, GTK_ALIGN_END);
		else
			gtk_widget_set_halign (child, GTK_ALIGN_END);
	}

	gtk_box_append (box, child);
}

/* =============================================================================
 * ButtonBox Emulation
 * =============================================================================
 * GtkButtonBox was removed in GTK4. These helpers emulate it with GtkBox
 * plus alignment properties.
 *
 * Layout values (matching old GTK3 GtkButtonBoxStyle):
 *   0 = SPREAD, 1 = EDGE, 2 = START, 3 = END (default), 4 = CENTER, 5 = EXPAND
 */

#define HC_BUTTONBOX_SPREAD  0
#define HC_BUTTONBOX_EDGE    1
#define HC_BUTTONBOX_START   2
#define HC_BUTTONBOX_END     3
#define HC_BUTTONBOX_CENTER  4
#define HC_BUTTONBOX_EXPAND  5

static inline GtkWidget *
hc_button_box_new_impl (GtkOrientation orientation)
{
	GtkWidget *box = gtk_box_new (orientation, 12);
	if (orientation == GTK_ORIENTATION_HORIZONTAL)
		gtk_widget_set_halign (box, GTK_ALIGN_END);
	else
		gtk_widget_set_valign (box, GTK_ALIGN_END);
	return box;
}

static inline void
hc_button_box_set_layout_impl (GtkWidget *bbox, int layout)
{
	GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (bbox));

	if (layout == HC_BUTTONBOX_END)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_END);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_END);
	}
	else if (layout == HC_BUTTONBOX_START)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_START);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_START);
	}
	else if (layout == HC_BUTTONBOX_CENTER)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_CENTER);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_CENTER);
	}
	else if (layout == HC_BUTTONBOX_SPREAD)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_FILL);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_FILL);
		g_object_set_data (G_OBJECT (bbox), "buttonbox-layout", GINT_TO_POINTER (layout + 1));
	}
	else if (layout == HC_BUTTONBOX_EXPAND)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_FILL);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_FILL);
		gtk_box_set_homogeneous (GTK_BOX (bbox), TRUE);
	}
	else if (layout == HC_BUTTONBOX_EDGE)
	{
		if (orient == GTK_ORIENTATION_HORIZONTAL)
			gtk_widget_set_halign (bbox, GTK_ALIGN_FILL);
		else
			gtk_widget_set_valign (bbox, GTK_ALIGN_FILL);
	}
}

/* Box add with buttonbox SPREAD awareness */
static inline void
hc_box_add_impl (GtkWidget *box, GtkWidget *child)
{
	gpointer layout_ptr = g_object_get_data (G_OBJECT (box), "buttonbox-layout");
	if (layout_ptr != NULL)
	{
		int layout = GPOINTER_TO_INT (layout_ptr) - 1;
		if (layout == HC_BUTTONBOX_SPREAD)
		{
			GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));
			if (orient == GTK_ORIENTATION_HORIZONTAL)
			{
				gtk_widget_set_hexpand (child, TRUE);
				gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
			}
			else
			{
				gtk_widget_set_vexpand (child, TRUE);
				gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
			}
		}
	}
	gtk_box_append (GTK_BOX (box), child);
}

/* =============================================================================
 * Widget Destruction
 * =============================================================================
 * GTK4 requires different destruction patterns for windows vs parented widgets
 * vs floating widgets. These helpers handle all cases correctly.
 */

static inline void
hc_widget_destroy_impl (GtkWidget *widget)
{
	if (GTK_IS_WINDOW (widget))
	{
		gtk_window_destroy (GTK_WINDOW (widget));
	}
	else
	{
		if (gtk_widget_get_parent (widget) != NULL)
		{
			gtk_widget_unparent (widget);
		}
		else
		{
			g_object_ref_sink (widget);
			g_object_unref (widget);
		}
	}
}

/*
 * Present the transient parent before destroying a window, so focus returns
 * to it instead of escaping to another application.
 */
static inline void
hc_window_destroy_fn (GtkWindow *window)
{
	GtkWindow *parent = gtk_window_get_transient_for (window);
	if (parent)
		gtk_window_present (parent);
	gtk_window_destroy (window);
}

/* =============================================================================
 * Event Controller Helpers
 * =============================================================================
 * GTK4 uses event controllers instead of GTK3's signal-based event system.
 * These helpers create and attach controllers in a single call.
 */

static inline GtkGesture *
hc_add_click_gesture (GtkWidget *widget,
                      GCallback pressed_cb,
                      GCallback released_cb,
                      gpointer user_data)
{
	GtkGesture *gesture = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 0);
	if (pressed_cb)
		g_signal_connect (gesture, "pressed", pressed_cb, user_data);
	if (released_cb)
		g_signal_connect (gesture, "released", released_cb, user_data);
	gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (gesture));
	return gesture;
}

static inline GtkEventController *
hc_add_motion_controller (GtkWidget *widget,
                          GCallback enter_cb,
                          GCallback motion_cb,
                          GCallback leave_cb,
                          gpointer user_data)
{
	GtkEventController *controller = gtk_event_controller_motion_new ();
	if (enter_cb)
		g_signal_connect (controller, "enter", enter_cb, user_data);
	if (motion_cb)
		g_signal_connect (controller, "motion", motion_cb, user_data);
	if (leave_cb)
		g_signal_connect (controller, "leave", leave_cb, user_data);
	gtk_widget_add_controller (widget, controller);
	return controller;
}

static inline GtkEventController *
hc_add_scroll_controller (GtkWidget *widget,
                          GCallback scroll_cb,
                          gpointer user_data)
{
	GtkEventController *controller = gtk_event_controller_scroll_new (
		GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	if (scroll_cb)
		g_signal_connect (controller, "scroll", scroll_cb, user_data);
	gtk_widget_add_controller (widget, controller);
	return controller;
}

static inline GtkEventController *
hc_add_key_controller (GtkWidget *widget,
                       GCallback pressed_cb,
                       GCallback released_cb,
                       gpointer user_data)
{
	GtkEventController *controller = gtk_event_controller_key_new ();
	if (pressed_cb)
		g_signal_connect (controller, "key-pressed", pressed_cb, user_data);
	if (released_cb)
		g_signal_connect (controller, "key-released", released_cb, user_data);
	gtk_widget_add_controller (widget, controller);
	return controller;
}

static inline GtkEventController *
hc_add_focus_controller (GtkWidget *widget,
                         GCallback enter_cb,
                         GCallback leave_cb,
                         gpointer user_data)
{
	GtkEventController *controller = gtk_event_controller_focus_new ();
	if (enter_cb)
		g_signal_connect (controller, "enter", enter_cb, user_data);
	if (leave_cb)
		g_signal_connect (controller, "leave", leave_cb, user_data);
	gtk_widget_add_controller (widget, controller);
	return controller;
}

static inline GtkEventController *
hc_add_crossing_controller (GtkWidget *widget,
                            GCallback enter_cb,
                            GCallback leave_cb,
                            gpointer user_data)
{
	GtkEventController *controller = gtk_event_controller_motion_new ();
	if (enter_cb)
		g_signal_connect (controller, "enter", enter_cb, user_data);
	if (leave_cb)
		g_signal_connect (controller, "leave", leave_cb, user_data);
	gtk_widget_add_controller (widget, controller);
	return controller;
}

/* =============================================================================
 * Drag and Drop Helpers
 * =============================================================================
 */

static inline GtkDropTarget *
hc_add_file_drop_target (GtkWidget *widget, GCallback drop_cb, gpointer user_data)
{
	GtkDropTarget *target;

	target = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", drop_cb, user_data);
	gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (target));

	return target;
}

static inline GtkDropTarget *
hc_add_string_drop_target (GtkWidget *widget, const char *content_type,
                           GCallback drop_cb, gpointer user_data)
{
	GtkDropTarget *target;

	target = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", drop_cb, user_data);
	gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (target));

	return target;
}

static inline GtkDragSource *
hc_add_drag_source (GtkWidget *widget, GCallback prepare_cb, gpointer user_data)
{
	GtkDragSource *source;

	source = gtk_drag_source_new ();
	g_signal_connect (source, "prepare", prepare_cb, user_data);
	gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (source));

	return source;
}

/* =============================================================================
 * Container Helpers
 * =============================================================================
 */

/* Get children list - caller must free with g_list_free() */
static inline GList *
hc_container_get_children (GtkWidget *container)
{
	GList *list = NULL;
	GtkWidget *child;

	for (child = gtk_widget_get_first_child (container);
	     child != NULL;
	     child = gtk_widget_get_next_sibling (child))
	{
		list = g_list_append (list, child);
	}
	return list;
}

/* Type-dispatching container removal (GTK4 has no generic gtk_container_remove) */
static void G_GNUC_UNUSED
hc_container_remove (GtkWidget *container, GtkWidget *widget)
{
	if (GTK_IS_BOX (container))
		gtk_box_remove (GTK_BOX (container), widget);
	else if (GTK_IS_PANED (container))
	{
		GtkPaned *paned = GTK_PANED (container);
		if (gtk_paned_get_start_child (paned) == widget)
			gtk_paned_set_start_child (paned, NULL);
		else if (gtk_paned_get_end_child (paned) == widget)
			gtk_paned_set_end_child (paned, NULL);
	}
	else if (GTK_IS_GRID (container))
		gtk_grid_remove (GTK_GRID (container), widget);
	else if (GTK_IS_WINDOW (container))
		gtk_window_set_child (GTK_WINDOW (container), NULL);
	else
	{
		gtk_widget_unparent (widget);
	}
}

/* Set all four margins at once */
static inline void
hc_widget_set_margin_all (GtkWidget *widget, int margin)
{
	gtk_widget_set_margin_start (widget, margin);
	gtk_widget_set_margin_end (widget, margin);
	gtk_widget_set_margin_top (widget, margin);
	gtk_widget_set_margin_bottom (widget, margin);
}

/* Map float alignment (0.0-1.0) to GtkAlign */
static inline void
hc_misc_set_alignment (GtkWidget *widget, float xalign, float yalign)
{
	GtkAlign halign = GTK_ALIGN_CENTER;
	if (xalign < 0.25f) halign = GTK_ALIGN_START;
	else if (xalign > 0.75f) halign = GTK_ALIGN_END;

	GtkAlign valign = GTK_ALIGN_CENTER;
	if (yalign < 0.25f) valign = GTK_ALIGN_START;
	else if (yalign > 0.75f) valign = GTK_ALIGN_END;

	gtk_widget_set_halign (widget, halign);
	gtk_widget_set_valign (widget, valign);
}

/* Reorder a child in a box by position index */
static inline void
hc_box_reorder_child (GtkBox *box, GtkWidget *child, int position)
{
	if (position == 0)
	{
		gtk_box_reorder_child_after (box, child, NULL);
	}
	else if (position < 0)
	{
		GtkWidget *sibling = gtk_widget_get_last_child (GTK_WIDGET (box));
		if (sibling == child)
			sibling = gtk_widget_get_prev_sibling (child);
		if (sibling)
			gtk_box_reorder_child_after (box, child, sibling);
	}
	else
	{
		GtkWidget *sibling = gtk_widget_get_first_child (GTK_WIDGET (box));
		int i = 0;
		while (sibling && i < position - 1)
		{
			if (sibling != child)
				i++;
			sibling = gtk_widget_get_next_sibling (sibling);
		}
		if (sibling == child)
			sibling = gtk_widget_get_next_sibling (sibling);
		if (sibling)
			gtk_box_reorder_child_after (box, child, sibling);
		else
			hc_box_reorder_child (box, child, -1);
	}
}

/* Convert GdkPixbuf to GdkTexture */
static inline GdkTexture *
hc_pixbuf_to_texture (GdkPixbuf *pixbuf)
{
	if (!pixbuf)
		return NULL;
	return gdk_texture_new_for_pixbuf (pixbuf);
}

/* Create image with GTK3-style size mapping */
static inline GtkWidget *
hc_image_new_from_icon_name (const char *icon_name, int gtk3_size)
{
	GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
	GtkIconSize gtk4_size = GTK_ICON_SIZE_NORMAL;
	/* GTK3: MENU=1, SMALL_TOOLBAR=2, LARGE_TOOLBAR=3, BUTTON=4, DND=5, DIALOG=6 */
	if (gtk3_size >= 3)
		gtk4_size = GTK_ICON_SIZE_LARGE;
	gtk_image_set_icon_size (GTK_IMAGE (image), gtk4_size);
	return image;
}

/* =============================================================================
 * Paned Widget Helpers
 * =============================================================================
 * GTK4 split pack1/pack2 into separate property setters.
 */

static inline void
hc_paned_pack1 (GtkPaned *paned, GtkWidget *child, gboolean resize, gboolean shrink)
{
	gtk_paned_set_start_child (paned, child);
	gtk_paned_set_resize_start_child (paned, resize);
	gtk_paned_set_shrink_start_child (paned, shrink);
}

static inline void
hc_paned_pack2 (GtkPaned *paned, GtkWidget *child, gboolean resize, gboolean shrink)
{
	gtk_paned_set_end_child (paned, child);
	gtk_paned_set_resize_end_child (paned, resize);
	gtk_paned_set_shrink_end_child (paned, shrink);
}

/* =============================================================================
 * Entry Text Dispatch
 * =============================================================================
 * Needed because HexInputEdit and GtkEntry have different APIs.
 */

#include "hex-input-edit.h"

#define hc_entry_get_text(entry) \
	(HEX_IS_INPUT_EDIT (entry) \
	 ? hex_input_edit_get_text (HEX_INPUT_EDIT (entry)) \
	 : gtk_editable_get_text (GTK_EDITABLE (entry)))

#define hc_entry_set_text(entry, text) \
	do { if (HEX_IS_INPUT_EDIT (entry)) \
	         hex_input_edit_set_text (HEX_INPUT_EDIT (entry), text); \
	     else gtk_editable_set_text (GTK_EDITABLE (entry), text); \
	} while (0)

/* =============================================================================
 * Toggle/Check Button Dispatch
 * =============================================================================
 * GTK4 split GtkCheckButton from GtkToggleButton. These dispatch based on
 * runtime type for code that handles both.
 */

static inline gboolean
hc_toggle_button_get_active_impl (GtkWidget *widget)
{
	if (GTK_IS_TOGGLE_BUTTON (widget))
		return gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
	else if (GTK_IS_CHECK_BUTTON (widget))
		return gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
	return FALSE;
}

static inline void
hc_toggle_button_set_active_impl (GtkWidget *widget, gboolean active)
{
	if (GTK_IS_TOGGLE_BUTTON (widget))
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), active);
	else if (GTK_IS_CHECK_BUTTON (widget))
		gtk_check_button_set_active (GTK_CHECK_BUTTON (widget), active);
}

/* =============================================================================
 * ListView / ColumnView Helpers
 * =============================================================================
 */

static inline GtkWidget *
hc_list_view_new_simple (GListModel *model,
                         GtkSelectionMode selection_mode,
                         GCallback setup_cb,
                         GCallback bind_cb,
                         gpointer user_data)
{
	GtkSelectionModel *sel_model;
	GtkListItemFactory *factory;

	if (selection_mode == GTK_SELECTION_MULTIPLE)
		sel_model = GTK_SELECTION_MODEL (gtk_multi_selection_new (model));
	else if (selection_mode == GTK_SELECTION_SINGLE)
		sel_model = GTK_SELECTION_MODEL (gtk_single_selection_new (model));
	else
		sel_model = GTK_SELECTION_MODEL (gtk_no_selection_new (model));

	factory = gtk_signal_list_item_factory_new ();
	if (setup_cb)
		g_signal_connect (factory, "setup", setup_cb, user_data);
	if (bind_cb)
		g_signal_connect (factory, "bind", bind_cb, user_data);

	GtkWidget *view = gtk_list_view_new (sel_model, factory);
	gtk_widget_set_name (view, "poxchat-list");
	return view;
}

static inline GtkWidget *
hc_column_view_new_simple (GListModel *model, GtkSelectionMode selection_mode)
{
	GtkSelectionModel *sel_model;

	if (selection_mode == GTK_SELECTION_MULTIPLE)
		sel_model = GTK_SELECTION_MODEL (gtk_multi_selection_new (model));
	else if (selection_mode == GTK_SELECTION_SINGLE)
		sel_model = GTK_SELECTION_MODEL (gtk_single_selection_new (model));
	else
		sel_model = GTK_SELECTION_MODEL (gtk_no_selection_new (model));

	GtkWidget *view = gtk_column_view_new (sel_model);
	gtk_widget_set_name (view, "poxchat-list");
	return view;
}

/* Hide the column header row in a GtkColumnView.
 * GtkColumnView always creates a header; this walks the widget tree
 * to find it and hides it. */
static inline void
hc_column_view_hide_headers (GtkColumnView *view)
{
	GtkWidget *child;
	for (child = gtk_widget_get_first_child (GTK_WIDGET (view));
	     child != NULL;
	     child = gtk_widget_get_next_sibling (child))
	{
		const char *name = gtk_widget_get_css_name (child);
		if (name && g_str_equal (name, "header"))
		{
			gtk_widget_set_visible (child, FALSE);
			break;
		}
	}
}

static inline GtkColumnViewColumn *
hc_column_view_add_column (GtkColumnView *view,
                           const char *title,
                           GCallback setup_cb,
                           GCallback bind_cb,
                           GCallback unbind_cb,
                           gpointer user_data)
{
	GtkListItemFactory *factory;
	GtkColumnViewColumn *column;

	factory = gtk_signal_list_item_factory_new ();
	if (setup_cb)
		g_signal_connect (factory, "setup", setup_cb, user_data);
	if (bind_cb)
		g_signal_connect (factory, "bind", bind_cb, user_data);
	if (unbind_cb)
		g_signal_connect (factory, "unbind", unbind_cb, user_data);

	column = gtk_column_view_column_new (title, factory);
	gtk_column_view_append_column (view, column);
	g_object_unref (column);	/* view holds its own ref */

	return column;
}

static inline gpointer
hc_selection_model_get_selected_item (GtkSelectionModel *model)
{
	GtkBitset *selection;
	guint position;
	gpointer item = NULL;

	selection = gtk_selection_model_get_selection (model);
	if (!gtk_bitset_is_empty (selection))
	{
		position = gtk_bitset_get_nth (selection, 0);
		item = g_list_model_get_item (G_LIST_MODEL (model), position);
	}
	gtk_bitset_unref (selection);

	return item;
}

static inline guint
hc_selection_model_get_selected_position (GtkSelectionModel *model)
{
	GtkBitset *selection;
	guint position = GTK_INVALID_LIST_POSITION;

	selection = gtk_selection_model_get_selection (model);
	if (!gtk_bitset_is_empty (selection))
		position = gtk_bitset_get_nth (selection, 0);
	gtk_bitset_unref (selection);

	return position;
}

/* =============================================================================
 * Clipboard
 * =============================================================================
 */

#define hc_clipboard_get_default() \
	gdk_display_get_clipboard (gdk_display_get_default ())

/* =============================================================================
 * Menu Popup Helper
 * =============================================================================
 */

static inline GtkWidget *
hc_popover_menu_popup_at (GtkWidget *parent, GMenuModel *menu_model, double x, double y)
{
	GtkWidget *popover = gtk_popover_menu_new_from_model (menu_model);
	GdkRectangle rect = { (int)x, (int)y, 1, 1 };

	gtk_widget_set_parent (popover, parent);
	gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
	gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
	gtk_popover_popup (GTK_POPOVER (popover));

	return popover;
}


/* =============================================================================
 * GtkStack Page Container
 * =============================================================================
 * Page container (like GtkNotebook without tabs) using GtkStack.
 */

static inline GtkWidget *
hc_page_container_new (void)
{
	GtkWidget *stack = gtk_stack_new ();
	gtk_stack_set_hhomogeneous (GTK_STACK (stack), FALSE);
	gtk_stack_set_vhomogeneous (GTK_STACK (stack), FALSE);
	return stack;
}

static inline void
hc_page_container_append (GtkWidget *container, GtkWidget *child)
{
	char name[32];
	g_snprintf (name, sizeof (name), "page_%p", (void *)child);
	gtk_stack_add_named (GTK_STACK (container), child, name);
}

static inline int
hc_page_container_get_page_num (GtkWidget *container, GtkWidget *child)
{
	GtkSelectionModel *pages = gtk_stack_get_pages (GTK_STACK (container));
	guint n = g_list_model_get_n_items (G_LIST_MODEL (pages));
	for (guint i = 0; i < n; i++)
	{
		GtkStackPage *page = GTK_STACK_PAGE (g_list_model_get_item (G_LIST_MODEL (pages), i));
		if (gtk_stack_page_get_child (page) == child)
		{
			g_object_unref (page);
			return (int)i;
		}
		g_object_unref (page);
	}
	return -1;
}

static inline void
hc_page_container_set_current_page (GtkWidget *container, int page_num)
{
	GtkSelectionModel *pages = gtk_stack_get_pages (GTK_STACK (container));
	guint n = g_list_model_get_n_items (G_LIST_MODEL (pages));
	if (page_num >= 0 && (guint)page_num < n)
	{
		GtkStackPage *page = GTK_STACK_PAGE (g_list_model_get_item (G_LIST_MODEL (pages), page_num));
		gtk_stack_set_visible_child (GTK_STACK (container), gtk_stack_page_get_child (page));
		g_object_unref (page);
	}
}

static inline void
hc_page_container_remove_page (GtkWidget *container, int page_num)
{
	GtkSelectionModel *pages = gtk_stack_get_pages (GTK_STACK (container));
	guint n = g_list_model_get_n_items (G_LIST_MODEL (pages));
	if (page_num >= 0 && (guint)page_num < n)
	{
		GtkStackPage *page = GTK_STACK_PAGE (g_list_model_get_item (G_LIST_MODEL (pages), page_num));
		GtkWidget *child = gtk_stack_page_get_child (page);
		g_object_unref (page);
		gtk_stack_remove (GTK_STACK (container), child);
	}
}

/* =============================================================================
 * Debug Logging
 * =============================================================================
 * File-based debug logging for troubleshooting. On Windows GUI apps,
 * stdout/stderr are not available, so this writes to a log file.
 *
 * Define HC_DEBUG_LOG to 1 before including this header to enable.
 */

#ifndef HC_DEBUG_LOG
#define HC_DEBUG_LOG 0
#endif

#if HC_DEBUG_LOG
#include <stdarg.h>
#include <stdio.h>

char *get_xdir (void);

static FILE *hc_debug_file = NULL;

static inline void
hc_debug_log (const char *fmt, ...)
{
	va_list args;
	if (!hc_debug_file)
	{
		char *path = g_build_filename (get_xdir (), "poxchat_debug.log", NULL);
		hc_debug_file = fopen (path, "a");
		g_free (path);
		if (hc_debug_file)
		{
			fprintf (hc_debug_file, "\n=== PoxChat Debug Log ===\n");
			fflush (hc_debug_file);
		}
	}
	if (hc_debug_file)
	{
		va_start (args, fmt);
		vfprintf (hc_debug_file, fmt, args);
		va_end (args);
		fprintf (hc_debug_file, "\n");
		fflush (hc_debug_file);
	}
}
#else
#define hc_debug_log(...) ((void)0)
#endif

/*
 * Legacy GTK3 enum aliases — maps removed constants to GTK4 equivalents.
 * These let existing code compile without changing every call site.
 */

/* GTK3 icon sizes → GTK4 (only NORMAL and LARGE exist) */
#define GTK_ICON_SIZE_MENU          GTK_ICON_SIZE_NORMAL
#define GTK_ICON_SIZE_SMALL_TOOLBAR GTK_ICON_SIZE_NORMAL
#define GTK_ICON_SIZE_LARGE_TOOLBAR GTK_ICON_SIZE_LARGE
#define GTK_ICON_SIZE_BUTTON        GTK_ICON_SIZE_NORMAL
#define GTK_ICON_SIZE_DND           GTK_ICON_SIZE_LARGE
#define GTK_ICON_SIZE_DIALOG        GTK_ICON_SIZE_LARGE

/* =============================================================================
 * Editable Label for GtkColumnView
 * =============================================================================
 *
 * GtkEditableLabel's built-in click gesture toggles editing on every click,
 * which conflicts with row selection in a GtkColumnView. These helpers create
 * an editable label that only enters edit mode on double-click and properly
 * selects the row on single click.
 *
 * Usage:
 *   static GtkEditableLabel *my_editing_label = NULL;
 *
 *   // In factory setup:
 *   GtkWidget *label = hc_editable_label_new (list_item, &my_editing_label);
 *   gtk_list_item_set_child (list_item, label);
 */

static G_GNUC_UNUSED void
hc_editable_label_stop_current (GtkEditableLabel **editing_label)
{
	if (*editing_label && gtk_editable_label_get_editing (*editing_label))
		gtk_editable_label_stop_editing (*editing_label, FALSE);
	*editing_label = NULL;
}

typedef struct {
	GtkListItem *list_item;
	GtkEditableLabel **editing_label;
} HcEditableClickData;

static G_GNUC_UNUSED gboolean
hc_editable_label_start_idle (gpointer user_data)
{
	GtkEditableLabel *label = GTK_EDITABLE_LABEL (user_data);
	if (gtk_widget_get_parent (GTK_WIDGET (label)) != NULL
		&& !gtk_editable_label_get_editing (label))
		gtk_editable_label_start_editing (label);
	return G_SOURCE_REMOVE;
}

static G_GNUC_UNUSED void
hc_editable_label_click_cb (GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data)
{
	HcEditableClickData *data = (HcEditableClickData *)user_data;
	GtkWidget *widget, *label;
	guint pos;

	pos = gtk_list_item_get_position (data->list_item);
	label = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

	/* If already editing, let clicks through for cursor positioning */
	if (gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (label)))
		return;

	hc_editable_label_stop_current (data->editing_label);

	/* Walk up to find the GtkColumnView and select the row */
	widget = label;
	while (widget && !GTK_IS_COLUMN_VIEW (widget))
		widget = gtk_widget_get_parent (widget);

	if (GTK_IS_COLUMN_VIEW (widget))
	{
		GtkSelectionModel *sel = gtk_column_view_get_model (GTK_COLUMN_VIEW (widget));
		gtk_selection_model_select_item (sel, pos, TRUE);
	}

	gtk_widget_grab_focus (label);
	gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

	if (n_press == 2)
	{
		*data->editing_label = GTK_EDITABLE_LABEL (label);
		g_idle_add (hc_editable_label_start_idle, label);
	}
}

static G_GNUC_UNUSED void
hc_editable_click_data_free (gpointer data, GClosure *closure)
{
	(void)closure;
	g_free (data);
}

static G_GNUC_UNUSED GtkWidget *
hc_editable_label_new (GtkListItem *list_item, GtkEditableLabel **editing_label)
{
	GtkWidget *label = gtk_editable_label_new ("");
	GtkGesture *click;
	HcEditableClickData *data;

	gtk_widget_set_name (label, "poxchat-editable");
	gtk_widget_set_hexpand (label, TRUE);
	gtk_widget_set_vexpand (label, TRUE);
	gtk_widget_set_halign (label, GTK_ALIGN_FILL);
	gtk_widget_set_valign (label, GTK_ALIGN_FILL);

	/* Remove GtkEditableLabel's built-in click gesture */
	{
		GListModel *clist = gtk_widget_observe_controllers (label);
		guint i, n = g_list_model_get_n_items (clist);
		for (i = 0; i < n; i++)
		{
			GtkEventController *controller = g_list_model_get_item (clist, i);
			if (GTK_IS_GESTURE_CLICK (controller))
			{
				gtk_widget_remove_controller (label, controller);
				g_object_unref (controller);
				break;
			}
			g_object_unref (controller);
		}
		g_object_unref (clist);
	}

	data = g_new (HcEditableClickData, 1);
	data->list_item = list_item;
	data->editing_label = editing_label;

	click = gtk_gesture_click_new ();
	gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
	g_signal_connect_data (click, "pressed", G_CALLBACK (hc_editable_label_click_cb),
	                       data, hc_editable_click_data_free, 0);
	gtk_widget_add_controller (label, GTK_EVENT_CONTROLLER (click));

	return label;
}

/* GTK4 renamed GDK_MOD1_MASK → GDK_ALT_MASK */
#ifndef GDK_MOD1_MASK
#define GDK_MOD1_MASK GDK_ALT_MASK
#endif

/* =============================================================================
 * Font Metrics
 * =============================================================================
 * pango_font_metrics_get_approximate_char_width returns a width closer to a
 * narrow-digit average than to typical letter width, which makes char-count
 * based layout math (detent thresholds, column widths, etc.) read too small.
 * Measure a reference glyph via a real PangoLayout instead — 'n' is a middle
 * ground between a narrow digit and 'M'.
 */
static inline int
hc_widget_char_width (GtkWidget *widget)
{
	PangoLayout *layout;
	int w = 0;

	if (!widget)
		return 8;

	layout = gtk_widget_create_pango_layout (widget, "n");
	if (layout)
	{
		pango_layout_get_pixel_size (layout, &w, NULL);
		g_object_unref (layout);
	}
	return w > 0 ? w : 8;
}

/* Pango's "approximate char width" — a digit-width average, narrower than
 * letter width. GTK uses this internally for gtk_label_set_width_chars(),
 * so when predicting the pixel floor imposed by a label's width_chars,
 * match GTK's math instead of measuring 'n' (which over-predicts, and the
 * overshoot widens at small fonts because of hinting rounding). */
static inline int
hc_widget_approx_char_width (GtkWidget *widget)
{
	PangoContext *ctx;
	PangoFontMetrics *metrics;
	int w = 0;

	if (!widget)
		return 5;

	ctx = gtk_widget_get_pango_context (widget);
	if (ctx)
	{
		metrics = pango_context_get_metrics (ctx, NULL, NULL);
		if (metrics)
		{
			w = pango_font_metrics_get_approximate_char_width (metrics)
			    / PANGO_SCALE;
			pango_font_metrics_unref (metrics);
		}
	}
	return w > 0 ? w : 5;
}

/* Measure the rendered pixel width of an arbitrary string at the widget's
 * current font. Use when char-count approximations misread — e.g. ellipsis
 * glyph width varies non-linearly with font size and is not 1 * char_w. */
static inline int
hc_widget_string_width (GtkWidget *widget, const char *str)
{
	PangoLayout *layout;
	int w = 0;

	if (!widget || !str)
		return 0;

	layout = gtk_widget_create_pango_layout (widget, str);
	if (layout)
	{
		pango_layout_get_pixel_size (layout, &w, NULL);
		g_object_unref (layout);
	}
	return w;
}

#endif /* POXCHAT_GTK_HELPERS_H */
