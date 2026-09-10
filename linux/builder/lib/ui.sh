# shellcheck shell=sh
# The desktop: MaryUI/linux (libmaryui + maryui-desktop, a wlroots compositor)
# compiled with the container's native toolchain. ui_build leaves a DESTDIR
# tree in OUT_DIR/ui (PREFIX=/usr); ui_install copies it into a target's
# rootfs. Sources come from MARYUI_SRC: the submodule linux/maryui, or the
# checkout named by MARYUI_DIR on the Mac (mounted read-only at /maryui).

ui_build() {
    local src build out
    need make cc pkg-config wayland-scanner
    src=$MARYUI_SRC
    [ -f "$src/Makefile" ] || die "no MaryUI sources at $src (git submodule update --init, or set MARYUI_DIR to a MaryUI checkout)"
    build=$WORK_DIR/ui
    out=$OUT_DIR/ui
    mkdir -p "$build"

    log "maryui: compiling $src (objects in $build)"
    make -C "$src" O="$build" -j"$(nproc)" all
    if [ "${MARYUI_SKIP_TESTS:-0}" = 1 ]; then
        log "maryui: tests skipped (MARYUI_SKIP_TESTS=1)"
    else
        log "maryui: tests"
        make -C "$src" O="$build" test
    fi

    rm -rf "$out.tmp"
    make -C "$src" O="$build" DESTDIR="$out.tmp" PREFIX=/usr install > /dev/null
    "$build/lp-render" --all "$out.tmp/renders"
    # The renders are previews and do not ship — except the wallpapers at the
    # VM's scanout size, which are what lp_wallpaper looks for before rendering
    # its own. The molten one costs ~7s under llvmpipe, so baking it here means
    # neither a first boot nor a dev-mode restart ever pays for it.
    for wp in wallpaper-1280x800 molten-platinum-1280x800; do
        [ -f "$out.tmp/renders/$wp.png" ] || continue
        install -D -m 0644 "$out.tmp/renders/$wp.png" "$out.tmp/usr/share/maryui/$wp.png"
    done
    printf 'MARYUI_GIT_SHA=%s\nSOURCE=%s\nBUILT=%s\n' "${MARYUI_GIT_SHA:-unknown}" "$src" "$(utc_now)" > "$out.tmp/usr/share/maryui/maryui.env"
    # One rename: a VM in dev mode watches this tree over virtiofs and must
    # never see it half-copied.
    rm -rf "$out"
    mv "$out.tmp" "$out"
    log "maryui ready: $out/usr/bin/maryui-desktop (${MARYUI_GIT_SHA:-unknown})"
}

# The compiled desktop into a rootfs tree, root-owned; ldconfig for libmaryui.so.
ui_install() {
    local root
    root=$1
    [ -x "$OUT_DIR/ui/usr/bin/maryui-desktop" ] || die "no compiled desktop in $OUT_DIR/ui (run: build.sh ui)"
    log "maryui: installing $(sed -n 's/^MARYUI_GIT_SHA=//p' "$OUT_DIR/ui/usr/share/maryui/maryui.env")"
    tar -C "$OUT_DIR/ui" --owner=0 --group=0 --numeric-owner --exclude=./renders --exclude=.DS_Store -cf - . | tar -C "$root" -xf -
    chroot_sh "$root" ldconfig
}

ui_git_sha() {
    sed -n 's/^MARYUI_GIT_SHA=//p' "$OUT_DIR/ui/usr/share/maryui/maryui.env" 2>/dev/null || echo unknown
}
