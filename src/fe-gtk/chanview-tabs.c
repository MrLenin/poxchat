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

typedef struct
{
	GtkWidget *outer;	/* outer box */
	GtkWidget *inner;	/* inner box */
	GtkWidget *b1;		/* button1 */
	GtkWidget *b2;		/* button2 */
	GtkWidget *scroll_box;	/* box containing b1/b2 (vertical mode only) */
	guint overflow_check_id;	/* pending idle callback for overflow check */
	guint scroll_reorient_id;	/* pending idle callback for scroll-box reorient */
	guint tick_cb_id;	/* frame tick callback id (outer) */
	int last_outer_width;	/* width at last tick, to detect changes */
} tabview;

static void chanview_populate (chanview *cv);
static gboolean cv_tabs_reorient_scroll_box_idle (gpointer data);
static gboolean cv_tabs_tick_cb (GtkWidget *widget, GdkFrameClock *clock,
								 gpointer user_data);

/* ignore "toggled" signal? */
static int ignore_toggle = FALSE;

/* userdata for gobjects used here:
 *
 * tab (togglebuttons inside boxes):
 *   "u" userdata passed to tab-focus callback function (sess)
 *   "c" the tab's (chan *)
 *
 * box (family box)
 *   "f" family
 *
 */

/*
 * GtkViewports request at least as much space as their children do.
 * If we don't intervene here, the GtkViewport will be granted its
 * request, even at the expense of resizing the top-level window.
 */
/*
 * Core logic to check overflow and show/hide scroll buttons.
 * Called from both size-allocate callback and adjustment changed callback.
 */
static void
cv_tabs_check_overflow (chanview *cv)
{
	GtkWidget *inner;
	GtkWidget *viewport;
	gint viewport_size;
	gint content_size;

	inner = ((tabview *)cv)->inner;
	viewport = gtk_widget_get_parent (inner);

	if (cv->vertical)
	{
		/* In GTK4, use the viewport's adjustment which tracks content vs visible size.
		 * upper = total content size, page_size = visible area size */
		{
			GtkAdjustment *adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport));
			viewport_size = (gint)gtk_adjustment_get_page_size (adj);
			content_size = (gint)gtk_adjustment_get_upper (adj);
		}
	} else
	{
		/* In GTK4, use the viewport's adjustment which tracks content vs visible size.
		 * upper = total content size, page_size = visible area size */
		{
			GtkAdjustment *adj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
			viewport_size = (gint)gtk_adjustment_get_page_size (adj);
			content_size = (gint)gtk_adjustment_get_upper (adj);
		}
	}

	if (content_size <= viewport_size)
	{
		gtk_widget_set_visible (((tabview *)cv)->b1, FALSE);
		gtk_widget_set_visible (((tabview *)cv)->b2, FALSE);
	} else
	{
		gtk_widget_set_visible (((tabview *)cv)->b1, TRUE);
		gtk_widget_set_visible (((tabview *)cv)->b2, TRUE);
	}
}

/*
 * Idle callback to check overflow after layout has settled.
 * Returns G_SOURCE_REMOVE to run only once.
 */
static gboolean
cv_tabs_check_overflow_idle (gpointer data)
{
	chanview *cv = data;
	((tabview *)cv)->overflow_check_id = 0;
	cv_tabs_check_overflow (cv);
	return G_SOURCE_REMOVE;
}

/*
 * Schedule an overflow check to run after the current layout pass.
 * Multiple calls before the check runs will be coalesced into one.
 */
static void
cv_tabs_schedule_overflow_check (chanview *cv)
{
	if (((tabview *)cv)->overflow_check_id == 0)
	{
		((tabview *)cv)->overflow_check_id =
			g_idle_add (cv_tabs_check_overflow_idle, cv);
	}
}

/*
 * Schedule a scroll-box reorient (vertical-tabs mode) — coalesces
 * multiple triggers into a single idle pass.
 */
static void
cv_tabs_schedule_reorient (chanview *cv)
{
	tabview *tv = (tabview *)cv;
	if (!tv->scroll_box)
		return;
	if (tv->scroll_reorient_id == 0)
		tv->scroll_reorient_id =
			g_idle_add (cv_tabs_reorient_scroll_box_idle, cv);
}

/*
 * Adjustment "changed" callback - fires when content size changes
 * OR when the viewport (page_size) resizes, so it also serves as
 * our pane-resize hook.
 */
static void
cv_tabs_adj_changed (GtkAdjustment *adj, chanview *cv)
{
	(void)adj;
	cv_tabs_schedule_overflow_check (cv);
	cv_tabs_schedule_reorient (cv);
}

/*
 * Re-orient the scroll-button box (vertical-tabs mode) based on available
 * horizontal space. When the pane is too narrow to fit both scroll arrows
 * side-by-side, stack them vertically so the scroll box's minimum width
 * drops to one icon and the channel-button labels can ellipsize further.
 * Hysteresis prevents flapping right at the threshold.
 */
static gboolean
cv_tabs_reorient_scroll_box_idle (gpointer data)
{
	chanview *cv = data;
	tabview *tv = (tabview *)cv;
	GtkWidget *scroll_box = tv->scroll_box;
	int width;
	int b1_min, b1_nat, b2_min, b2_nat;
	int threshold;
	GtkOrientation current, desired;

	tv->scroll_reorient_id = 0;

	if (!scroll_box || !tv->b1 || !tv->b2 || !tv->outer)
		return G_SOURCE_REMOVE;

	width = gtk_widget_get_width (tv->outer);
	if (width <= 0)
		return G_SOURCE_REMOVE;

	gtk_widget_measure (tv->b1, GTK_ORIENTATION_HORIZONTAL, -1,
		&b1_min, &b1_nat, NULL, NULL);
	gtk_widget_measure (tv->b2, GTK_ORIENTATION_HORIZONTAL, -1,
		&b2_min, &b2_nat, NULL, NULL);

	current = gtk_orientable_get_orientation (GTK_ORIENTABLE (scroll_box));
	/* Stack when the pane can't fit both arrows side-by-side with a
	 * small padding margin. */
	threshold = b1_nat + b2_nat + 8;
	if (current == GTK_ORIENTATION_VERTICAL)
		threshold += 16;	/* hysteresis: need extra room before un-stacking */

	desired = (width < threshold)
		? GTK_ORIENTATION_VERTICAL
		: GTK_ORIENTATION_HORIZONTAL;

	if (current != desired)
		gtk_orientable_set_orientation (GTK_ORIENTABLE (scroll_box), desired);

	return G_SOURCE_REMOVE;
}

/*
 * Per-frame tick callback on outer. The hadjustment "changed" signal
 * can miss horizontal allocation shrinks under POLICY_NEVER (GTK4
 * doesn't always re-fire it on shrink). The tick fires every frame;
 * we only schedule a reorient when the width has actually changed.
 */
static gboolean
cv_tabs_tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
	chanview *cv = user_data;
	tabview *tv = (tabview *)cv;
	int width;
	(void)clock;

	width = gtk_widget_get_width (widget);
	if (width != tv->last_outer_width)
	{
		tv->last_outer_width = width;
		cv_tabs_schedule_reorient (cv);
	}
	return G_SOURCE_CONTINUE;
}


static gint
tab_search_offset (GtkWidget *inner, gint start_offset,
				   gboolean forward, gboolean vertical)
{
	GList *boxes;
	GList *tabs;
	GtkWidget *box;
	GtkWidget *button;
	gint found;

	boxes = hc_container_get_children (inner);
	if (!forward && boxes)
		boxes = g_list_last (boxes);

	while (boxes)
	{
		box = (GtkWidget *)boxes->data;
		boxes = (forward ? boxes->next : boxes->prev);

		tabs = hc_container_get_children (box);
		if (!forward && tabs)
			tabs = g_list_last (tabs);

		while (tabs)
		{
			graphene_point_t p;
			button = (GtkWidget *)tabs->data;
			tabs = (forward ? tabs->next : tabs->prev);

			if (!GTK_IS_TOGGLE_BUTTON (button))
				continue;

			if (!gtk_widget_compute_point (button, inner,
					&GRAPHENE_POINT_INIT (0, 0), &p))
				continue;
			found = (gint)(vertical ? p.y : p.x);
			if ((forward && found > start_offset) ||
				(!forward && found < start_offset))
				return found;
		}
	}

	return 0;
}

static void
tab_scroll_left_up_clicked (GtkWidget *widget, chanview *cv)
{
	GtkAdjustment *adj;
	gint viewport_size;
	gfloat new_value;
	GtkWidget *inner = ((tabview *)cv)->inner;
	GtkWidget *viewport = gtk_widget_get_parent (inner);

	adj = cv->vertical
		? gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport))
		: gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
	viewport_size = (gint)gtk_adjustment_get_page_size (adj);

	new_value = tab_search_offset (inner, gtk_adjustment_get_value (adj), 0, cv->vertical);

	if (new_value + viewport_size > gtk_adjustment_get_upper (adj))
		new_value = gtk_adjustment_get_upper (adj) - viewport_size;

	gtk_adjustment_set_value (adj, new_value);
}

static void
tab_scroll_right_down_clicked (GtkWidget *widget, chanview *cv)
{
	GtkAdjustment *adj;
	gint viewport_size;
	gfloat new_value;
	GtkWidget *inner = ((tabview *)cv)->inner;
	GtkWidget *viewport = gtk_widget_get_parent (inner);

	adj = cv->vertical
		? gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport))
		: gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
	viewport_size = (gint)gtk_adjustment_get_page_size (adj);

	new_value = tab_search_offset (inner, gtk_adjustment_get_value (adj), 1, cv->vertical);

	if (new_value == 0 || new_value + viewport_size > gtk_adjustment_get_upper (adj))
		new_value = gtk_adjustment_get_upper (adj) - viewport_size;

	gtk_adjustment_set_value (adj, new_value);
}

/*
 * Scroll event handler for tabs
 * GTK4: Uses GtkEventControllerScroll with different signature
 */
static gboolean
tab_scroll_cb (GtkEventControllerScroll *controller, double dx, double dy, gpointer cv_ptr)
{
	chanview *cv = (chanview *)cv_ptr;
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

	if (prefs.hex_gui_tab_scrollchans)
	{
		if (dy > 0)
			mg_switch_page (1, 1);
		else if (dy < 0)
			mg_switch_page (1, -1);
	}
	else
	{
		/* mouse wheel scrolling */
		if (dy < 0)
			tab_scroll_left_up_clicked (widget, cv);
		else if (dy > 0)
			tab_scroll_right_down_clicked (widget, cv);
	}

	return FALSE;
}

static void
cv_tabs_xclick_cb (GtkWidget *button, chanview *cv)
{
	cv->cb_xbutton (cv, cv->focused, cv->focused->tag, cv->focused->userdata);
}

/* make a Scroll (arrow) button */

static GtkWidget *
make_sbutton (GtkArrowType type, void *click_cb, void *userdata)
{
	GtkWidget *button, *image;
	const char *icon_name;

	/* Map arrow types to icon names */
	switch (type)
	{
		case GTK_ARROW_UP:
			icon_name = "pan-up-symbolic";
			break;
		case GTK_ARROW_DOWN:
			icon_name = "pan-down-symbolic";
			break;
		case GTK_ARROW_LEFT:
			icon_name = "pan-start-symbolic";
			break;
		case GTK_ARROW_RIGHT:
			icon_name = "pan-end-symbolic";
			break;
		default:
			icon_name = "pan-down-symbolic";
			break;
	}

	button = gtk_button_new ();
	image = gtk_image_new_from_icon_name (icon_name);
	gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
	gtk_button_set_child (GTK_BUTTON (button), image);
	gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
	/* Let the button drop below icon width — otherwise the two scroll
	 * buttons side-by-side floor outer's min width and keep the paned
	 * from allocating outer narrow enough for tabs to ellipsize. */
	gtk_button_set_can_shrink (GTK_BUTTON (button), TRUE);
	g_signal_connect (G_OBJECT (button), "clicked",
							G_CALLBACK (click_cb), userdata);
	hc_add_scroll_controller (button, G_CALLBACK (tab_scroll_cb), userdata);

	return button;
}

/* Detent hint: true "clip-start" width for the tabs bar. In vertical
 * orientation, min ≈ close button + ellipsized "..." + optional icon +
 * padding. Horizontal orientation has wider min but is rarely inside a
 * shrinking pane. */
static int
cv_tabs_detent_min (GtkWidget *outer)
{
	chanview *cv;
	int char_w, min_w;

	char_w = hc_widget_char_width (outer);
	cv = g_object_get_data (G_OBJECT (outer), "chanview-cv");

	if (cv && cv->vertical)
	{
		/* Vertical tabs: each tab fills outer width. The global close
		 * button sits below (stacked), not beside, so it doesn't add to
		 * width. Width per tab = icon + "..." (1 char) + button padding. */
		min_w = char_w + 11;
		if (cv && cv->use_icons)
			min_w += 18;
	}
	else
	{
		/* horizontal: 2 scroll buttons + 1 tab min */
		min_w = 2 * 18 + 18 + char_w + 8;
	}
	return min_w;
}

static void
cv_tabs_init (chanview *cv)
{
	GtkWidget *box, *hbox = NULL;
	GtkWidget *viewport;
	GtkWidget *outer;
	GtkWidget *button;

	if (cv->vertical)
		outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	else
		outer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	((tabview *)cv)->outer = outer;
	((tabview *)cv)->last_outer_width = -1;
	if (cv->vertical)
		((tabview *)cv)->tick_cb_id =
			gtk_widget_add_tick_callback (outer, cv_tabs_tick_cb, cv, NULL);
/*	hc_widget_set_margin_all (GTK_WIDGET (outer), 2);*/

	{
		/* GTK4: Wrap viewport in a GtkScrolledWindow with EXTERNAL scroll policy.
		 * This allows the viewport to be smaller than its child content without
		 * forcing the window to expand. We use our own scroll buttons, not scrollbars.
		 *
		 * In GTK3 this was done via the size_request signal callback (cv_tabs_sizerequest)
		 * which forced minimal size requests. GTK4 doesn't have that signal. */
		GtkWidget *scrollw = gtk_scrolled_window_new ();
		GtkAdjustment *adj;

		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollw),
			cv->vertical ? GTK_POLICY_NEVER : GTK_POLICY_EXTERNAL,
			cv->vertical ? GTK_POLICY_EXTERNAL : GTK_POLICY_NEVER);
		/* Disable overlay scrolling since we use external scroll buttons */
		gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scrollw), FALSE);

		viewport = gtk_viewport_new (NULL, NULL);
		gtk_viewport_set_scroll_to_focus (GTK_VIEWPORT (viewport), FALSE);
		gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrollw), viewport);
		hc_add_scroll_controller (scrollw, G_CALLBACK (tab_scroll_cb), cv);

		/* Connect to adjustment "changed" signal to detect when content size changes.
		 * This fires when tabs are added/removed and the adjustment bounds update.
		 * In vertical mode we also listen on the horizontal adjustment so pane
		 * resizes (which change the viewport's page_size on the non-scroll axis)
		 * trigger the scroll-box reorient check. */
		adj = cv->vertical ?
			gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport)) :
			gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
		g_signal_connect (G_OBJECT (adj), "changed",
								G_CALLBACK (cv_tabs_adj_changed), cv);
		if (cv->vertical)
		{
			GtkAdjustment *hadj =
				gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
			g_signal_connect (G_OBJECT (hadj), "changed",
									G_CALLBACK (cv_tabs_adj_changed), cv);
		}

		hc_box_pack_start_impl (GTK_BOX (outer), scrollw, TRUE);
	}

	if (cv->vertical)
		box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	else
		box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	((tabview *)cv)->inner = box;
	gtk_viewport_set_child (GTK_VIEWPORT (viewport), box);
	/* GTK4 removed the "size-allocate" signal (it is now only a vfunc), so we
	 * can't watch the inner box for tab add/remove that way. Instead the
	 * viewport adjustment's "changed" signal (connected above) covers it:
	 * tab_add_real() queues a resize on the viewport after every add/remove,
	 * which updates the adjustment bounds and triggers the overflow check. */

	/* if vertical, the buttons can be side by side; may re-orient
	 * to vertical stacking when the pane is too narrow to fit both
	 * icons side-by-side (see cv_tabs_scroll_box_sizealloc). */
	if (cv->vertical)
	{
		hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_box_append (GTK_BOX (outer), hbox);
		((tabview *)cv)->scroll_box = hbox;
	}

	/* make the Scroll buttons */
	((tabview *)cv)->b2 = make_sbutton (cv->vertical ?
													GTK_ARROW_UP : GTK_ARROW_LEFT,
													tab_scroll_left_up_clicked,
													cv);

	((tabview *)cv)->b1 = make_sbutton (cv->vertical ?
													GTK_ARROW_DOWN : GTK_ARROW_RIGHT,
													tab_scroll_right_down_clicked,
													cv);

	if (hbox)
	{
		/* When vertical, expand scroll buttons horizontally */
		gtk_widget_set_hexpand (((tabview *)cv)->b2, TRUE);
		gtk_widget_set_hexpand (((tabview *)cv)->b1, TRUE);
		gtk_box_append (GTK_BOX (hbox), ((tabview *)cv)->b2);
		gtk_box_append (GTK_BOX (hbox), ((tabview *)cv)->b1);
	} else
	{
		gtk_box_append (GTK_BOX (outer), ((tabview *)cv)->b2);
		gtk_box_append (GTK_BOX (outer), ((tabview *)cv)->b1);
	}

	/* Start with scroll buttons hidden - the overflow check will show them if
	 * needed. In GTK4 widgets are visible by default, so we must explicitly
	 * hide them. */
	gtk_widget_set_visible (((tabview *)cv)->b1, FALSE);
	gtk_widget_set_visible (((tabview *)cv)->b2, FALSE);

	button = gtkutil_button (outer, "window-close", NULL, cv_tabs_xclick_cb,
									 cv, 0);
	gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
	gtk_widget_set_can_focus (button, FALSE);
	/* Same reason as scroll buttons: don't floor outer's min width. */
	gtk_button_set_can_shrink (GTK_BUTTON (button), TRUE);

	gtk_box_append (GTK_BOX (cv->box), outer);

	g_object_set_data (G_OBJECT (outer), "chanview-cv", cv);
	mg_set_detent_min_func (outer, cv_tabs_detent_min);
}

static void
cv_tabs_postinit (chanview *cv)
{
	GtkWidget *outer = ((tabview *)cv)->outer;
	GtkWidget *inner = ((tabview *)cv)->inner;
	if (!outer)
		return;

	/* Use the inner tabs container for the drag snapshot so the icon
	 * is tightly cropped around the visible tabs, not the outer box's
	 * full allocation (which can extend past the last tab). */
	if (inner)
		g_object_set_data (G_OBJECT (outer), "hc-drag-snapshot-target", inner);

	mg_setup_chanview_drag_source (outer);
}

static void
tab_add_sorted (chanview *cv, GtkWidget *box, GtkWidget *tab, chan *ch)
{
	GList *list, *head;
	GtkWidget *child;
	int i = 0;
	void *b;

	if (!cv->sorted)
	{
		gtk_box_append (GTK_BOX (box), tab);
		return;
	}

	/* sorting TODO:
    *   - move tab if renamed (dialogs) */

	/* userdata, passed to mg_tabs_compare() */
	b = ch->userdata;

	head = list = hc_container_get_children (box);
	while (list)
	{
		child = list->data;
		if (!GTK_IS_SEPARATOR (child))
		{
			void *a = g_object_get_data (G_OBJECT (child), "u");

			if (ch->tag == 0 && cv->cb_compare (a, b) > 0)
			{
				/* Insert before this child (at position i).
				 * The new tab should come before the existing tab. */
				gtk_box_append (GTK_BOX (box), tab);
				hc_box_reorder_child (GTK_BOX (box), tab, i);
				g_list_free (head);
				return;
			}
		}
		i++;
		list = list->next;
	}

	g_list_free (head);

	/* append at end */
	gtk_box_append (GTK_BOX (box), tab);
}

/* remove empty boxes and separators */

static void
cv_tabs_prune (chanview *cv)
{
	GList *boxes, *children;
	GtkWidget *box, *inner;
	GtkWidget *child;
	int empty;

	inner = ((tabview *)cv)->inner;
	boxes = hc_container_get_children (inner);
	while (boxes)
	{
		child = boxes->data;
		box = child;
		boxes = boxes->next;

		/* check if the box is empty (except a vseperator) */
		empty = TRUE;
		children = hc_container_get_children (box);
		while (children)
		{
			if (!GTK_IS_SEPARATOR ((GtkWidget *)children->data))
			{
				empty = FALSE;
				break;
			}
			children = children->next;
		}

		if (empty)
			hc_widget_destroy_impl (GTK_WIDGET (box));
	}
}

static void
tab_add_real (chanview *cv, GtkWidget *tab, chan *ch)
{
	GList *boxes, *children;
	GtkWidget *sep, *box, *inner;
	GtkWidget *child;
	int empty;

	inner = ((tabview *)cv)->inner;
	/* see if a family for this tab already exists */
	boxes = hc_container_get_children (inner);
	while (boxes)
	{
		child = boxes->data;
		box = child;

		if (g_object_get_data (G_OBJECT (box), "f") == ch->family)
		{
			tab_add_sorted (cv, box, tab, ch);
			gtk_widget_queue_resize (gtk_widget_get_parent(inner));
			return;
		}

		boxes = boxes->next;

		/* check if the box is empty (except a vseperator) */
		empty = TRUE;
		children = hc_container_get_children (box);
		while (children)
		{
			if (!GTK_IS_SEPARATOR ((GtkWidget *)children->data))
			{
				empty = FALSE;
				break;
			}
			children = children->next;
		}

		if (empty)
			hc_widget_destroy_impl (GTK_WIDGET (box));
	}

	/* create a new family box */
	if (cv->vertical)
	{
		/* vertical */
		box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	} else
	{
		/* horiz */
		box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
	}

	hc_box_pack_end_impl (GTK_BOX (box), sep, FALSE);
	gtk_box_append (GTK_BOX (inner), box);
	g_object_set_data (G_OBJECT (box), "f", ch->family);
	gtk_box_append (GTK_BOX (box), tab);
	gtk_widget_queue_resize (gtk_widget_get_parent(inner));
}

/*
 * Ignore enter/leave events to avoid prelights on tabs
 * GTK4: Uses GtkEventControllerMotion - but we can just not connect handlers
 */
/* GTK4: Simply don't connect prelight signals - no callback needed */

/* called when a tab is clicked (button down) */

static void
tab_pressed_cb (GtkToggleButton *tab, chan *ch)
{
	chanview *cv = ch->cv;
	gboolean is_switching = (cv->focused != ch);

	/* Activate this tab; the toggle group automatically deactivates the old one.
	 * ignore_toggle prevents tab_toggled_cb from recursing when called
	 * programmatically (e.g. from cv_tabs_focus). */
	if (!gtk_toggle_button_get_active (tab))
	{
		ignore_toggle = TRUE;
		gtk_toggle_button_set_active (tab, TRUE);
		ignore_toggle = FALSE;
	}

	cv->focused = ch;

	if (is_switching)
		cv->cb_focus (cv, ch, ch->tag, ch->userdata);
}

/* called when tab toggle state changes */
static void
tab_toggled_cb (GtkToggleButton *tab, chan *ch)
{
	if (ignore_toggle)
		return;

	/* Only act on activation — the toggle group handles deactivation of the
	 * old tab automatically.  Deselection of the active tab by clicking it
	 * is prevented by a capture-phase gesture (tab_deselect_guard_cb). */
	if (gtk_toggle_button_get_active (tab))
		tab_pressed_cb (tab, ch);
}

/* Capture-phase guard for left-click:
 * - On press: prevent deselecting the active tab (toggle groups allow it)
 * - On release: clear any stuck :active CSS state from prior right-click
 *   gesture corruption */
static void
tab_deselect_guard_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y, chan *ch)
{
	GtkToggleButton *tab = GTK_TOGGLE_BUTTON (
		gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)));
	(void)n_press;
	(void)x;
	(void)y;
	(void)ch;

	if (gtk_toggle_button_get_active (tab))
		gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
tab_deselect_guard_released_cb (GtkGestureClick *gesture, int n_press, double x, double y, chan *ch)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	(void)n_press;
	(void)x;
	(void)y;
	(void)ch;

	/* Right-click gestures can leave residual state that makes subsequent
	 * left-clicks (especially drags) get :active stuck.  Clear it on every
	 * button release so no stale state persists. */
	gtk_widget_unset_state_flags (widget, GTK_STATE_FLAG_ACTIVE);
}

/*
 * Tab click handlers for context menu and middle-click close.
 * These use button-specific gestures (not button 0 / all buttons)
 * to avoid interfering with the toggle button's internal handler.
 */
static void
tab_right_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, chan *ch)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	(void)n_press;
	(void)gesture;

	ch->cv->context_menu_active = 1;
	ch->cv->cb_contextmenu (ch->cv, ch, ch->tag, ch->userdata, widget, x, y);

	/* The popover grabs input, so the button-release never reaches the
	 * gesture — leaving :active CSS state stuck.  Clear it explicitly. */
	gtk_widget_unset_state_flags (widget, GTK_STATE_FLAG_ACTIVE);
}

static void
tab_middle_click_cb (GtkGestureClick *gesture, int n_press, double x, double y, chan *ch)
{
	(void)gesture;
	(void)n_press;
	(void)x;
	(void)y;

	if (prefs.hex_gui_tab_middleclose)
		ch->cv->cb_xbutton (ch->cv, ch, ch->tag, ch->userdata);
}

static void *
cv_tabs_add (chanview *cv, chan *ch, char *name, gboolean has_parent)
{
	GtkWidget *but;

	but = gtk_toggle_button_new_with_label (name);
	gtk_widget_set_name (but, "poxchat-tab");
	g_object_set_data (G_OBJECT (but), "c", ch);

	/* GTK4: Allow button to shrink below natural size for compact tab bar */
	gtk_button_set_can_shrink (GTK_BUTTON (but), TRUE);

	/* Ellipsize the label so narrow tabs (or narrow vertical panes) clip
	 * the name instead of forcing the tab bar to the full text width.
	 * GtkButton with can-shrink=TRUE handles label layout itself. */
	{
		GtkWidget *label = gtk_button_get_child (GTK_BUTTON (but));
		if (GTK_IS_LABEL (label))
			gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	}

	/* When tabs are vertical (on left/right side), expand horizontally to fill width */
	gtk_widget_set_hexpand (but, cv->vertical);
	/* Join the toggle group so only one tab can be active at a time.
	 * Group to any existing tab — cv->focused may be NULL during initial
	 * population, so fall back to finding the first toggle button in the
	 * tab bar (family boxes contain the actual buttons). */
	{
		GtkToggleButton *group_to = NULL;
		if (cv->focused && cv->focused->impl)
			group_to = GTK_TOGGLE_BUTTON (cv->focused->impl);
		else
		{
			/* Find any existing toggle button in the tab bar */
			GtkWidget *inner = ((tabview *)cv)->inner;
			GtkWidget *family = gtk_widget_get_first_child (inner);
			while (family && !group_to)
			{
				GtkWidget *child = gtk_widget_get_first_child (family);
				while (child)
				{
					if (GTK_IS_TOGGLE_BUTTON (child))
					{
						group_to = GTK_TOGGLE_BUTTON (child);
						break;
					}
					child = gtk_widget_get_next_sibling (child);
				}
				family = gtk_widget_get_next_sibling (family);
			}
		}
		if (group_to)
			gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (but), group_to);
	}
	/* Left-click capture: prevent deselecting the active tab, and clear
	 * stuck :active CSS state on release */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 1);
		gtk_event_controller_set_propagation_phase (
			GTK_EVENT_CONTROLLER (gesture), GTK_PHASE_CAPTURE);
		g_signal_connect (gesture, "pressed", G_CALLBACK (tab_deselect_guard_pressed_cb), ch);
		g_signal_connect (gesture, "released", G_CALLBACK (tab_deselect_guard_released_cb), ch);
		gtk_widget_add_controller (but, GTK_EVENT_CONTROLLER (gesture));
	}
	/* Right-click for context menu */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 3);
		g_signal_connect (gesture, "pressed", G_CALLBACK (tab_right_click_cb), ch);
		gtk_widget_add_controller (but, GTK_EVENT_CONTROLLER (gesture));
	}
	/* Middle-click to close tab */
	{
		GtkGesture *gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 2);
		g_signal_connect (gesture, "pressed", G_CALLBACK (tab_middle_click_cb), ch);
		gtk_widget_add_controller (but, GTK_EVENT_CONTROLLER (gesture));
	}
	/* GTK4: No need to connect enter/leave signals - prelights handled by CSS */

	/* GTK4: Only connect to "toggled" signal. The toggle button automatically
	 * toggles on click, and the toggled callback handles all focus logic.
	 * Connecting to "pressed" would cause double-handling. */
	g_signal_connect (G_OBJECT (but), "toggled",
						 	G_CALLBACK (tab_toggled_cb), ch);
	g_object_set_data (G_OBJECT (but), "u", ch->userdata);

	tab_add_real (cv, but, ch);

	return but;
}

/* traverse all the family boxes of tabs 
 *
 * A "group" is basically:
 * GtkV/HBox
 * `-GtkViewPort
 *   `-GtkV/HBox (inner box)
 *     `- GtkBox (family box)
 *        `- GtkToggleButton
 *        `- GtkToggleButton
 *        `- ...
 *     `- GtkBox
 *        `- GtkToggleButton
 *        `- GtkToggleButton
 *        `- ...
 *     `- ...
 *
 * */

static int
tab_group_for_each_tab (chanview *cv,
								int (*callback) (GtkWidget *tab, int num, int usernum),
								int usernum)
{
	GList *tabs;
	GList *boxes;
	GtkWidget *child;
	GtkBox *innerbox;
	int i;

	innerbox = (GtkBox *) ((tabview *)cv)->inner;
	boxes = hc_container_get_children (GTK_WIDGET (innerbox));
	i = 0;
	while (boxes)
	{
		child = boxes->data;
		tabs = hc_container_get_children (child);

		while (tabs)
		{
			child = tabs->data;

			if (!GTK_IS_SEPARATOR (child))
			{
				if (callback (child, i, usernum) != -1)
					return i;
				i++;
			}
			tabs = tabs->next;
		}

		boxes = boxes->next;
	}

	return i;
}

static int
tab_check_focus_cb (GtkWidget *tab, int num, int unused)
{
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab)))
		return num;

	return -1;
}

/* returns the currently focused tab number */

static int
tab_group_get_cur_page (chanview *cv)
{
	return tab_group_for_each_tab (cv, tab_check_focus_cb, 0);
}

/*
 * Scroll the viewport to make the specified tab visible.
 * If the tab is already fully visible, does nothing.
 */
static void
cv_tabs_scroll_to_tab_impl (chanview *cv, GtkWidget *tab)
{
	GtkWidget *inner;
	GtkWidget *viewport;
	GtkAdjustment *adj;
	gdouble adj_value, page_size, upper;
	gdouble tab_start, tab_end;
	graphene_point_t tab_origin;
	gboolean transform_ok;

	if (!tab || !cv)
		return;

	inner = ((tabview *)cv)->inner;
	viewport = gtk_widget_get_parent (inner);

	if (!viewport || !GTK_IS_VIEWPORT (viewport))
		return;

	/* In GTK4, use gtk_widget_compute_point to get position relative to inner box.
	 * gtk_widget_get_allocation returns position relative to parent (family box),
	 * not relative to the scrollable content area. */
	transform_ok = gtk_widget_compute_point (tab, inner,
		&GRAPHENE_POINT_INIT (0, 0), &tab_origin);

	if (!transform_ok)
		return;

	if (cv->vertical)
	{
		adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport));
		tab_start = tab_origin.y;
		tab_end = tab_origin.y + gtk_widget_get_height (tab);
	}
	else
	{
		adj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
		tab_start = tab_origin.x;
		tab_end = tab_origin.x + gtk_widget_get_width (tab);
	}

	adj_value = gtk_adjustment_get_value (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	upper = gtk_adjustment_get_upper (adj);

	/* Calculate target scroll position to center the tab */
	{
		gdouble tab_center = (tab_start + tab_end) / 2.0;
		gdouble viewport_center = adj_value + (page_size / 2.0);
		gdouble new_value = tab_center - (page_size / 2.0);

		/* Check if tab is already reasonably centered (within 1/4 of viewport from center).
		 * This allows some tolerance so we don't scroll for minor position differences,
		 * but will scroll if the tab is too far from center even if fully visible. */
		{
			gdouble tolerance = page_size / 4.0;
			gdouble offset_from_center = tab_center - viewport_center;
			if (offset_from_center < 0)
				offset_from_center = -offset_from_center;

			/* If tab is fully visible AND reasonably centered, don't scroll */
			if (tab_start >= adj_value && tab_end <= adj_value + page_size &&
			    offset_from_center <= tolerance)
				return;
		}

		/* Clamp to valid range */
		if (new_value < 0)
			new_value = 0;
		if (new_value > upper - page_size)
			new_value = upper - page_size;

		gtk_adjustment_set_value (adj, new_value);
	}
}

/*
 * Data structure for deferred scroll-to-tab operation.
 * We need to defer the scroll until after the tab has been laid out
 * AND the viewport adjustment has been updated to include the new tab.
 */
typedef struct {
	chanview *cv;
	GtkWidget *tab;
	int retry_count;
} ScrollToTabData;

static gboolean cv_tabs_scroll_to_tab_timeout (gpointer user_data);

static void
cv_tabs_scroll_data_free (ScrollToTabData *data)
{
	if (data->tab)
		g_object_remove_weak_pointer (G_OBJECT (data->tab), (gpointer *) &data->tab);
	g_free (data);
}

static gboolean
cv_tabs_scroll_to_tab_try (gpointer user_data)
{
	ScrollToTabData *data = user_data;
	GtkWidget *inner;
	GtkWidget *viewport;
	GtkAdjustment *adj;
	gdouble upper;
	gdouble tab_end;
	graphene_point_t tab_origin;
	gboolean transform_ok;
	gint tab_width, tab_height;

	/* Verify the tab widget is still valid and mapped.
	 * data->tab is a weak pointer — NULLed automatically on destroy. */
	if (!data->tab || gtk_widget_get_parent (data->tab) == NULL)
	{
		cv_tabs_scroll_data_free (data);
		return G_SOURCE_REMOVE;
	}

	/* Check if tab has valid size yet */
	tab_width = gtk_widget_get_width (data->tab);
	tab_height = gtk_widget_get_height (data->tab);
	if (tab_width == 0 || tab_height == 0)
	{
		/* Still not laid out - schedule retry with timeout to allow layout */
		data->retry_count++;
		if (data->retry_count < 20)
		{
			g_timeout_add (10, cv_tabs_scroll_to_tab_timeout, data);
			return G_SOURCE_REMOVE;  /* Remove this idle, timeout will retry */
		}

		/* Give up after too many retries */
		cv_tabs_scroll_data_free (data);
		return G_SOURCE_REMOVE;
	}

	/* Get tab position relative to inner box using coordinate transform */
	inner = ((tabview *)data->cv)->inner;
	transform_ok = gtk_widget_compute_point (data->tab, inner,
		&GRAPHENE_POINT_INIT (0, 0), &tab_origin);

	if (!transform_ok)
	{
		/* Transform failed - schedule retry */
		data->retry_count++;
		if (data->retry_count < 20)
		{
			g_timeout_add (10, cv_tabs_scroll_to_tab_timeout, data);
			return G_SOURCE_REMOVE;
		}

		cv_tabs_scroll_data_free (data);
		return G_SOURCE_REMOVE;
	}

	/* Check that the adjustment's upper bound includes this tab.
	 * The adjustment may not have updated yet even if the tab has a size. */
	viewport = gtk_widget_get_parent (inner);
	if (viewport && GTK_IS_VIEWPORT (viewport))
	{
		adj = data->cv->vertical ?
			gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport)) :
			gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (viewport));
		upper = gtk_adjustment_get_upper (adj);
		tab_end = data->cv->vertical ?
			(tab_origin.y + tab_height) :
			(tab_origin.x + tab_width);

		if (tab_end > upper)
		{
			/* Adjustment hasn't updated yet - schedule retry */
			data->retry_count++;
			if (data->retry_count < 20)
			{
				g_timeout_add (10, cv_tabs_scroll_to_tab_timeout, data);
				return G_SOURCE_REMOVE;
			}

			/* Give up after too many retries */
			g_free (data);
			return G_SOURCE_REMOVE;
		}
	}

	cv_tabs_scroll_to_tab_impl (data->cv, data->tab);
	cv_tabs_scroll_data_free (data);
	return G_SOURCE_REMOVE;
}

/* Timeout callback - just calls the try function */
static gboolean
cv_tabs_scroll_to_tab_timeout (gpointer user_data)
{
	return cv_tabs_scroll_to_tab_try (user_data);
}

static void
cv_tabs_scroll_to_tab (chanview *cv, GtkWidget *tab)
{
	/* In GTK4, defer the scroll until the tab has been laid out.
	 * New tabs don't have valid allocations until after the layout pass,
	 * and the viewport adjustment needs to update to include the new tab. */
	ScrollToTabData *data;

	data = g_new0 (ScrollToTabData, 1);
	data->cv = cv;
	data->tab = tab;
	data->retry_count = 0;

	/* Use weak ref so data->tab is NULLed if the widget is destroyed
	 * before the idle/timeout fires (e.g., closing a server tab). */
	g_object_add_weak_pointer (G_OBJECT (tab), (gpointer *) &data->tab);

	g_idle_add (cv_tabs_scroll_to_tab_try, data);
}

static void
cv_tabs_focus (chan *ch)
{
	if (ch->impl)
	{
		/* focus the new one (tab_pressed_cb defocuses the old one) */
		tab_pressed_cb (GTK_TOGGLE_BUTTON (ch->impl), ch);
		/* scroll to make the focused tab visible */
		cv_tabs_scroll_to_tab (ch->cv, ch->impl);
	}
}

static int
tab_focus_num_cb (GtkWidget *tab, int num, int want)
{
	if (num == want)
	{
		cv_tabs_focus (g_object_get_data (G_OBJECT (tab), "c"));
		return 1;
	}

	return -1;
}

static void
cv_tabs_change_orientation (chanview *cv)
{
	/* cleanup the old one */
	if (cv->func_cleanup)
		cv->func_cleanup (cv);

	/* now rebuild a new tabbar or tree */
	cv->func_init (cv);
	/* Re-run postinit so the drag source (and any other one-time widget
	 * wiring) attaches to the fresh outer widget. Without this, dragging
	 * the tab bar stops working after any orientation change — the old
	 * outer (with the drag source) is destroyed and the new one never
	 * gets its controller installed. */
	if (cv->func_postinit)
		cv->func_postinit (cv);
	chanview_populate (cv);
}

/* switch to the tab number specified */

static void
cv_tabs_move_focus (chanview *cv, gboolean relative, int num)
{
	int i, max;

	if (relative)
	{
		max = cv->size;
		i = tab_group_get_cur_page (cv) + num;
		/* make it wrap around at both ends */
		if (i < 0)
			i = max - 1;
		if (i >= max)
			i = 0;
		tab_group_for_each_tab (cv, tab_focus_num_cb, i);
		return;
	}

	tab_group_for_each_tab (cv, tab_focus_num_cb, num);
}

static void
cv_tabs_remove (chan *ch)
{
	hc_widget_destroy_impl (GTK_WIDGET (ch->impl));
	ch->impl = NULL;

	cv_tabs_prune (ch->cv);
}

static void
cv_tabs_move (chan *ch, int delta)
{
	int i = 0;
	int pos = 0;
	GList *list;
	GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET (ch->impl));

	for (list = hc_container_get_children (parent); list; list = list->next)
	{
		GtkWidget *child_entry;

		child_entry = list->data;
		if (child_entry == ch->impl)
			pos = i;

		/* keep separator at end to not throw off our count */
		if (GTK_IS_SEPARATOR (child_entry))
			hc_box_reorder_child (GTK_BOX (parent), child_entry, -1);
		else
			i++;
	}

	pos = (pos - delta) % i;
	hc_box_reorder_child (GTK_BOX (parent), ch->impl, pos);
}

static void
cv_tabs_move_family (chan *ch, int delta)
{
	int i, pos = 0;
	GList *list;
	GtkWidget *box = NULL;

	/* find position of tab's family */
	i = 0;
	for (list = hc_container_get_children (((tabview *)ch->cv)->inner); list; list = list->next)
	{
		GtkWidget *child_entry;
		void *fam;

		child_entry = list->data;
		fam = g_object_get_data (G_OBJECT (child_entry), "f");
		if (fam == ch->family)
		{
			box = child_entry;
			pos = i;
		}
		i++;
	}

	pos = (pos - delta) % i;
	hc_box_reorder_child (GTK_BOX (gtk_widget_get_parent(box)), box, pos);
}

static void
cv_tabs_cleanup (chanview *cv)
{
	guint n_items, n_children, i, j;
	HcChanItem *item, *child;

	/* Cancel any pending overflow check */
	if (((tabview *)cv)->overflow_check_id != 0)
	{
		g_source_remove (((tabview *)cv)->overflow_check_id);
		((tabview *)cv)->overflow_check_id = 0;
	}
	if (((tabview *)cv)->scroll_reorient_id != 0)
	{
		g_source_remove (((tabview *)cv)->scroll_reorient_id);
		((tabview *)cv)->scroll_reorient_id = 0;
	}
	if (((tabview *)cv)->tick_cb_id != 0 && ((tabview *)cv)->outer)
	{
		gtk_widget_remove_tick_callback (((tabview *)cv)->outer,
										 ((tabview *)cv)->tick_cb_id);
		((tabview *)cv)->tick_cb_id = 0;
	}

	/* Clear ch->impl before destroying the tab widgets. Otherwise a
	 * subsequent cv_tabs_add (during rebuild) reads cv->focused->impl
	 * as a dangling pointer and passes it to gtk_toggle_button_set_group,
	 * corrupting the group's linked list and causing gtk_toggle_button_set_active
	 * to loop forever in the group-walk at gtktogglebutton.c. */
	n_items = g_list_model_get_n_items (G_LIST_MODEL (cv->store));
	for (i = 0; i < n_items; i++)
	{
		item = g_list_model_get_item (G_LIST_MODEL (cv->store), i);
		if (!item)
			continue;
		if (item->ch)
			item->ch->impl = NULL;
		if (item->children)
		{
			n_children = g_list_model_get_n_items (G_LIST_MODEL (item->children));
			for (j = 0; j < n_children; j++)
			{
				child = g_list_model_get_item (G_LIST_MODEL (item->children), j);
				if (child)
				{
					if (child->ch)
						child->ch->impl = NULL;
					g_object_unref (child);
				}
			}
		}
		g_object_unref (item);
	}

	if (cv->box)
		hc_widget_destroy_impl (GTK_WIDGET (((tabview *)cv)->outer));

	((tabview *)cv)->scroll_box = NULL;
	((tabview *)cv)->b1 = NULL;
	((tabview *)cv)->b2 = NULL;
	((tabview *)cv)->outer = NULL;
	((tabview *)cv)->inner = NULL;
	((tabview *)cv)->tick_cb_id = 0;
	((tabview *)cv)->last_outer_width = -1;
}

static void
cv_tabs_set_color (chan *ch, PangoAttrList *list)
{
	GtkWidget *child = gtk_button_get_child (GTK_BUTTON (ch->impl));
	if (child && GTK_IS_LABEL (child))
		gtk_label_set_attributes (GTK_LABEL (child), list);
}

/* The label is the togglebutton's child; fetched fresh each frame so a rename
 * (which replaces the child) can't leave the pulse holding a dead pointer. */
static GtkWidget *
cv_tabs_get_label (chan *ch)
{
	GtkWidget *child;

	if (!ch->impl)
		return NULL;
	child = gtk_button_get_child (GTK_BUTTON (ch->impl));
	return (child && GTK_IS_LABEL (child)) ? child : NULL;
}

static void
cv_tabs_rename (chan *ch, char *name)
{
	PangoAttrList *attr;
	GtkWidget *tab = ch->impl;
	GtkWidget *label;

	label = gtk_button_get_child (GTK_BUTTON (tab));

	attr = (label && GTK_IS_LABEL (label)) ? gtk_label_get_attributes (GTK_LABEL (label)) : NULL;
	if (attr)
		pango_attr_list_ref (attr);

	gtk_button_set_label (GTK_BUTTON (tab), name);
	gtk_widget_queue_resize (gtk_widget_get_parent(gtk_widget_get_parent(gtk_widget_get_parent(tab))));

	if (attr)
	{
		label = gtk_button_get_child (GTK_BUTTON (tab));
		if (label && GTK_IS_LABEL (label))
			gtk_label_set_attributes (GTK_LABEL (label), attr);
		pango_attr_list_unref (attr);
	}
}

static gboolean
cv_tabs_is_collapsed (chan *ch)
{
	return FALSE;
}

static chan *
cv_tabs_get_parent (chan *ch)
{
	return NULL;
}
