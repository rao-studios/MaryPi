#!/bin/sh
# Identity: os-release, lsb-release, hostname, hosts, issue, motd.
#
# /usr/lib/os-release belongs to Ubuntu's base-files package, which would put
# its own copy back on the next upgrade. A dpkg diversion sends base-files'
# copy to /usr/lib/os-release.ubuntu forever, so ours survives. Runs at the
# base stage.
set -eu

if ! dpkg-divert --list /usr/lib/os-release | grep -q os-release; then
    dpkg-divert --local --rename --divert /usr/lib/os-release.ubuntu --add /usr/lib/os-release
fi
# VERSION_CODENAME is MaryOS's; UBUNTU_CODENAME stays the base suite for
# tools that pick apt suites from it.
cat > /usr/lib/os-release <<EOF
PRETTY_NAME="${DISTRO_NAME} ${DISTRO_VERSION} (${DISTRO_CODENAME_PRETTY})"
NAME="${DISTRO_NAME}"
VERSION_ID="${DISTRO_VERSION}"
VERSION="${DISTRO_VERSION} (${DISTRO_CODENAME_PRETTY})"
VERSION_CODENAME=${DISTRO_CODENAME}
ID=${DISTRO_ID}
ID_LIKE="ubuntu debian"
UBUNTU_CODENAME=${BASE_SUITE}
HOME_URL="${DISTRO_HOME_URL}"
EOF
ln -sf ../usr/lib/os-release /etc/os-release

# /etc/lsb-release is a conffile: dpkg keeps a locally modified copy. Only
# scripts that read this file directly see DISTRIB_CODENAME; `lsb_release`
# itself reads os-release and prints MaryOS's codename.
cat > /etc/lsb-release <<EOF
DISTRIB_ID=${DISTRO_NAME}
DISTRIB_RELEASE=${DISTRO_VERSION}
DISTRIB_CODENAME=${BASE_SUITE}
DISTRIB_DESCRIPTION="${DISTRO_NAME} ${DISTRO_VERSION} (${DISTRO_CODENAME_PRETTY})"
EOF

echo "$HOSTNAME" > /etc/hostname
cat > /etc/hosts <<EOF
127.0.0.1	localhost
127.0.1.1	${HOSTNAME}

::1	localhost ip6-localhost ip6-loopback
ff02::1	ip6-allnodes
ff02::2	ip6-allrouters
EOF

printf '%s %s (%s) \\n \\l\n\n' "$DISTRO_NAME" "$DISTRO_VERSION" "$DISTRO_CODENAME_PRETTY" > /etc/issue
printf '%s %s (%s) on Ubuntu %s %s\n' "$DISTRO_NAME" "$DISTRO_VERSION" "$DISTRO_CODENAME_PRETTY" "$BASE_SUITE" "$ARCH" > /etc/issue.net

# Ubuntu's dynamic motd scripts announce "Welcome to Ubuntu"; keep a plain one.
if [ -d /etc/update-motd.d ]; then
    chmod -x /etc/update-motd.d/* 2>/dev/null || true
fi
cat > /etc/motd <<EOF

  ${DISTRO_NAME} ${DISTRO_VERSION} "${DISTRO_CODENAME_PRETTY}" on ${ARCH}, built on Ubuntu ${BASE_SUITE}.
  Source: ${DISTRO_HOME_URL}
  First login: change the password with 'passwd'.

EOF
rm -f /etc/legal
