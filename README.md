# MaryPi

Two kits for putting an operating system on a Raspberry Pi 5 from a Mac. Each one
builds an image, boots it in a virtual machine window on the Mac so nothing has to
be tried on hardware first, and writes it to an SD card with one admin prompt.

| Kit | System | VM on the Mac | State |
|---|---|---|---|
| [ravynos/](ravynos/README.md) | ravynOS (XNU/Darwin) arm64 bring-up | QEMU `virt` + EDK2 UEFI | Boots to a stand-in userland (`ravyninit`). Kept as is. |
| [linux/](linux/README.md) | **[MaryOS](https://github.com/rao-studios/MaryOS)**, an Ubuntu 24.04 (Noble) arm64 fork with the Liquid Platinum desktop, kept in its own repository and checked out here as `linux/maryos` | Apple Virtualization.framework | Active. |

The ravynOS bring-up ([the journey](ravynos/docs/README.md)) got an XNU kernel to
process 1 on the QEMU test bed. What remained (an SD controller driver, a complete
arm64 userland) was judged not worth the effort against a Linux base, so the same
ideas now go into MaryOS: the whole system is defined in one repository, images come
out of a reproducible pipeline, the Pi 5 card and the VM share one layout, and the
tooling says plainly what a card will do.

MaryOS itself (the distro, the image pipeline, the desktop and Mary) lives in
[rao-studios/MaryOS](https://github.com/rao-studios/MaryOS). This repository keeps the
Mac side of it: the `maryos` CLI and MaryOS.app, which drive the image build, run the VM
window and flash cards.

## Quick start

```sh
git submodule update --init             # linux/maryos: the MaryOS repository

# ravynOS kit
cd ravynos
swift run marypi vm run                 # build an image and boot it in a QEMU window

# MaryOS kit (Docker Desktop for the image build, Xcode for the VM and the flasher)
cd linux
maryos/vm.sh                            # compile the desktop and Mary, build the VM image if missing, boot to the desktop; run again to rebuild into the running VM
maryos/terminal.sh                      # boot the same image to a login prompt
make image TARGET=vm                    # just the image: rootfs from Ubuntu's archive + MaryOS -> maryos/out/maryos-0.0-vm.img
make image TARGET=pi5                   # the Raspberry Pi 5 image
make flash DISK=disk4                   # erase disk4 and write it (asks first)
```

## Layout

```
ravynos/   Swift package MaryPi (MaryPiKit, marypi CLI, MaryPi.app), vm/ QEMU profiles, docs/
linux/     Swift package MaryOS (MaryOSKit, maryos CLI, MaryOS.app with the VZ window and the flasher);
           maryos/ is the MaryOS submodule: distro/, builder/, maryui/ (the desktop in C), mary/, docs/
Makefile   make ravynos-<target> / make linux-<target> delegate to the kits
```
