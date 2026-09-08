#!/bin/sh
# Last hook of every target build: leave no package lists, logs, host keys or
# machine identity in the image. Runs inside the chroot after the target
# packages and hooks. Downloaded .debs are not touched here: the builder
# bind-mounts its shared apt cache over /var/cache/apt/archives and unmounts
# it afterwards, so the image never contains them anyway.
set -eu

apt-get -y -q autoremove --purge
rm -rf /var/lib/apt/lists/* /var/cache/apt/*.bin
rm -rf /tmp/* /var/tmp/*

# Regenerated on the device: ssh host keys by maryos-firstboot, machine-id by systemd
rm -f /etc/ssh/ssh_host_*
: > /etc/machine-id
rm -f /var/lib/dbus/machine-id

rm -f /root/.bash_history
find /var/log -type f -exec truncate -s 0 {} + 2>/dev/null || true
