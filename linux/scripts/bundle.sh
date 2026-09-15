#!/bin/sh
# Build an app bundle from a SwiftPM product.
#
#   scripts/bundle.sh                -> dist/MaryOS.app (release build of MaryOSApp)
#   scripts/bundle.sh MaryVNC        -> dist/MaryVNC.app (release build of MaryVNCApp, the MaryVNC viewer)
#   scripts/bundle.sh MaryVNCLight   -> dist/MaryVNCLight.app (MaryVNCLightApp, MaryVNC in the menu bar)
#   CONFIG=debug scripts/bundle.sh MaryVNCLight   a debug build instead (vnc-light.sh does this)
#   open dist/MaryOS.app
#
# The kit (distro/, builder/, the desktop's maryui/, Mary's mary/ and MaryVNC's maryvnc/, from the MaryOS
# submodule in maryos/) is copied into MaryOS.app so the app can build images outside a
# checkout (output then goes to ~/Library/Caches/MaryOS), and the bundle is ad-hoc signed
# with the Virtualization entitlement. The MaryVNC apps carry no kit and need no entitlement.
set -eu
cd "$(dirname "$0")/.."

APP_NAME=${1:-MaryOS}
CONFIG=${CONFIG:-release}
case $APP_NAME in
    MaryOS) PRODUCT=MaryOSApp ;;
    MaryVNC) PRODUCT=MaryVNCApp ;;
    MaryVNCLight) PRODUCT=MaryVNCLightApp ;;
    *) echo "bundle: unknown app $APP_NAME (MaryOS, MaryVNC or MaryVNCLight)" >&2; exit 2 ;;
esac

if [ "$APP_NAME" = MaryOS ]; then
    [ -f maryos/distro/distro.conf ] || { echo "bundle: maryos/ is empty (git submodule update --init)" >&2; exit 1; }
fi
swift build -c "$CONFIG" --product "$PRODUCT"

APP=dist/$APP_NAME.app
BIN=.build/$CONFIG/$PRODUCT
# MaryVNC Light wears MaryVNC's icon.
ICONSET=Sources/$PRODUCT/AppIcon.iconset
[ "$APP_NAME" = MaryVNCLight ] && ICONSET=Sources/MaryVNCApp/AppIcon.iconset

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/$APP_NAME"
cp "Sources/$PRODUCT/Info.plist" "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
if [ -d "$ICONSET" ]; then
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
fi
if [ "$APP_NAME" = MaryOS ]; then
    mkdir -p "$APP/Contents/Resources/kit"
    for dir in distro builder maryui mary maryvnc; do
        # maryvnc/ arrived in MaryOS after some of the commits this submodule may be pinned to.
        [ -d "maryos/$dir" ] && cp -R "maryos/$dir" "$APP/Contents/Resources/kit/"
    done
    codesign --force --sign - --entitlements Entitlements.plist "$APP"
else
    codesign --force --sign - "$APP"
fi
echo "Built $APP"
