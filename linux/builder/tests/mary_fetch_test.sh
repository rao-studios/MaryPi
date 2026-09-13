#!/bin/sh
# mary_fetch (builder/lib/mary.sh) against a pinned lock: a download that matches is
# kept and reused; one whose hash or size differs fails the build and leaves nothing
# behind. curl is replaced by a shell function, so nothing reaches the network.
set -eu
builder=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
KIT_DIR=$(dirname "$builder")
. "$builder/lib/common.sh"
. "$builder/lib/mary.sh"
log() { :; }

failures=0
# check DESCRIPTION COMMAND...: the command, in a subshell, must succeed.
check() {
    local what
    what=$1
    shift
    if ( "$@" ) 2> "$tmp/stderr"; then
        printf 'ok   %s\n' "$what"
    else
        printf 'FAIL %s\n' "$what"
        sed 's/^/     /' "$tmp/stderr"
        failures=$((failures + 1))
    fi
}
# fails COMMAND...: the command must fail (die exits its own subshell); its stderr is kept in $tmp/why.
fails() { ! ( "$@" ) 2> "$tmp/why"; }
downloads() { wc -l < "$tmp/downloads" | tr -d ' '; }

: > "$tmp/downloads"
BODY=pinned
curl() {
    local out
    out=
    while [ $# -gt 0 ]; do
        if [ "$1" = -o ]; then out=$2; shift; fi
        shift
    done
    echo x >> "$tmp/downloads"
    printf '%s' "$BODY" > "$out"
}
if command -v sha256sum > /dev/null 2>&1; then
    sum=$(printf pinned | sha256sum | cut -d' ' -f1)
else
    sum=$(printf pinned | shasum -a 256 | cut -d' ' -f1)
fi
cat > "$tmp/lock" <<LOCK
# name  sha256  bytes  url
good    $sum    6      https://example.invalid/good
long    $sum    7      https://example.invalid/long
LOCK

check "a download that matches its pin is kept" mary_fetch "$tmp/lock" "$tmp/cache" good
check "  ... with the pinned bytes" grep -qx pinned "$tmp/cache/good"
BODY=changed
check "a cached copy that still matches is not downloaded again" mary_fetch "$tmp/lock" "$tmp/cache" good
check "  ... one download in all" [ "$(downloads)" = 1 ]

BODY=pinneD
check "a download with the wrong hash fails the build" fails mary_fetch "$tmp/lock" "$tmp/fresh" good
check "  ... says why" grep -q "does not match" "$tmp/why"
check "  ... and leaves nothing in the cache" [ ! -e "$tmp/fresh/good" ] && [ ! -e "$tmp/fresh/good.part" ]

BODY=pinned
check "a download of the wrong size fails the build" fails mary_fetch "$tmp/lock" "$tmp/fresh" long
printf pinneD > "$tmp/cache/good"
check "a tampered cached copy is fetched again" mary_fetch "$tmp/lock" "$tmp/cache" good
check "  ... and replaced" grep -qx pinned "$tmp/cache/good"
printf pinneD > "$tmp/cache/good"
BODY=pinneD
check "a tampered cached copy that downloads wrong again fails" fails mary_fetch "$tmp/lock" "$tmp/cache" good
check "  ... and is removed" [ ! -e "$tmp/cache/good" ]
check "a name the lock does not pin fails" fails mary_fetch "$tmp/lock" "$tmp/cache" unpinned

real=$KIT_DIR/mary/third_party.lock
check "mary/third_party.lock pins a sha256, a size and an https URL on every line" \
    awk '!/^#/ && NF { if (NF != 4 || length($2) != 64 || $2 ~ /[^0-9a-f]/ || $3 ~ /[^0-9]/ || $4 !~ /^https:\/\//) bad = 1 } END { exit bad }' "$real"
for name in $MARY_THIRD_PARTY; do
    check "mary/third_party.lock pins $name" awk -v n="$name" '$1 == n { found = 1 } END { exit !found }' "$real"
done

[ "$failures" = 0 ]
