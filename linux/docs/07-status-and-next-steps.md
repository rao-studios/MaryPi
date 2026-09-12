# 7. Status and next steps

State at the end of this journey (September 8, 2026), verified on the Mac
with the Virtualization.framework test bed. The Raspberry Pi 5 image is
built and inspected but has not yet been booted on a board.

## What works

| Stage | Result |
|---|---|
| Builder | `builder/build.sh all both` in Docker Desktop: debootstrap of Noble arm64, base packages, overlay and hooks, a 222 MB cached base tarball; two targets; two images (about 2.9 GB each, 1.8 GB on disk) with manifests and checksums. About 10 minutes from nothing, 3 minutes per target with the cache |
| Kernel extraction | `unzboot.py` unpacks Ubuntu's `vmlinuz-6.8.0-139-generic` (an EFI zboot image) into a 59 MB raw arm64 `Image` that Virtualization.framework accepts |
| VM boot | `maryos vm run` boots the image in about 4 seconds to `maryos login:` on `hvc0` and on `tty1` in the window: virtio disk `vda` with both partitions, root mounted by label, `Welcome to MaryOS 0.0 (Liquid Platinum)!` |
| Identity | `/etc/os-release` says MaryOS with `ID_LIKE="ubuntu debian"`, hostname `maryos`, MaryOS motd and issue |
| First boot | `maryos-firstboot` runs once: grows the root partition to the disk, creates the ssh host keys, stamps itself |
| Networking | `enp0s1` gets `192.168.64.2/24` from the NAT network; `systemd-networkd`, `systemd-resolved` and `systemd-timesyncd` active; names resolve through the stub resolver and `apt-get update` reaches ports.ubuntu.com (36 MB of lists in 4 s) |
| Services | `ssh.socket` active (sshd on demand); the `mary` user logs in with the password from `distro.conf` and has sudo |
| Shares | `out/` is mounted at `/mnt/maryos-out` over virtiofs; the guest can read the images it came from |
| Shutdown | `maryos vm stop`, Ctrl-C and closing the window ask the guest to power off; systemd does, the runner exits 0 |
| Pi 5 image | MBR with a bootable FAT32 `MARYOS` partition holding `config.txt`, `cmdline.txt`, `usercfg.txt`, the gzip `vmlinuz` (6.8.0-1064-raspi), `initrd.img`, `bcm2712-rpi-5-b.dtb` and the other BCM2711/2712 device trees, 324 overlays, the Pi 4 firmware blobs and `MARYOS.txt`; an ext4 `maryos-root` partition with the raspi kernel installed and `flash-kernel` ready |
| Flasher | `maryos list`, `maryos flash --disk diskN` and the app's Prepare flow: the same verified-target, one-admin-prompt `dd` as the ravynOS kit |
| App | `MaryOS.app` builds, is signed with the entitlement, and bundles the kit so it runs outside a checkout |
| Desktop | `./ui.sh` boots the same image to the Liquid Platinum desktop in about a second after `graphical.target`: the molten wallpaper, the ambient clock (View › Show Clock hides it), Finder and Gallery, lit metal object icons, 42px title bars with glass-bead traffic lights, window drag with the jelly and the liquid corners, eight-handle resize, shade, zoom with the flight, close, `foot` as a decorated Wayland client with keyboard focus; an idle desktop handles no frames (chapter 9) |
| Spotlight and TextEdit | `Ctrl+Space` opens the search bar that is also the dock, with the frontmost app's File / Edit / View / Window / Help as pills under it (there is no menu bar); typing filters apps and open windows, `Enter` launches. TextEdit, the first application, is reached from it alone and saves to `~/Documents/<name>.txt`; with it the compositor gained key repeat for its own controls and an I-beam cursor |
| System apps | Calculator (Spotlight only) is the first of the Linux-only built-ins: a keypad with precedence, percent, memory, repeated equals and the keyboard; the desktop gained event sources and routing by file kind for the apps to come, and the builder and image carry their libraries (chapter 9) |
| Dev loop | `make ui` rebuilds and tests the desktop in about 20 s incrementally (`MARYUI_DIR=…` to build a sibling checkout instead of the submodule); `./ui.sh` compiles on every run and boots that build, and a VM it booted restarts the desktop within three seconds of each rebuild |

## What is still plain Ubuntu

Every binary. MaryOS today is Ubuntu Noble's packages, chosen and configured
by `distro/`: no rebuilt package, no MaryOS apt repository, no MaryOS
kernel. That is the honest description of the first iteration and it is a
fine base; it also means `apt upgrade` on a MaryOS device gets Ubuntu's
updates unchanged.

## Known issues

- **Not yet booted on a Raspberry Pi 5.** The image follows Ubuntu 24.04's
  own boot layout for the Pi 5 exactly, but the board has not seen it. The
  first hardware boot should be watched on the UART.
- **Passwords in the repository.** `DEFAULT_PASSWORD` is in `distro.conf`
  and the built images; change it before publishing an image.
- **No Wi-Fi configuration in the image.** The firmware and `wpasupplicant`
  are there; the netplan file only covers wired interfaces.
- **The window has no clipboard or resize integration.** Virtualization's
  virtio-gpu gives a fixed 1280×800 scanout and no 3D; the desktop runs on the
  pixman software renderer there, and nothing like `spice-vdagent` exists for
  this framework. The molten wallpaper still renders — through a surfaceless
  EGL context that mesa serves with llvmpipe — but at about 7.5 s a frame, so
  it is baked at build time and never animated in the VM.
- **Ubuntu tools that check `ID=ubuntu`.** A few (e.g. some third-party
  install scripts) refuse `ID=maryos` even with `ID_LIKE`. Nothing in the
  base list does.

## Next milestones

1. **Boot the Pi 5 image on hardware**, watch the UART, confirm the root
   grows to the card and ssh works over Ethernet; then Wi-Fi.
2. **The desktop on the Pi**: boot `./ui.sh`'s counterpart on hardware —
   `cmdline.txt` already selects `graphical.target` — and watch the GLES2
   renderer on vc4/v3d. That is also the only way to exercise the molten
   wallpaper's animated flow, which is written but gated off on software
   renderers and has therefore never run. Then the parity follow-ups in
   MaryUI's `PARITY.md` (raster wallpaper, menu blur, the jelly on clients,
   caching the merge filter's silhouette between hover changes, promoting the
   last code constants to `tokens.json`).
3. **A MaryOS apt repository**: `reprepro` or `aptly` in the builder, signed
   with a MaryOS key, one more `Types: deb` stanza in the sources, so
   MaryOS packages (starting with a `maryos-base-files` that owns the
   branding instead of a diversion) install and upgrade like any other.
4. **A kernel built from source**: Ubuntu's `linux-raspi` and `linux`
   trees, cross-compiled in the builder, packaged as `.deb`s in that
   repository; the first real divergence from Ubuntu's binaries.
5. **Reproducibility**: a lockfile of package versions per build, so an
   image can be rebuilt bit for bit later.
6. **CI**: a job that builds the VM image and boots it headless to the
   login prompt, the way this journey was verified.
