# MaryOS kit

MaryOS is an Ubuntu 24.04 (Noble) arm64 fork built from source in this
directory: Ubuntu's package archive plus MaryOS's own package lists, file
overlay, hooks and branding under [distro/](distro/). This kit builds the
images (in Docker), boots the VM image in an Apple Virtualization.framework
window, and writes the Raspberry Pi 5 image to an SD card with one admin
prompt. It is the Linux sibling of the [ravynOS kit](../ravynos/README.md)
and follows the same rule: say plainly what a card or a VM will do.

The full story, from `distro.conf` to a login prompt on the Pi and in the VM,
is in [docs/](docs/README.md).

## Requirements

- macOS 15 or newer with Xcode 26 (Swift 6) for the CLI and the app.
- Docker Desktop on Apple silicon for building images. The builder is an
  arm64 Ubuntu container running `debootstrap`; it needs no loop devices and
  no root on the Mac. Start Docker Desktop before `make image`.
- A Raspberry Pi 5 with a bootloader EEPROM recent enough for Ubuntu 24.04.
- Virtualization.framework only serves processes with the
  `com.apple.security.virtualization` entitlement, which `swift build` does
  not add. The CLI and the app check their own executable before touching
  the framework and, when the entitlement is missing, ad-hoc sign it in
  place and restart themselves, so `swift run maryos vm run` and
  `swift run MaryOSApp` work. `make build` signs ahead of time with
  [scripts/sign.sh](scripts/sign.sh) and [Entitlements.plist](Entitlements.plist).

## Quick start

```sh
cd linux
./vm.sh                         # one command: build the CLI, build the VM image if missing, boot it in a window
./vm.sh --console               # same, with this terminal on the serial console (Ctrl-] stops the VM)
./vm.sh stop                    # ask the guest to shut down
./ui.sh                         # compile the desktop, then boot it (from out/ui over virtiofs, restarted on every rebuild); Ctrl+Space opens Spotlight
./ui.sh                         # again while it runs: recompile, and the running VM restarts the desktop on the new build
./ui.sh --image                 # the desktop embedded in the image instead (--rebuild re-embeds the fresh one)

make build                      # swift build, then scripts/sign.sh on the binaries
make cli ARGS=doctor            # tools, kit, Docker, Virtualization, entitlement, built images
make image TARGET=vm            # builder/build.sh all vm -> out/maryos-0.0-vm.img + out/vm/
make ui                         # builder/build.sh ui: compile + test MaryUI/linux -> out/ui (MARYUI_DIR=… for a sibling checkout)
make desktop / make ui-dev      # both ui.sh
make vm                         # vm.sh; log in as mary (password in distro.conf)
make image TARGET=pi5           # out/maryos-0.0-pi5.img
make cli ARGS=list              # removable disks the flasher is willing to erase
make flash DISK=disk4           # erase disk4 and write the pi5 image (asks first, one admin prompt)
make run-app                    # the GUI
make app                        # dist/MaryOS.app (release, kit bundled, signed)
make test                       # swift test + the builder's Python tests
```

The CLI, once built and signed (`.build/debug/maryos`):

```
maryos doctor                       what this Mac can do
maryos config                       distro.conf, directories, built images
maryos build [--target pi5|vm|both] [--stage rootfs|ui|target|image|all] [--fresh] [--dry-run]
maryos list [--all]                 removable disks
maryos flash --disk diskN [--image PATH] [--target pi5] [--build] [--yes]
maryos vm run [--desktop [--dev]] [--headless] [--console] [--memory MiB] [--cpus N] [--share tag=/dir] [--fresh] [--disk-size GiB] [--dry-run]
maryos vm stop | status | serial [--no-follow] | reset
```

Every command accepts `--kit <dir>` to point at a kit directory; otherwise it
is found from the current directory, `MARYOS_KIT_DIR`, the app bundle, or
this checkout. `swift run maryos <command>` works too: `vm run` signs the
binary on its first use after a build and starts again.

## What the fork is

Everything MaryOS-specific is data in `distro/`:

```
distro.conf          name, id, version and codename (0.0 "Liquid Platinum"), Ubuntu suite and mirror, first user, labels, sizes
packages/base.list   every target: an explicit list, no ubuntu-* metapackages, no snapd
packages/pi5.list    linux-raspi, Raspberry Pi firmware, flash-kernel, Wi-Fi and Bluetooth
packages/vm.list     linux-generic (virtio drivers as modules)
overlay/             files copied onto every rootfs (netplan, the first-boot service)
overlay-pi5/         boot/firmware/config.txt and usercfg.txt for the Pi firmware
hooks/               scripts run in the chroot: locale, user, branding, services, fstab
hooks/pi5, hooks/vm  target-only steps (cmdline.txt; the virtiofs share)
hooks/final/         cleanup at the end of every target build
```

Changing MaryOS means editing these files and running `make image`.
[docs/02-the-fork.md](docs/02-the-fork.md) walks through each of them.

## How an image is built

`builder/build.sh` runs three stages inside the `maryos-builder` container
(or natively as root on an arm64 Linux host):

1. **rootfs**: `debootstrap --variant=minbase noble` from ports.ubuntu.com,
   `apt-get install` of `packages/base.list`, the overlay, the base hooks,
   then a tarball cached by the content of `distro/`.
2. **target**: unpack the tarball, install `packages/<target>.list`, apply
   `overlay-<target>/` and `hooks/<target>/`, collect the boot files (Pi:
   kernel, initrd, device trees and overlays into `/boot/firmware`; VM: the
   raw arm64 `Image` extracted from Ubuntu's compressed `vmlinuz`, the initrd
   and a `boot.json`), run `hooks/final/`.
3. **image**: `mkfs.vfat` + `mcopy` for partition 1, `mke2fs -d` for
   partition 2 straight from the tree, `sfdisk` for the MBR, `dd` into a
   sparse file. No loop devices, no mounts.

Outputs land in `out/`: `maryos-0.0-pi5.img`, `maryos-0.0-vm.img`, their
`.sha256` and `.txt` manifests, and `out/vm/{Image,initrd.img,boot.json}`.
Downloads and the base tarball live in Docker volumes (`maryos-cache`,
`maryos-work`), so a rebuild after editing `distro/` takes minutes.

## What a card looks like

MBR partition table, the same for both targets:

1. `MARYOS`, FAT32, 512 MiB, active, at 4 MiB. Mounted at `/boot/firmware`.
2. `maryos-root`, ext4, the rest of the image. Grown to the whole card or
   disk by `maryos-firstboot` on the first boot.

On the Pi's FAT partition:

```
config.txt              kernel=vmlinuz, initramfs initrd.img followkernel, enable_uart=1, KMS
cmdline.txt             console=serial0,115200 console=tty1 root=LABEL=maryos-root rootfstype=ext4 rootwait fixrtc
usercfg.txt             local additions, included by config.txt
vmlinuz, initrd.img     Ubuntu's raspi kernel and initramfs (flash-kernel updates them on upgrade)
bcm2712-rpi-5-b.dtb     device tree, plus overlays/
bootcode.bin, start*.elf, fixup*.dat   firmware blobs older boards need; the Pi 5 ignores them
MARYOS.txt              name, version, target, kernel, build date, git sha, layout
```

## The VM

Virtualization.framework boots the kernel directly: `VZLinuxBootLoader`
with `out/vm/Image` and `initrd.img`, command line
`console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait`. The
disk is an APFS clone of the built image in `state/vm/vm/disk.img`, grown
(sparsely) to `VM_DISK_GIB` so the guest fills it on first boot. The guest
gets NAT networking with a fixed MAC address, a virtio-gpu window with USB
keyboard and pointer, and `out/` shared over virtiofs as `maryos-out`
(mounted at `/mnt/maryos-out`). Everything on the serial console goes to
`state/vm/vm/serial.log`; `--console` attaches your terminal to it (Ctrl-]
stops the VM) and `--headless` drops the window.

`maryos vm stop`, Ctrl-C, or closing the window sends the guest an ACPI
power button; systemd powers off cleanly, and the VM is forced off after 20
seconds if it does not. `maryos vm reset` deletes the disk so the next run
starts from the built image again.

## How flashing works

1. The image comes from `out/` (built first if missing).
2. The target disk is re-read with `diskutil info` right before writing and
   must still be a whole, physical, external disk of the same size.
3. One generated shell script runs under `osascript … with administrator
   privileges`: `diskutil unmountDisk force`, `dd bs=4m` to the raw device
   (polled with `SIGINFO` for progress), `sync`, `diskutil eject`. Its log is
   tailed into the app or the terminal.

## Serial console on the Pi

Connect a 3.3 V USB-UART adapter to the Pi 5's 3-pin debug header (GND, TX,
RX) and open it at 115200 8N1, for example `screen /dev/cu.usbserial-XXXX
115200`. The kernel console and a login prompt are there; HDMI shows the same
console.

## The app

`make run-app` opens the window; `make app` produces `dist/MaryOS.app`.

- **Left**: removable disks (SD readers, USB, Thunderbolt; 4 GB to 2 TB),
  refreshed when a card is inserted or removed.
- **Center**: the MaryOS card: name, base, where the kit is, and for each
  target whether an image is built, with Build/Rebuild buttons.
- **Right/bottom**: the step list and the live log (builder output, flash
  log, VM serial console).
- **Test in VM** builds the VM image if needed and opens the VM window.
  **Build Pi 5 Image** builds without writing. **Prepare Raspberry Pi 5**
  confirms the disk (type its identifier), builds if needed, then flashes.

## Development

```sh
swift build && sh scripts/sign.sh .build/debug/maryos .build/debug/MaryOSApp   # what make build does
swift test                                                                       # MaryOSKit unit tests
python3 builder/tests/unzboot_test.py                                            # the kernel extractor
builder/build.sh all vm --dry-run                                                # the stages, in Docker
MARYUI_DIR=../../MaryUI builder/build.sh ui                                      # the desktop from a sibling MaryUI checkout
```

Layout:

```
distro/                the MaryOS definition (see above)
builder/               Dockerfile, run-in-docker.sh, build.sh, lib/{common,chroot,rootfs,ui,target,image}.sh, unzboot.py, tests/
maryui/                MaryUI (git submodule): web/ is the design system, linux/ the C library + compositor the ui stage builds (pinned to a commit that carries both)
Sources/MaryOSKit      Distro (config, paths), Build (runner, artifacts), VM (spec, configuration, runner,
                       controller, window), Disks, Flash, Shell, Model, Orchestration (doctor, coordinator)
Sources/maryos         the CLI (swift-argument-parser); synchronous commands that pump the main run loop
Sources/MaryOSApp      the SwiftUI app (product MaryOSApp; bundled as MaryOS.app)
Tests/MaryOSKitTests   unit tests and diskutil fixtures
scripts/               sign.sh, bundle.sh
vm.sh                  one command to build what is missing and boot the VM
ui.sh                  compile the desktop and boot to it, live-reloading on each rebuild (--image for the embedded one)
docs/                  the MaryOS journey
out/ cache/ work/ state/   build output, caches, VM state (gitignored)
```

## Relationship to the ravynOS kit

Both kits share one idea: one definition of the system, one image layout for
the Pi and the VM, and tooling that says what a card will do. The ravynOS kit
brought an XNU kernel to a stand-in userland on a QEMU test bed and stopped
there; MaryOS starts from a complete Linux userland and puts the effort into
the distro instead. The Mac-side plumbing (disk listing, privileged `dd`,
log tailing) is deliberately duplicated so each kit stands alone.
