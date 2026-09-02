"""Self-contained repro for GDK/Win32 un-maximizing a toplevel when a popover opens
(the defect gtk4-0003-win32-maximized-toplevel-keeps-its-size.patch fixes).

Needs PyGObject on top of the GTK under test, e.g. MSYS2:
  pacman -S mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-python-gobject
  /ucrt64/bin/python gtk4-0003-repro.py
Fails on stock GTK 4.20.3 and 4.22.4 (2026-09-01).

Sequence, all in-process, no keyboard:
  t+1.0s  maximize the way the title bar does (ShowWindow SW_MAXIMIZE on the HWND)
  t+2.5s  open a GtkPopover
  t+4.0s  report IsZoomed + GetWindowRect, then restore and resize, report again, quit.
Exit code 0 = stayed maximized and resize stuck; 1 otherwise.
"""
import ctypes, sys, os
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Gdk

user32 = ctypes.windll.user32
SW_MAXIMIZE, SW_RESTORE = 3, 9

class RECT(ctypes.Structure):
    _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]

def rect(h):
    r = RECT(); user32.GetWindowRect(h, ctypes.byref(r)); return (r.l, r.t, r.r - r.l, r.b - r.t)

def hwnd_of(win):
    # GdkWin32Surface.get_handle() is exposed to introspection
    surf = win.get_surface()
    return int(surf.get_handle()) if hasattr(surf, "get_handle") else user32.FindWindowW(None, win.get_title())

result = {"ok": True}
def log(*a):
    print(*a, flush=True)

def main():
    app = Gtk.Application(application_id="org.poxchat.MaximizeRepro", flags=0)
    def on_activate(app):
        win = Gtk.ApplicationWindow(application=app, title="MaximizeRepro")
        win.set_default_size(600, 400)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        btn = Gtk.MenuButton(label="menu")
        pop = Gtk.Popover(); pop.set_child(Gtk.Label(label="popover content"))
        btn.set_popover(pop)
        box.append(btn); box.append(Gtk.Label(label="GTK %d.%d.%d" % (Gtk.get_major_version(), Gtk.get_minor_version(), Gtk.get_micro_version())))
        win.set_child(box)
        win.present()

        def step1():
            h = hwnd_of(win); log("hwnd", hex(h), "before:", rect(h), "zoomed", bool(user32.IsZoomed(h)))
            user32.ShowWindow(h, SW_MAXIMIZE); return False
        def step2():
            h = hwnd_of(win); log("maximized:", rect(h), "zoomed", bool(user32.IsZoomed(h)))
            pop.popup(); return False
        def step3():
            h = hwnd_of(win); r = rect(h); z = bool(user32.IsZoomed(h))
            log("after popover:", r, "zoomed", z)
            if not z or r[2] < 1000: result["ok"] = False; log("FAIL: window lost its maximized size when the popover opened")
            pop.popdown()
            user32.ShowWindow(h, SW_RESTORE)
            GLib.timeout_add(500, step4); return False
        def step4():
            h = hwnd_of(win); log("restored:", rect(h), "zoomed", bool(user32.IsZoomed(h)))
            user32.SetWindowPos(h, 0, 0, 0, 900, 700, 0x0016)
            GLib.timeout_add(1500, step5); return False
        def step5():
            h = hwnd_of(win); r = rect(h); log("after resize to 900x700:", r)
            if (r[2], r[3]) != (900, 700): result["ok"] = False; log("FAIL: resize did not stick")
            app.quit(); return False
        GLib.timeout_add(1000, step1); GLib.timeout_add(2500, step2); GLib.timeout_add(4000, step3)
    app.connect("activate", on_activate)
    app.run([])
    log("RESULT:", "PASS" if result["ok"] else "FAIL")
    sys.exit(0 if result["ok"] else 1)

main()
