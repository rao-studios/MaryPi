#!/bin/sh
# The VM runner shares the host's linux/out directory over virtiofs with the
# tag "maryos-out"; mount it at /mnt/maryos-out when present. Runs inside the
# chroot at the vm target stage.
set -eu

mkdir -p /mnt/maryos-out
grep -q '^maryos-out' /etc/fstab || \
    printf 'maryos-out\t/mnt/maryos-out\tvirtiofs\tdefaults,nofail\t0\t0\n' >> /etc/fstab
