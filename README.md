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
window and flash cards, and MaryVNC.app, which shows a MaryOS desktop on the Mac over the USB
cable or the network.

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
./vnc.sh                                # MaryVNC: a MaryOS Pi's desktop on this Mac (see MaryVNC below)
```

## MaryVNC: a Pi's desktop on your Mac

MaryVNC shows a MaryOS Pi's desktop in a Liquid Platinum window on the Mac and sends it your pointer and keys.
A Pi answers only Macs it has paired with, and tells nothing else on the network that it is there.

**Before you start:** a Pi running MaryOS with MaryVNC Nearby (MaryOS e3bf253 or later, written with MaryOS's
`./pi.sh flash`) on the same network as the Mac.

**1. Open the viewer.**

```sh
cd linux
make app-vnc && open dist/MaryVNC.app
```

Allow Local Network access when macOS asks: MaryVNC calls for Pis on your network.

**2. Pair, once per Mac.** Press the Pi's power button briefly. For two minutes it is ready to pair, and its green
light blinks. It appears in the sidebar under **Ready to pair**: select it and click **Pair** (the sheet shows the
key the Pi offers, which `maryvncctl status` on the Pi shows too). The desktop appears, and the Pi's window closes
behind this Mac. On the Pi, `maryvncctl pairs` now lists this Mac by name. Without the button:
`ssh mary@<the Pi's address> maryvncctl pair-window 120`.

**3. From then on it connects on its own.** Whenever a paired Pi is nearby, MaryVNC finds it and connects (the one
used last, if several are), and when the link drops it looks for the Pi again and reconnects wherever it is.

**Using it**

| | |
|---|---|
| Pointer, clicks, scrolling | go to the Pi |
| ⌘ | is the Pi's Super key: ⌘W closes a window, ⌘Space toggles Spotlight |
| ⌘Q, ⌘H, ⌘M, ⌘, | stay with the Mac |
| Toolbar | Disconnect; Refresh (redraw the whole desktop) or, when not connected, Look Again; Best or Fast picture; Pair |
| MaryVNC › Settings… | the accent (Blue or Graphite), ⌘ as Super or as Control (for terminal programs), the picture |
| File › Forget This Pi… | unpairs on the Mac; `maryvncctl forget <fingerprint>` unpairs on the Pi |

**When it does not work**

| What you see | What to do |
|---|---|
| Nothing in the sidebar | Allow MaryVNC under System Settings › Privacy & Security › Local Network. A viewer started from an editor's terminal gets the editor's network permission, which is often off: open `dist/MaryVNC.app` instead. On the Pi, `maryvncctl status` shows where it answers (`nearby: UDP 5901 on wlan0`). |
| A paired Pi never appears | Some networks keep their devices apart or drop multicast (guest Wi-Fi, some mesh routers). Use Connect to Address… once with the Pi's address; MaryVNC then calls that address directly. |
| "did not accept pairing" | The pairing window closed: it lasts two minutes and closes when a Mac pairs. Press the power button again. |
| "did not accept this Mac" | The Pi forgot this Mac or was reflashed, or another machine now has its old address. If `maryvncctl pairs` on the Pi does not list this Mac, choose File › Forget This Pi… and pair again. |
| "Another Mac is watching" | A Pi shows its desktop to one Mac at a time; Connect takes it back. |
| "Connecting…" never ends | On the Pi, `maryvncctl status` should say `desktop: 1280x800 (connected)`; if it does not, see `systemctl status maryos-desktop`. |
| A Keychain prompt after a rebuild | Choose Always Allow: the viewer keeps this Mac's key in the Keychain. |

**Nobody else sees the Pi.** MaryVNC Nearby (MaryOS docs/14) takes Bonjour's place: the viewer calls, and a Pi
answers only a call carrying the tag of a Mac it is paired with, or any call while its pairing window is open. It
announces nothing, so no other device on the network can list it.

Building, testing and the files behind the viewer are in [linux/README.md](linux/README.md#maryvnc); how MaryVNC
works and what keeps it private are in MaryOS's
[docs/14-maryvnc.md](https://github.com/rao-studios/MaryOS/blob/main/docs/14-maryvnc.md).

## Layout

```
ravynos/   Swift package MaryPi (MaryPiKit, marypi CLI, MaryPi.app), vm/ QEMU profiles, docs/
linux/     Swift package MaryOS (MaryOSKit, maryos CLI, MaryOS.app with the VZ window and the flasher;
           MaryVNCKit, LiquidPlatinum and MaryVNC.app, the remote-desktop viewer);
           maryos/ is the MaryOS submodule: distro/, builder/, maryui/ (the desktop in C), mary/, docs/
Makefile   make ravynos-<target> / make linux-<target> delegate to the kits
```
