#!/bin/sh
# The VM runner shares the host's linux/out directory over virtiofs with the
# tag "maryos-out"; mount it at /mnt/maryos-out when present. nofail keeps a
# missing share from failing the boot, but it also drops the mount's ordering
# before local-fs.target, so at shutdown systemd unmounted the share at the same
# time as it stopped the services and sessions that might still be using it (a
# dev-mode desktop or Mary daemon, a console login running a tool from the
# share), and printed "Failed unmounting mnt-maryos\x2dout.mount" whenever one
# still held it. x-systemd.before puts the ordering back: every service and
# session stops before the share is unmounted. Runs inside the chroot at the vm
# target stage.
set -eu

mkdir -p /mnt/maryos-out
sed -i '/^maryos-out[[:space:]]/d' /etc/fstab
printf 'maryos-out\t/mnt/maryos-out\tvirtiofs\tdefaults,nofail,x-systemd.before=local-fs.target\t0\t0\n' >> /etc/fstab
