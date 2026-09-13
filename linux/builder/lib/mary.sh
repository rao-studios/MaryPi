# shellcheck shell=sh
# Mary's packages (linux/mary: sewnd, threadd, maryd and the libraries under
# them), compiled with the container's native toolchain like the desktop.
# mary_build leaves a DESTDIR tree in OUT_DIR/mary (PREFIX=/usr); mary_install
# copies it into a target's rootfs. The sources are part of this repository,
# so the tree's commit is MARYOS_GIT_SHA.

MARY_SRC=${MARY_SRC:-$KIT_DIR/mary}
MARY_ENV=usr/share/doc/mary/mary.env
MARY_LOCK=${MARY_LOCK:-$MARY_SRC/third_party.lock}
# What mary_sherpa takes from the lock.
MARY_THIRD_PARTY="sherpa-onnx-lib.tar.bz2 sherpa-onnx-c-api.h sherpa-onnx-LICENSE onnxruntime-LICENSE kws-zipformer-gigaspeech.tar.bz2"

mary_build() {
    local src build out sherpa
    need make cc pkg-config
    src=$MARY_SRC
    [ -f "$src/Makefile" ] || die "no Mary sources at $src"
    build=$WORK_DIR/mary
    out=$OUT_DIR/mary
    mkdir -p "$build"

    # sherpa-onnx's release is built for aarch64, which both targets are.
    sherpa=
    if [ "$(uname -m)" = aarch64 ]; then
        sherpa=$build/sherpa-onnx
        mary_sherpa "$sherpa"
    else
        log "mary: sherpa-onnx is pinned for aarch64, not $(uname -m); compiling without \"Hey Mary\""
    fi

    log "mary: compiling $src (objects in $build)"
    make -C "$src" O="$build" SHERPA_DIR="$sherpa" -j"$(nproc)" all
    make -C "$src" O="$build" SHERPA_DIR="$sherpa" check-deps
    if [ "${MARY_SKIP_TESTS:-0}" = 1 ]; then
        log "mary: tests skipped (MARY_SKIP_TESTS=1)"
    else
        log "mary: tests"
        MARY_KWS_MODEL=${sherpa:+$sherpa/model} make -C "$src" O="$build" SHERPA_DIR="$sherpa" test
    fi

    rm -rf "$out.tmp"
    make -C "$src" O="$build" SHERPA_DIR="$sherpa" DESTDIR="$out.tmp" PREFIX=/usr install > /dev/null
    printf 'MARY_GIT_SHA=%s\nSOURCE=%s\nBUILT=%s\n' "${MARYOS_GIT_SHA:-unknown}" "$src" "$(utc_now)" > "$out.tmp/$MARY_ENV"
    # One rename, as for out/ui: a VM in dev mode runs the daemons from this
    # tree over virtiofs and must never see it half-copied.
    rm -rf "$out"
    mv "$out.tmp" "$out"
    log "mary ready: $out (bin: $(ls "$out/usr/bin" 2>/dev/null | tr '\n' ' '))"
}

# sherpa-onnx and the "Hey Mary" keyword model, fetched as pinned and laid out the
# way make's SHERPA_DIR wants: include/sherpa-onnx/c-api/c-api.h; lib/ with the C API
# and the onnxruntime it loads from $ORIGIN; model/ (with the model's test_wavs, which
# only the tests read); licenses/.
mary_sherpa() {
    local dir cache name
    dir=$1
    cache=$CACHE_DIR/mary
    need curl tar bzip2 awk
    for name in $MARY_THIRD_PARTY; do
        mary_fetch "$MARY_LOCK" "$cache" "$name"
    done
    rm -rf "$dir"
    mkdir -p "$dir/include/sherpa-onnx/c-api" "$dir/lib" "$dir/model" "$dir/licenses"
    cp "$cache/sherpa-onnx-c-api.h" "$dir/include/sherpa-onnx/c-api/c-api.h"
    tar -xjf "$cache/sherpa-onnx-lib.tar.bz2" -C "$dir/lib" --strip-components=2 --wildcards \
        '*/lib/libsherpa-onnx-c-api.so' '*/lib/libonnxruntime.so'
    tar -xjf "$cache/kws-zipformer-gigaspeech.tar.bz2" -C "$dir/model" --strip-components=1 --wildcards \
        '*/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx' '*/decoder-epoch-12-avg-2-chunk-16-left-64.onnx' \
        '*/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx' '*/tokens.txt' '*/README.md' '*/test_wavs/*'
    cp "$cache/sherpa-onnx-LICENSE" "$dir/licenses/sherpa-onnx.LICENSE"
    cp "$cache/onnxruntime-LICENSE" "$dir/licenses/onnxruntime.LICENSE"
    cp "$dir/model/README.md" "$dir/licenses/kws-zipformer-gigaspeech.README.md"
}

# One file a lock pins (name, sha256, bytes, url per line) into DIR/name. A cached
# copy is kept while it still matches; anything else is downloaded again, and a
# download whose hash or size differs is deleted and fails the build.
mary_fetch() {
    local lock dir name sha bytes url file
    lock=$1
    dir=$2
    name=$3
    sha=$(awk -v n="$name" '$1 == n { print $2; exit }' "$lock")
    bytes=$(awk -v n="$name" '$1 == n { print $3; exit }' "$lock")
    url=$(awk -v n="$name" '$1 == n { print $4; exit }' "$lock")
    [ -n "$sha" ] && [ -n "$bytes" ] && [ -n "$url" ] || die "$name is not pinned in $lock"
    file=$dir/$name
    mkdir -p "$dir"
    if [ -f "$file" ] && mary_matches "$file" "$sha" "$bytes"; then
        return 0
    fi
    log "mary: fetching $name"
    rm -f "$file" "$file.part"
    curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$file.part" "$url" || { rm -f "$file.part"; die "could not download $url"; }
    if ! mary_matches "$file.part" "$sha" "$bytes"; then
        rm -f "$file.part"
        die "$name does not match $lock (sha256 $sha, $bytes bytes); refusing it"
    fi
    mv "$file.part" "$file"
}

# FILE SHA256 BYTES: true when the file is exactly what the lock pins.
mary_matches() {
    local size sum
    size=$(wc -c < "$1" | tr -d ' ')
    [ "$size" = "$3" ] || return 1
    if command -v sha256sum > /dev/null 2>&1; then
        sum=$(sha256sum "$1" | cut -d' ' -f1)
    else
        sum=$(shasum -a 256 "$1" | cut -d' ' -f1)
    fi
    [ "$sum" = "$2" ]
}

# The compiled packages into a rootfs tree, root-owned.
mary_install() {
    local root
    root=$1
    [ -f "$OUT_DIR/mary/$MARY_ENV" ] || die "no compiled Mary packages in $OUT_DIR/mary (run: build.sh mary)"
    log "mary: installing $(mary_git_sha)"
    tar -C "$OUT_DIR/mary" --owner=0 --group=0 --numeric-owner --exclude=.DS_Store -cf - . | tar -C "$root" -xf -
    chroot_sh "$root" ldconfig
}

mary_git_sha() {
    sed -n 's/^MARY_GIT_SHA=//p' "$OUT_DIR/mary/$MARY_ENV" 2>/dev/null || echo unknown
}
