#!/bin/sh
# The Finder's folders: Desktop, Documents and Downloads in DEFAULT_USER's home
# (and in /etc/skel, for users made later), plus the welcome note in Documents
# from overlay-desktop/usr/share/maryos/welcome.txt. Runs inside the chroot at
# the target stage, for every target.
set -eu

home=$(getent passwd "$DEFAULT_USER" | cut -d: -f6)
for dir in Desktop Documents Downloads; do
    install -d -m 755 "/etc/skel/$dir"
    install -d -m 755 -o "$DEFAULT_USER" -g "$DEFAULT_USER" "$home/$dir"
done

if [ -f /usr/share/maryos/welcome.txt ]; then
    install -m 644 -o "$DEFAULT_USER" -g "$DEFAULT_USER" /usr/share/maryos/welcome.txt "$home/Documents/Welcome to ${DISTRO_NAME:-MaryOS}.txt"
fi
