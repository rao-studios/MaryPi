#!/bin/sh
# Boot MaryOS in a Virtualization.framework window with one command.
#
#   linux/vm.sh                  build the CLI if needed, build the VM image if it
#                                is missing (Docker), boot it in a window
#   linux/vm.sh --console        also attach this terminal to the serial console (Ctrl-] stops)
#   linux/vm.sh --headless       no window
#   linux/vm.sh --rebuild        rebuild the image first, then boot from a fresh disk
#   linux/vm.sh stop|status|serial|reset
#
# Any other option goes to `maryos vm run` (see `maryos vm run --help`).
# The CLI signs itself with the Virtualization entitlement when needed.
set -eu
cd "$(dirname "$0")"
SWIFT=${SWIFT:-swift}
CLI=.build/debug/maryos

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case ${1:-} in
    -h|--help|help) usage 0 ;;
esac

echo "vm.sh: building the maryos CLI" >&2
"$SWIFT" build --product maryos 2>&1 | grep -E "error|warning: unre" || true
[ -x "$CLI" ] || { echo "vm.sh: $CLI was not built" >&2; exit 1; }

case ${1:-} in
    stop|status|serial|reset)
        cmd=$1
        shift
        exec "$CLI" vm "$cmd" "$@" ;;
esac

# Take --rebuild out of the arguments, keep everything else for `vm run`.
REBUILD=0
n=$#
i=0
while [ "$i" -lt "$n" ]; do
    a=$1
    shift
    i=$((i + 1))
    if [ "$a" = --rebuild ]; then REBUILD=1; else set -- "$@" "$a"; fi
done

if [ "$REBUILD" = 1 ] || "$CLI" config | grep -q '^vm: *not built'; then
    echo "vm.sh: building the VM image from source (Docker Desktop must be running)" >&2
    "$CLI" build --target vm
    # A rebuilt image needs a fresh disk, or the VM keeps booting the old one.
    set -- --fresh "$@"
fi
exec "$CLI" vm run "$@"
