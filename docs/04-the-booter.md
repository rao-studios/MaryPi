# 4. The booter

On an iPhone or a Mac, iBoot loads XNU. It hands the kernel a physical
memory layout, a flattened Apple device tree, a `boot_args` structure and a
framebuffer. The Raspberry Pi 5 boots UEFI instead, so ravynOS has its own
UEFI application that does iBoot's job: `Kernel/booter/` builds
`bootaa64.efi`, which MaryPi puts at `EFI/BOOT/BOOTAA64.EFI`.

## What is on the boot partition

```
EFI/BOOT/BOOTAA64.EFI             the booter
ravynos/com.ravynos.boot.plist    <key>Kernel</key>, <key>Kernel Flags</key>
ravynos/kernel                    the arm64 XNU Mach-O
ravynos/Extensions/*.kext         drivers (chapter 5), including System.kext/PlugIns
config.txt, RPI_EFI.fd, ...       Raspberry Pi firmware + rpi5-uefi (level 0)
```

The plist is tiny on purpose: the booter's XML parser understands exactly
`<key>`/`<string>` pairs. The default flags are `-v serial=3 debug=0x8
cpus=1`; MaryPi adds `rd=disk0s2` when the card carries a storage stack.

## The steps

1. **Read the configuration and the kernel.** The kernel is a plain Mach-O.
   The booter inspects its segments to learn the virtual range it was
   linked for (`0xfffffff007004000` and up) and its entry point.
2. **Read the kexts.** Every `*.kext` under `ravynos/Extensions` and under
   `ravynos/Extensions/System.kext/PlugIns` is read into memory: the
   `Info.plist` and, if the plist names one, the executable. Both bundle
   layouts are accepted (`Contents/MacOS/Name` and the flat layout XNU's
   own build uses for the KPI plug-ins).
3. **Discover the hardware** from the firmware's FDT: the console PL011
   (following `stdout-path` when set), the GIC (v2 or v3, with its register
   windows), the CPUs, and on QEMU every `virtio,mmio` transport with its
   interrupt. On a Pi 5 whose firmware publishes no FDT, the booter reads
   `bcm2712-rpi-5-b.dtb` from the card.
4. **Choose RAM.** The kernel owns one contiguous run of RAM starting at a
   2 MiB aligned `physBase`. The kernel is loaded unslid at
   `physBase + (vmaddr - 0xfffffff000000000)`, so the booter walks up in 2 MiB
   steps until a region big enough for kernel + kexts + device tree +
   boot_args + bootstrap page tables is free of firmware allocations.
5. **Build the Apple device tree.** A fresh tree, not a translation of the
   whole FDT: `/chosen` (boot-args, random-seed, `memory-map`), `/cpus`,
   and `/arm-io` with `pl011`, `interrupt-controller` and one `virtio` node
   per transport. `#address-cells`/`#size-cells` are 2 everywhere, the GIC
   node carries `AAPL,phandle` and `#interrupt-cells = 1`, and every device
   with an interrupt carries `interrupt-parent` and its GIC interrupt ID.
   Those properties are what IOKit's device-tree support needs to resolve
   `reg` through `/arm-io`'s `ranges` and to map interrupts to a controller.
6. **Place everything.** Kernel, then the kext bundles (each in its own
   16 KiB-aligned block holding a small header, the bundle path, the plist
   and the executable), then the device tree and `boot_args`.
   `topOfKernelData` covers all of it, so the kernel never hands those
   pages to its page allocator. Each kext gets a `Driver-<name>` entry in
   `/chosen/memory-map` whose value is a 32-bit `{paddr, length}` pair,
   which is the format XNU's `readBooterExtensions()` expects.
7. **Fill `boot_args`**: physical and virtual bases, memory size, the
   command line, the framebuffer from the UEFI Graphics Output Protocol
   (text mode when `-v` is present).
8. **Leave UEFI.** `ExitBootServices`, clean the data cache, drop from EL2
   to EL1 if the firmware left us there (setting `CNTHCTL_EL2` and
   `ICC_SRE_EL2` so the kernel can use the timer and the GIC system
   registers), and jump to the kernel's entry point with `x0 = boot_args`.

## Limits worth knowing

- XNU keeps 32-bit physical addresses for booter-loaded kexts, so the kext
  area must lie below 4 GiB; the booter refuses otherwise. On an 8 GB Pi 5
  the largest RAM run is above 4 GiB, so the booter will need to prefer the
  low run once that board is exercised.
- Property names in the Apple device tree are 31 characters; `Driver-`
  names are truncated to fit.
- The booter prints everything it does on the UEFI console (serial and
  screen). When a boot goes wrong before the kernel says anything, that
  transcript is the first thing to read.
