/* hex-emoji-chooser.h: Emoji chooser with Twemoji sprite rendering
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

#ifndef HEX_EMOJI_CHOOSER_H
#define HEX_EMOJI_CHOOSER_H

#include <gtk/gtk.h>
#include "xtext-emoji.h"

G_BEGIN_DECLS

#define HEX_TYPE_EMOJI_CHOOSER (hex_emoji_chooser_get_type ())
G_DECLARE_FINAL_TYPE (HexEmojiChooser, hex_emoji_chooser, HEX, EMOJI_CHOOSER, GtkPopover)

GtkWidget *hex_emoji_chooser_new            (void);
void       hex_emoji_chooser_set_emoji_cache (HexEmojiChooser   *chooser,
                                               xtext_emoji_cache *cache);

/* Signal: "emoji-picked" (HexEmojiChooser *chooser, const char *text, gpointer user_data) */

G_END_DECLS

#endif /* HEX_EMOJI_CHOOSER_H */
