# shellcheck shell=sh
# Stage 1: the base rootfs. debootstrap Ubuntu Noble, install the base package
# list, apply the shared overlay and the base hooks, pack a tarball keyed by
# the content of distro/. Both targets start from this tarball.

write_apt_sources() {
    local root
    root=$1
    rm -f "$root/etc/apt/sources.list"
    mkdir -p "$root/etc/apt/sources.list.d"
    cat > "$root/etc/apt/sources.list.d/ubuntu.sources" <<SOURCES
# ${DISTRO_NAME}: Ubuntu ${BASE_SUITE} ${ARCH} from ${BASE_MIRROR} (written by the builder from distro.conf)
Types: deb
URIs: ${BASE_MIRROR}
Suites: ${BASE_SUITE} ${BASE_SUITE}-updates ${BASE_SUITE}-security
Components: ${BASE_COMPONENTS}
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SOURCES
}

rootfs_build() {
    local tarball work root components
    need debootstrap tar zstd sha256sum
    tarball=$(base_tarball)
    if [ "$FRESH" = 0 ] && [ -f "$tarball" ]; then
        log "base rootfs is cached: $tarball"
        return 0
    fi

    work=$WORK_DIR/base
    root=$work/rootfs
    rm -rf "$work"
    mkdir -p "$root" "$CACHE_DIR/apt/debootstrap" "$(dirname "$tarball")"

    log "debootstrap $BASE_SUITE $ARCH from $BASE_MIRROR"
    components=$(printf '%s' "$BASE_COMPONENTS" | tr ' ' ',')
    debootstrap --arch="$ARCH" --variant=minbase --components="$components" \
        --cache-dir="$CACHE_DIR/apt/debootstrap" "$BASE_SUITE" "$root" "$BASE_MIRROR"

    write_apt_sources "$root"
    chroot_prepare "$root"
    # shellcheck disable=SC2046
    chroot_apt_install "$root" $(read_list "$DISTRO_DIR/packages/base.list")
    copy_overlay "$root" "$DISTRO_DIR/overlay"
    chmod +x "$root/usr/lib/maryos/firstboot"
    chroot_run_hooks "$root" "$DISTRO_DIR/hooks"
    chroot_finish
    # The chroot session borrows the builder's resolv.conf and puts the
    # previous file back; the device must use systemd-resolved's stub instead.
    ln -sf ../run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"

    log "packing $tarball"
    tar -C "$root" --numeric-owner --xattrs --xattrs-include='*' -cf - . | zstd -T0 -q -o "$tarball.tmp"
    mv "$tarball.tmp" "$tarball"
    rm -rf "$work"
    log "base rootfs ready: $tarball ($(du -h "$tarball" | cut -f1))"
}
