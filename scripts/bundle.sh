#!/bin/sh
# Build MaryPi.app from the SwiftPM MaryPiApp product.
#
#   scripts/bundle.sh            -> dist/MaryPi.app (release build)
#   open dist/MaryPi.app
#
# SwiftPM cannot emit an .app bundle itself, so this wraps the release binary
# with the Info.plist and the MaryPiKit resource bundle, then ad-hoc signs it.
set -eu
cd "$(dirname "$0")/.."

swift build -c release --product MaryPiApp

APP=dist/MaryPi.app
BIN=.build/release/MaryPiApp
RESOURCES=.build/release/MaryPi_MaryPiKit.bundle

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/MaryPi"
cp Sources/MaryPiApp/Info.plist "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
if [ -d "$RESOURCES" ]; then
    cp -R "$RESOURCES" "$APP/Contents/Resources/"
else
    echo "warning: $RESOURCES not found; Manifest.json will be missing from the app" >&2
fi
if [ -d Sources/MaryPiApp/AppIcon.iconset ]; then
    iconutil -c icns Sources/MaryPiApp/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
fi
codesign --force --sign - "$APP"
echo "Built $APP"
