#!/bin/sh
# /etc/fstab by label, the same on every target: the ext4 root and the FAT
# boot partition at /boot/firmware (where the Pi firmware and flash-kernel
# expect it). Runs at the base stage.
set -eu

mkdir -p /boot/firmware
cat > /etc/fstab <<EOF
# MaryOS: partitions are found by label so the image boots from any device
LABEL=${ROOT_LABEL}	/	ext4	defaults,noatime	0	1
LABEL=${BOOT_LABEL}	/boot/firmware	vfat	defaults	0	2
EOF
