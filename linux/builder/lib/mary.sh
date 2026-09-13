# shellcheck shell=sh
# Mary's packages (linux/mary: sewnd, threadd, maryd and the libraries under
# them), compiled with the container's native toolchain like the desktop.
# mary_build leaves a DESTDIR tree in OUT_DIR/mary (PREFIX=/usr); mary_install
# copies it into a target's rootfs. The sources are part of this repository,
# so the tree's commit is MARYOS_GIT_SHA.

MARY_SRC=${MARY_SRC:-$KIT_DIR/mary}
MARY_ENV=usr/share/doc/mary/mary.env

mary_build() {
    local src build out
    need make cc pkg-config
    src=$MARY_SRC
    [ -f "$src/Makefile" ] || die "no Mary sources at $src"
    build=$WORK_DIR/mary
    out=$OUT_DIR/mary
    mkdir -p "$build"

    log "mary: compiling $src (objects in $build)"
    make -C "$src" O="$build" -j"$(nproc)" all
    make -C "$src" O="$build" check-deps
    if [ "${MARY_SKIP_TESTS:-0}" = 1 ]; then
        log "mary: tests skipped (MARY_SKIP_TESTS=1)"
    else
        log "mary: tests"
        make -C "$src" O="$build" test
    fi

    rm -rf "$out.tmp"
    make -C "$src" O="$build" DESTDIR="$out.tmp" PREFIX=/usr install > /dev/null
    printf 'MARY_GIT_SHA=%s\nSOURCE=%s\nBUILT=%s\n' "${MARYOS_GIT_SHA:-unknown}" "$src" "$(utc_now)" > "$out.tmp/$MARY_ENV"
    # One rename, as for out/ui: a VM in dev mode runs the daemons from this
    # tree over virtiofs and must never see it half-copied.
    rm -rf "$out"
    mv "$out.tmp" "$out"
    log "mary ready: $out (bin: $(ls "$out/usr/bin" 2>/dev/null | tr '\n' ' '))"
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
