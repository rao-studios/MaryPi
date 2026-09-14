#!/bin/sh
# MaryVNC: the Liquid Platinum viewer for MaryOS's remote desktop (maryvncd; MaryOS docs/14-maryvnc.md).
# It finds Pis on the USB cable and the local network, pairs with one over the cable, and connects on its
# own to the paired Pi it used last.
#
#   ./vnc.sh                          build and open the viewer
#   ./vnc.sh --connect HOST[:PORT]    connect by address, with the key of the Pi used most recently
#   ./vnc.sh --connect HOST --pair    pair with it: over the cable, or while `maryvncctl pair-window` is open
#   ./vnc.sh --no-build               open the last build without compiling
#   ./vnc.sh --release                build and open a release build
#   ./vnc.sh --test-profile DIR       for tests: this Mac's key, pairs and settings in DIR (not the Keychain), no browsing
#
# The first run asks for Local Network access (the Pis are on it) and, after a rebuild, for the Keychain item
# that holds this Mac's key: choose Always Allow. `make vnc` runs this; `make app-vnc` builds dist/MaryVNC.app.
set -eu
cd "$(dirname "$0")"

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

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

if [ "$BUILD" = 1 ]; then
    echo "vnc.sh: building the viewer ($CONFIG)" >&2
    swift build -c "$CONFIG" --product MaryVNCApp
fi
BIN=.build/$CONFIG/MaryVNCApp
[ -x "$BIN" ] || { echo "vnc.sh: $BIN is missing; run without --no-build" >&2; exit 1; }
exec "$BIN" "$@"
