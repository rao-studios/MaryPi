#!/bin/sh
# Services and their prerequisites. `systemctl enable` only creates symlinks,
# so it works inside the chroot. Runs at the base stage.
set -eu

chmod 600 /etc/netplan/*.yaml 2>/dev/null || true

# /etc/resolv.conf is set to systemd-resolved's stub by the builder after the
# chroot session (the session borrows the builder's copy for apt).

systemctl enable systemd-networkd systemd-resolved systemd-timesyncd
systemctl enable maryos-firstboot
# openssh-server enables itself (ssh.socket on Noble); nothing to do here.
