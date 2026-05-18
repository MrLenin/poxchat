#!/bin/sh
# Trampoline executed as Contents/MacOS/PoxChat. Sets the env vars GTK4
# needs to find its runtime data inside the bundle, then execs the real
# binary (Contents/MacOS/PoxChat-bin).

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
RES="$HERE/../Resources"
FW="$HERE/../Frameworks"

export DYLD_FRAMEWORK_PATH="$FW${DYLD_FRAMEWORK_PATH:+:$DYLD_FRAMEWORK_PATH}"
export GDK_PIXBUF_MODULE_FILE="$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GIO_EXTRA_MODULES="$RES/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$RES/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$RES/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
export XDG_CONFIG_DIRS="$RES/etc/xdg${XDG_CONFIG_DIRS:+:$XDG_CONFIG_DIRS}"

# Finder passes -psn_X_Y on launch; strip it so poxchat doesn't choke.
case "${1:-}" in
    -psn_*) shift ;;
esac

exec "$HERE/PoxChat-bin" "$@"
