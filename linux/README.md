# MaryOS kit

The Mac side of [MaryOS](https://github.com/rao-studios/MaryOS), an Ubuntu 24.04 (Noble) arm64
fork with the Liquid Platinum desktop. MaryOS itself (the definition in `distro/`, the image
pipeline in `builder/`, the desktop in `maryui/`, Mary in `mary/` and the docs) is its own
repository, checked out here as the submodule [maryos/](maryos/README.md). This directory is the
Swift package that works on it: it drives the image build (in Docker), boots the VM image in an
Apple Virtualization.framework window, and writes the Raspberry Pi 5 image to an SD card with one
admin prompt. It is the Linux sibling of the [ravynOS kit](../ravynos/README.md) and follows the
same rule: say plainly what a card or a VM will do. It also holds [MaryVNC](#maryvnc)'s viewer, which shows a
MaryOS desktop on this Mac over the USB cable or the network.

The full story, from `distro.conf` to a login prompt and a desktop on the Pi and in the VM, is in
[maryos/docs/](maryos/docs/README.md).

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
git submodule update --init     # maryos/, the MaryOS repository
cd linux
maryos/vm.sh                    # build the CLI, compile the desktop and Mary, build the VM image if missing, boot to the desktop; Shift+Space opens Spotlight
maryos/vm.sh                    # again while it runs: recompile, and the running VM restarts the desktop on the new build
maryos/vm.sh --image            # the desktop embedded in the image instead (--rebuild re-embeds the fresh one)
maryos/terminal.sh              # the same image to a login prompt
maryos/terminal.sh --console    # with this terminal on the serial console (Ctrl-] stops the VM)
maryos/vm.sh stop               # ask the guest to shut down

make build                      # swift build, then scripts/sign.sh on the binaries
make cli ARGS=doctor            # tools, kit, Docker, Virtualization, entitlement, built images
make image TARGET=vm            # maryos/builder/build.sh all vm -> maryos/out/maryos-0.0-vm.img + maryos/out/vm/
make image TARGET=pi5           # maryos/out/maryos-0.0-pi5.img
make ui                         # compile + test the desktop (maryos/maryui) -> maryos/out/ui
make mary                       # compile + test Mary's packages (maryos/mary) -> maryos/out/mary
make vm / make terminal         # maryos/vm.sh / maryos/terminal.sh; log in as mary (password in distro.conf)
make cli ARGS=list              # removable disks the flasher is willing to erase
make flash DISK=disk4           # erase disk4 and write the pi5 image (asks first, one admin prompt)
make run-app                    # the GUI
make app                        # dist/MaryOS.app (release, kit bundled, signed)
make test                       # the token check, swift test and the builder's tests
./vnc.sh                        # MaryVNC: the viewer for a MaryOS desktop, finding Pis with MaryVNC Nearby
./vnc.sh --connect 10.0.0.73    # by address, with the key of the Pi used last (add --pair to pair)
make app-vnc                    # dist/MaryVNC.app
./vnc-light.sh                  # MaryVNC Light: MaryVNC in the menu bar; a press of the Pi's power button opens its desktop
make lp-tokens                  # regenerate Liquid Platinum's tokens from maryos/maryui's lp_tokens.h
```

`vm.sh` and `terminal.sh` belong to MaryOS. They build this package's `maryos` CLI, finding it
through `MARYPI_DIR`, through `..` when MaryOS is this submodule, or in a MaryPi clone beside a
MaryOS clone, and point it at their own checkout. A standalone MaryOS clone therefore boots the
same way. Each kit keeps its own images in `out/` and its VM disk in `state/`, so to flash an
image built in a standalone clone, name that kit: `MARYOS_KIT_DIR=../../MaryOS make flash DISK=disk4`.

The CLI, once built and signed (`.build/debug/maryos`):

```
maryos doctor                       what this Mac can do
maryos config                       distro.conf, directories, built images
maryos build [--target pi5|vm|both] [--stage rootfs|ui|mary|maryvnc|apps|target|image|all] [--fresh] [--dry-run]
maryos list [--all]                 removable disks
maryos flash --disk diskN [--image PATH] [--target pi5] [--build] [--yes]
maryos vm run [--desktop [--dev]] [--headless] [--console] [--microphone] [--memory MiB] [--cpus N] [--share tag=/dir] [--fresh] [--disk-size GiB] [--dry-run]
maryos vm stop | status | serial [--no-follow] | reset
```

Every command accepts `--kit <dir>` to point at a kit (a MaryOS checkout); otherwise it is found
from `MARYOS_KIT_DIR`, from the current directory or its `maryos/` or `linux/maryos/`, from the app
bundle, or in this checkout's submodule. `swift run maryos <command>` works too: `vm run` signs the
binary on its first use after a build and starts again.

## What a card looks like

MBR partition table, the same for both targets:

1. `MARYOS`, FAT32, 512 MiB, active, at 4 MiB. Mounted at `/boot/firmware`.
2. `maryos-root`, ext4, the rest of the image. Grown to the whole card or
   disk by `maryos-firstboot` on the first boot.

What goes into the image, and how it is built, is MaryOS's to explain:
[the fork](maryos/docs/02-the-fork.md), [building from source](maryos/docs/03-building-from-source.md)
and [booting on the Pi 5](maryos/docs/04-boot-on-pi5.md).

## The VM

Virtualization.framework boots the kernel directly: `VZLinuxBootLoader`
with the kit's `out/vm/Image` and `initrd.img`, command line
`console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait`. The
disk is an APFS clone of the built image in the kit's `state/vm/vm/disk.img`,
grown (sparsely) to `VM_DISK_GIB` so the guest fills it on first boot. The guest
gets NAT networking with a fixed MAC address, a virtio-gpu window with USB
keyboard and pointer, and the kit's `out/` shared over virtiofs as `maryos-out`
(mounted at `/mnt/maryos-out`). Everything on the serial console goes to
`state/vm/vm/serial.log`; `--console` attaches your terminal to it (Ctrl-]
stops the VM) and `--headless` drops the window.

`maryos vm stop`, Ctrl-C, or closing the window sends the guest an ACPI
power button; systemd powers off cleanly, and the VM is forced off after 20
seconds if it does not. `maryos vm reset` deletes the disk so the next run
starts from the built image again.

## How flashing works

1. The image comes from the kit's `out/` (built first if missing).
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

## MaryVNC

MaryVNC shows a MaryOS desktop on this Mac and sends it this Mac's pointer and keys. The server is MaryOS's
`maryvncd` ([maryos/docs/14-maryvnc.md](maryos/docs/14-maryvnc.md)); the viewers are here. `./vnc.sh` (or
`make vnc`) builds and opens MaryVNC.app, the windowed viewer; `make app-vnc` makes `dist/MaryVNC.app`.
`./vnc-light.sh` (or `make vnc-light`) builds `dist/MaryVNCLight.app` and opens it: MaryVNC Light, the same viewer
in the menu bar, whose portal opens when a Pi's power button is pressed. They share this Mac's key and the paired
Pis, `MaryVNCKit` and `MaryVNCViewer` (the desktop view and one session's lifecycle); run one of them at a time.

- **Finding Pis.** MaryVNC Nearby (`Sources/MaryVNCKit/Nearby`; the protocol is in maryos/docs/14-maryvnc.md):
  the viewer calls on UDP 5901, to a multicast group on each interface and to the address each paired Pi last
  answered from, and a Pi answers only a call carrying this Mac's tag, or any call while its pairing window is
  open. Paired Pis that answer are listed under Nearby, Pis ready to pair under Ready to pair; nothing else on the
  network can see a Pi. The viewer calls at start, when the network changes, when the Mac wakes and on Look Again,
  then every 5 s for two minutes and every 15 s after, and not while connected. It never listens on a port: the
  answers are replies. macOS asks once for Local Network access.
- **Pairing.** A short press of the Pi's power button (or `maryvncctl pair-window 120`) opens its pairing window,
  and the Pi appears under Ready to pair. Pair runs Noise XX expecting the key the Pi's answer offered, and the
  window closes once a Mac has paired, or once a paired Mac connects while it is open. `./vnc.sh --connect HOST --pair` pairs by address. A paired Pi that
  answers then connects on its own, the one used last first, and a lost one is looked for and reached wherever it
  answers from. A refusal never unpairs this Mac on its own: only the Pi's `bye` `forgotten`, or Forget This Pi….
- **Keys.** This Mac's private key is in the login Keychain (service `com.maryos.MaryVNC`, this device only);
  after a rebuild macOS asks whether the new binary may read it: choose Always Allow. The paired Pis are in
  `~/Library/Application Support/MaryVNC/pairs.json` (public keys and last addresses), with the settings beside it.
- **The clipboard.** Text crosses both ways while connected (`PasteboardBridge` in `Sources/MaryVNCViewer`): the
  Mac's pasteboard goes to the Pi when the desktop shows and whenever it changes (looked for twice a second), unless
  the copying app marked it concealed or transient (`ClipboardText`), and the Pi's clipboard lands on the Mac's
  pasteboard without being sent back. UTF-8 up to 1 MiB, text only.
- **The keyboard.** ⌘ is Super on the Pi by default, so MaryOS's own chords work (⌘Space opens Spotlight, ⌘W
  closes a window); Settings switches it to Control for terminal programs. ⌘Q, ⌘H, ⌘⌥H, ⌘M and ⌘, stay with
  the Mac.
- **MaryVNC Light.** A menu bar app (`Sources/MaryVNCLightApp`, `LSUIElement`) with no window until a Pi's
  desktop shows. It calls every second while a paired Pi is answering and every 1.5 s otherwise, to the paired
  Pis' last addresses and the groups in turn (never both in one call, so a Pi answers each of this Mac's
  addresses at most once a call), and not while its portal is showing. `PortalRules` (`Sources/MaryVNCKit/Nearby`)
  decides when the portal opens on its own: a paired Pi's answer shows its window open once it has been seen
  closed (its power button was pressed, and connecting closes the window again); a paired Pi answers after 20 s of
  watching without a word (it has just booted), outside the 15 s after launch, wake or a network change; or a Pi
  whose portal was lost answers again. A Pi ready to pair brings up a pair dialog (a list when there are several);
  Not Now keeps it quiet until its window closes. The portal is a borderless, non-activating panel that takes the
  keys as it appears, one point per Pi pixel up to 90% of the screen: rest the pointer on its top edge for a
  grabber that moves it (double-click fills the screen), drag its edges to resize, and ⌘Q, ⌘H or ⌘M put it away.
  A lost link dims it and retries for 20 s. The menu lists the Pis nearby and ready to pair, the picture, ⌘ on the
  Pi and Forget. Its log: `log stream --predicate 'subsystem == "com.maryos.MaryVNC"'`.
- **Liquid Platinum.** The window is drawn with `Sources/LiquidPlatinum`. Its tokens are generated from the
  submodule's `lp_tokens.h` (`make lp-tokens`; `make test` fails when they drift), and the brushed grain is
  maryui's tile byte for byte. `LP_GALLERY_OUT=/tmp/lp swift test --filter GalleryRenderTests` renders every
  piece to PNGs.
- **Without a Pi.** `MARYVNCD=/path/to/maryvncd swift test --filter MaryvncdInteropTests` runs MaryVNCKit
  against a real `maryvncd` built on this Mac from MaryOS's `maryvnc/` (`make -C maryvnc all`, with Homebrew's
  openssl@3, jpeg-turbo and json-c). For the app, run `maryvncd --test-pattern 1280x800 --bind 127.0.0.1
  --pair-window 300`, then `./vnc.sh --test-profile /tmp/vnc --connect 127.0.0.1 --pair`: the test profile keeps
  this Mac's key, the pairs and the settings in that directory instead of the Keychain, and makes no Nearby calls
  unless `--nearby` is added, so macOS asks for nothing.

## Development

```sh
swift build && sh scripts/sign.sh .build/debug/maryos .build/debug/MaryOSApp   # what make build does
swift test                                                                       # MaryOSKit, MaryVNCKit and LiquidPlatinum tests
make -C maryos test                                                              # the builder's tests
maryos/builder/build.sh all vm --dry-run                                         # the stages, in Docker
```

Layout:

```
maryos/                MaryOS (git submodule): distro/, builder/, maryui/ (the desktop in C), mary/, docs/,
                       vm.sh and terminal.sh; its out/, cache/, work/ and state/ are gitignored there
Sources/MaryOSKit      Distro (config, paths), Build (runner, artifacts), VM (spec, configuration, runner,
                       controller, window), Disks, Flash, Shell, Model, Orchestration (doctor, coordinator)
Sources/maryos         the CLI (swift-argument-parser); synchronous commands that pump the main run loop
Sources/MaryOSApp      the SwiftUI app (product MaryOSApp; bundled as MaryOS.app)
Sources/MaryVNCKit     MaryVNC's viewer side: Noise XX and IK, the wire, MaryVNC Nearby, the session, pairing, the key map
Sources/LiquidPlatinum Liquid Platinum for SwiftUI: generated tokens, the brushed grain, surfaces and controls
Sources/MaryVNCViewer  what the two viewers share: RemoteView (the desktop, pointer and keys) and PiLink (one session)
Sources/MaryVNCApp     the MaryVNC viewer (product MaryVNCApp; bundled as MaryVNC.app; vnc.sh runs it)
Sources/MaryVNCLightApp  MaryVNC Light, the menu bar portal (product MaryVNCLightApp; bundled as MaryVNCLight.app by vnc-light.sh)
Tests/MaryOSKitTests   unit tests and diskutil fixtures
Tests/MaryVNCKitTests  the Noise and Nearby vectors (the same fixtures as MaryOS's maryvnc/), the wire, pairing, the session,
                       the Pis in view and MaryVNC Light's portal rules
Tests/LiquidPlatinumTests  the tokens, the brush tile against maryui's, the gallery render
scripts/               sign.sh, bundle.sh (MaryOS, MaryVNC or MaryVNCLight), gen-lp-tokens.py
```

## Relationship to the ravynOS kit

Both kits share one idea: one definition of the system, one image layout for
the Pi and the VM, and tooling that says what a card will do. The ravynOS kit
brought an XNU kernel to a stand-in userland on a QEMU test bed and stopped
there; MaryOS starts from a complete Linux userland and puts the effort into
the distro instead. The Mac-side plumbing (disk listing, privileged `dd`,
log tailing) is deliberately duplicated so each kit stands alone.
