# 4. Booting on the Raspberry Pi 5

## The chain

1. The Pi 5's bootloader lives in EEPROM. It finds the first FAT partition
   on the card and reads `config.txt`.
2. `config.txt` says `kernel=vmlinuz`, `initramfs initrd.img followkernel`
   and `cmdline=cmdline.txt`. The firmware loads the kernel (it understands
   gzip-compressed arm64 kernels), places the initrd after it, patches the
   device tree (`bcm2712-rpi-5-b.dtb`, plus the overlays the file asks for)
   and jumps into the kernel. There is no U-Boot and no UEFI, unlike the
   ravynOS kit's card.
3. `cmdline.txt` puts the console on the debug UART and HDMI and names the
   root by label: `root=LABEL=maryos-root rootfstype=ext4 rootwait`.
4. The initramfs (Ubuntu's `initramfs-tools`, `MODULES=most`) finds the
   partition by label, mounts it and hands over to systemd.
5. systemd mounts `/boot/firmware` from `fstab` (by label), runs
   `maryos-firstboot` once (chapter 6), and starts getty on the console and
   ssh.

## The kernel

`linux-raspi` is Ubuntu's flavour of the Raspberry Pi kernel: BCM2712
support, the RP1 peripherals, the Raspberry Pi device trees under
`/lib/firmware/<version>/device-tree` (or `/usr/lib/linux-image-<version>`,
the builder looks in both). The builder copies the kernel, initrd, device
trees and overlays to `/boot/firmware` itself, so the first boot needs no
help. After that, `flash-kernel` does the same on every kernel upgrade on
the device: it is installed and quiet during the build (`FLASH_KERNEL_SKIP`).

`linux-firmware` and `linux-firmware-raspi` bring the Wi-Fi and Bluetooth
firmware and the `start*.elf`/`fixup*.dat` blobs that Pi 4 boards need; the
Pi 5 has their equivalent in its EEPROM and ignores them.

## config.txt

Based on Ubuntu's own for Noble: `arm_64bit=1`, `enable_uart=1` for the
3-pin debug header, audio, I2C and SPI on the GPIO header, camera and
display auto-detection, `vc4-kms-v3d` for KMS graphics with
`disable_fw_kms_setup=1`. Local additions go in `usercfg.txt`, which
`config.txt` includes last.

## Serial console

`console=serial0,115200` maps to the debug UART on the Pi 5 (the 3-pin
header, `ttyAMA10`). `console=tty1` last makes HDMI `/dev/console` as well,
so both show the boot and both get a login prompt.

## The EEPROM

Ubuntu 24.04 needs a bootloader EEPROM from 2023 or later. If nothing
appears on HDMI or the UART, update it with Raspberry Pi Imager first; the
kit never touches the EEPROM. `rpi-eeprom` is installed on the card for
later updates from within MaryOS.
