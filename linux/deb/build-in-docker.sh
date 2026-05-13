#!/usr/bin/env bash
# Host-side wrapper: build the container image if missing, then run the
# inside-container build script with the repo mounted at /src. Produces
# .deb files in $REPO_ROOT/dist/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${IMAGE:-poxchat-deb-build:latest}"
DOCKER="${DOCKER:-docker}"

if ! command -v "$DOCKER" >/dev/null 2>&1; then
    echo "error: '$DOCKER' not found in PATH; install Docker, or set DOCKER=podman" >&2
    exit 1
fi

echo "==> Building image $IMAGE"
"$DOCKER" build -t "$IMAGE" "$REPO_ROOT/linux/deb"

mkdir -p "$REPO_ROOT/dist"

echo "==> Running build inside $IMAGE"
"$DOCKER" run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$REPO_ROOT/dist:/out" \
    --env BUILD_UID="$(id -u)" \
    --env BUILD_GID="$(id -g)" \
    "$IMAGE" \
    /src/linux/deb/build-deb.sh

echo "==> Artifacts in $REPO_ROOT/dist/:"
ls -1 "$REPO_ROOT/dist/"*.deb 2>/dev/null || echo "(no .deb files produced)"
