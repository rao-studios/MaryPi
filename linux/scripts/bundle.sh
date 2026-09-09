#!/bin/sh
# Build MaryOS.app from the SwiftPM MaryOSApp product.
#
#   scripts/bundle.sh            -> dist/MaryOS.app (release build)
#   open dist/MaryOS.app
#
# The kit (distro/, builder/ and the desktop's sources maryui/linux) is copied
# into the bundle so the app can build images outside a checkout (output then
# goes to ~/Library/Caches/MaryOS), and the bundle is ad-hoc signed with the
# Virtualization entitlement.
set -eu
cd "$(dirname "$0")/.."

swift build -c release --product MaryOSApp

APP=dist/MaryOS.app
BIN=.build/release/MaryOSApp

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/kit"
cp "$BIN" "$APP/Contents/MacOS/MaryOS"
cp Sources/MaryOSApp/Info.plist "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cp -R distro builder "$APP/Contents/Resources/kit/"
if [ -f maryui/linux/Makefile ]; then
    mkdir -p "$APP/Contents/Resources/kit/maryui"
    cp -R maryui/linux "$APP/Contents/Resources/kit/maryui/linux"
else
    echo "bundle: maryui/linux is missing (git submodule update --init); the app will not be able to compile the desktop" >&2
fi
if [ -d Sources/MaryOSApp/AppIcon.iconset ]; then
    iconutil -c icns Sources/MaryOSApp/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
fi
codesign --force --sign - --entitlements Entitlements.plist "$APP"
echo "Built $APP"
