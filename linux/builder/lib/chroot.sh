# shellcheck shell=sh
# Working inside a rootfs tree: kernel filesystems, DNS, no service starts,
# the shared apt cache, package installs, overlays and hooks.

CHROOT_ROOT=""

chroot_prepare() {
    CHROOT_ROOT=$1
    trap 'chroot_finish' EXIT INT TERM
    mount -t proc proc "$CHROOT_ROOT/proc"
    mount -t sysfs sysfs "$CHROOT_ROOT/sys"
    mount --bind /dev "$CHROOT_ROOT/dev"
    mount --bind /dev/pts "$CHROOT_ROOT/dev/pts"
    mount -t tmpfs tmpfs "$CHROOT_ROOT/run"
    mkdir -p "$CACHE_DIR/apt/archives/partial" "$CHROOT_ROOT/var/cache/apt/archives"
    mount --bind "$CACHE_DIR/apt/archives" "$CHROOT_ROOT/var/cache/apt/archives"

    # DNS for apt: the rootfs points resolv.conf at systemd-resolved's stub,
    # which does not exist at build time. Use the builder's for the session.
    if [ -e "$CHROOT_ROOT/etc/resolv.conf" ] || [ -L "$CHROOT_ROOT/etc/resolv.conf" ]; then
        mv "$CHROOT_ROOT/etc/resolv.conf" "$CHROOT_ROOT/etc/resolv.conf.maryos-build"
    fi
    cp /etc/resolv.conf "$CHROOT_ROOT/etc/resolv.conf"

    # Package postinsts must not start services in the build environment.
    printf '#!/bin/sh\nexit 101\n' > "$CHROOT_ROOT/usr/sbin/policy-rc.d"
    chmod +x "$CHROOT_ROOT/usr/sbin/policy-rc.d"
    distro_env_file > "$CHROOT_ROOT/tmp/maryos.env"
}

chroot_finish() {
    local root m
    root=$CHROOT_ROOT
    [ -n "$root" ] || return 0
    CHROOT_ROOT=""
    rm -f "$root/usr/sbin/policy-rc.d" "$root/tmp/maryos.env" "$root/tmp/maryos-hook.sh"
    rm -f "$root/etc/resolv.conf"
    if [ -e "$root/etc/resolv.conf.maryos-build" ] || [ -L "$root/etc/resolv.conf.maryos-build" ]; then
        mv "$root/etc/resolv.conf.maryos-build" "$root/etc/resolv.conf"
    fi
    for m in var/cache/apt/archives run dev/pts dev sys proc; do
        umount -l "$root/$m" 2>/dev/null || true
    done
}

chroot_sh() {
    local root
    root=$1
    shift
    chroot "$root" /usr/bin/env -i \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        HOME=/root LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive FLASH_KERNEL_SKIP=true \
        "$@"
}

chroot_apt() {
    local root
    root=$1
    shift
    chroot_sh "$root" apt-get -y -q -o Dpkg::Options::=--force-confnew -o Acquire::ForceIPv4=true "$@"
}

chroot_apt_install() {
    local root
    root=$1
    shift
    log "apt-get update"
    chroot_apt "$root" update
    log "apt-get install $*"
    # shellcheck disable=SC2086
    chroot_apt "$root" install $*
}

# Files from an overlay directory onto the rootfs, root-owned, modes kept.
copy_overlay() {
    local root dir
    root=$1
    dir=$2
    [ -d "$dir" ] || return 0
    log "overlay $(basename "$dir")"
    tar -C "$dir" --owner=0 --group=0 --numeric-owner -cf - . | tar -C "$root" -xf -
}

# Every *.sh in a hooks directory, in name order, inside the chroot with the
# distro settings exported.
chroot_run_hooks() {
    local root dir hook
    root=$1
    dir=$2
    [ -d "$dir" ] || return 0
    for hook in "$dir"/*.sh; do
        [ -f "$hook" ] || continue
        log "hook $(basename "$dir")/$(basename "$hook")"
        cp "$hook" "$root/tmp/maryos-hook.sh"
        chroot_sh "$root" /bin/sh -e -c '. /tmp/maryos.env; . /tmp/maryos-hook.sh'
    done
}
