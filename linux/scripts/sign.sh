#!/bin/sh
# Ad-hoc sign binaries with the Virtualization entitlement. Apple's
# Virtualization.framework refuses a process without it ("not entitled"),
# and that is exactly what a bare `swift build` produces; so the Makefile
# runs this after every build.
#
#   scripts/sign.sh .build/debug/maryos .build/debug/MaryOSApp
set -eu
cd "$(dirname "$0")/.."
for binary in "$@"; do
    if [ ! -f "$binary" ]; then
        echo "sign.sh: $binary not found" >&2
        continue
    fi
    codesign --force --sign - --entitlements Entitlements.plist "$binary"
done
