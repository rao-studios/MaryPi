# 1. The goal and the honest levels

## What we are building

ravynOS is a Darwin system: the XNU kernel (Mach + BSD + IOKit), Apple-style
kernel extensions ("kexts"), and a userland that starts with `launchd`. It was
an x86_64 system booted under QEMU. The Raspberry Pi 5 is an arm64 board with
a Broadcom BCM2712 SoC, four Cortex-A76 cores, an ARM GIC-400 interrupt
controller, PL011 UARTs and the ARM generic timer. None of that looks like an
Apple SoC, which is the only arm64 hardware XNU ever ran on.

MaryPi is the Mac app and CLI that prepares the SD card. Its job is to be
seamless for the person holding the card, and honest about what the card will
do, because for a long time the answer is "less than a full operating system".

## The three payload levels

MaryPi scans the ravynOS build tree every time and reports a level:

| Level | Name | On the card | What the Pi does |
|---|---|---|---|
| 0 | Bootstrap only | Raspberry Pi firmware files, `config.txt`, the rpi5-uefi UEFI firmware | Shows the UEFI setup screen. Proves the board, EEPROM and serial line work. |
| 1 | Kernel bring-up | Level 0 + the ravynOS booter (`EFI/BOOT/BOOTAA64.EFI`), the arm64 kernel, the boot plist, and (when built) the kexts under `ravynos/Extensions` | UEFI starts the booter, the booter starts XNU. With kexts, XNU finds its platform driver, interrupt controller and disk, mounts the card's second partition and starts process 1. |
| 2 | Full system | Level 1 + a prelinked kernelcache and a complete arm64 root filesystem | Starts the real `launchd`. Not reachable yet: there is no arm64 userland. |

Level 1 is where the work of this journey happened. Within it there are
sub-stages, and `marypi payload` spells out which one applies:

1. Kernel only: XNU boots, prints on the serial line and on the screen, then
   panics "Unable to find driver for this platform" because IOKit has no
   platform driver. This is the expected end of a kernel-only card.
2. Kernel + kexts: the booter also loads the drivers; XNU links them at boot
   and can reach the disk.
3. Kernel + kexts + minimal root: the second partition holds a tiny root
   filesystem with `ravyninit` standing in for `launchd`, so the kernel gets
   all the way to running a userland process.

## Two targets, one image

The same image boots on the Pi 5 and on QEMU's `virt` machine. That is not a
coincidence: both are "UEFI firmware + a flattened device tree + a GIC + a
PL011", so the booter reads the hardware description from the firmware and
the kernel does not care which board it is on. The differences (a virtio disk
on QEMU, an SD controller on the Pi) live in individual drivers. Chapter 7
covers the VM side.
