#!/usr/bin/env bash
# Inside-container build script. Assumes the repo is mounted at /src
# and that linuxdeploy / linuxdeploy-plugin-gtk / appimagetool are on PATH
# (provided by the Dockerfile in this directory).
#
# Outputs PoxChat-x86_64.AppImage in the repo root.

set -euo pipefail

cd "$(dirname "$0")/../.."

BUILDDIR="${BUILDDIR:-builddir-appimage}"
APPDIR="${APPDIR:-$PWD/AppDir}"
OUTPUT="${OUTPUT:-PoxChat-x86_64.AppImage}"

# linuxdeploy-plugin-gtk: select GTK4 bundling path
export DEPLOY_GTK_VERSION=4

echo "==> meson setup ($BUILDDIR)"
rm -rf "$BUILDDIR"
meson setup "$BUILDDIR" \
    --prefix=/usr \
    --buildtype=release \
    -Dwith-perl=false \
    -Dwith-python=python3 \
    -Dwith-libwebsockets=enabled \
    -Dinstall-appdata=true

echo "==> meson compile"
meson compile -C "$BUILDDIR"

echo "==> meson install -> $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" meson install -C "$BUILDDIR" --no-rebuild

echo "==> linuxdeploy: bundle deps + build AppImage"
export OUTPUT
linuxdeploy \
    --appdir "$APPDIR" \
    --plugin gtk \
    --desktop-file "$APPDIR/usr/share/applications/io.github.evilnet.PoxChat.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/io.github.evilnet.PoxChat.png" \
    --output appimage

echo "==> Built: $OUTPUT"
