#!/bin/sh
# Locale and timezone from distro.conf (LOCALE, TIMEZONE). Runs inside the
# chroot at the base stage.
set -eu

if ! grep -q "^${LOCALE} " /etc/locale.gen 2>/dev/null; then
    sed -i "s|^# *${LOCALE} |${LOCALE} |" /etc/locale.gen 2>/dev/null || true
fi
grep -q "^${LOCALE} " /etc/locale.gen 2>/dev/null || echo "${LOCALE} UTF-8" >> /etc/locale.gen
locale-gen
update-locale LANG="$LOCALE"

ln -sf "/usr/share/zoneinfo/${TIMEZONE}" /etc/localtime
echo "$TIMEZONE" > /etc/timezone
