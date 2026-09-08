# 1. The goal

## Why a Linux fork

The ravynOS kit next door took an XNU kernel from "boots under UEFI" to a
process 1 with a shell on a QEMU test bed. What remained before it could do
the same on a Raspberry Pi 5 was an SD-controller driver, then a complete
arm64 userland (dyld, libSystem, launchd, the BSD tools), then everything a
desktop needs. That is years of work for a system whose only advantage over
Linux on this board is being Darwin.

MaryOS keeps the goal, a custom operating system for the Pi 5 built and
tested from a Mac, and changes the base: Ubuntu 24.04 for arm64 already has
the kernel, the drivers, the userland and the package archive. The effort
goes into what makes the system *ours* instead.

## What MaryOS is

MaryOS is an Ubuntu 24.04 (Noble) derivative in the same sense as the
Ubuntu flavours or Linux Mint: it installs packages from Ubuntu's archive,
it chooses which ones, it adds its own files and configuration, and it
carries its own name. In its first iteration nothing is recompiled: the
kernel is Ubuntu's, the packages are Ubuntu's, the identity, the selection,
the defaults and the first-boot behaviour are MaryOS's. Later iterations
can add an apt repository of MaryOS's own packages and a kernel built from
source; the build pipeline is laid out so those slot in as further stages.

The whole definition lives in `distro/`. If a file is not in `distro/`, it
is Ubuntu's. That is the kit's version of the ravynOS kit's payload levels:
an honest line between what is inherited and what is made here.

## One rootfs, two targets

The same base rootfs becomes two images:

| Target | Kernel | Boot | Where |
|---|---|---|---|
| `pi5` | Ubuntu's `linux-raspi` flavour (BCM2712, Raspberry Pi device trees) | The Pi's EEPROM firmware reads `config.txt` on the FAT partition and loads `vmlinuz` + `initrd.img` directly; no U-Boot | An SD card written by the flasher |
| `vm` | Ubuntu's `linux-generic` flavour (virtio as modules) | Apple's Virtualization.framework loads the raw kernel `Image` and the initrd from files on the Mac | A window on the Mac |

Both images have the same partition layout and labels, the same first-boot
service, the same user and network defaults. They differ only in the kernel
flavour and in the files on the boot partition. The VM is therefore a real
test of the system, not a different system that happens to share a name.

Why not one image with both kernels? Because `flash-kernel`, which keeps
the Pi's boot partition in step with kernel upgrades, would then have two
flavours to choose from and could put the generic kernel on the card. Two
images from one tree is the safer shape.
