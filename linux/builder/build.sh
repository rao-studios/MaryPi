#!/bin/sh
# MaryOS image builder: Ubuntu Noble packages + distro/ -> bootable images.
# Runs as root on an arm64 Linux host, or hands itself to Docker
# (builder/run-in-docker.sh) from a Mac.
#
#   builder/build.sh rootfs                base rootfs tarball (cached by content of distro/)
#   builder/build.sh target <pi5|vm>       base + target packages, overlay, hooks -> rootfs tree
#   builder/build.sh image  <pi5|vm>       rootfs tree -> out/<id>-<version>-<target>.img (vm: + out/vm/)
#   builder/build.sh all    [pi5|vm|both]  rootfs, target, image (default: both targets)
#
#   --fresh     rebuild the base rootfs even when a cached one matches
#   --keep      leave the rootfs tree in the work directory after the image
#   --dry-run   print the stages that would run
#
# Directories (overridable): MARYOS_CACHE (linux/cache), MARYOS_WORK (linux/work),
# MARYOS_OUT (linux/out). Inside Docker, cache and work are named volumes.
set -eu

BUILDER_DIR=$(cd "$(dirname "$0")" && pwd)
KIT_DIR=$(cd "$BUILDER_DIR/.." && pwd)
DISTRO_DIR=$KIT_DIR/distro
CACHE_DIR=${MARYOS_CACHE:-$KIT_DIR/cache}
WORK_DIR=${MARYOS_WORK:-$KIT_DIR/work}
OUT_DIR=${MARYOS_OUT:-$KIT_DIR/out}
FRESH=0
KEEP=0
DRY_RUN=0

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

CMD=${1:-}
[ -n "$CMD" ] || usage 1
shift
FLAGS=""
POSITIONAL=""
while [ $# -gt 0 ]; do
    case $1 in
        --fresh) FRESH=1; FLAGS="$FLAGS --fresh" ;;
        --keep) KEEP=1; FLAGS="$FLAGS --keep" ;;
        --dry-run) DRY_RUN=1; FLAGS="$FLAGS --dry-run" ;;
        -h|--help) usage 0 ;;
        -*) echo "build: unknown option $1" >&2; usage 1 ;;
        *) POSITIONAL="$POSITIONAL $1" ;;
    esac
    shift
done
# shellcheck disable=SC2086
set -- $POSITIONAL

# Not root on Linux: this is a Mac (or a plain user); build inside Docker.
if [ "$(uname -s)" != Linux ] || [ "$(id -u)" != 0 ]; then
    if [ -n "${MARYOS_IN_CONTAINER:-}" ]; then
        echo "build: must run as root inside the container" >&2
        exit 1
    fi
    # shellcheck disable=SC2086
    exec "$BUILDER_DIR/run-in-docker.sh" "$CMD" $FLAGS "$@"
fi

# shellcheck disable=SC1091
. "$BUILDER_DIR/lib/common.sh"
# shellcheck disable=SC1091
. "$BUILDER_DIR/lib/chroot.sh"
# shellcheck disable=SC1091
. "$BUILDER_DIR/lib/rootfs.sh"
# shellcheck disable=SC1091
. "$BUILDER_DIR/lib/target.sh"
# shellcheck disable=SC1091
. "$BUILDER_DIR/lib/image.sh"

load_distro
if [ "$ARCH" = arm64 ] && [ "$(uname -m)" != aarch64 ]; then
    die "building arm64 needs an arm64 host; this one is $(uname -m)"
fi
mkdir -p "$CACHE_DIR" "$WORK_DIR" "$OUT_DIR"

targets_of() {
    case ${1:-both} in
        pi5|vm) printf '%s' "$1" ;;
        both|all) printf 'pi5 vm' ;;
        *) die "unknown target $1 (pi5, vm or both)" ;;
    esac
}

stage() {
    log "stage: $*"
    [ "$DRY_RUN" = 1 ] && return 0
    "$@"
}

case $CMD in
    rootfs)
        stage rootfs_build ;;
    target)
        t=$(targets_of "${1:?usage: build.sh target <pi5|vm>}")
        [ "$t" = "pi5 vm" ] && die "target takes one of pi5, vm"
        stage target_build "$t" ;;
    image)
        t=$(targets_of "${1:?usage: build.sh image <pi5|vm>}")
        [ "$t" = "pi5 vm" ] && die "image takes one of pi5, vm"
        stage image_build "$t" ;;
    all)
        for t in $(targets_of "${1:-both}"); do
            stage rootfs_build
            stage target_build "$t"
            stage image_build "$t"
        done ;;
    -h|--help|help)
        usage 0 ;;
    *)
        die "unknown command $CMD (rootfs, target, image, all)" ;;
esac
