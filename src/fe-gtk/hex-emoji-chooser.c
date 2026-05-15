/* hex-emoji-chooser.c: Emoji chooser with Twemoji sprite rendering
 *
 * Based on GtkEmojiChooser from GTK 4.x
 * Copyright 2017, Red Hat, Inc.
 * Copyright 2026, PoxChat contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Twemoji graphics are Copyright Twitter/X, licensed under CC-BY 4.0.
 * https://github.com/twitter/twemoji
 */

#include "../common/cfgfiles.h"
#include "../common/poxchat.h"
#include "../common/poxchatc.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <pango/pango.h>
#include <glib/gi18n.h>

#include "hex-emoji-chooser.h"
#include "xtext-emoji.h"

#define BOX_SPACE 6

/* --- HexEmojiChooserChild: custom flow box child with variations popover --- */

GType hex_emoji_chooser_child_get_type (void);

#define HEX_TYPE_EMOJI_CHOOSER_CHILD (hex_emoji_chooser_child_get_type ())

typedef struct
{
	GtkFlowBoxChild parent;
	GtkWidget *variations;
} HexEmojiChooserChild;

typedef struct
{
	GtkFlowBoxChildClass parent_class;
} HexEmojiChooserChildClass;

G_DEFINE_TYPE (HexEmojiChooserChild, hex_emoji_chooser_child, GTK_TYPE_FLOW_BOX_CHILD)

static void
hex_emoji_chooser_child_init (HexEmojiChooserChild *child)
{
}

static void
hex_emoji_chooser_child_dispose (GObject *object)
{
	HexEmojiChooserChild *child = (HexEmojiChooserChild *)object;

	g_clear_pointer (&child->variations, gtk_widget_unparent);

	G_OBJECT_CLASS (hex_emoji_chooser_child_parent_class)->dispose (object);
}

static void
hex_emoji_chooser_child_size_allocate (GtkWidget *widget,
                                       int        width,
                                       int        height,
                                       int        baseline)
{
	HexEmojiChooserChild *child = (HexEmojiChooserChild *)widget;

	GTK_WIDGET_CLASS (hex_emoji_chooser_child_parent_class)->size_allocate (widget, width, height, baseline);
	if (child->variations)
		gtk_popover_present (GTK_POPOVER (child->variations));
}

static gboolean
hex_emoji_chooser_child_focus (GtkWidget        *widget,
                               GtkDirectionType  direction)
{
	HexEmojiChooserChild *child = (HexEmojiChooserChild *)widget;

	if (child->variations && gtk_widget_is_visible (child->variations))
	{
		if (gtk_widget_child_focus (child->variations, direction))
			return TRUE;
	}

	return GTK_WIDGET_CLASS (hex_emoji_chooser_child_parent_class)->focus (widget, direction);
}

static void scroll_to_child (GtkWidget *child);

static gboolean
hex_emoji_chooser_child_grab_focus (GtkWidget *widget)
{
	GTK_WIDGET_CLASS (hex_emoji_chooser_child_parent_class)->grab_focus (widget);
	scroll_to_child (widget);
	return TRUE;
}

static void show_variations (HexEmojiChooser *chooser,
                             GtkWidget       *child);

static void
hex_emoji_chooser_child_popup_menu (GtkWidget  *widget,
                                    const char *action_name,
                                    GVariant   *parameters)
{
	GtkWidget *chooser;

	chooser = gtk_widget_get_ancestor (widget, HEX_TYPE_EMOJI_CHOOSER);

	show_variations (HEX_EMOJI_CHOOSER (chooser), widget);
}

static void
hex_emoji_chooser_child_class_init (HexEmojiChooserChildClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

	object_class->dispose = hex_emoji_chooser_child_dispose;
	widget_class->size_allocate = hex_emoji_chooser_child_size_allocate;
	widget_class->focus = hex_emoji_chooser_child_focus;
	widget_class->grab_focus = hex_emoji_chooser_child_grab_focus;

	gtk_widget_class_install_action (widget_class, "menu.popup", NULL, hex_emoji_chooser_child_popup_menu);

	gtk_widget_class_add_binding_action (widget_class,
	                                     GDK_KEY_F10, GDK_SHIFT_MASK,
	                                     "menu.popup",
	                                     NULL);
	gtk_widget_class_add_binding_action (widget_class,
	                                     GDK_KEY_Menu, 0,
	                                     "menu.popup",
	                                     NULL);

	gtk_widget_class_set_css_name (widget_class, "emoji");
}

/* --- EmojiSection: groups the box, heading, and toolbar button for one category --- */

typedef struct {
	GtkWidget *box;
	GtkWidget *heading;
	GtkWidget *button;
	int group;
	gunichar label;
	gboolean empty;
} EmojiSection;

/* --- HexEmojiChooser struct --- */

struct _HexEmojiChooser
{
	GtkPopover parent_instance;

	GtkWidget *search_entry;
	GtkWidget *stack;
	GtkWidget *scrolled_window;

	int emoji_max_width;

	EmojiSection recent;
	EmojiSection people;
	EmojiSection body;
	EmojiSection nature;
	EmojiSection food;
	EmojiSection travel;
	EmojiSection activities;
	EmojiSection objects;
	EmojiSection symbols;
	EmojiSection flags;

	GVariant *data;
	GtkWidget *box;
	GVariantIter *iter;
	guint populate_idle;

	xtext_emoji_cache *emoji_cache;     /* shared cache from xtext (NOT owned) */
	xtext_emoji_cache *chooser_cache;   /* own cache at 32px for chooser display */
	GHashTable *texture_cache;          /* filename -> GdkTexture* */
};

struct _HexEmojiChooserClass {
	GtkPopoverClass parent_class;
};

enum {
	EMOJI_PICKED,
	LAST_SIGNAL
};

static int signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE (HexEmojiChooser, hex_emoji_chooser, GTK_TYPE_POPOVER)

/* --- Embedded UI template --- */

static const char hex_emoji_chooser_ui[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<interface>\n"
"  <template class=\"HexEmojiChooser\" parent=\"GtkPopover\">\n"
"    <style>\n"
"      <class name=\"emoji-picker\"/>\n"
"    </style>\n"
"    <property name=\"child\">\n"
"      <object class=\"GtkBox\" id=\"box\">\n"
"        <property name=\"orientation\">1</property>\n"
"        <child>\n"
"          <object class=\"GtkBox\">\n"
"            <style>\n"
"              <class name=\"emoji-searchbar\"/>\n"
"            </style>\n"
"            <child>\n"
"              <object class=\"GtkSearchEntry\" id=\"search_entry\">\n"
"                <property name=\"hexpand\">1</property>\n"
"                <signal name=\"search-changed\" handler=\"search_changed\"/>\n"
"                <signal name=\"stop-search\" handler=\"stop_search\"/>\n"
"              </object>\n"
"            </child>\n"
"          </object>\n"
"        </child>\n"
"        <child>\n"
"          <object class=\"GtkStack\" id=\"stack\">\n"
"            <child>\n"
"              <object class=\"GtkStackPage\">\n"
"                <property name=\"name\">list</property>\n"
"                <property name=\"child\">\n"
"                  <object class=\"GtkBox\">\n"
"                    <property name=\"orientation\">1</property>\n"
"                    <child>\n"
"                      <object class=\"GtkScrolledWindow\" id=\"scrolled_window\">\n"
"                        <property name=\"vexpand\">1</property>\n"
"                        <property name=\"hscrollbar-policy\">2</property>\n"
"                        <property name=\"propagate-natural-height\">1</property>\n"
"                        <property name=\"max-content-height\">320</property>\n"
"                        <style>\n"
"                          <class name=\"view\"/>\n"
"                        </style>\n"
"                        <property name=\"child\">\n"
"                          <object class=\"GtkBox\" id=\"emoji_box\">\n"
"                            <property name=\"orientation\">1</property>\n"
"                            <property name=\"margin-start\">6</property>\n"
"                            <property name=\"margin-end\">6</property>\n"
"                            <property name=\"margin-top\">6</property>\n"
"                            <property name=\"margin-bottom\">6</property>\n"
"                            <property name=\"spacing\">6</property>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"recent.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureLongPress\">\n"
"                                    <signal name=\"pressed\" handler=\"long_pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureClick\">\n"
"                                    <property name=\"button\">3</property>\n"
"                                    <signal name=\"pressed\" handler=\"pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"people.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Smileys &amp; People</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"people.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureLongPress\">\n"
"                                    <signal name=\"pressed\" handler=\"long_pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureClick\">\n"
"                                    <property name=\"button\">3</property>\n"
"                                    <signal name=\"pressed\" handler=\"pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"body.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Body &amp; Clothing</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"body.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureLongPress\">\n"
"                                    <signal name=\"pressed\" handler=\"long_pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                                <child>\n"
"                                  <object class=\"GtkGestureClick\">\n"
"                                    <property name=\"button\">3</property>\n"
"                                    <signal name=\"pressed\" handler=\"pressed_cb\"/>\n"
"                                  </object>\n"
"                                </child>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"nature.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Animals &amp; Nature</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"nature.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"food.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Food &amp; Drink</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"food.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"travel.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Travel &amp; Places</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"travel.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"activities.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Activities</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"activities.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"objects.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Objects</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"objects.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"symbols.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Symbols</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"symbols.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkLabel\" id=\"flags.heading\">\n"
"                                <property name=\"label\" translatable=\"yes\" context=\"emoji category\">Flags</property>\n"
"                                <property name=\"xalign\">0</property>\n"
"                              </object>\n"
"                            </child>\n"
"                            <child>\n"
"                              <object class=\"GtkFlowBox\" id=\"flags.box\">\n"
"                                <property name=\"homogeneous\">1</property>\n"
"                                <property name=\"selection-mode\">0</property>\n"
"                                <signal name=\"child-activated\" handler=\"emoji_activated\"/>\n"
"                                <signal name=\"keynav-failed\" handler=\"keynav_failed\"/>\n"
"                              </object>\n"
"                            </child>\n"
"                          </object>\n"
"                        </property>\n"
"                      </object>\n"
"                    </child>\n"
"                    <child>\n"
"                      <object class=\"GtkFlowBox\">\n"
"                        <property name=\"min-children-per-line\">3</property>\n"
"                        <property name=\"max-children-per-line\">10</property>\n"
"                        <property name=\"selection-mode\">0</property>\n"
"                        <style>\n"
"                          <class name=\"emoji-toolbar\"/>\n"
"                        </style>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"recent.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Recent</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"people.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Smileys &amp; People</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"body.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Body &amp; Clothing</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"nature.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Animals &amp; Nature</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"food.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Food &amp; Drink</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"travel.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Travel &amp; Places</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"activities.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Activities</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"objects.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Objects</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"symbols.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Symbols</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                        <child>\n"
"                          <object class=\"GtkButton\" id=\"flags.button\">\n"
"                            <property name=\"has-frame\">0</property>\n"
"                            <property name=\"tooltip-text\" translatable=\"yes\" context=\"emoji category\">Flags</property>\n"
"                            <style>\n"
"                              <class name=\"emoji-section\"/>\n"
"                            </style>\n"
"                          </object>\n"
"                        </child>\n"
"                      </object>\n"
"                    </child>\n"
"                  </object>\n"
"                </property>\n"
"              </object>\n"
"            </child>\n"
"            <child>\n"
"              <object class=\"GtkStackPage\">\n"
"                <property name=\"name\">empty</property>\n"
"                <property name=\"child\">\n"
"                  <object class=\"GtkGrid\">\n"
"                    <property name=\"margin-top\">18</property>\n"
"                    <property name=\"margin-bottom\">18</property>\n"
"                    <property name=\"row-spacing\">12</property>\n"
"                    <property name=\"halign\">3</property>\n"
"                    <property name=\"valign\">3</property>\n"
"                    <style>\n"
"                      <class name=\"dim-label\"/>\n"
"                    </style>\n"
"                    <child>\n"
"                      <object class=\"GtkImage\">\n"
"                        <property name=\"icon-name\">edit-find-symbolic</property>\n"
"                        <property name=\"pixel-size\">72</property>\n"
"                        <style>\n"
"                          <class name=\"dim-label\"/>\n"
"                        </style>\n"
"                        <layout>\n"
"                          <property name=\"column\">0</property>\n"
"                          <property name=\"row\">0</property>\n"
"                        </layout>\n"
"                      </object>\n"
"                    </child>\n"
"                    <child>\n"
"                      <object class=\"GtkLabel\">\n"
"                        <property name=\"label\" translatable=\"yes\">No Results Found</property>\n"
"                        <property name=\"attributes\">0 -1 weight bold, 0 -1 scale 1.44</property>\n"
"                        <layout>\n"
"                          <property name=\"column\">0</property>\n"
"                          <property name=\"row\">1</property>\n"
"                        </layout>\n"
"                      </object>\n"
"                    </child>\n"
"                    <child>\n"
"                      <object class=\"GtkLabel\">\n"
"                        <property name=\"label\" translatable=\"yes\">Try a different search</property>\n"
"                        <style>\n"
"                          <class name=\"dim-label\"/>\n"
"                        </style>\n"
"                        <layout>\n"
"                          <property name=\"column\">0</property>\n"
"                          <property name=\"row\">2</property>\n"
"                        </layout>\n"
"                      </object>\n"
"                    </child>\n"
"                  </object>\n"
"                </property>\n"
"              </object>\n"
"            </child>\n"
"          </object>\n"
"        </child>\n"
"      </object>\n"
"    </property>\n"
"  </template>\n"
"</interface>\n";

/* --- Forward declarations --- */

static void
add_emoji (GtkWidget    *box,
           gboolean      prepend,
           GVariant     *item,
           gunichar      modifier,
           HexEmojiChooser *chooser);

/* --- Finalize / Dispose --- */

static void
hex_emoji_chooser_finalize (GObject *object)
{
	HexEmojiChooser *chooser = HEX_EMOJI_CHOOSER (object);

	if (chooser->populate_idle)
		g_source_remove (chooser->populate_idle);

	g_clear_pointer (&chooser->data, g_variant_unref);
	g_clear_pointer (&chooser->iter, g_variant_iter_free);

	if (chooser->texture_cache)
		g_hash_table_destroy (chooser->texture_cache);

	G_OBJECT_CLASS (hex_emoji_chooser_parent_class)->finalize (object);
}

static void
hex_emoji_chooser_dispose (GObject *object)
{
	HexEmojiChooser *chooser = HEX_EMOJI_CHOOSER (object);

	if (chooser->chooser_cache)
	{
		xtext_emoji_cache_free (chooser->chooser_cache);
		chooser->chooser_cache = NULL;
	}
	chooser->emoji_cache = NULL;  /* not owned */

	gtk_widget_dispose_template (GTK_WIDGET (object), HEX_TYPE_EMOJI_CHOOSER);

	G_OBJECT_CLASS (hex_emoji_chooser_parent_class)->dispose (object);
}

/* --- Scroll helpers --- */

static void
scroll_to_section (EmojiSection *section)
{
	HexEmojiChooser *chooser;
	GtkAdjustment *adj;
	graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, 0, 0);

	chooser = HEX_EMOJI_CHOOSER (gtk_widget_get_ancestor (section->box, HEX_TYPE_EMOJI_CHOOSER));

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
	if (section->heading)
	{
		if (!gtk_widget_compute_bounds (section->heading, gtk_widget_get_parent (section->heading), &bounds))
			graphene_rect_init (&bounds, 0, 0, 0, 0);
	}

	gtk_adjustment_set_value (adj, bounds.origin.y - BOX_SPACE);
}

static void
scroll_to_child (GtkWidget *child)
{
	HexEmojiChooser *chooser;
	GtkAdjustment *adj;
	graphene_point_t p;
	double value;
	double page_size;
	graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, 0, 0);

	chooser = HEX_EMOJI_CHOOSER (gtk_widget_get_ancestor (child, HEX_TYPE_EMOJI_CHOOSER));

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));

	if (!gtk_widget_compute_bounds (child, gtk_widget_get_parent (child), &bounds))
		graphene_rect_init (&bounds, 0, 0, 0, 0);

	value = gtk_adjustment_get_value (adj);
	page_size = gtk_adjustment_get_page_size (adj);

	if (!gtk_widget_compute_point (child, gtk_widget_get_parent (chooser->recent.box),
	                               &GRAPHENE_POINT_INIT (0, 0), &p))
		return;

	if (p.y < value)
		gtk_adjustment_set_value (adj, p.y);
	else if (p.y + bounds.size.height >= value + page_size)
		gtk_adjustment_set_value (adj, value + ((p.y + bounds.size.height) - (value + page_size)));
}

/* --- Recent emoji persistence (file-based, replaces GSettings) --- */

#define MAX_RECENT (7*3)

static GVariant *
load_recent_emoji (void)
{
	char *path = g_build_filename (get_xdir (), "recent_emoji.dat", NULL);
	char *contents = NULL;
	gsize len = 0;
	GVariant *result = NULL;

	if (g_file_get_contents (path, &contents, &len, NULL) && len > 0)
	{
		GBytes *bytes = g_bytes_new_take (contents, len);
		result = g_variant_new_from_bytes (
			G_VARIANT_TYPE ("a((aussasasu)u)"), bytes, FALSE);
		g_bytes_unref (bytes);
	}
	else
	{
		g_free (contents);
	}
	g_free (path);
	return result;
}

static void
save_recent_emoji (GVariant *data)
{
	char *path = g_build_filename (get_xdir (), "recent_emoji.dat", NULL);
	gconstpointer bytes_data;
	gsize size;

	g_variant_ref_sink (data);
	bytes_data = g_variant_get_data (data);
	size = g_variant_get_size (data);
	if (bytes_data && size > 0)
		g_file_set_contents (path, bytes_data, size, NULL);
	g_variant_unref (data);
	g_free (path);
}

/* --- Recent section --- */

static void
populate_recent_section (HexEmojiChooser *chooser)
{
	GVariant *variant;
	GVariant *item;
	GVariantIter iter;
	gboolean empty = TRUE;

	variant = load_recent_emoji ();
	if (!variant)
	{
		gtk_widget_set_visible (chooser->recent.box, FALSE);
		gtk_widget_set_sensitive (chooser->recent.button, FALSE);
		return;
	}

	g_variant_iter_init (&iter, variant);
	while ((item = g_variant_iter_next_value (&iter)))
	{
		GVariant *emoji_data;
		gunichar modifier;

		emoji_data = g_variant_get_child_value (item, 0);
		g_variant_get_child (item, 1, "u", &modifier);
		add_emoji (chooser->recent.box, FALSE, emoji_data, modifier, chooser);
		g_variant_unref (emoji_data);
		g_variant_unref (item);
		empty = FALSE;
	}

	gtk_widget_set_visible (chooser->recent.box, !empty);
	gtk_widget_set_sensitive (chooser->recent.button, !empty);

	g_variant_unref (variant);
}

static void
add_recent_item (HexEmojiChooser *chooser,
                 GVariant        *item,
                 gunichar         modifier)
{
	GList *children, *l;
	int i;
	GVariantBuilder builder;
	GtkWidget *child;

	g_variant_ref (item);

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a((aussasasu)u)"));
	g_variant_builder_add (&builder, "(@(aussasasu)u)", item, modifier);

	children = NULL;
	for (child = gtk_widget_get_last_child (chooser->recent.box);
	     child != NULL;
	     child = gtk_widget_get_prev_sibling (child))
		children = g_list_prepend (children, child);

	for (l = children, i = 1; l; l = l->next, i++)
	{
		GVariant *item2 = g_object_get_data (G_OBJECT (l->data), "emoji-data");
		gunichar modifier2 = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (l->data), "modifier"));

		if (modifier == modifier2 && g_variant_equal (item, item2))
		{
			gtk_flow_box_remove (GTK_FLOW_BOX (chooser->recent.box), l->data);
			i--;
			continue;
		}
		if (i >= MAX_RECENT)
		{
			gtk_flow_box_remove (GTK_FLOW_BOX (chooser->recent.box), l->data);
			continue;
		}

		g_variant_builder_add (&builder, "(@(aussasasu)u)", item2, modifier2);
	}
	g_list_free (children);

	add_emoji (chooser->recent.box, TRUE, item, modifier, chooser);

	/* Enable recent */
	gtk_widget_set_visible (chooser->recent.box, TRUE);
	gtk_widget_set_sensitive (chooser->recent.button, TRUE);

	save_recent_emoji (g_variant_builder_end (&builder));

	g_variant_unref (item);
}

/* --- emoji_activated: handle click/activation on an emoji child --- */

static void
emoji_activated (GtkFlowBox      *box,
                 GtkFlowBoxChild *child,
                 gpointer         data)
{
	HexEmojiChooser *chooser = data;
	const char *text;
	GVariant *item;
	gunichar modifier;

	text = g_object_get_data (G_OBJECT (child), "emoji-text");
	if (!text)
		return;

	item = (GVariant*) g_object_get_data (G_OBJECT (child), "emoji-data");
	modifier = (gunichar) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (child), "modifier"));
	if ((GtkWidget *) box != chooser->recent.box)
		add_recent_item (chooser, item, modifier);

	g_signal_emit (data, signals[EMOJI_PICKED], 0, text);

	/* Close any variations sub-popover, but keep the main chooser open.
	 * The main popover dismisses via autohide (click outside) or typing. */
	{
		GtkWidget *popover;

		popover = gtk_widget_get_ancestor (GTK_WIDGET (box), GTK_TYPE_POPOVER);
		if (popover != GTK_WIDGET (chooser))
			gtk_popover_popdown (GTK_POPOVER (popover));
	}
}

/* --- Variation support --- */

static gboolean
has_variations (GVariant *emoji_data)
{
	GVariant *codes;
	gsize i;
	gboolean result;

	result = FALSE;
	codes = g_variant_get_child_value (emoji_data, 0);
	for (i = 0; i < g_variant_n_children (codes); i++)
	{
		gunichar code;
		g_variant_get_child (codes, i, "u", &code);
		if (code == 0 || code == 0x1f3fb)
		{
			result = TRUE;
			break;
		}
	}
	g_variant_unref (codes);

	return result;
}

static void
show_variations (HexEmojiChooser *chooser,
                 GtkWidget       *child)
{
	GtkWidget *popover;
	GtkWidget *view;
	GtkWidget *box;
	GVariant *emoji_data;
	GtkWidget *parent_popover;
	gunichar modifier;
	HexEmojiChooserChild *ch = (HexEmojiChooserChild *)child;

	if (!child)
		return;

	emoji_data = (GVariant*) g_object_get_data (G_OBJECT (child), "emoji-data");
	if (!emoji_data)
		return;

	if (!has_variations (emoji_data))
		return;

	parent_popover = gtk_widget_get_ancestor (child, GTK_TYPE_POPOVER);
	g_clear_pointer (&ch->variations, gtk_widget_unparent);
	popover = ch->variations = gtk_popover_new ();
	gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);
	gtk_widget_set_parent (popover, child);
	view = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class (view, "view");
	box = gtk_flow_box_new ();
	gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (box), TRUE);
	gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (box), 6);
	gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (box), 6);
	gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (box), TRUE);
	gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (box), GTK_SELECTION_NONE);
	g_object_set (box, "accept-unpaired-release", TRUE, NULL);
	gtk_popover_set_child (GTK_POPOVER (popover), view);
	gtk_box_append (GTK_BOX (view), box);

	g_signal_connect (box, "child-activated", G_CALLBACK (emoji_activated), parent_popover);

	add_emoji (box, FALSE, emoji_data, 0, chooser);
	for (modifier = 0x1f3fb; modifier <= 0x1f3ff; modifier++)
		add_emoji (box, FALSE, emoji_data, modifier, chooser);

	gtk_popover_popup (GTK_POPOVER (popover));
}

/* --- Gesture callbacks --- */

static void
long_pressed_cb (GtkGesture *gesture,
                 double      x,
                 double      y,
                 gpointer    data)
{
	HexEmojiChooser *chooser = data;
	GtkWidget *box;
	GtkWidget *child;

	box = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	child = GTK_WIDGET (gtk_flow_box_get_child_at_pos (GTK_FLOW_BOX (box), x, y));
	show_variations (chooser, child);
}

static void
pressed_cb (GtkGesture *gesture,
            int         n_press,
            double      x,
            double      y,
            gpointer    data)
{
	HexEmojiChooser *chooser = data;
	GtkWidget *box;
	GtkWidget *child;

	box = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
	child = GTK_WIDGET (gtk_flow_box_get_child_at_pos (GTK_FLOW_BOX (box), x, y));
	show_variations (chooser, child);
}

/* --- add_emoji: core function, sprite-based with GtkLabel fallback --- */

static void
add_emoji (GtkWidget    *box,
           gboolean      prepend,
           GVariant     *item,
           gunichar      modifier,
           HexEmojiChooser *chooser)
{
	GtkWidget *child;
	GtkWidget *content;
	GVariant *codes;
	char text[64];
	char *p = text;
	gunichar codepoints[32];
	int cp_count = 0;
	int i;
	gunichar code;

	codes = g_variant_get_child_value (item, 0);
	for (i = 0; i < g_variant_n_children (codes); i++)
	{
		g_variant_get_child (codes, i, "u", &code);
		if (code == 0)
			code = modifier != 0 ? modifier : 0xfe0f;
		if (code == 0x1f3fb)
			code = modifier;
		if (code != 0)
		{
			if (cp_count < 32)
				codepoints[cp_count++] = code;
			p += g_unichar_to_utf8 (code, p);
		}
	}
	g_variant_unref (codes);
	p[0] = 0;

	/* Try sprite rendering if enabled */
	if (prefs.hex_gui_emoji_sprites && chooser->chooser_cache)
	{
		char basename[64];
		char filename[72];
		cairo_surface_t *surface;

		xtext_emoji_build_filename (codepoints, cp_count, basename, sizeof (basename));
		g_snprintf (filename, sizeof (filename), "%s.png", basename);

		surface = xtext_emoji_cache_get (chooser->chooser_cache, filename);
		if (surface)
		{
			GdkTexture *texture;
			int width = cairo_image_surface_get_width (surface);
			int height = cairo_image_surface_get_height (surface);
			int stride = cairo_image_surface_get_stride (surface);
			GBytes *bytes;

			cairo_surface_flush (surface);
			bytes = g_bytes_new (cairo_image_surface_get_data (surface),
			                     stride * height);
			texture = gdk_memory_texture_new (width, height,
				GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
			g_bytes_unref (bytes);

			content = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
			gtk_picture_set_content_fit (GTK_PICTURE (content), GTK_CONTENT_FIT_CONTAIN);
			gtk_widget_set_size_request (content, 32, 32);
			g_object_unref (texture);
			goto have_content;
		}
	}

	/* Fallback: render as GtkLabel with system font */
	{
		PangoAttrList *attrs;
		PangoLayout *layout;
		PangoRectangle rect;

		content = gtk_label_new (text);
		attrs = pango_attr_list_new ();
		pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_X_LARGE));
		gtk_label_set_attributes (GTK_LABEL (content), attrs);
		pango_attr_list_unref (attrs);

		layout = gtk_label_get_layout (GTK_LABEL (content));
		pango_layout_get_extents (layout, &rect, NULL);

		if (pango_layout_get_unknown_glyphs_count (layout) > 0 ||
		    rect.width >= 1.5 * chooser->emoji_max_width)
		{
			g_object_ref_sink (content);
			g_object_unref (content);
			return;
		}
	}

have_content:
	child = g_object_new (HEX_TYPE_EMOJI_CHOOSER_CHILD, NULL);
	g_object_set_data_full (G_OBJECT (child), "emoji-data",
	                        g_variant_ref (item),
	                        (GDestroyNotify) g_variant_unref);
	if (modifier != 0)
		g_object_set_data (G_OBJECT (child), "modifier",
		                   GUINT_TO_POINTER (modifier));
	g_object_set_data_full (G_OBJECT (child), "emoji-text",
	                        g_strdup (text), g_free);

	gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), content);
	gtk_flow_box_insert (GTK_FLOW_BOX (box), child, prepend ? 0 : -1);
}

/* --- Emoji data loading from GResource --- */

static GBytes *
get_emoji_data_by_language (const char *lang)
{
	char *path = g_strconcat ("/org/gtk/libgtk/emoji/", lang, ".data", NULL);
	GBytes *bytes = g_resources_lookup_data (path, 0, NULL);
	g_free (path);
	return bytes;
}

static GBytes *
get_emoji_data (void)
{
	GBytes *bytes;
	const char *lang;

	lang = pango_language_to_string (gtk_get_default_language ());
	bytes = get_emoji_data_by_language (lang);
	if (bytes)
		return bytes;

	if (strchr (lang, '-'))
	{
		char q[5];
		int i;

		for (i = 0; lang[i] != '-' && i < 4; i++)
			q[i] = lang[i];
		q[i] = '\0';

		bytes = get_emoji_data_by_language (q);
		if (bytes)
			return bytes;
	}

	bytes = get_emoji_data_by_language ("en");
	g_assert (bytes);

	return bytes;
}

/* --- Public: emoji literal -> English short-name lookup ---
 *
 * Builds a process-wide GHashTable<gchar* utf8, gchar* name> on first call by
 * walking the "en" emoji GVariant.  Both a FE0F-substituted form and a stripped
 * form are inserted as separate keys so reactions arriving with or without the
 * variation selector both hit.  Skin-tone variants are not enumerated — the
 * base sequence is what users normally react with, and that's what we resolve. */

static GHashTable *emoji_name_cache = NULL;

static void
emoji_name_cache_init (void)
{
	GBytes *bytes;
	GVariant *data;
	GVariantIter iter;
	GVariant *item;

	if (emoji_name_cache)
		return;

	emoji_name_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                          g_free, g_free);

	bytes = get_emoji_data_by_language ("en");
	if (!bytes)
		return;

	data = g_variant_new_from_bytes (G_VARIANT_TYPE ("a(aussasasu)"),
	                                 bytes, TRUE);
	g_variant_ref_sink (data);
	g_bytes_unref (bytes);

	g_variant_iter_init (&iter, data);
	while ((item = g_variant_iter_next_value (&iter)))
	{
		GVariant *codes;
		const char *name_en;
		char with_fe0f[64];
		char without_fe0f[64];
		char *p_with = with_fe0f;
		char *p_without = without_fe0f;
		guint i, n;

		codes = g_variant_get_child_value (item, 0);
		n = g_variant_n_children (codes);
		for (i = 0; i < n; i++)
		{
			gunichar code;

			g_variant_get_child (codes, i, "u", &code);
			if (code == 0)
				code = 0xfe0f;
			if (code == 0x1f3fb)
			{
				/* skip skin-tone placeholders for the base form */
				continue;
			}
			if ((p_with - with_fe0f) < (gssize)(sizeof (with_fe0f) - 6))
				p_with += g_unichar_to_utf8 (code, p_with);
			if (code != 0xfe0f &&
			    (p_without - without_fe0f) < (gssize)(sizeof (without_fe0f) - 6))
				p_without += g_unichar_to_utf8 (code, p_without);
		}
		*p_with = 0;
		*p_without = 0;
		g_variant_unref (codes);

		g_variant_get_child (item, 1, "&s", &name_en);
		if (name_en && with_fe0f[0] &&
		    !g_hash_table_contains (emoji_name_cache, with_fe0f))
			g_hash_table_insert (emoji_name_cache,
			                     g_strdup (with_fe0f),
			                     g_strdup (name_en));
		if (name_en && without_fe0f[0] &&
		    !g_hash_table_contains (emoji_name_cache, without_fe0f))
			g_hash_table_insert (emoji_name_cache,
			                     g_strdup (without_fe0f),
			                     g_strdup (name_en));

		g_variant_unref (item);
	}

	g_variant_unref (data);
}

const char *
hex_emoji_lookup_name (const char *utf8)
{
	if (!utf8 || !utf8[0])
		return NULL;

	emoji_name_cache_init ();
	if (!emoji_name_cache)
		return NULL;

	return g_hash_table_lookup (emoji_name_cache, utf8);
}

/* --- Idle population of emoji grid --- */

static gboolean
populate_emoji_chooser (gpointer data)
{
	HexEmojiChooser *chooser = data;
	GVariant *item;
	gint64 start, now;

	start = g_get_monotonic_time ();

	if (!chooser->data)
	{
		GBytes *bytes;

		bytes = get_emoji_data ();

		chooser->data = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE ("a(aussasasu)"), bytes, TRUE));
		g_bytes_unref (bytes);
	}

	if (!chooser->iter)
	{
		chooser->iter = g_variant_iter_new (chooser->data);
		chooser->box = chooser->people.box;
	}

	while ((item = g_variant_iter_next_value (chooser->iter)))
	{
		guint group;

		g_variant_get_child (item, 5, "u", &group);

		if (group == chooser->people.group)
			chooser->box = chooser->people.box;
		else if (group == chooser->body.group)
			chooser->box = chooser->body.box;
		else if (group == chooser->nature.group)
			chooser->box = chooser->nature.box;
		else if (group == chooser->food.group)
			chooser->box = chooser->food.box;
		else if (group == chooser->travel.group)
			chooser->box = chooser->travel.box;
		else if (group == chooser->activities.group)
			chooser->box = chooser->activities.box;
		else if (group == chooser->objects.group)
			chooser->box = chooser->objects.box;
		else if (group == chooser->symbols.group)
			chooser->box = chooser->symbols.box;
		else if (group == chooser->flags.group)
			chooser->box = chooser->flags.box;

		add_emoji (chooser->box, FALSE, item, 0, chooser);
		g_variant_unref (item);

		now = g_get_monotonic_time ();
		if (now > start + 200) /* 2 ms */
			return G_SOURCE_CONTINUE;
	}

	g_variant_iter_free (chooser->iter);
	chooser->iter = NULL;
	chooser->box = NULL;
	chooser->populate_idle = 0;

	return G_SOURCE_REMOVE;
}

/* --- Scroll position tracking for section highlighting --- */

static void
adj_value_changed (GtkAdjustment *adj,
                   gpointer       data)
{
	HexEmojiChooser *chooser = data;
	double value = gtk_adjustment_get_value (adj);
	EmojiSection const *sections[] = {
		&chooser->recent,
		&chooser->people,
		&chooser->body,
		&chooser->nature,
		&chooser->food,
		&chooser->travel,
		&chooser->activities,
		&chooser->objects,
		&chooser->symbols,
		&chooser->flags,
	};
	EmojiSection const *select_section = sections[0];
	gsize i;

	/* Figure out which section the current scroll position is within */
	for (i = 0; i < G_N_ELEMENTS (sections); ++i)
	{
		EmojiSection const *section = sections[i];
		GtkWidget *child;
		graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, 0, 0);

		if (!gtk_widget_get_visible (section->box))
			continue;

		if (section->heading)
			child = section->heading;
		else
			child = section->box;

		if (!gtk_widget_compute_bounds (child, gtk_widget_get_parent (child), &bounds))
			graphene_rect_init (&bounds, 0, 0, 0, 0);

		if (value < bounds.origin.y - BOX_SPACE)
			break;

		select_section = section;
	}

	/* Un/Check the section buttons accordingly */
	for (i = 0; i < G_N_ELEMENTS (sections); ++i)
	{
		EmojiSection const *section = sections[i];

		if (section == select_section)
			gtk_widget_set_state_flags (section->button, GTK_STATE_FLAG_CHECKED, FALSE);
		else
			gtk_widget_unset_state_flags (section->button, GTK_STATE_FLAG_CHECKED);
	}
}

/* --- Search / filter --- */

static gboolean
match_tokens (const char **term_tokens,
              const char **hit_tokens)
{
	int i, j;
	gboolean matched;

	matched = TRUE;

	for (i = 0; term_tokens[i]; i++)
	{
		for (j = 0; hit_tokens[j]; j++)
			if (g_str_has_prefix (hit_tokens[j], term_tokens[i]))
				goto one_matched;

		matched = FALSE;
		break;

one_matched:
		continue;
	}

	return matched;
}

static gboolean
filter_func (GtkFlowBoxChild *child,
             gpointer         data)
{
	EmojiSection *section = data;
	HexEmojiChooser *chooser;
	GVariant *emoji_data;
	const char *text;
	const char *name_en;
	const char *name;
	const char **keywords_en;
	const char **keywords;
	char **term_tokens;
	char **name_tokens_en;
	char **name_tokens;
	gboolean res;

	res = TRUE;

	chooser = HEX_EMOJI_CHOOSER (gtk_widget_get_ancestor (GTK_WIDGET (child), HEX_TYPE_EMOJI_CHOOSER));
	text = gtk_editable_get_text (GTK_EDITABLE (chooser->search_entry));
	emoji_data = (GVariant *) g_object_get_data (G_OBJECT (child), "emoji-data");

	if (text[0] == 0)
		goto out;

	if (!emoji_data)
		goto out;

	term_tokens = g_str_tokenize_and_fold (text, "en", NULL);
	g_variant_get_child (emoji_data, 1, "&s", &name_en);
	name_tokens = g_str_tokenize_and_fold (name_en, "en", NULL);
	g_variant_get_child (emoji_data, 2, "&s", &name);
	name_tokens_en = g_str_tokenize_and_fold (name, "en", NULL);
	g_variant_get_child (emoji_data, 3, "^a&s", &keywords_en);
	g_variant_get_child (emoji_data, 4, "^a&s", &keywords);

	res = match_tokens ((const char **)term_tokens, (const char **)name_tokens) ||
	      match_tokens ((const char **)term_tokens, (const char **)name_tokens_en) ||
	      match_tokens ((const char **)term_tokens, keywords) ||
	      match_tokens ((const char **)term_tokens, keywords_en);

	g_strfreev (term_tokens);
	g_strfreev (name_tokens);
	g_strfreev (name_tokens_en);
	g_free (keywords_en);
	g_free (keywords);

out:
	if (res)
		section->empty = FALSE;

	return res;
}

static void
invalidate_section (EmojiSection *section)
{
	section->empty = TRUE;
	gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (section->box));
}

static void
update_headings (HexEmojiChooser *chooser)
{
	gtk_widget_set_visible (chooser->people.heading, !chooser->people.empty);
	gtk_widget_set_visible (chooser->people.box, !chooser->people.empty);
	gtk_widget_set_visible (chooser->body.heading, !chooser->body.empty);
	gtk_widget_set_visible (chooser->body.box, !chooser->body.empty);
	gtk_widget_set_visible (chooser->nature.heading, !chooser->nature.empty);
	gtk_widget_set_visible (chooser->nature.box, !chooser->nature.empty);
	gtk_widget_set_visible (chooser->food.heading, !chooser->food.empty);
	gtk_widget_set_visible (chooser->food.box, !chooser->food.empty);
	gtk_widget_set_visible (chooser->travel.heading, !chooser->travel.empty);
	gtk_widget_set_visible (chooser->travel.box, !chooser->travel.empty);
	gtk_widget_set_visible (chooser->activities.heading, !chooser->activities.empty);
	gtk_widget_set_visible (chooser->activities.box, !chooser->activities.empty);
	gtk_widget_set_visible (chooser->objects.heading, !chooser->objects.empty);
	gtk_widget_set_visible (chooser->objects.box, !chooser->objects.empty);
	gtk_widget_set_visible (chooser->symbols.heading, !chooser->symbols.empty);
	gtk_widget_set_visible (chooser->symbols.box, !chooser->symbols.empty);
	gtk_widget_set_visible (chooser->flags.heading, !chooser->flags.empty);
	gtk_widget_set_visible (chooser->flags.box, !chooser->flags.empty);

	if (chooser->recent.empty && chooser->people.empty &&
	    chooser->body.empty && chooser->nature.empty &&
	    chooser->food.empty && chooser->travel.empty &&
	    chooser->activities.empty && chooser->objects.empty &&
	    chooser->symbols.empty && chooser->flags.empty)
		gtk_stack_set_visible_child_name (GTK_STACK (chooser->stack), "empty");
	else
		gtk_stack_set_visible_child_name (GTK_STACK (chooser->stack), "list");
}

static void
search_changed (GtkEntry *entry,
                gpointer  data)
{
	HexEmojiChooser *chooser = data;

	invalidate_section (&chooser->recent);
	invalidate_section (&chooser->people);
	invalidate_section (&chooser->body);
	invalidate_section (&chooser->nature);
	invalidate_section (&chooser->food);
	invalidate_section (&chooser->travel);
	invalidate_section (&chooser->activities);
	invalidate_section (&chooser->objects);
	invalidate_section (&chooser->symbols);
	invalidate_section (&chooser->flags);

	update_headings (chooser);
}

static void
stop_search (GtkEntry *entry,
             gpointer  data)
{
	gtk_popover_popdown (GTK_POPOVER (data));
}

/* --- Section setup --- */

static void
setup_section (HexEmojiChooser *chooser,
               EmojiSection    *section,
               int              group,
               const char      *icon)
{
	section->group = group;

	gtk_button_set_icon_name (GTK_BUTTON (section->button), icon);

	gtk_flow_box_set_filter_func (GTK_FLOW_BOX (section->box), filter_func, section, NULL);
	g_signal_connect_swapped (section->button, "clicked", G_CALLBACK (scroll_to_section), section);
}

/* --- init --- */

static void
hex_emoji_chooser_init (HexEmojiChooser *chooser)
{
	GtkAdjustment *adj;

	gtk_widget_init_template (GTK_WIDGET (chooser));

	/* GtkSearchEntry defaults `focusable` to FALSE in GTK4 — without this
	 * grab_focus() and click-to-focus both no-op, leaving the input box
	 * stuck with focus and the search field unusable. */
	gtk_widget_set_focusable (chooser->search_entry, TRUE);

	/* Get a reasonable maximum width for an emoji. We do this to
	 * skip overly wide fallback rendering for certain emojis the
	 * font does not contain and therefore end up being rendered
	 * as multiple glyphs.
	 */
	{
		PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (chooser), "\xf0\x9f\x99\x82");
		PangoAttrList *attrs;
		PangoRectangle rect;

		attrs = pango_attr_list_new ();
		pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_X_LARGE));
		pango_layout_set_attributes (layout, attrs);
		pango_attr_list_unref (attrs);

		pango_layout_get_extents (layout, &rect, NULL);
		chooser->emoji_max_width = rect.width;

		g_object_unref (layout);
	}

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
	g_signal_connect (adj, "value-changed", G_CALLBACK (adj_value_changed), chooser);

	setup_section (chooser, &chooser->recent, -1, "emoji-recent-symbolic");
	setup_section (chooser, &chooser->people, 0, "emoji-people-symbolic");
	setup_section (chooser, &chooser->body, 1, "emoji-body-symbolic");
	setup_section (chooser, &chooser->nature, 3, "emoji-nature-symbolic");
	setup_section (chooser, &chooser->food, 4, "emoji-food-symbolic");
	setup_section (chooser, &chooser->travel, 5, "emoji-travel-symbolic");
	setup_section (chooser, &chooser->activities, 6, "emoji-activities-symbolic");
	setup_section (chooser, &chooser->objects, 7, "emoji-objects-symbolic");
	setup_section (chooser, &chooser->symbols, 8, "emoji-symbols-symbolic");
	setup_section (chooser, &chooser->flags, 9, "emoji-flags-symbolic");

	populate_recent_section (chooser);

	chooser->populate_idle = g_idle_add (populate_emoji_chooser, chooser);
	g_source_set_name_by_id (chooser->populate_idle, "[hex] populate_emoji_chooser");
}

/* --- show / map --- */

static void
hex_emoji_chooser_show (GtkWidget *widget)
{
	HexEmojiChooser *chooser = HEX_EMOJI_CHOOSER (widget);
	GtkAdjustment *adj;

	GTK_WIDGET_CLASS (hex_emoji_chooser_parent_class)->show (widget);

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
	gtk_adjustment_set_value (adj, 0);
	adj_value_changed (adj, chooser);

	gtk_editable_set_text (GTK_EDITABLE (chooser->search_entry), "");
}

/* --- Section navigation helpers --- */

static EmojiSection *
find_section (HexEmojiChooser *chooser,
              GtkWidget       *box)
{
	if (box == chooser->recent.box)
		return &chooser->recent;
	else if (box == chooser->people.box)
		return &chooser->people;
	else if (box == chooser->body.box)
		return &chooser->body;
	else if (box == chooser->nature.box)
		return &chooser->nature;
	else if (box == chooser->food.box)
		return &chooser->food;
	else if (box == chooser->travel.box)
		return &chooser->travel;
	else if (box == chooser->activities.box)
		return &chooser->activities;
	else if (box == chooser->objects.box)
		return &chooser->objects;
	else if (box == chooser->symbols.box)
		return &chooser->symbols;
	else if (box == chooser->flags.box)
		return &chooser->flags;
	else
		return NULL;
}

static EmojiSection *
find_next_section (HexEmojiChooser *chooser,
                   GtkWidget       *box,
                   gboolean         down)
{
	EmojiSection *next;

	if (box == chooser->recent.box)
		next = down ? &chooser->people : NULL;
	else if (box == chooser->people.box)
		next = down ? &chooser->body : &chooser->recent;
	else if (box == chooser->body.box)
		next = down ? &chooser->nature : &chooser->people;
	else if (box == chooser->nature.box)
		next = down ? &chooser->food : &chooser->body;
	else if (box == chooser->food.box)
		next = down ? &chooser->travel : &chooser->nature;
	else if (box == chooser->travel.box)
		next = down ? &chooser->activities : &chooser->food;
	else if (box == chooser->activities.box)
		next = down ? &chooser->objects : &chooser->travel;
	else if (box == chooser->objects.box)
		next = down ? &chooser->symbols : &chooser->activities;
	else if (box == chooser->symbols.box)
		next = down ? &chooser->flags : &chooser->objects;
	else if (box == chooser->flags.box)
		next = down ? NULL : &chooser->symbols;
	else
		next = NULL;

	return next;
}

static void
hex_emoji_chooser_scroll_section (GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *parameter)
{
	HexEmojiChooser *chooser = HEX_EMOJI_CHOOSER (widget);
	int direction = g_variant_get_int32 (parameter);
	GtkWidget *focus;
	GtkWidget *box;
	EmojiSection *next;

	focus = gtk_root_get_focus (gtk_widget_get_root (widget));
	if (focus == NULL)
		return;

	if (gtk_widget_is_ancestor (focus, chooser->search_entry))
		box = chooser->recent.box;
	else
		box = gtk_widget_get_ancestor (focus, GTK_TYPE_FLOW_BOX);

	next = find_next_section (chooser, box, direction > 0);

	if (next)
	{
		gtk_widget_child_focus (next->box, GTK_DIR_TAB_FORWARD);
		scroll_to_section (next);
	}
}

/* --- Keyboard navigation across sections --- */

static gboolean
keynav_failed (GtkWidget        *box,
               GtkDirectionType  direction,
               HexEmojiChooser  *chooser)
{
	EmojiSection *next;
	GtkWidget *focus;
	GtkWidget *child;
	GtkWidget *sibling;
	int i;
	int column;
	int child_x;
	graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, 0, 0);

	focus = gtk_root_get_focus (gtk_widget_get_root (box));
	if (focus == NULL)
		return FALSE;

	child = gtk_widget_get_ancestor (focus, HEX_TYPE_EMOJI_CHOOSER_CHILD);

	column = 0;
	child_x = G_MAXINT;
	for (sibling = gtk_widget_get_first_child (box);
	     sibling;
	     sibling = gtk_widget_get_next_sibling (sibling))
	{
		if (!gtk_widget_get_child_visible (sibling))
			continue;

		if (!gtk_widget_compute_bounds (sibling, box, &bounds))
			graphene_rect_init (&bounds, 0, 0, 0, 0);

		if (bounds.origin.x < child_x)
			column = 0;
		else
			column++;

		child_x = (int) bounds.origin.x;

		if (sibling == child)
			break;
	}

	if (direction == GTK_DIR_DOWN)
	{
		next = find_section (chooser, box);
		while (TRUE)
		{
			next = find_next_section (chooser, next->box, TRUE);
			if (next == NULL)
				return FALSE;

			i = 0;
			child_x = G_MAXINT;
			for (sibling = gtk_widget_get_first_child (next->box);
			     sibling;
			     sibling = gtk_widget_get_next_sibling (sibling))
			{
				if (!gtk_widget_get_child_visible (sibling))
					continue;

				if (!gtk_widget_compute_bounds (sibling, next->box, &bounds))
					graphene_rect_init (&bounds, 0, 0, 0, 0);

				if (bounds.origin.x < child_x)
					i = 0;
				else
					i++;

				child_x = (int) bounds.origin.x;

				if (i == column)
				{
					gtk_widget_grab_focus (sibling);
					return TRUE;
				}
			}
		}
	}
	else if (direction == GTK_DIR_UP)
	{
		next = find_section (chooser, box);
		while (TRUE)
		{
			next = find_next_section (chooser, next->box, FALSE);
			if (next == NULL)
				return FALSE;

			i = 0;
			child_x = G_MAXINT;
			child = NULL;
			for (sibling = gtk_widget_get_first_child (next->box);
			     sibling;
			     sibling = gtk_widget_get_next_sibling (sibling))
			{
				if (!gtk_widget_get_child_visible (sibling))
					continue;

				if (!gtk_widget_compute_bounds (sibling, next->box, &bounds))
					graphene_rect_init (&bounds, 0, 0, 0, 0);

				if (bounds.origin.x < child_x)
					i = 0;
				else
					i++;

				child_x = (int) bounds.origin.x;

				if (i == column)
					child = sibling;
			}

			if (child)
			{
				gtk_widget_grab_focus (child);
				return TRUE;
			}
		}
	}

	return FALSE;
}

/* --- map --- */

static void
hex_emoji_chooser_map (GtkWidget *widget)
{
	HexEmojiChooser *chooser = HEX_EMOJI_CHOOSER (widget);

	GTK_WIDGET_CLASS (hex_emoji_chooser_parent_class)->map (widget);

	gtk_widget_grab_focus (chooser->search_entry);
}

/* --- class_init --- */

static void
hex_emoji_chooser_class_init (HexEmojiChooserClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	GBytes *ui_bytes;

	object_class->finalize = hex_emoji_chooser_finalize;
	object_class->dispose = hex_emoji_chooser_dispose;
	widget_class->show = hex_emoji_chooser_show;
	widget_class->map = hex_emoji_chooser_map;

	signals[EMOJI_PICKED] = g_signal_new ("emoji-picked",
	                                      G_OBJECT_CLASS_TYPE (object_class),
	                                      G_SIGNAL_RUN_LAST,
	                                      0,
	                                      NULL, NULL,
	                                      NULL,
	                                      G_TYPE_NONE, 1, G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

	ui_bytes = g_bytes_new_static (hex_emoji_chooser_ui, sizeof (hex_emoji_chooser_ui) - 1);
	gtk_widget_class_set_template (widget_class, ui_bytes);
	g_bytes_unref (ui_bytes);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, search_entry);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, stack);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, scrolled_window);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, recent.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, recent.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, people.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, people.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, people.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, body.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, body.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, body.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, nature.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, nature.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, nature.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, food.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, food.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, food.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, travel.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, travel.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, travel.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, activities.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, activities.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, activities.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, objects.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, objects.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, objects.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, symbols.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, symbols.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, symbols.button);

	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, flags.box);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, flags.heading);
	gtk_widget_class_bind_template_child (widget_class, HexEmojiChooser, flags.button);

	gtk_widget_class_bind_template_callback (widget_class, emoji_activated);
	gtk_widget_class_bind_template_callback (widget_class, search_changed);
	gtk_widget_class_bind_template_callback (widget_class, stop_search);
	gtk_widget_class_bind_template_callback (widget_class, pressed_cb);
	gtk_widget_class_bind_template_callback (widget_class, long_pressed_cb);
	gtk_widget_class_bind_template_callback (widget_class, keynav_failed);

	gtk_widget_class_install_action (widget_class, "scroll.section", "i",
	                                 hex_emoji_chooser_scroll_section);

	gtk_widget_class_add_binding_action (widget_class, GDK_KEY_n, GDK_CONTROL_MASK,
	                                     "scroll.section", "i", 1);
	gtk_widget_class_add_binding_action (widget_class, GDK_KEY_p, GDK_CONTROL_MASK,
	                                     "scroll.section", "i", -1);
}

/* --- Public API --- */

GtkWidget *
hex_emoji_chooser_new (void)
{
	return g_object_new (HEX_TYPE_EMOJI_CHOOSER, NULL);
}

void
hex_emoji_chooser_set_emoji_cache (HexEmojiChooser   *chooser,
                                    xtext_emoji_cache *cache)
{
	g_return_if_fail (HEX_IS_EMOJI_CHOOSER (chooser));
	chooser->emoji_cache = cache;

	/* Create our own cache at 32px for chooser display */
	if (cache && cache->sprite_dir)
	{
		if (chooser->chooser_cache)
			xtext_emoji_cache_free (chooser->chooser_cache);
		chooser->chooser_cache = xtext_emoji_cache_new (cache->sprite_dir, 32);

		/* Re-populate recent section now that sprites are available —
		 * it was initially populated before the cache was set. */
		{
			GtkWidget *child;
			while ((child = gtk_widget_get_first_child (chooser->recent.box)))
				gtk_flow_box_remove (GTK_FLOW_BOX (chooser->recent.box), child);
			populate_recent_section (chooser);
		}
	}
}
