#!/usr/bin/env bash
# Build an unsigned PoxChat.app from a staged meson install.
#
# Inputs:
#   STAGE      - dir containing the staged install (DESTDIR + prefix layout).
#                Must contain bin/poxchat, lib/poxchat/plugins/*, share/...
#   PREFIX     - the meson prefix used at configure time (e.g. /usr/local).
#                Used to locate files inside $STAGE.
#   OUT_DIR    - where PoxChat.app should be written.
#   VERSION    - full project version (CFBundleVersion).
#   SHORT_VERSION - sanitized numeric-only version (CFBundleShortVersionString).
#   ICNS       - path to the .icns icon to embed.
#
# Requires: dylibbundler, gdk-pixbuf-query-loaders, glib-compile-schemas,
# Homebrew-installed gtk4 / glib / adwaita-icon-theme.

set -euo pipefail

: "${STAGE:?STAGE must be set}"
: "${PREFIX:?PREFIX must be set}"
: "${OUT_DIR:?OUT_DIR must be set}"
: "${VERSION:?VERSION must be set}"
: "${SHORT_VERSION:?SHORT_VERSION must be set}"
: "${ICNS:?ICNS must be set}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP="$OUT_DIR/PoxChat.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RES="$CONTENTS/Resources"
FW="$CONTENTS/Frameworks"

BREW_PREFIX="$(brew --prefix)"
GTK4_PREFIX="$(brew --prefix gtk4)"
GDK_PIXBUF_PREFIX="$(brew --prefix gdk-pixbuf)"
GLIB_PREFIX="$(brew --prefix glib)"
ADWAITA_PREFIX="$(brew --prefix adwaita-icon-theme)"
HICOLOR_PREFIX="$(brew --prefix hicolor-icon-theme 2>/dev/null || true)"

echo "==> Resetting $APP"
rm -rf "$APP"
mkdir -p "$MACOS" "$RES" "$FW"

echo "==> Copying staged install into Resources"
# The meson prefix lives at $STAGE$PREFIX; mirror its share/, lib/, etc.
# directly into Resources/.
STAGED="$STAGE$PREFIX"
if [ ! -d "$STAGED" ]; then
    echo "error: staged install not found at $STAGED" >&2
    exit 1
fi
# Copy everything except bin/ (we relocate the binary below) and any
# Linux-only subtrees we wouldn't ship.
for sub in lib share etc; do
    if [ -d "$STAGED/$sub" ]; then
        mkdir -p "$RES/$sub"
        cp -R "$STAGED/$sub/." "$RES/$sub/"
    fi
done

echo "==> Relocating main binary to Contents/MacOS"
if [ ! -f "$STAGED/bin/poxchat" ]; then
    echo "error: no poxchat binary at $STAGED/bin/poxchat" >&2
    exit 1
fi
cp "$STAGED/bin/poxchat" "$MACOS/PoxChat-bin"
chmod +x "$MACOS/PoxChat-bin"

echo "==> Installing launcher trampoline"
install -m 0755 "$SCRIPT_DIR/launcher.sh" "$MACOS/PoxChat"

echo "==> Bundling GTK runtime data (gdk-pixbuf, gio, schemas, icons)"

# gdk-pixbuf loaders
GP_VER="2.10.0"
mkdir -p "$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders"
if [ -d "$GDK_PIXBUF_PREFIX/lib/gdk-pixbuf-2.0/$GP_VER/loaders" ]; then
    cp "$GDK_PIXBUF_PREFIX"/lib/gdk-pixbuf-2.0/$GP_VER/loaders/*.so \
       "$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders/" 2>/dev/null || true
fi

# GIO modules (TLS backend, etc.) — copy whatever Homebrew shipped.
mkdir -p "$RES/lib/gio/modules"
if [ -d "$GLIB_PREFIX/lib/gio/modules" ]; then
    cp "$GLIB_PREFIX"/lib/gio/modules/*.so \
       "$RES/lib/gio/modules/" 2>/dev/null || true
fi

# GSettings schemas — merge poxchat's (if any) + glib's + gtk4's.
mkdir -p "$RES/share/glib-2.0/schemas"
for src in \
    "$GLIB_PREFIX/share/glib-2.0/schemas" \
    "$GTK4_PREFIX/share/glib-2.0/schemas"; do
    if [ -d "$src" ]; then
        cp "$src"/*.gschema.xml "$RES/share/glib-2.0/schemas/" 2>/dev/null || true
        cp "$src"/*.enums.xml   "$RES/share/glib-2.0/schemas/" 2>/dev/null || true
    fi
done
glib-compile-schemas "$RES/share/glib-2.0/schemas"

# Icon themes — Adwaita is required for GTK4 symbolic icons.
mkdir -p "$RES/share/icons"
if [ -d "$ADWAITA_PREFIX/share/icons/Adwaita" ]; then
    cp -R "$ADWAITA_PREFIX/share/icons/Adwaita" "$RES/share/icons/"
fi
if [ -n "$HICOLOR_PREFIX" ] && [ -d "$HICOLOR_PREFIX/share/icons/hicolor" ]; then
    # Merge hicolor with whatever poxchat installed (it ships poxchat icons
    # under hicolor); cp -R into an existing dir won't clobber.
    cp -R "$HICOLOR_PREFIX/share/icons/hicolor" "$RES/share/icons/" 2>/dev/null || true
fi
# Refresh icon theme caches so GTK doesn't fall back to slow per-file lookup.
for theme in "$RES/share/icons/"*/; do
    [ -d "$theme" ] && gtk4-update-icon-cache -f -t "$theme" >/dev/null 2>&1 || true
done

echo "==> Writing Info.plist"
sed -e "s/@VERSION@/${VERSION}/g" \
    -e "s/@SHORT_VERSION@/${SHORT_VERSION}/g" \
    "$SCRIPT_DIR/Info.plist.in" > "$CONTENTS/Info.plist"
printf 'APPL????' > "$CONTENTS/PkgInfo"

echo "==> Embedding icon"
cp "$ICNS" "$RES/poxchat.icns"

echo "==> Bundling dylibs via dylibbundler"
# Collect every Mach-O we shipped so dylibbundler rewrites all of them.
declare -a EXTRAS=()
while IFS= read -r f; do EXTRAS+=("-x" "$f"); done < <(
    find "$RES/lib" -type f \( -name '*.so' -o -name '*.dylib' \)
)
# -od overwrite, -b skip frameworks (we use plain dylibs),
# -cd codesign-id (we leave unsigned), -p sets the @executable_path prefix.
dylibbundler \
    -od -b \
    -x "$MACOS/PoxChat-bin" \
    "${EXTRAS[@]}" \
    -d "$FW/" \
    -p "@executable_path/../Frameworks/"

echo "==> Regenerating gdk-pixbuf loaders.cache for bundled paths"
# Run with GDK_PIXBUF_MODULEDIR pointing at the bundle so the cache has
# @executable_path-relative entries, not /opt/homebrew ones.
GDK_PIXBUF_MODULEDIR="$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders" \
    gdk-pixbuf-query-loaders \
    > "$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders.cache"
# Rewrite absolute loader paths to a runtime-relative form. Both forms work
# because the launcher exports GDK_PIXBUF_MODULE_FILE, but keeping the cache
# free of build-host paths makes the bundle self-contained.
sed -i.bak \
    -e "s|$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders/|@executable_path/../Resources/lib/gdk-pixbuf-2.0/$GP_VER/loaders/|g" \
    "$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders.cache"
rm -f "$RES/lib/gdk-pixbuf-2.0/$GP_VER/loaders.cache.bak"

echo "==> Built $APP"
