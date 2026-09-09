# shellcheck shell=sh
# Stage 3: the disk image, without loop devices or mounts. Partition 1 is a
# FAT32 filesystem built with mkfs.vfat and filled with mcopy; partition 2 is
# ext4 built straight from the rootfs tree with mke2fs -d; sfdisk writes the
# MBR into a sparse file and dd drops the two filesystems into place.
#
# Both targets get the smallest image that fits (ROOT_MIN_MIB or the tree
# plus 30 %). On the Pi, maryos-firstboot grows the root to the card; for the
# VM, the runner enlarges its private copy of the image before the first boot
# and the same firstboot service does the rest.

write_manifest() {
    local out target
    out=$1
    target=$2
    cat > "$out" <<MANIFEST
${DISTRO_NAME} ${DISTRO_VERSION} (${DISTRO_CODENAME_PRETTY}) ${ARCH}
target:  ${target}
kernel:  ${KERNEL_VERSION}
base:    Ubuntu ${BASE_SUITE} from ${BASE_MIRROR}
built:   ${BUILT}
source:  ${MARYOS_GIT_SHA:-unknown} ${DISTRO_HOME_URL} (linux/)
desktop: maryui ${MARYUI_SHA:-unknown} (boots with systemd.unit=graphical.target)
layout:  MBR; p1 FAT32 "${BOOT_LABEL}" ${BOOT_PARTITION_MIB} MiB at 4 MiB; p2 ext4 "${ROOT_LABEL}", grows on first boot
login:   ${DEFAULT_USER} (see distro.conf)
MANIFEST
}

image_build() {
    local target dir root name img bootdir boot_mib used_kib root_mib boot_start root_start total_mib
    target=$1
    need mkfs.vfat mcopy mke2fs sfdisk truncate dd sha256sum
    dir=$WORK_DIR/$target
    root=$dir/rootfs
    [ -f "$dir/target.env" ] || die "no target tree for $target (run: build.sh target $target)"
    # shellcheck disable=SC1091
    . "$dir/target.env"

    name=${DISTRO_ID}-${DISTRO_VERSION}-${target}.img
    img=$dir/$name

    # Partition 1 content is whatever the tree has under /boot/firmware.
    bootdir=$dir/boot
    rm -rf "$bootdir"
    mkdir -p "$root/boot/firmware"
    mv "$root/boot/firmware" "$bootdir"
    mkdir -p "$root/boot/firmware"
    write_manifest "$bootdir/MARYOS.txt" "$target"

    boot_mib=$BOOT_PARTITION_MIB
    used_kib=$(du -sxk "$root" | cut -f1)
    root_mib=$((used_kib * 13 / 10 / 1024))
    [ "$root_mib" -lt "$ROOT_MIN_MIB" ] && root_mib=$ROOT_MIN_MIB
    root_mib=$(((root_mib + 63) / 64 * 64))
    boot_start=4
    root_start=$((boot_start + boot_mib))
    total_mib=$((root_start + root_mib))
    log "image $name: boot ${boot_mib} MiB, root ${root_mib} MiB (tree ${used_kib} KiB), total ${total_mib} MiB"

    log "partition 1: FAT32 $BOOT_LABEL"
    rm -f "$dir/boot.vfat"
    mkfs.vfat -F 32 -n "$BOOT_LABEL" -C "$dir/boot.vfat" $((boot_mib * 1024)) > /dev/null
    mcopy -i "$dir/boot.vfat" -s -o "$bootdir"/* ::/

    log "partition 2: ext4 $ROOT_LABEL from the rootfs tree"
    rm -f "$dir/root.ext4"
    truncate -s "${root_mib}M" "$dir/root.ext4"
    mke2fs -q -F -t ext4 -L "$ROOT_LABEL" -d "$root" -E lazy_itable_init=1 "$dir/root.ext4" "${root_mib}M"

    log "assembling $img"
    rm -f "$img"
    truncate -s "${total_mib}M" "$img"
    sfdisk -q "$img" <<TABLE
label: dos
unit: sectors
start=$((boot_start * 2048)), size=$((boot_mib * 2048)), type=c, bootable
start=$((root_start * 2048)), size=$((root_mib * 2048)), type=83
TABLE
    dd if="$dir/boot.vfat" of="$img" bs=1M seek="$boot_start" conv=notrunc,sparse status=none
    dd if="$dir/root.ext4" of="$img" bs=1M seek="$root_start" conv=notrunc,sparse status=none
    rm -f "$dir/boot.vfat" "$dir/root.ext4"

    mkdir -p "$OUT_DIR"
    cp --sparse=always "$img" "$OUT_DIR/$name"
    (cd "$OUT_DIR" && sha256sum "$name" > "$name.sha256")
    cp "$bootdir/MARYOS.txt" "$OUT_DIR/$name.txt"
    if [ "$target" = vm ]; then
        mkdir -p "$OUT_DIR/vm"
        cp "$dir/vmboot/Image" "$dir/vmboot/initrd.img" "$dir/vmboot/boot.json" "$OUT_DIR/vm/"
    fi
    if [ "$KEEP" = 0 ]; then
        rm -rf "$dir"
    fi
    log "image ready: $OUT_DIR/$name"
}
