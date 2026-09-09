# shellcheck shell=sh
# Stage 2: one target. Unpack the base rootfs, add the target's and the
# desktop's packages, overlays and hooks, install the compiled desktop
# (out/ui), collect the boot files, run the final hooks. Leaves
# WORK_DIR/<target>/rootfs plus target.env for the image stage.
#
# The desktop lives here rather than in the base stage so a change to
# packages/desktop.list, overlay-desktop/ or hooks/desktop/ costs a target
# build, not a bootstrap.

# Set by collect_boot_*: the kernel version that went onto the boot files.
COLLECTED_KERNEL_VERSION=""

# Highest kernel version installed in the rootfs whose name matches a flavour.
kernel_version() {
    local root flavour
    root=$1
    flavour=$2
    ls "$root/boot" 2>/dev/null | sed -n "s/^vmlinuz-\(.*-$flavour\)$/\1/p" | sort -V | tail -n 1
}

# Raspberry Pi 5: the firmware reads partition 1 (mounted at /boot/firmware):
# kernel, initrd, device trees, overlays and the Pi 4-era firmware blobs that
# linux-firmware-raspi ships. Same names Ubuntu and flash-kernel use.
collect_boot_pi5() {
    local root kver fw dtbdir candidate
    root=$1
    kver=$(kernel_version "$root" raspi)
    [ -n "$kver" ] || die "no raspi kernel in the rootfs (is linux-raspi in packages/pi5.list?)"
    fw=$root/boot/firmware
    mkdir -p "$fw"
    cp "$root/boot/vmlinuz-$kver" "$fw/vmlinuz"
    cp "$root/boot/initrd.img-$kver" "$fw/initrd.img"

    dtbdir=""
    for candidate in "$root/usr/lib/linux-image-$kver" "$root/lib/firmware/$kver/device-tree" "$root/usr/lib/firmware/$kver/device-tree"; do
        if [ -d "$candidate/broadcom" ]; then dtbdir=$candidate; break; fi
    done
    [ -n "$dtbdir" ] || die "no device trees for $kver (looked for broadcom/ under /usr/lib/linux-image-* and /lib/firmware/*/device-tree)"
    cp "$dtbdir"/broadcom/bcm2712*.dtb "$fw/"
    cp "$dtbdir"/broadcom/bcm2711*.dtb "$fw/" 2>/dev/null || true
    rm -rf "$fw/overlays"
    cp -r "$dtbdir/overlays" "$fw/overlays"
    if [ -d "$root/usr/lib/linux-firmware-raspi" ]; then
        cp "$root"/usr/lib/linux-firmware-raspi/* "$fw/" 2>/dev/null || true
    fi
    log "pi5 boot files: kernel $kver, device trees from ${dtbdir#"$root"}"
    COLLECTED_KERNEL_VERSION=$kver
}

# Virtualization.framework boots the kernel directly from files on the host,
# and needs the raw arm64 Image rather than Ubuntu's compressed vmlinuz.
collect_boot_vm() {
    local root dir kver boot cmdline
    root=$1
    dir=$2
    kver=$(kernel_version "$root" generic)
    [ -n "$kver" ] || die "no generic kernel in the rootfs (is linux-generic in packages/vm.list?)"
    boot=$dir/vmboot
    rm -rf "$boot"
    mkdir -p "$boot"
    python3 "$BUILDER_DIR/unzboot.py" "$root/boot/vmlinuz-$kver" "$boot/Image"
    cp "$root/boot/initrd.img-$kver" "$boot/initrd.img"
    cmdline="console=hvc0 root=LABEL=$ROOT_LABEL rootfstype=ext4 rw rootwait"
    cat > "$boot/boot.json" <<JSON
{
  "kernelVersion": "$kver",
  "kernel": "Image",
  "initrd": "initrd.img",
  "cmdline": "$cmdline",
  "built": "$(utc_now)"
}
JSON
    log "vm boot files: kernel $kver extracted to $boot/Image"
    COLLECTED_KERNEL_VERSION=$kver
}

target_build() {
    local target tarball dir root
    target=$1
    need tar zstd python3
    [ -f "$DISTRO_DIR/packages/$target.list" ] || die "no package list distro/packages/$target.list"
    tarball=$(base_tarball)
    [ -f "$tarball" ] || die "no base rootfs for the current distro/ (run: build.sh rootfs)"
    dir=$WORK_DIR/$target
    root=$dir/rootfs
    rm -rf "$dir"
    mkdir -p "$root"

    log "unpacking $tarball"
    zstd -d -c "$tarball" | tar -C "$root" --numeric-owner --xattrs --xattrs-include='*' -xf -

    chroot_prepare "$root"
    # shellcheck disable=SC2046
    chroot_apt_install "$root" $(read_list "$DISTRO_DIR/packages/$target.list") $(read_list "$DISTRO_DIR/packages/desktop.list")
    copy_overlay "$root" "$DISTRO_DIR/overlay-$target"
    copy_overlay "$root" "$DISTRO_DIR/overlay-desktop"
    ui_install "$root"
    chroot_run_hooks "$root" "$DISTRO_DIR/hooks/$target"
    chroot_run_hooks "$root" "$DISTRO_DIR/hooks/desktop"
    COLLECTED_KERNEL_VERSION=""
    case $target in
        pi5) collect_boot_pi5 "$root" ;;
        vm) collect_boot_vm "$root" "$dir" ;;
    esac
    chroot_run_hooks "$root" "$DISTRO_DIR/hooks/final"
    chroot_finish
    # Read by the image stage; nothing else may write to this file.
    printf 'KERNEL_VERSION=%s\nTARGET=%s\nBUILT=%s\nMARYUI_SHA=%s\n' "$COLLECTED_KERNEL_VERSION" "$target" "$(utc_now)" "$(ui_git_sha)" > "$dir/target.env"
    log "target $target ready: $root"
}
