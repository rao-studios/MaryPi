# 8. Status and next steps

State at the end of this journey (September 7, 2026), verified on the QEMU
`virt` test bed with the `qemu-virt` profile (GICv2, software emulation):

## What works

| Stage | Result |
|---|---|
| Firmware and booter | EDK2 starts `bootaa64.efi`; the booter loads the kernel and 21 kext bundles, builds the device tree, drops to EL1 and enters XNU |
| Kernel | Bootstraps with the PL011 console mirrored on the screen, GICv2 (and GICv3 on TCG), generic timer, 16 KiB pages, 2 GB RAM |
| Drivers | `corecrypto`, `pthread`, `RavynARMPlatform` (platform expert, GIC, CPU count), `IOStorageFamily`, `RavynVirtIOBlock`, `hfs` + `hfs_encodings`, `AppleFileSystemDriver` are linked at boot by kxld and started |
| Storage | The virtio disk appears as `disk0`; the MBR partition scheme publishes `disk0s1` and `disk0s2`; reads and writes complete through the interrupt path |
| Root | `rd=disk0s2` → `BSD root: disk0s2`, HFS+ mounted, devfs mounted, `bsd_init` completes |
| Userland | `/sbin/launchd` (ravyninit) is exec'd as process 1, prints on `/dev/console` and serves a shell: `help`, `uname`, `mem`, `ls`, `cat`, `write`, `mkdir`, `sync`, `reboot` |

MaryPi reports all of this as **Level 1 with drivers and a minimal root**;
`marypi payload` lists the kexts and the init program it found.

## What is a stand-in

- **ravyninit instead of launchd.** No libSystem, no dyld, no real userland
  for arm64. The shell exists to prove the kernel/user boundary works
  (system calls, exec, devfs, the console tty, filesystem writes).
- **Kexts linked at boot instead of a kernelcache.** Correct and fast enough
  for a handful of kexts; a kernelcache tool for arm64 remains desirable.
- **`serial=3` console.** The screen mirrors the console; there is no
  keyboard driver, so input only comes from the serial line.
- **QEMU disk, not the Pi's SD card.** The Raspberry Pi 5 needs an SDHCI
  driver (`brcm,bcm2712-sdhci`) before the same image reaches root on real
  hardware; everything above the block device is already in place.

## Known issues

- **HVF/GICv3 timer.** With `-accel hvf` on Apple silicon the guest never
  receives the timer interrupt (QEMU uses Apple's in-kernel vGIC). The
  `qemu-virt-gicv3` profile boots but stalls; use `qemu-virt` (TCG).
- **Kexts above 4 GiB.** XNU's booter-kext descriptors carry 32-bit
  addresses. The booter refuses to place kexts above 4 GiB, which will
  matter on an 8 GB Pi 5 until it prefers the low RAM run.
- **Development-kernel features turned off.** dtrace and the
  interrupt-masked watchdog are disabled in the BCM2712 configuration
  (chapter 9 explains both). Re-enabling dtrace needs the kext loader
  ordering fixed.
- **Kernel debug chatter.** The kernel prints one line per kext link and the
  first eight virtio requests on the serial line; useful now, to be quieted
  later.
- **Linux hosts untested.** The VM tooling is written to run there (QEMU +
  `vm/run.sh`, Swift CLI builds without macOS-only parts) but has only been
  exercised on macOS.

## Next milestones

1. **Pi 5 hardware:** SDHCI block driver for the SoC's SD controller (an
   `IOBlockStorageDevice` like the virtio one, matching the `sdhci` node the
   booter must add from the FDT), then the same image boots to ravyninit on
   the board over the debug UART. Also: RP1 peripherals, USB (xHCI +
   IOUSBFamily), HDMI is already handled by the UEFI framebuffer.
2. **Real userland:** cross-build dyld, libSystem and its members, launchd
   and the base BSD tools for arm64; then MaryPi's Level 2 becomes real and
   ravyninit retires.
3. **SMP:** describe all four cores (PSCI `CPU_ON` from the platform expert,
   `RavynARMCPU` as an `IOCPU`), then drop `cpus=1`.
4. **Kernelcache for arm64:** either port `plktool` to macOS/arm64 or write
   a small kxld-based tool, so a card carries one prelinked file.
5. **Comfort:** a HID path for the QEMU window's keyboard, a Pi 5 device tree
   check against real firmware, and CI that boots the VM image and expects
   the `ravynOS#` prompt.
