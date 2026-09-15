#!/bin/sh
# MaryVNC Light: MaryVNC in the menu bar (maryvncd; MaryOS docs/14-maryvnc.md). It calls for Pis every second or
# so: a press of a paired Pi's power button opens its desktop in a borderless portal, and a Pi ready to pair asks
# to pair. It shares this Mac's key and the paired Pis with MaryVNC.app; run one of the two at a time.
#
#   ./vnc-light.sh                        build dist/MaryVNCLight.app and open it (a running one quits first)
#   ./vnc-light.sh --release              a release build
#   ./vnc-light.sh --no-build             open the last build
#   ./vnc-light.sh --test-profile DIR --nearby
#                                         for tests: this Mac's key, pairs and settings in DIR, not the Keychain
#
# It opens as an app, not a bare binary, so macOS asks for Local Network access for MaryVNC Light itself (allow it)
# instead of lending it the terminal's. After a rebuild the Keychain asks for this Mac's key: choose Always Allow.
# Its log: log stream --predicate 'subsystem == "com.maryos.MaryVNC"'. `make vnc-light` runs this.
set -eu
cd "$(dirname "$0")"

usage() { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

BUILD=1
CONFIG=debug
# Take our own flags out and keep the rest, in order, for the app.
n=$#
while [ "$n" -gt 0 ]; do
    arg=$1
    shift
    n=$((n - 1))
    case $arg in
        --no-build) BUILD=0 ;;
        --release) CONFIG=release ;;
        -h|--help) usage ;;
        *) set -- "$@" "$arg" ;;
    esac
done

APP=dist/MaryVNCLight.app
# `open` only brings a running copy forward, and drops the arguments: quit it first.
if pgrep -x MaryVNCLight > /dev/null; then
    echo "vnc-light.sh: quitting the MaryVNC Light that is running" >&2
    pkill -x MaryVNCLight || true
    i=0
    while pgrep -x MaryVNCLight > /dev/null; do
        i=$((i + 1))
        [ "$i" -le 50 ] || { echo "vnc-light.sh: MaryVNC Light did not quit" >&2; exit 1; }
        sleep 0.1
    done
fi
if [ "$BUILD" = 1 ]; then
    echo "vnc-light.sh: building MaryVNC Light ($CONFIG)" >&2
    CONFIG=$CONFIG sh scripts/bundle.sh MaryVNCLight
fi
[ -d "$APP" ] || { echo "vnc-light.sh: $APP is missing; run without --no-build" >&2; exit 1; }
if [ $# -gt 0 ]; then
    open "$APP" --args "$@"
else
    open "$APP"
fi
echo "vnc-light.sh: MaryVNC Light is in the menu bar" >&2
