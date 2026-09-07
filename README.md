# MaryPi

One-click Raspberry Pi 5 provisioning for [ravynOS](https://github.com/riteshpakala/ravynos) from a Mac.

Plug in an SD card, pick it, press **Prepare Raspberry Pi 5**, type the disk
identifier, enter your password once, and the card is partitioned, populated
and ejected. MaryPi is a Swift package: a library (`MaryPiKit`), a command-line
tool (`marypi`) and a SwiftUI app (`MaryPi.app`).

MaryPi is honest about what it can put on a card. ravynOS does not yet boot on
arm64, so the app always shows the current **payload level** and what the Pi
will actually do with the card.

## Payload levels

| Level | Name | What is on the card | What the Pi does |
|---|---|---|---|
| 0 | Bootstrap only | Raspberry Pi firmware config + [rpi5-uefi](https://github.com/worproject/rpi5-uefi) UEFI | Boots into the UEFI setup screen. No ravynOS code runs. Verifies the board, EEPROM and serial console. |
| 1 | Kernel bring-up | Level 0 + `EFI/BOOT/BOOTAA64.EFI` (ravynOS booter) + `ravynos/kernel` + boot plist | UEFI runs the booter, which loads the arm64 XNU kernel. Serial output only; the kernel bootstraps, starts IOKit and panics because no platform driver exists yet. |
| 2 | Full system | Level 1 + `ravynos/kernelcache` + an arm64 root filesystem on the HFS+ partition | Kernel mounts the root partition and starts launchd. Experimental. |

The level is recomputed from the ravynOS build tree on every scan:

- Level 1 needs an arm64 Mach-O kernel (`../build/sysroot-arm64/System/Library/Kernels/kernel[.development]`)
  and a booter (`../build/booter/bootaa64.efi`, `$MARYPI_BOOTER`, or `sysroot-arm64/System/Library/CoreServices/bootaa64.efi`).
- Level 2 additionally needs an arm64 `../build/kernelcache-arm64`, arm64 `sbin/launchd` and
  `usr/lib/libSystem.B.dylib` in `sysroot-arm64`, and `IOStorageFamily.kext` in its Extensions.

`marypi payload` prints the level, the paths it found and why it is not higher.

## Requirements

- macOS 15 or newer, Xcode 26 (Swift 6). No extra tools are needed for Level 0.
- A Raspberry Pi 5 with a bootloader EEPROM recent enough for rpi5-uefi. If nothing
  appears on HDMI or serial, update the EEPROM with Raspberry Pi Imager first. MaryPi
  never touches the EEPROM.
- For kernel payloads: a ravynOS checkout built with `bmake TARGET_ARCH=arm64`.
- For the QEMU test bed: `brew install qemu`.

## Quick start

```sh
cd MaryPi
swift build
swift run marypi doctor                 # tools, checkout, build tree, firmware cache
swift run marypi list                   # removable disks MaryPi is willing to erase
swift run marypi payload                # what would go on a card right now
swift run marypi firmware fetch         # download + verify rpi5-uefi into ~/Library/Caches/MaryPi
swift run marypi build-image --out ~/Desktop/pi5.img
swift run marypi flash --disk disk4     # erase disk4 and write a fresh image (asks for confirmation)
swift run MaryPiApp                     # the GUI
```

Every subcommand accepts `--ravynos <path>` to point at a ravynOS checkout. The
checkout is otherwise found through `MARYPI_RAVYNOS_ROOT`, the saved setting
(`marypi config set ravynos <path>`, or **Choose ravynOS Checkout…** in the app),
or `~/Documents/repositories/ravynos`. The build directory defaults to the
checkout's sibling `build/` and can be overridden with `MARYPI_BUILD_DIR`.

## The app

`swift run MaryPiApp` opens the window; `scripts/bundle.sh` produces
`dist/MaryPi.app` (release build, ad-hoc signed).

- **Left**: removable disks (SD readers, USB, Thunderbolt; 4 GB to 2 TB). Internal
  and virtual disks are never listed. The list refreshes itself when a card is
  inserted or removed.
- **Center**: the payload card with the level badge, the honest summary, what the
  Pi will do, the discovered paths and any findings.
- **Right/bottom**: the step list with progress, and the live log.
- **Prepare Raspberry Pi 5** opens a confirmation sheet that names the disk, its
  size and bus, and requires typing the `diskN` identifier. Then macOS asks for an
  administrator password once. **Build Image Only** writes a `.img` without touching
  any disk.

## What a card looks like

MBR partition table:

1. `MARYPI`, FAT32, 256 MiB, active. Read by the Pi firmware and UEFI.
2. `ravynOS`, HFS+ (journaled), the rest of the card. The root filesystem (Level 2), otherwise empty.

On the FAT partition:

```
config.txt              rpi5-uefi's config.txt verbatim + a MaryPi block (enable_uart=1, uart_2ndstage=1)
RPI_EFI.fd              EDK2 UEFI firmware for the Pi 5
bcm2712-rpi-5-b.dtb     device tree
MARYPI.txt              level, versions, kernel SHA-256, ravynOS git SHA, date
MARYPI-README.txt       the same explanation as this section
EFI/BOOT/BOOTAA64.EFI   ravynOS booter                     (Level >= 1)
ravynos/kernel          arm64 XNU                          (Level 1)
ravynos/kernelcache     prelinked arm64 XNU + kexts        (Level 2)
ravynos/com.ravynos.boot.plist
```

`com.ravynos.boot.plist` mirrors Apple's `com.apple.Boot.plist` and is read by the
ravynOS AArch64 booter (`Kernel/booter` in the ravynOS tree):

```xml
<key>Kernel</key>        <string>\ravynos\kernel</string>
<key>Kernel Flags</key>  <string>-v serial=3 debug=0x8 cpus=1</string>
```

Level 2 adds `rd=disk0s2`. Override with `marypi build-image "--kernel-flags=..."` (the
`=` form, because the value starts with `-v`). Avoid `debug` bit `0x40`: it makes the
kernel's serial KDP stub wait for a debugger before IOKit starts.

## How flashing works

1. The image is built without root: a sparse file is attached with
   `hdiutil attach -nomount`, partitioned with `diskutil partitionDisk … MBR`, the
   mounted volumes are populated with plain file copies, and the image is ejected.
2. The target disk is re-read with `diskutil info` immediately before writing and
   must still be a whole, physical, external disk of the same size.
3. One generated shell script runs under `osascript … with administrator
   privileges`: `diskutil unmountDisk force`, `dd bs=4m` to the raw device (polled
   with `SIGINFO` for progress), `sync`, `diskutil eject`. Its log is tailed into the
   app. The script and log live under `$TMPDIR/MaryPi/<uuid>/`.

## Serial console

Connect a 3.3 V USB-UART adapter to the Pi 5's 3-pin debug header (GND, TX, RX) and
open it at 115200 8N1, for example `screen /dev/cu.usbserial-XXXX 115200`. The UEFI
firmware and the ravynOS kernel both log there.

## QEMU test bed

QEMU has no Raspberry Pi 5 model, but its `virt` machine has the same building
blocks the Pi 5 bring-up needs (GICv2, PL011 UART, generic timer, EDK2 UEFI):

```sh
swift run marypi build-image --qemu --out /tmp/q.img
scripts/qemu-virt.sh /tmp/q.img
```

`--qemu` builds the same image without the `rd=` kernel flag. The script boots it with
`-M virt,gic-version=2 -cpu cortex-a76` and the serial console on the terminal
(`QEMU_ACCEL=hvf` switches to hardware virtualization with `-cpu host`).

## Updating the pinned firmware

`Sources/MaryPiKit/Resources/Manifest.json` pins the rpi5-uefi release by URL, size
and SHA-256. To move to a new release:

```sh
scripts/update-manifest.sh v0.4
swift test
```

The script downloads the asset, records the real hash and file list, rewrites the
manifest and prints the diff. `ManifestTests` validates the result.

## Development

```sh
swift build          # everything
swift test           # MaryPiKit unit tests; no privileged operations
make app             # dist/MaryPi.app
```

Layout:

```
Sources/MaryPiKit      Model, Shell (CommandRunner), Disks, Payload, Firmware, Image, Flash, Orchestration
Sources/marypi         the CLI (swift-argument-parser)
Sources/MaryPiApp      the SwiftUI app (product MaryPiApp; bundled as MaryPi.app)
Tests/MaryPiKitTests   unit tests and fixtures
scripts/               bundle.sh, qemu-virt.sh, update-manifest.sh
```

## Relationship to the ravynOS arm64 bring-up

MaryPi lights up higher payload levels as the ravynOS tree gains arm64 support:

| ravynOS milestone | MaryPi effect |
|---|---|
| `bmake TARGET_ARCH=arm64 -C Kernel xnu_all` links an arm64 kernel | `marypi payload` reports the kernel (still Level 0 without a booter) |
| `Kernel/booter` produces `bootaa64.efi` | Level 1; `--qemu` images boot under `scripts/qemu-virt.sh` |
| kernelcache with kexts + arm64 userland in `sysroot-arm64` | Level 2 |
