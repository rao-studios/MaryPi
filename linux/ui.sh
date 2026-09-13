#!/bin/sh
# Boot MaryOS's desktop (Liquid Platinum, maryui-desktop) in a Virtualization.framework
# window with one command. vm.sh boots the same image to a login prompt.
#
#   linux/ui.sh                  compile the desktop and Mary (make ui mary), build the VM image if missing, boot the fresh desktop
#   linux/ui.sh --no-build       boot without compiling, running whatever out/ui already holds
#   linux/ui.sh --image          boot the desktop embedded in the image instead of out/ui (no live reload)
#   linux/ui.sh --rebuild        rebuild the image (embedding the fresh desktop) and boot it from a fresh disk
#   linux/ui.sh --no-microphone  keep this Mac's microphone from the VM (by default Mary can hear you; macOS asks once)
#   linux/ui.sh stop|status|serial|reset
#
# Every run compiles first, so what boots is what MaryUI holds now. The desktop runs from
# out/ui over virtiofs and restarts whenever `make ui` replaces it; with a VM already
# running, ui.sh only compiles and that VM picks the new build up. --dev and --rebuild-ui
# are still accepted and are the default. Any other option goes to `maryos vm run --desktop`.
# MARYUI_DIR=/path/to/MaryUI compiles that checkout instead of the submodule.
set -eu
cd "$(dirname "$0")"
SWIFT=${SWIFT:-swift}
CLI=.build/debug/maryos
UI=out/ui/usr/bin/maryui-desktop

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case ${1:-} in
    -h|--help|help) usage 0 ;;
esac

echo "ui.sh: building the maryos CLI" >&2
"$SWIFT" build --product maryos 2>&1 | grep -E "error|warning: unre" || true
[ -x "$CLI" ] || { echo "ui.sh: $CLI was not built" >&2; exit 1; }

case ${1:-} in
    stop|status|serial|reset)
        cmd=$1
        shift
        exec "$CLI" vm "$cmd" "$@" ;;
esac

# Take our own flags out of the arguments, keep everything else for `vm run`.
REBUILD=0
BUILD=1
IMAGE=0
MIC=1
n=$#
i=0
while [ "$i" -lt "$n" ]; do
    a=$1
    shift
    i=$((i + 1))
    case $a in
        --rebuild) REBUILD=1; IMAGE=1 ;;
        --no-build) BUILD=0 ;;
        --image) IMAGE=1 ;;
        --no-microphone) MIC=0 ;;
        --dev|--rebuild-ui) ;;
        *) set -- "$@" "$a" ;;
    esac
done

# Docker Desktop is often not running; start it rather than fail the build.
need_docker() {
    docker info > /dev/null 2>&1 && return 0
    echo "ui.sh: starting Docker Desktop" >&2
    open -a Docker 2> /dev/null || { echo "ui.sh: Docker Desktop is not installed" >&2; exit 1; }
    tries=0
    until docker info > /dev/null 2>&1; do
        tries=$((tries + 1))
        [ "$tries" -le 90 ] || { echo "ui.sh: Docker did not come up; start it and retry" >&2; exit 1; }
        sleep 2
    done
}

running=0
"$CLI" vm status 2> /dev/null | grep -q '^status: *running' && running=1

if [ "$BUILD" = 1 ] || [ ! -x "$UI" ]; then
    need_docker
    echo "ui.sh: compiling the desktop from MaryUI" >&2
    "$CLI" build --stage ui
    echo "ui.sh: compiling Mary's packages (linux/mary)" >&2
    "$CLI" build --stage mary
fi

if [ "$running" = 1 ]; then
    echo "ui.sh: a VM is already running; one booted by ui.sh restarts the desktop on the new build within a few seconds" >&2
    echo "ui.sh: (a VM booted with --image keeps its embedded desktop: ./ui.sh stop, then ./ui.sh)" >&2
    exit 0
fi

if [ "$REBUILD" = 1 ] || "$CLI" config | grep -q '^vm: *not built'; then
    need_docker
    echo "ui.sh: building the VM image from source" >&2
    "$CLI" build --target vm
elif [ "$IMAGE" = 1 ] && [ -n "$(find "$UI" -newer out/vm/boot.json 2>/dev/null)" ]; then
    echo "ui.sh: note: $UI is newer than the image; drop --image to run it, or --rebuild to embed it" >&2
fi
# A rebuilt image needs a fresh disk, or the VM keeps booting the old one.
[ "$REBUILD" = 1 ] && set -- --fresh "$@"
# The Mac's microphone is the guest's sound input, so "Hey Mary" can be heard in the VM.
[ "$MIC" = 1 ] && set -- --microphone "$@"
if [ "$IMAGE" = 1 ]; then
    exec "$CLI" vm run --desktop "$@"
fi
exec "$CLI" vm run --desktop --dev "$@"
