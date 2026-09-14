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
A Pi answers only Macs it has paired with.

**Before you start:** a Pi running MaryOS with MaryVNC (MaryOS 6b87c54 or later, written with MaryOS's
`./pi.sh flash`) on the same network as the Mac, and `ssh mary@maryos.local` working from Terminal.app.

**1. Open the viewer.** From Terminal.app:

```sh
cd linux
make app-vnc && open dist/MaryVNC.app
```

Allow Local Network access when macOS asks. Pis on the network appear in the sidebar.

**2. Pair, once per Mac.** Let the Pi accept a new Mac for five minutes:

```sh
ssh mary@maryos.local maryvncctl pair-window 300
```

Then in MaryVNC choose **Connect to Address…**, enter `maryos.local`, tick **Pair** and click **Connect**. The
Pi's desktop appears. On the Pi, `maryvncctl pairs` now lists this Mac by name.

**3. From then on it connects on its own.** Whenever a paired Pi is on the network, MaryVNC connects to it (the
one used last, if several are), and it reconnects when the link drops.

**Using it**

| | |
|---|---|
| Pointer, clicks, scrolling | go to the Pi |
| ⌘ | is the Pi's Super key: ⌘W closes a window, ⌘Space toggles Spotlight |
| ⌘Q, ⌘H, ⌘M, ⌘, | stay with the Mac |
| Toolbar | Disconnect, Refresh (redraw the whole desktop), Best or Fast picture |
| MaryVNC › Settings… | the accent (Blue or Graphite), ⌘ as Super or as Control (for terminal programs), the picture |
| File › Forget This Pi… | unpairs on the Mac; `maryvncctl forget <fingerprint>` unpairs on the Pi |

**When it does not work**

| What you see | What to do |
|---|---|
| Nothing in the sidebar | Allow MaryVNC under System Settings › Privacy & Security › Local Network, and check the Pi: `ssh mary@maryos.local systemctl is-active maryvncd`. A viewer started from an editor's terminal gets the editor's network permission, which is often off; start it from Terminal.app or as the app. |
| The Pi answers ssh but is not listed | Images before MaryOS 0f770b7 do not announce it. On the Pi: `sudo chmod 755 /etc/systemd/dnssd && sudo systemctl restart systemd-resolved`. Connect to Address… works either way. |
| "did not accept pairing" | The pairing window closed; open it again (step 2). |
| "does not know this Mac" | The Pi was reflashed or forgot this Mac; pair again. |
| "Another Mac is watching" | A Pi shows its desktop to one Mac at a time; Connect takes it back. |
| "Connecting…" never ends | On the Pi, `maryvncctl status` should say `desktop: 1280x800 (connected)`; if it does not, see `systemctl status maryos-desktop`. |
| A Keychain prompt after a rebuild | Choose Always Allow: the viewer keeps this Mac's key in the Keychain. |

**Over the USB cable.** A Pi on the Mac's USB cable appears under **On the cable**, and **Pair over USB** pairs it
without a window. The cable has to carry power and data, and the obvious ones do not: over USB-C-to-USB-C the Mac
powers a Pi 5 but never connects its USB data, and a USB-A port cannot power a Pi 5. A USB-C power/data splitter
or a powered hub should do both; neither has been tried yet.

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
