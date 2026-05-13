#!/usr/bin/env bash
# Inside-container build script. Assumes the repo is read-only mounted at
# /src and that /out is writable. Produces binary .deb files in /out.

set -euo pipefail

SRC=/src
WORK=/build
OUT=/out

# Derive upstream version from the changelog so this script does not need
# to be edited on version bumps.
VERSION="$(dpkg-parsechangelog -l "$SRC/debian/changelog" --show-field Version | sed 's/-.*//')"
PKG=poxchat
TREE="$WORK/${PKG}-${VERSION}"

echo "==> Staging source tree at $TREE"
rm -rf "$TREE"
mkdir -p "$TREE"
tar -C "$SRC" \
    --exclude='./.git' \
    --exclude='./builddir*' \
    --exclude='./AppDir' \
    --exclude='./*.AppImage' \
    --exclude='./dist' \
    --exclude='./flatpak/shared-modules' \
    --exclude='./external/tray' \
    -cf - . \
  | tar -C "$TREE" -xf -

echo "==> Synthesising orig tarball"
tar -C "$WORK" --exclude="${PKG}-${VERSION}/debian" \
    -czf "$WORK/${PKG}_${VERSION}.orig.tar.gz" "${PKG}-${VERSION}"

echo "==> Installing build-deps from debian/control"
cd "$TREE"
apt-get update
# mk-build-deps writes a synthetic .deb of Build-Depends and installs it.
mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control

echo "==> dpkg-buildpackage"
dpkg-buildpackage -b -us -uc

echo "==> lintian (informational, build does not fail on warnings yet)"
cd "$WORK"
lintian -i -E --pedantic ${PKG}_*_*.changes || true

echo "==> Copying artefacts to $OUT"
cp "$WORK"/*.deb "$OUT/"

if [ -n "${BUILD_UID:-}" ]; then
    chown -R "${BUILD_UID}:${BUILD_GID:-$BUILD_UID}" "$OUT"
fi

echo "==> Done. Produced:"
ls -1 "$OUT"/*.deb
