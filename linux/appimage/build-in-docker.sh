#!/usr/bin/env bash
# Host-side wrapper: build the container image if missing, then run the
# inside-container build script with the repo mounted at /src.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${IMAGE:-poxchat-appimage-build:latest}"
DOCKER="${DOCKER:-docker}"

if ! command -v "$DOCKER" >/dev/null 2>&1; then
    echo "error: '$DOCKER' not found in PATH; install Docker, or set DOCKER=podman" >&2
    exit 1
fi

echo "==> Building image $IMAGE"
"$DOCKER" build -t "$IMAGE" "$REPO_ROOT/linux/appimage"

echo "==> Running build inside $IMAGE"
"$DOCKER" run --rm \
    -v "$REPO_ROOT:/src" \
    -w /src \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    "$IMAGE" \
    /src/linux/appimage/build-appimage.sh

echo "==> Artifact: $REPO_ROOT/PoxChat-x86_64.AppImage"
