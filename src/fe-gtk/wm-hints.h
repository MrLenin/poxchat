/* PoxChat — window-manager hint abstraction.
 *
 * Properties like "visible on all workspaces" have no GTK4 API; each
 * windowing system needs its own implementation. Backends:
 *   X11  — _NET_WM_STATE_STICKY (EWMH)
 *   else — no-op (Wayland has no standard protocol for this yet)
 */

#ifndef POXCHAT_WM_HINTS_H
#define POXCHAT_WM_HINTS_H

#include <gtk/gtk.h>

gboolean wm_hints_supports_sticky (GtkWindow *win);
gboolean wm_hints_get_sticky (GtkWindow *win);
void     wm_hints_set_sticky (GtkWindow *win, gboolean sticky);

/* Outer (frame-relative) window geometry. GTK4 dropped window position
 * APIs; on X11 we go through Xlib to read/restore where the window is.
 * Returns FALSE if the backend can't supply geometry. */
gboolean wm_hints_get_geometry (GtkWindow *win, int *x, int *y, int *w, int *h);
void     wm_hints_set_position (GtkWindow *win, int x, int y);

/* Returns TRUE if (x, y) falls within any currently-connected monitor's
 * geometry — used to skip restoring a position that would be off-screen
 * (e.g. the second monitor was unplugged since last close). */
gboolean wm_hints_position_on_screen (GtkWindow *win, int x, int y);

#endif /* POXCHAT_WM_HINTS_H */
