# Input Widget Modernization

`HexInputEdit` (`src/fe-gtk/hex-input-edit.c`, `src/fe-gtk/hex-input-edit.h`)
is a from-scratch GTK4 entry widget built on top of the xtext rendering
pipeline (Pango + Cairo + emoji sprites). It replaces `GtkText`/`GtkEntry` to
gain control over emoji sprites, color codes, and selection appearance — but
in doing so it skips a number of behaviors that `GtkText` provides for free.

This document audits what's missing relative to native entries and tracks the
work to close those gaps.

## Priority ordering (rough)

1. Primary selection + middle-click paste (item 1)
2. Drag-and-drop receive (item 2)
3. Accessibility (item 4)
4. Emoji chooser shortcut (item 5)
5. Everything else

---

## 1. Primary selection sync + middle-click paste

The widget only registers `GtkGestureClick` controllers for
`GDK_BUTTON_PRIMARY` and `GDK_BUTTON_SECONDARY`
(`hex-input-edit.c:2540-2553`). Middle button is unbound. `do_copy()` and
`do_cut()` (`hex-input-edit.c:1030,1045`) write only to the CLIPBOARD
selection via `gdk_display_get_clipboard()` — there is no use of
`gdk_display_get_primary_clipboard()` anywhere in this file. As a result, on
Linux:

- Selecting text inside the input does not populate PRIMARY, so other apps
  can't middle-click-paste from us.
- Middle-clicking the input does nothing — no insertion from PRIMARY.

`src/fe-gtk/gtkutil.c:709,717` already uses the primary clipboard elsewhere,
so the pattern is established in the codebase.

- [ ] 1a. Refactor `do_paste()` (`hex-input-edit.c:1062`) to take a
      `GdkClipboard *` argument, so it can paste from either CLIPBOARD or
      PRIMARY. Existing callers pass `gdk_display_get_clipboard(...)`.
- [ ] 1b. Add a helper `push_selection_to_primary(HexInputEdit *)` that
      grabs the current selection text and calls `gdk_clipboard_set_text()`
      on the primary clipboard. No-op when there is no selection.
- [ ] 1c. Call the helper from every place the selection changes:
      `drag_update_cb`, `click_pressed_cb` n_press==2 (word-select),
      `click_pressed_cb` n_press==3 (select-all), `do_select_all`, the
      Shift+arrow key paths in `key_pressed_cb`, and the
      Shift+Home/Shift+End paths.
- [ ] 1d. Add a third `GtkGestureClick` with
      `gtk_gesture_single_set_button(GDK_BUTTON_MIDDLE)`. In its `pressed`
      handler: move the cursor to the click position with
      `xy_to_byte_offset()` (matching `click_pressed_cb` behavior at
      `hex-input-edit.c:1806`), clear any selection, then paste from the
      primary clipboard via the refactored `do_paste`.
- [ ] 1e. Test: select in another app → middle-click in input → text
      inserts at click point. Select in our input → middle-click in another
      app (or another instance) → text inserts there.

## 2. Drag-and-drop receive (text / URLs / files)

No `GtkDropTarget` is attached to the input. `src/fe-gtk/maingui.c:5340` and
`src/fe-gtk/userlistgui.c:1292` show the rest of the app accepting drops, but
the input silently rejects dragged text, URLs, and files. Dragging a URL
from a browser onto the input — which would natively insert the URL —
currently does nothing.

- [ ] 2a. Attach a `GtkDropTarget` accepting `G_TYPE_STRING` (text/plain,
      text/uri-list) for inserting text/URL at the drop position.
- [ ] 2b. Decide policy for `G_TYPE_FILE` drops onto the input. Options:
      insert the filename, insert a `file://` URI, trigger a DCC SEND
      offer (matches userlist behavior at `userlistgui.c:1292`), or
      reject. Probably "insert URI" is the least surprising default; DCC
      should require dropping on a user/channel, not on the input box.
- [ ] 2c. Hit-test the drop coordinate with `xy_to_byte_offset()` so text
      is inserted at the drop point, not the current cursor.

## 3. Drag-and-drop source (drag selection out)

There's no `GtkDragSource` on the selection, so you can't drag selected text
out of the input into other applications or rearrange it within the field.

- [ ] 3a. Attach a `GtkDragSource` whose content provider returns the
      currently selected text. Activate it only when a drag starts on a
      point inside the existing selection (so plain drags on unselected
      text still extend selection via the existing `drag_update_cb`).
- [ ] 3b. On successful `GDK_ACTION_MOVE` drop, delete the source selection
      from the input (matching `GtkText` semantics).

## 4. Accessibility

`GtkText` declares `GTK_ACCESSIBLE_ROLE_TEXT_BOX` and pushes value, caret,
and selection updates to ATK / AT-SPI. This widget makes no
`gtk_accessible_*` calls and sets no role, so screen readers (Orca on Linux,
NVDA via the AT-SPI bridge) see an opaque custom widget — they can't read
what you're typing, announce the caret, or convey the selection.

- [ ] 4a. Set `GTK_ACCESSIBLE_ROLE_TEXT_BOX` (or `_SEARCH_BOX`?) via
      `gtk_widget_class_set_accessible_role()` in `class_init`.
- [ ] 4b. Push value updates with
      `gtk_accessible_update_property(GTK_ACCESSIBLE_PROPERTY_VALUE_TEXT,
      …)` whenever `priv->text` changes.
- [ ] 4c. Push caret/selection state via
      `gtk_accessible_update_state(GTK_ACCESSIBLE_STATE_SELECTED, …)` and
      the appropriate caret-position property — check exactly which
      GTK4 properties `GtkText` uses and mirror them.
- [ ] 4d. Manual test with Orca: type, select, copy/paste, navigate, and
      confirm the spoken output matches what `GtkText` would say.

## 5. Emoji chooser shortcut

`GtkText` binds `Ctrl+.` / `Ctrl+;` to an `insert-emoji` action that pops up
`GtkEmojiChooser`. The actions installed at `hex-input-edit.c:2619-2627`
don't include one. Given that emoji rendering is the marquee feature of
this widget, the absence of an emoji *picker* is conspicuous.

- [ ] 5a. Install an `edit.insert-emoji` action via
      `gtk_widget_class_install_action()` that constructs a
      `GtkEmojiChooser`, anchors it at the caret position, and inserts the
      chosen emoji at the cursor.
- [ ] 5b. Bind `Ctrl+.` and `Ctrl+;` to the action via
      `gtk_widget_class_add_binding_action()`.
- [ ] 5c. Append an "Insert Emoji" item to the right-click context menu
      (currently built in `show_context_menu`, populated at
      `hex-input-edit.c:1422`).

## 6. Input-methods menu in context menu

`GtkEntry`'s right-click menu includes "Input Methods" and "Insert Unicode
Control Character" submenus, driven by
`gtk_im_multicontext_append_menuitems`. Not present here. Matters for
users on non-Latin IME configurations.

- [ ] 6a. Append the IM submenu to the right-click `GMenu` in
      `show_context_menu`. (GTK4 changed the API from GTK3 — verify the
      modern equivalent; may need to build the submenu manually from
      `gtk_im_multicontext_get_context_ids()`.)
- [ ] 6b. Append a Unicode-control-characters submenu (LRM/RLM/ZWJ/ZWNJ
      etc.) — or decide it's not worth the surface area and skip.

## 7. Theme integration (caret blink, selection color)

The widget runs its own caret blink timer and sources colors from the xtext
palette. This means it ignores:

- `gtk-cursor-blink` (whether caret blinks at all)
- `gtk-cursor-blink-time` (period)
- `gtk-cursor-blink-timeout` (when to stop blinking after idle)
- Theme selection background/foreground

This is a deliberate tradeoff — we want the input to match xtext's color
scheme, not the GTK theme. But the blink settings probably should be
honored even if the colors aren't.

- [ ] 7a. Read `gtk-cursor-blink`, `-time`, `-timeout` from
      `GtkSettings` and feed them into the existing blink timer. Disable
      blinking entirely when `gtk-cursor-blink` is FALSE.
- [ ] 7b. Decide whether selection color should follow theme or palette.
      Currently it follows the palette; leaving this as-is is defensible
      since xtext does the same.

## 8. BiDi / RTL correctness

`GtkText` handles logical-vs-visual cursor movement around RTL runs,
mirrored arrow-key meaning (right-arrow moves to end-of-run in RTL), and
Unicode override character insertion. Custom byte-offset-driven cursor
code typically doesn't get this right by default.

- [ ] 8a. Test with Hebrew and Arabic text: arrow keys, Home/End,
      Shift+selection, double-click word selection, mouse positioning.
      Document what breaks.
- [ ] 8b. If broken: switch cursor movement to use Pango's
      `pango_layout_move_cursor_visually()` instead of raw byte stepping.

## 9. Touch / long-press selection

`GtkText` shows touch selection bubbles and a long-press popover with
cut/copy/paste/select-all. No `GtkGestureLongPress` is registered here.
Not relevant on a typical desktop but matters for touchscreens and Wayland
tablets.

- [ ] 9a. Decide whether PoxChat supports touch as a target. If yes, add a
      `GtkGestureLongPress` that shows a popover with the same actions as
      the right-click context menu. If no, document the decision and
      close the item.

## 10. Undo coalescing audit

`GtkText` coalesces consecutive keystrokes into one undo step and breaks at
word boundaries / IM commit. Verify what `priv->undo_stack`
(`hex-input-edit.c:1072+`) does today.

- [ ] 10a. Type a sentence, press Ctrl+Z. Does it undo the whole sentence,
      one word, or one character? Native entries undo by word.
- [ ] 10b. If per-character, add coalescing: extend the current undo
      snapshot when the new input is contiguous and not whitespace.

## 11. Selection auto-scroll during drag past widget edges

If you drag-select past the right edge of the widget on a multi-line or
overflowing input, native entries auto-scroll. Verify the custom one does.

- [ ] 11a. Type a long line, drag from the middle to past the right edge,
      hold. Does the selection extend and the view scroll?
- [ ] 11b. If not: in `drag_update_cb` (`hex-input-edit.c:1880`), detect
      drag past widget bounds and start a scroll-while-held tick callback.

## 12. Overwrite mode (Insert key)

Minor. `GtkText` toggles overwrite mode with the Insert key. Custom widget
doesn't.

- [ ] 12a. Add an `overwrite` boolean to `HexInputEditPriv`, toggle it on
      Insert key, and change `insert_at_cursor`/caret rendering when true.
      Caret should render as a block in overwrite mode.

---

## Out of scope (intentional non-goals)

- **Password mode / invisible char.** Not relevant for chat input.
- **`GtkEntryCompletion` API surface.** PoxChat has tab-completion in
  `fkeys.c`; the GTK completion popdown protocol is a separate concept
  and probably not worth re-creating.
- **Stylus pressure events.** Not meaningful for a text entry.
