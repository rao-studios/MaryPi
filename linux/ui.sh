#!/bin/sh
# Boot MaryOS's desktop (Liquid Platinum, maryui-desktop) in a Virtualization.framework
# window with one command. vm.sh boots the same image to a login prompt.
#
#   linux/ui.sh                  build the CLI, the desktop (make ui) and the VM image if missing, boot to the desktop
#   linux/ui.sh --dev            dev loop: the guest runs out/ui over virtiofs and restarts it when `make ui` replaces it
#   linux/ui.sh --rebuild-ui     recompile the desktop (maryos build --stage ui) first
#   linux/ui.sh --rebuild        rebuild the image (which embeds the desktop) first, then boot from a fresh disk
#   linux/ui.sh stop|status|serial|reset
#
# Any other option goes to `maryos vm run --desktop` (see `maryos vm run --help`).
# MARYUI_DIR=/path/to/MaryUI compiles that checkout instead of the submodule.
set -eu
cd "$(dirname "$0")"
SWIFT=${SWIFT:-swift}
CLI=.build/debug/maryos
UI=out/ui/usr/bin/maryui-desktop

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
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
REBUILD_UI=0
DEV=0
n=$#
i=0
while [ "$i" -lt "$n" ]; do
    a=$1
    shift
    i=$((i + 1))
    case $a in
        --rebuild) REBUILD=1 ;;
        --rebuild-ui) REBUILD_UI=1 ;;
        --dev) DEV=1 ;;
        *) set -- "$@" "$a" ;;
    esac
done

if [ "$REBUILD_UI" = 1 ] || [ ! -x "$UI" ]; then
    echo "ui.sh: compiling the desktop from MaryUI (Docker Desktop must be running)" >&2
    "$CLI" build --stage ui
fi
if [ "$REBUILD" = 1 ] || "$CLI" config | grep -q '^vm: *not built'; then
    echo "ui.sh: building the VM image from source (Docker Desktop must be running)" >&2
    "$CLI" build --target vm
elif [ "$DEV" = 0 ] && [ -n "$(find "$UI" -newer out/vm/boot.json 2>/dev/null)" ]; then
    echo "ui.sh: note: $UI is newer than the image; use --dev to run it, or --rebuild to embed it" >&2
fi
# A rebuilt image needs a fresh disk, or the VM keeps booting the old one.
[ "$REBUILD" = 1 ] && set -- --fresh "$@"
if [ "$DEV" = 1 ]; then
    exec "$CLI" vm run --desktop --dev "$@"
fi
exec "$CLI" vm run --desktop "$@"
