#!/usr/bin/env bash
# Top-level driver: configure + build + stage-install + bundle into a .app
# and wrap as a .dmg. Designed to run on a macOS host with Homebrew deps
# already installed (see .github/workflows/macos-build.yml).
#
# Outputs:
#   dist/PoxChat.app
#   dist/PoxChat-<version>.dmg

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-builddir}"
STAGE_DIR="${STAGE_DIR:-$REPO_ROOT/build/stage}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
PREFIX="${PREFIX:-/usr/local}"

mkdir -p "$DIST_DIR"
rm -rf "$STAGE_DIR"

if [ ! -x "$(command -v brew)" ]; then
    echo "error: Homebrew is required (brew not in PATH)" >&2
    exit 1
fi

# openssl@3 is keg-only on macOS; surface its pkg-config files.
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "==> meson setup ($BUILD_DIR, prefix=$PREFIX)"
if [ ! -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" \
        --prefix="$PREFIX" \
        -Dgtk-frontend=true \
        -Dtext-frontend=false \
        -Dtls=enabled \
        -Dplugin=true \
        -Ddbus=disabled \
        -Dlibcanberra=disabled \
        -Dwith-libwebsockets=enabled \
        -Dwith-sysinfo=false \
        -Dwith-lua=false \
        -Dwith-perl=false \
        -Dwith-python=false \
        -Dwith-checksum=true \
        -Dwith-fishlim=true \
        -Dtheme-manager=false \
        -Delectron-frontend=false \
        -Dinstall-appdata=false
fi

echo "==> meson compile"
meson compile -C "$BUILD_DIR"

echo "==> meson install (DESTDIR=$STAGE_DIR)"
DESTDIR="$STAGE_DIR" meson install -C "$BUILD_DIR" --no-rebuild

# Project version from meson.build, plus a sanitized numeric form for
# CFBundleShortVersionString (Apple wants integers + dots only).
VERSION="$(awk -F"'" '/^[[:space:]]*version:/ {print $2; exit}' "$REPO_ROOT/meson.build")"
SHORT_VERSION="$(printf '%s' "$VERSION" | sed -E 's/[~-].*//; s/[^0-9.]//g')"
[ -n "$SHORT_VERSION" ] || SHORT_VERSION="0.0.0"

ICNS="${ICNS:-$REPO_ROOT/osx/poxchat.icns}"
if [ ! -f "$ICNS" ]; then
    echo "error: icon not found at $ICNS" >&2
    exit 1
fi

echo "==> Building PoxChat.app (version=$VERSION, short=$SHORT_VERSION)"
STAGE="$STAGE_DIR" \
PREFIX="$PREFIX" \
OUT_DIR="$DIST_DIR" \
VERSION="$VERSION" \
SHORT_VERSION="$SHORT_VERSION" \
ICNS="$ICNS" \
"$REPO_ROOT/macos/make-bundle.sh"

DMG="$DIST_DIR/PoxChat-${SHORT_VERSION}.dmg"
echo "==> Wrapping into DMG: $DMG"
APP_PATH="$DIST_DIR/PoxChat.app" \
OUT="$DMG" \
VOLNAME="PoxChat" \
"$REPO_ROOT/macos/make-dmg.sh"

echo "==> Done."
echo "    App: $DIST_DIR/PoxChat.app"
echo "    DMG: $DMG"
