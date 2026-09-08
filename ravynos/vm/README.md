# MaryPi VM test bed

Boots a MaryPi image under QEMU's `virt` machine with EDK2 UEFI firmware, on
arm64 macOS (Homebrew QEMU, hvf) or arm64 Linux (distro QEMU, kvm), in a
window or on the terminal. It is the ravynOS arm64 bring-up test bed: QEMU
has no Raspberry Pi 5 model, but `virt` offers the same building blocks the
Pi 5 kernel uses (GICv2 or GICv3, PL011 UART, ARM generic timer, UEFI).

```sh
# macOS: build an image and boot it in a window
swift run marypi vm run

# any host with a POSIX shell and QEMU: boot an existing image
vm/run.sh run --image /path/to/ravynOS-qemu-virt-L1.img
vm/run.sh run --image q.img --display serial      # serial console on this terminal (Ctrl-A X quits)
vm/run.sh status
vm/run.sh serial                                  # follow the serial log
vm/run.sh stop
vm/run.sh doctor                                  # what would be used and why
```

## Profiles

| Profile | Interrupt controller | Accelerator | When |
|---|---|---|---|
| `qemu-virt` (default) | GICv2, like the Pi 5's GIC-400 | tcg on macOS; kvm only on Linux hosts with a GICv2-compatible GIC (Raspberry Pi 5 as host qualifies), else tcg | Pi 5 parity |
| `qemu-virt-gicv3` | GICv3 | hvf on Apple Silicon, kvm on GICv3 Linux hosts | fast iteration |

QEMU cannot use hvf with a GICv2 guest ("HVF does not support GICv2
emulation"), so `--accel hvf` is refused for `qemu-virt`.

Profiles are `KEY=VALUE` files in `profiles/`, sourced by `run.sh` and parsed
by MaryPiKit. `VM_DEVICES` adds `ramfb` (a UEFI framebuffer, so the kernel's
boot console draws in the window), an xHCI controller with a keyboard and
tablet, and the disk on `virtio-blk-device`. `VM_EXTRA_ARGS` and anything
after `--` on the command line go to QEMU verbatim.

## What the window shows

The kernel flags MaryPi writes (`-v serial=3 debug=0x8 cpus=1`, plus
`rd=disk0s2` when the tree has a storage stack) send the console to the
serial port, so `serial.log` is the primary record. On the bring-up boards
the kernel also mirrors that console onto the framebuffer, so the QEMU
window shows the same text as the serial log (boot-arg `vcmirror=0` turns
the mirror off). The window is EDK2's while the firmware runs (Esc or F2
during the countdown opens its menus), then the booter's, then the kernel's.

What the boot ends with depends on what the ravynOS tree provides:

- kernel only: `panic(...): "Unable to find driver for this platform"` followed
  by `Waiting for remote debugger connection.` The kernel has bootstrapped,
  taken timer interrupts and started IOKit; there is no platform driver.
  It is not hung: KDP is waiting on the serial port.
- kernel + kexts + minimal root (`bmake TARGET_ARCH=arm64 kexts ravyninit`):
  the drivers link at boot, the HFS+ partition mounts as root and process 1
  prints

  ```
  ravynOS init: userland is alive on arm64 (process 1)
  This is ravyninit, a freestanding stand-in for launchd. Type 'help'.
  ravynOS#
  ```

  The prompt reads the serial port, so use `--display serial` (or a serial
  socket) to type at it; the window's keyboard is not wired up yet.

In both cases only `marypi vm stop`, `vm/run.sh stop` or closing the window
ends the session.

## State

Per profile, under `vm/state/<profile>/` in a checkout (gitignored), or under
`~/Library/Caches/MaryPi/vm/` (macOS app bundle) / `$XDG_CACHE_HOME/marypi/vm`
(Linux); `MARYPI_VM_STATE` overrides:

```
vars.fd      writable UEFI variable store (copied from the firmware's template)
code.fd      the firmware image padded to 64 MiB, only when the distro ships a 2 MiB one
disk.img     default image written by `marypi vm run` / `marypi build-image --qemu`
serial.log   everything the guest printed, every session appended with a header
qemu.out     QEMU's own stdout/stderr when detached or launched from the app
qemu.pid     written by QEMU (-pidfile), removed when it exits
qmp.sock     QMP control socket used by stop/status
```

## Firmware

Looked up in this order: `MARYPI_VM_EFI_CODE` (+ `MARYPI_VM_EFI_VARS`), the
`share/qemu` next to the QEMU binary or under `/opt/homebrew`, `/usr/local`,
`/usr/share` (`edk2-aarch64-code.fd` + `edk2-arm-vars.fd`),
`/usr/share/AAVMF/AAVMF_CODE.fd` (Debian, Ubuntu `qemu-efi-aarch64`, Fedora
`edk2-aarch64`), `/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw`, then the 2 MiB
`QEMU_EFI.fd` images, which get padded.

## Linux packages

Debian/Ubuntu: `apt install qemu-system-arm qemu-efi-aarch64`. Fedora:
`dnf install qemu-system-aarch64 edk2-aarch64`. Arch: `pacman -S qemu-system-aarch64 edk2-aarch64`.
For a window you need a GTK or SDL build of QEMU and a `DISPLAY`/`WAYLAND_DISPLAY`;
otherwise the launcher falls back to serial-only and tells you.
