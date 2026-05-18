#!/usr/bin/env bash
# Wrap PoxChat.app into a compressed read-only DMG via hdiutil.
#
# Inputs:
#   APP_PATH  - path to PoxChat.app
#   OUT       - output .dmg path
#   VOLNAME   - DMG volume name shown in Finder (default: PoxChat)

set -euo pipefail

: "${APP_PATH:?APP_PATH must be set}"
: "${OUT:?OUT must be set}"
VOLNAME="${VOLNAME:-PoxChat}"

if [ ! -d "$APP_PATH" ]; then
    echo "error: $APP_PATH not found" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP_PATH" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

rm -f "$OUT"
hdiutil create \
    -volname "$VOLNAME" \
    -srcfolder "$STAGE" \
    -ov \
    -format UDZO \
    "$OUT"

echo "==> Wrote $OUT"
