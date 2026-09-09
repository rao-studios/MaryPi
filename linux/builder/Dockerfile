# The MaryOS builder image: an arm64 Ubuntu Noble with the tools build.sh
# needs, plus the compiler and libraries that build the desktop (MaryUI/linux). Docker Desktop on Apple silicon runs this natively; no emulation.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# Docker Desktop resolves names to IPv6 first and IPv6 egress may stall;
# apt and debootstrap's wget stick to IPv4 here.
RUN echo 'Acquire::ForceIPv4 "true";' > /etc/apt/apt.conf.d/99force-ipv4 \
    && printf 'inet4-only = on\n' > /root/.wgetrc \
    && apt-get update && apt-get install -y --no-install-recommends \
        debootstrap ubuntu-keyring \
        e2fsprogs dosfstools mtools fdisk util-linux coreutils \
        python3 rsync tar gzip xz-utils zstd file ca-certificates \
        build-essential pkg-config libwlroots-dev libwayland-dev wayland-protocols \
        libxkbcommon-dev libinput-dev libpixman-1-dev libcairo2-dev libpango1.0-dev \
        libfontconfig1-dev libseat-dev libdrm-dev \
        fonts-inter fonts-urw-base35 fonts-jetbrains-mono \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
ENTRYPOINT ["/work/builder/build.sh"]
