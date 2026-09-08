# shellcheck shell=sh
# Shared helpers for the MaryOS builder. Sourced by build.sh after it has set
# KIT_DIR, DISTRO_DIR, CACHE_DIR, WORK_DIR and OUT_DIR.
#
# Every function declares its variables `local` (dash and bash both have
# it): plain sh variables are global, and a helper that reuses a caller's
# name silently corrupts the caller.

if [ -t 2 ]; then
    log() { printf '\033[1;36m==> %s\033[0m\n' "$*" >&2; }
else
    log() { printf '==> %s\n' "$*" >&2; }
fi
die() { printf 'build: %s\n' "$*" >&2; exit 1; }
need() {
    local tool
    for tool in "$@"; do
        command -v "$tool" > /dev/null 2>&1 || die "missing tool: $tool"
    done
}

# Every key distro.conf must define. Exported for the build and handed to
# hooks inside the chroot.
DISTRO_KEYS="DISTRO_NAME DISTRO_ID DISTRO_VERSION DISTRO_CODENAME DISTRO_CODENAME_PRETTY DISTRO_HOME_URL
BASE_SUITE BASE_MIRROR BASE_COMPONENTS ARCH DEFAULT_USER DEFAULT_PASSWORD HOSTNAME
LOCALE TIMEZONE BOOT_LABEL ROOT_LABEL BOOT_PARTITION_MIB ROOT_MIN_MIB VM_DISK_GIB"

load_distro() {
    local k
    [ -f "$DISTRO_DIR/distro.conf" ] || die "no $DISTRO_DIR/distro.conf"
    # shellcheck disable=SC1091
    . "$DISTRO_DIR/distro.conf"
    # Optional keys and their defaults
    : "${DISTRO_CODENAME_PRETTY:=${DISTRO_CODENAME:-}}"
    for k in $DISTRO_KEYS; do
        eval "[ -n \"\${$k:-}\" ]" || die "distro.conf does not set $k"
        export "$k"
    done
}

shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# The distro settings as `export KEY='value'` lines for scripts in the chroot.
distro_env_file() {
    local k v
    for k in $DISTRO_KEYS; do
        eval "v=\$$k"
        printf 'export %s=%s\n' "$k" "$(shell_quote "$v")"
    done
}

# Package names from a .list file, comments and blank lines removed, on one line.
read_list() {
    [ -f "$1" ] || die "no package list $1"
    sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$1" | tr '\n' ' '
}

# Fingerprint of everything that goes into the base rootfs: distro.conf, the
# base package list, the shared overlay and the base-stage hooks.
base_cache_key() {
    (
        cd "$DISTRO_DIR"
        for f in distro.conf packages/base.list $(find overlay -type f | sort) $(find hooks -maxdepth 1 -type f | sort); do
            printf '%s\n' "$f"
            cat "$f"
        done
    ) | sha256sum | cut -c1-16
}

base_tarball() {
    printf '%s/rootfs/base-%s-%s-%s.tar.zst' "$CACHE_DIR" "$BASE_SUITE" "$ARCH" "$(base_cache_key)"
}

utc_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
