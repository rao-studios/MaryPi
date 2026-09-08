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
| 1 | Kernel bring-up | Level 0 + `EFI/BOOT/BOOTAA64.EFI` (ravynOS booter) + `ravynos/kernel` + boot plist, plus `ravynos/Extensions/*.kext` and a minimal root partition when the ravynOS tree has them | UEFI runs the booter, which loads the arm64 XNU kernel and its kexts. The console goes to the serial port and is mirrored on the screen. Without kexts the kernel panics at the missing platform driver; with them it links the drivers at boot, mounts the card's HFS+ partition and starts `ravyninit`, a freestanding stand-in for launchd with a small console shell. |
| 2 | Full system | Level 1 + `ravynos/kernelcache` + an arm64 root filesystem on the HFS+ partition | Kernel mounts the root partition and starts launchd. Experimental. |

The level is recomputed from the ravynOS build tree on every scan:

- Level 1 needs an arm64 Mach-O kernel (`../build/sysroot-arm64/System/Library/Kernels/kernel[.development]`)
  and a booter (`../build/booter/bootaa64.efi`, `$MARYPI_BOOTER`, or `sysroot-arm64/System/Library/CoreServices/bootaa64.efi`).
  It picks up `sysroot-arm64/System/Library/Extensions` (`bmake TARGET_ARCH=arm64 kexts`) and
  `sysroot-arm64/sbin/ravyninit` (`bmake TARGET_ARCH=arm64 ravyninit`) when they exist; with a
  storage stack among the kexts the kernel flags gain `rd=disk0s2`.
- Level 2 additionally needs an arm64 `../build/kernelcache-arm64`, arm64 `sbin/launchd` and
  `usr/lib/libSystem.B.dylib` in `sysroot-arm64`, and `IOStorageFamily.kext` in its Extensions.

`marypi payload` prints the level, the paths it found and why it is not higher.

The full story, from an empty card to a kernel running process 1, is in
[docs/](docs/README.md).

## Requirements

- macOS 15 or newer, Xcode 26 (Swift 6). No extra tools are needed for Level 0.
- A Raspberry Pi 5 with a bootloader EEPROM recent enough for rpi5-uefi. If nothing
  appears on HDMI or serial, update the EEPROM with Raspberry Pi Imager first. MaryPi
  never touches the EEPROM.
- For kernel payloads: a ravynOS checkout built with `bmake TARGET_ARCH=arm64`.
- For the VM test bed: `brew install qemu` (macOS) or `apt install qemu-system-arm qemu-efi-aarch64` (Linux).

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

## VM test bed

QEMU has no Raspberry Pi 5 model, but its `virt` machine has the same building
blocks the Pi 5 bring-up uses (GICv2 or GICv3, PL011 UART, generic timer, EDK2
UEFI). The VM definition lives in `vm/` and runs on arm64 macOS and Linux:

```sh
swift run marypi vm doctor                       # qemu, firmware, accelerator, display
swift run marypi vm run                          # build a --qemu image and boot it in a window
swift run marypi vm run --profile qemu-virt-gicv3   # GICv3 guest: hvf on Apple Silicon, kvm on Linux
swift run marypi vm run --display serial         # serial console on this terminal (Ctrl-A X quits)
swift run marypi vm run --detach                 # leave it running; then:
swift run marypi vm status
swift run marypi vm serial                       # follow vm/state/<profile>/serial.log
swift run marypi vm stop                         # QMP quit, then signals
```

`vm/run.sh` does the same without Swift (`vm/run.sh run --image q.img`), so a
Linux box only needs QEMU and its EDK2 firmware package. Profiles are
`KEY=VALUE` files in `vm/profiles/`; per-profile state (UEFI variables, disk
image, `serial.log`, QMP socket, pid) goes to `vm/state/` in a checkout or the
user's cache directory for the app bundle. See `vm/README.md`.

Two profiles ship: `qemu-virt` (GICv2, the Pi 5's interrupt controller family;
runs on TCG on macOS because HVF cannot emulate a GICv2) and `qemu-virt-gicv3`
(hvf/kvm fast path; the kernel selects its virtual timer automatically under
Apple's hypervisor). The `ramfb` device gives the booter a UEFI framebuffer,
so the kernel's boot console appears in the QEMU window. In the app,
**Test in VM** builds the image and opens the window; serial output streams
into the Log pane; **Stop VM** ends it.

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
Sources/MaryPiKit      Model, Shell (CommandRunner), Disks, Payload, Firmware, Image, Flash, Orchestration, VM
Sources/marypi         the CLI (swift-argument-parser)
Sources/MaryPiApp      the SwiftUI app (product MaryPiApp; bundled as MaryPi.app)
Tests/MaryPiKitTests   unit tests and fixtures
vm/                    profiles/, run.sh (portable launcher), README.md; vm/state/ is local
scripts/               bundle.sh, qemu-virt.sh (wrapper), update-manifest.sh
```

## Relationship to the ravynOS arm64 bring-up

MaryPi lights up higher payload levels as the ravynOS tree gains arm64 support:

| ravynOS milestone | MaryPi effect |
|---|---|
| `bmake TARGET_ARCH=arm64 -C Kernel xnu_all` links an arm64 kernel | `marypi payload` reports the kernel (still Level 0 without a booter) |
| `Kernel/booter` produces `bootaa64.efi` | Level 1; `marypi vm run` boots the kernel in QEMU |
| kernelcache with kexts + arm64 userland in `sysroot-arm64` | Level 2 |
