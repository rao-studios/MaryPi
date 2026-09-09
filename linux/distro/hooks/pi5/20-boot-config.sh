#!/bin/sh
# Kernel command line for the Raspberry Pi 5. config.txt comes verbatim from
# overlay-pi5/; the command line is generated here so the root label follows
# distro.conf. Runs inside the chroot at the pi5 target stage.
#
# console=serial0,115200 puts the console on the 3-pin debug UART; console=tty1
# (last) makes the HDMI console /dev/console as well. systemd.unit=graphical.target
# starts the desktop (maryos-desktop.service takes tty1 over from the getty).
set -eu

mkdir -p /boot/firmware
printf 'console=serial0,115200 console=tty1 root=LABEL=%s rootfstype=ext4 rootwait fixrtc systemd.unit=graphical.target\n' "$ROOT_LABEL" \
    > /boot/firmware/cmdline.txt
