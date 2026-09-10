#!/bin/sh
# The desktop service: maryui-desktop as DEFAULT_USER on tty1 with a logind
# session (PAMName=maryos-desktop, see overlay-desktop/etc/pam.d, runs the
# account and session stacks only, so nothing asks for a password), wanted by
# graphical.target only. vm.sh boots multi-user.target
# and never starts it; ui.sh and the Pi's cmdline.txt select graphical.target.
# Output goes to the journal and to the console, so the compositor's log is on
# the serial console too (ui.sh serial, vm.sh --console).
# Written by a hook because the unit needs DEFAULT_USER from distro.conf.
# Runs inside the chroot at the target stage, for every target.
set -eu

chmod 755 /usr/lib/maryos/desktop

cat > /etc/systemd/system/maryos-desktop.service <<UNIT
[Unit]
Description=${DISTRO_NAME} desktop (maryui-desktop on tty1)
Documentation=https://github.com/rao-studios/MaryUI
After=systemd-user-sessions.service systemd-logind.service maryos-firstboot.service getty@tty1.service
Wants=systemd-logind.service
Conflicts=getty@tty1.service

[Service]
Type=simple
User=${DEFAULT_USER}
PAMName=maryos-desktop
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
StandardInput=tty-fail
# journal only: the console is a virtio serial port the host drains, and every
# wlr_log line was a blocking write to it — a frame-hitch source. journalctl -u
# maryos-desktop still has everything.
StandardOutput=journal
StandardError=journal
# The compositor is the session; it should not queue behind background units.
Nice=-5
UtmpIdentifier=tty1
UtmpMode=user
Environment=XDG_SESSION_TYPE=wayland
ExecStart=/usr/lib/maryos/desktop
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical.target
Alias=display-manager.service
UNIT

systemctl enable maryos-desktop.service
# Ubuntu's default.target is graphical.target; the image boots to the login
# prompt unless graphical.target is asked for (ui.sh, the Pi's cmdline.txt).
systemctl set-default multi-user.target

# The font cache in the image, so the first start does not stall on fc-cache.
fc-cache -f > /dev/null 2>&1 || true
