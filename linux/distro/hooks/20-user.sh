#!/bin/sh
# The first user from distro.conf (DEFAULT_USER, DEFAULT_PASSWORD), with sudo.
# root keeps no password (locked), like Ubuntu. Runs at the base stage.
set -eu

groups=""
for g in adm sudo dialout cdrom audio video plugdev netdev input render gpio i2c spi; do
    if getent group "$g" > /dev/null; then
        groups="${groups:+$groups,}$g"
    fi
done

if ! id "$DEFAULT_USER" > /dev/null 2>&1; then
    useradd --create-home --shell /bin/bash --user-group --groups "$groups" "$DEFAULT_USER"
else
    usermod --groups "$groups" "$DEFAULT_USER"
fi
echo "${DEFAULT_USER}:${DEFAULT_PASSWORD}" | chpasswd
