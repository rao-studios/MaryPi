#!/bin/sh
# Run the MaryOS builder inside an arm64 Ubuntu container. build.sh calls
# this itself when it is not root on Linux, so on a Mac you never run it
# directly. Mounts:
#   linux/               -> /work    distro/ and builder/ are read, out/ is written
#   volume maryos-cache  -> /cache   apt downloads and base rootfs tarballs (survive rebuilds)
#   volume maryos-work   -> /build   rootfs trees and partition images (fast, never on the bind mount)
set -eu
BUILDER_DIR=$(cd "$(dirname "$0")" && pwd)
KIT_DIR=$(cd "$BUILDER_DIR/.." && pwd)
DOCKER=${MARYOS_DOCKER:-docker}
IMAGE=${MARYOS_BUILDER_IMAGE:-maryos-builder}

command -v "$DOCKER" > /dev/null 2>&1 || { echo "build: $DOCKER not found; install Docker Desktop" >&2; exit 1; }
"$DOCKER" info > /dev/null 2>&1 || { echo "build: the Docker daemon is not reachable; start Docker Desktop" >&2; exit 1; }

echo "build: preparing the builder image ($IMAGE)" >&2
"$DOCKER" build --platform linux/arm64 -t "$IMAGE" "$BUILDER_DIR" > /dev/null

tty_flags=""
if [ -t 0 ] && [ -t 1 ]; then tty_flags="-it"; fi
git_sha=$(git -C "$KIT_DIR" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
if [ -n "$(git -C "$KIT_DIR" status --porcelain -- . 2>/dev/null)" ]; then git_sha="$git_sha-dirty"; fi

# Finished images go to MARYOS_OUT when the caller sets it (the app bundle
# does, so nothing lands inside the bundle), else to linux/out.
out_mount="-v $KIT_DIR/out:/out"
if [ -n "${MARYOS_OUT:-}" ]; then
    mkdir -p "$MARYOS_OUT"
    out_mount="-v $MARYOS_OUT:/out"
else
    mkdir -p "$KIT_DIR/out"
fi

# shellcheck disable=SC2086
exec "$DOCKER" run --rm $tty_flags --privileged --platform linux/arm64 \
    -v "$KIT_DIR:/work" \
    $out_mount \
    -v maryos-cache:/cache \
    -v maryos-work:/build \
    -e MARYOS_IN_CONTAINER=1 \
    -e MARYOS_CACHE=/cache -e MARYOS_WORK=/build -e MARYOS_OUT=/out \
    -e MARYOS_GIT_SHA="$git_sha" \
    "$IMAGE" "$@"
