#!/bin/sh
# Wi-Fi for System Settings › Network: iwd joins networks (etc/iwd/main.conf)
# and systemd-networkd addresses them (etc/systemd/network/25-wireless.network).
# The user must be in netdev to talk to iwd over D-Bus; 20-user.sh only adds
# the groups that exist at the base stage, and iwd arrives with the desktop
# packages. wpa_supplicant is masked so the two never fight over one radio.
# Runs inside the chroot at the target stage, for every target.
set -eu

groupadd -f netdev
usermod -aG netdev "$DEFAULT_USER"
systemctl enable iwd.service > /dev/null 2>&1 || true
systemctl mask wpa_supplicant.service > /dev/null 2>&1 || true
