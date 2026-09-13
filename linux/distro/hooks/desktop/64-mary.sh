#!/bin/sh
# Mary (linux/mary, installed from out/mary by the target stage before the hooks):
# sewnd and threadd run as system users of their own and start with the machine;
# maryd is a user unit the desktop launcher starts inside the graphical session.
# The desktop user joins sewn and thread to reach their sockets, and is already in
# sudo, which is who sewnd lets set the Mistral key. Runs inside the chroot at the
# target stage, for every target.
set -eu

if [ ! -x /usr/bin/sewnd ]; then
    echo "64-mary: this image carries no Mary packages (builder/build.sh mary); skipping"
    exit 0
fi

systemd-sysusers /usr/lib/sysusers.d/mary.conf
usermod -aG sewn,thread "$DEFAULT_USER"
systemctl enable sewnd.service threadd.service
