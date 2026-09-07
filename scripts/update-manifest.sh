#!/bin/sh
# Re-pin the rpi5-uefi firmware release in Sources/MaryPiKit/Resources/Manifest.json.
#
#   scripts/update-manifest.sh v0.4
#
# Downloads the release asset, records its size and SHA-256, lists the files
# in the archive, rewrites the manifest and prints the diff. Then run
# `swift test` (ManifestTests validates the result) and commit.
set -eu
cd "$(dirname "$0")/.."

TAG=${1:?usage: update-manifest.sh <rpi5-uefi release tag, e.g. v0.3>}
ASSET=${ASSET:-RPi5_UEFI_Release_${TAG}.zip}
URL="https://github.com/worproject/rpi5-uefi/releases/download/$TAG/$ASSET"
MANIFEST=Sources/MaryPiKit/Resources/Manifest.json
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
curl -fL -o "$TMP/$ASSET" "$URL"
SIZE=$(stat -f %z "$TMP/$ASSET")
SHA=$(shasum -a 256 "$TMP/$ASSET" | cut -d' ' -f1)
FILES=$(unzip -Z1 "$TMP/$ASSET" | grep -v '/$' | tr '\n' ' ')
echo "size=$SIZE sha256=$SHA files=$FILES"

cp "$MANIFEST" "$TMP/before.json"
python3 - "$MANIFEST" "$TAG" "$URL" "$SHA" "$SIZE" $FILES <<'PY'
import json, sys, datetime
path, tag, url, sha, size, *files = sys.argv[1:]
manifest = json.load(open(path))
for source in manifest["sources"]:
    if source["id"] == "rpi5-uefi":
        source.update(version=tag, url=url, sha256=sha, sizeBytes=int(size), files=files)
        break
else:
    sys.exit("no rpi5-uefi source in manifest")
manifest["updated"] = datetime.date.today().isoformat()
json.dump(manifest, open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
diff -u "$TMP/before.json" "$MANIFEST" || true
echo "Now run: swift test"
