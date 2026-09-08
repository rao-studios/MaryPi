# 5. Drivers at boot

The kernel-only card ends at `Unable to find driver for this platform`.
IOKit works by matching drivers (kexts) to devices (nubs); with no kexts
there is nothing to match, and the board's root nub is claimed by
`IOPanicPlatform`, whose only job is to panic with that message. This
chapter is about getting drivers into the kernel and what the first drivers
are.

## Why not a kernelcache

Apple arm64 kernels only boot from a kernelcache: the kernel and every kext
prelinked into one file by a userland tool. ravynOS has such a tool for
x86_64 (`plktool`) but it builds only on Linux and only knows x86_64, and
the arm64 kernelcache layout (split segments, `__PLK_TEXT_EXEC`, ...) is a
project of its own.

XNU has a second, older mechanism: before kernelcaches, `boot.efi` loaded
each kext into memory and described it in the device tree under
`/chosen/memory-map` as a `Driver-*` entry, and the kernel linked the kexts
itself with its built-in linker, **kxld**. That code is still in XNU
(`libsa/bootstrap.cpp: readBooterExtensions()`), it just is not compiled
into arm64 kernels. The booter (chapter 4) speaks that protocol, and the
kernel was taught to accept it on arm64.

## Making kxld work on arm64

Enabling `config_kxld` on the BCM2712 configuration was the easy part.
The rest is a list of assumptions Apple baked in because in-kernel linking
never happened on arm64:

1. **`#error CONFIG_KXLD not expected for this arch`** in
   `OSKext::removeKextBootstrap()`. The x86 code there re-maps the kernel's
   `__LINKEDIT` as pageable memory; on arm64 the segment simply stays
   resident, so that branch is skipped instead of forbidden.
2. **Branch range.** An arm64 `bl` reaches ±128 MiB. Kext memory allocated
   anywhere in the kernel map would be gigabytes from the kernel's text, and
   kxld correctly refuses to link such branches. x86_64 has the same
   problem (±2 GiB) and solves it with a "kext basement": a sub-map
   reserved right below the kernel's text. The arm64 port adds the same
   idea (`config_kext_basement`): 64 MiB directly below `__TEXT`, in the
   virtual hole the booter leaves between `virtBase` and the kernel image.
3. **The static-region reservation.** arm64 `kmem_init()` reserves that
   whole hole as a permanent map entry and the exception handler panics on
   any fault inside it ("Unexpected fault in kernel static region").
   `pmap_virtual_region()` now reserves around the basement instead of
   across it, `kext_alloc()` wires basement pages as soon as it allocates
   them, and the fault handler exempts the basement range.
4. **Kexts are VM-mapped now.** `OSKext.cpp` treated every arm64 kext as
   prelinked and cemented in physical memory. With a basement present it
   uses the x86 code paths (`VM_MAPPED_KEXTS`): protect segments with
   `vm_map_protect`, wire them, validate the mapping before calling the
   kext's start function.
5. **Segment protections.** kxld assigns protections by name: `__TEXT`
   gets read+execute and everything else read+write. arm64 kexts keep
   their code in `__TEXT_EXEC`, which kxld therefore made non-executable;
   the start-function check then failed for every kext.
   `kxld_seg_set_vm_protections()` now treats `__TEXT_EXEC` like `__TEXT`.
6. **Entitlement check.** `loadExecutable()` demands a code-signing
   entitlement from the calling task; IOKit matching runs in the kernel
   task, which is exempt now.
7. **Page size.** The kernel uses 16 KiB pages; kexts are linked with 16 KiB
   segment alignment so protections can be applied per segment.

## Identical class layouts

libkern's C++ classes reserve spare virtual-table slots
(`OSMetaClassDeclareReservedUnused`) on macOS but not on `CONFIG_EMBEDDED`
kernels. The BCM2712 kernel is `CONFIG_EMBEDDED`; kexts compiled with the
default headers would have padded vtables, and every virtual call between a
kext and the kernel would land in the wrong slot. Kexts for this kernel are
therefore built with `-DAPPLE_KEXT_VTABLE_PADDING=0`, and `OSMetaClass.h`
honors a pre-defined value. The easy way to check a kext: it must not have
undefined `_RESERVED...` symbols.

## What the booter hands over

Twenty-one bundles today: twelve KPI plug-ins from `System.kext`
(plist-only descriptions of `com.apple.kpi.*`, most with a symbol-set
Mach-O), and the drivers built by `bmake TARGET_ARCH=arm64 kexts`:

| Kext | Role |
|---|---|
| `RavynARMPlatform` | `RavynARMPE`, the platform expert (matches the root nub `qemu-virt` / `bcm2712`), and `RavynGIC`, the IOKit interrupt controller |
| `RavynVirtIOBlock` | virtio block device on the virtio-mmio transport (the QEMU disk) |
| `IOStorageFamily` | Apple's block-storage stack: IOMedia, partition schemes, the BSD disk nodes |
| `corecrypto` | the crypto functions the kernel registers at load |
| `pthread` | the pthread kernel component `bsd_init` insists on |
| `hfs` + `hfs_encodings` | the HFS+ filesystem of the root partition and its encodings plug-in (a library kext of its own) |
| `AppleFileSystemDriver` | root-by-UUID support (unused while `rd=` is given) |

Kexts declare `OSBundleLibraries` on the KPI versions; a kext whose
dependencies cannot be resolved is simply never loaded, and `kextlog=0xfff`
in the kernel flags prints why.

## The platform expert

`RavynARMPE` subclasses `IODTPlatformExpert`, IOKit's device-tree platform
expert. `IODTPlatformExpert::start()` publishes the top-level nodes of the
device tree as `IOPlatformDevice` nubs; `RavynARMPE::processTopLevel()` also
publishes the children of `/arm-io`, so `interrupt-controller`, `pl011` and
the `virtio` nodes become nubs directly under the platform expert. Two
device-tree details make the rest work without any board-specific code:

- `reg` values are resolved to physical addresses through the parent's
  `ranges` (`IODTResolveAddressing`), which is why the booter writes
  `#address-cells`/`#size-cells` and the `/arm-io` `ranges`.
- interrupts are resolved through `interrupt-parent` to a controller named
  by its `AAPL,phandle` (`IOInterruptController%08X`); every nub with an
  `interrupts` property gets `IOInterruptControllers` and
  `IOInterruptSpecifiers` at tree-build time. A node without
  `interrupt-parent` whose device-tree parent is not a controller makes
  `IODTMapOneInterrupt` loop forever, so the booter always writes it.

One more duty is easy to miss: the kernel's `vm_commpage_init()` calls
`ml_get_max_cpus()`, which sleeps until a platform driver has announced the
CPU count with `ml_init_max_cpus()`. On Apple boards that happens when the
platform expert creates an `IOCPUInterruptController`; `RavynARMPE` does the
same with the number of `cpu` nodes the booter wrote (one, for now). Without
it the bootstrap thread sleeps forever right after IOKit starts, with the
drivers apparently fine and nothing left to print.

`RavynGIC` subclasses `IOInterruptController` and registers itself under
that name. It does not touch the hardware: pexpert already programmed the
GIC and owns the top-level IRQ handler (the timer needs it before IOKit
exists). Enabling an IOKit vector registers a per-interrupt-ID callback with
pexpert (`pe_gic_register`), and the callback runs the `IOInterruptVector`
handler exactly like `AppleAPIC` does on x86. The kernel exports the
`pe_gic_*` functions through the `com.apple.kpi.private` symbol set for
this.

## Storage

`RavynVirtIOBlock` is an `IOBlockStorageDevice`. It probes every `virtio`
nub (32 transports on QEMU's `virt`), keeps the one whose device ID is 2
(block), negotiates features on either the legacy or the modern virtio-mmio
transport, sets up one split virtqueue in physically contiguous memory
below 4 GiB, and completes requests from an `IOInterruptEventSource` on its
transport interrupt. Requests carry the data buffer's physical segments
straight into descriptors; QEMU's transport is cache coherent and has no
IOMMU.

Above it sits Apple's `IOStorageFamily`: `IOBlockStorageDriver` wraps the
device, `IOMedia` represents the whole disk, `IOFDiskPartitionScheme` reads
the MBR MaryPi wrote and publishes `disk0s1` (FAT32, firmware) and
`disk0s2` (HFS+, root), and `IOMediaBSDClient` creates the `/dev` nodes.

## Mounting root

`bsd_init` asks IOKit for the root device. `rd=disk0s2` in the kernel flags
makes `IOFindBSDRoot` wait (up to 30 seconds) for an `IOMedia` with that
BSD name, then `vfs_mountroot` tries every registered filesystem; `hfs.kext`
registers HFS+ when it loads (it matches `IOResources`, so it loads as
soon as the BSD side publishes that resource). With root mounted, the
kernel opens `/dev/console` and execs process 1: chapter 6.

On the Raspberry Pi 5 itself the disk is an SD card behind the SoC's SDHCI
controller, for which no ravynOS driver exists yet; the storage chain
above it is identical. That driver is the next hardware milestone
(chapter 8).
