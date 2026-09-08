# 9. Troubleshooting

Everything below happened at least once during this bring-up. The serial
log (`vm/state/<profile>/serial.log` in the VM, the 3-pin UART on the Pi)
is the source of truth; the screen shows only the console part of it.

## The window stopped updating

Not a hang, almost always. After a panic the kernel prints `Waiting for
remote debugger connection.` and waits for KDP on the serial line. Read
the panic line above it. `marypi vm stop` ends the session.

## `Unable to find driver for this platform: "qemu-virt"`

No platform expert matched the root nub. Either no kexts were on the card
(`marypi payload` shows `Extensions: -`), or `RavynARMPlatform` failed to
load. Look for `Kext com.ravynos.driver.RavynARMPlatform ... failed` lines
above the panic; `kextlog=0xfff` in the kernel flags makes OSKext explain
every step.

## `Nested panic detected: "VCPUTC_LOCK_LOCK"`

A panic happened while the video console held its lock, and the console
mirror then tried to draw the panic text. The original cause was
re-acquiring the screen with `PE_state.video` after the VM system was up:
its base address is physical and the kernel treated it as virtual. Acquire
with a NULL video descriptor. The mirror now resets the lock in the panic
path, so this particular deadlock cannot recur even if a fault happens
inside the video console again.

## `vm_map_delete(...): attempt to remove permanent VM map entry`

`kext_alloc_init` tried to carve the kext basement out of the permanent
reservation arm64 `kmem_init` makes for the static kernel range.
`pmap_virtual_region()` must leave the basement unreserved (chapter 5).

## `vm_map_delete(...): no map entry at ...`

The complementary mistake: creating the basement sub-map with
`VM_FLAGS_OVERWRITE` when the range is already free. On arm64 the sub-map
is created with plain `VM_FLAGS_FIXED`.

## `Unexpected fault in kernel static region`

A page fault at an address between `virtBase` and the end of the static
mappings. The exception handler assumes nothing there can legitimately
fault. When the faulting address is inside the kext basement, the basement
pages were not wired at allocation, or the handler's exemption for the
basement is missing.

## `Kext ... memory region containing module start function is not executable`

`validateKextMapping` found the kext's start function in a region without
execute permission. Causes seen: kxld assigning read-write to `__TEXT_EXEC`
(it only knew `__TEXT`), and segments not aligned to the kernel's 16 KiB
pages so per-segment protection skipped them.

## `Kernel instruction fetch abort at pc <inside a kext>`

The VM map says executable, the page tables say no. The arm pmap
deliberately refuses to clear the privileged-execute-never bit for the
kernel pmap in `pmap_protect`; on the bring-up kernel that path is allowed
(`CONFIG_KEXT_BASEMENT`), because `vm_map_protect` already guarantees a page
is never writable and executable at the same time.

## Kext has undefined `_RESERVED...` symbols / crashes in a virtual call

The kext was compiled with padded vtables and the kernel without (or the
other way round). Build kexts for the BCM2712 kernel with
`-DAPPLE_KEXT_VTABLE_PADDING=0`; `bmake ... kexts` does this.

## `machine_routines.h: unknown type name 'IOInterruptHandler'`

While compiling a kext: the userland copy of `pexpert/pexpert.h` under
`System.framework/PrivateHeaders` was found before the kernel's. The kext
build puts `Kernel/xnu/pexpert` and `Kernel/xnu/iokit` first on the include
path.

## `conflicting deployment targets` from clang

`MACOSX_DEPLOYMENT_TARGET` is exported by the ravynOS build and clang
refuses to combine it with a target triple that carries its own version.
Pass `-mmacos-version-min` explicitly (the kext and ravyninit makefiles do).

## `kernel extensions must sit below 4 GiB`

The booter placed the kext area above 4 GiB, which XNU's 32-bit
`_BooterKextFileInfo` cannot address. Seen only on boards whose largest
RAM run is high (8 GB Pi 5); the booter must prefer a low run there.

## HVF: kernel boots but never takes a timer interrupt

QEMU on Apple silicon with `-accel hvf` uses Apple's in-kernel GICv3
emulation, and the guest timer interrupt is not delivered to our kernel.
Unresolved; the `qemu-virt` profile uses software emulation (TCG), which
is slower but correct.

## Boot goes quiet right after IOKit starts (no `bsd_init:` lines)

`vm_commpage_init()` sleeps in `ml_get_max_cpus()` until a platform driver
calls `ml_init_max_cpus()`. `RavynARMPE` creates an `IOCPUInterruptController`
for that; if it fails to load, the bootstrap thread never reaches `bsd_init`.

## Boot goes quiet after `bsd_init: calling os_reason_init`

A lock-order deadlock between dtrace and the kext loader. `bsd_init` calls
`dtrace_postinit()`, which registers every loaded kext with dtrace under the
kext lock; meanwhile a kext still being linked holds that lock and waits on
dtrace's module lock. Prelinked systems never see this because all kexts
are loaded before `bsd_init`; boot-linked kexts race it. The bring-up
kernel is built without `config_dtrace`.

## `Kext ... link failed` with nothing else said

Run with `kextlog=0xfff` in the kernel flags; kxld then names the missing
symbols. Symbols exist in the kernel but are not in any `config/*.exports`
list (`Private.arm64.exports` for the bring-up additions), or a library kext
was not declared in the dependent's `OSBundleLibraries`. A quick static
check: undefined symbols of the kext (`llvm-nm -u`) against the symbol sets
in `System.kext/PlugIns/*/`; indirect symbols show as `I name (indirect ...)`.

## `pthread kernel extension not loaded (function table is NULL)`

`bsd_init` needs `pthread.kext` (`com.apple.kec.pthread`), which the kernel
loads at startup as a "kernel external component". It must be among the
kexts on the card.

## `Interrupts held disabled for N nanoseconds`

DEVELOPMENT arm64 kernels carry a watchdog (`interrupt_masked_debug`) that
panics when interrupts stay masked for more than a few tens of microseconds.
Under QEMU's software emulation ordinary paths take that long. The bring-up
kernel is built without the option.

## `ml_static_vtop: illegal VA: <basement address>`

Something asked for the physical address of a boot-linked kext page as if it
were static kernel memory (`ml_static_unslide` on a kext address, for
example). Kexts live below `virtBase` since the layout change of chapter 5;
`ml_static_unslide` passes such addresses through unchanged.

## `C_SLOT_UNPACK_PTR failed on zone_map_min_address`

The VM compressor packs kernel heap pointers relative to the low-globals
window (`0xfffffff000000000`). If the booter's `virtBase` coincides with
that window the kernel never reserves the addresses below it, the heap
starts far lower, and the packing self-check fails. The booter now uses the
2 MiB block holding the kernel text as `virtBase`, exactly like iBoot.

## `load_init_program: attempting to load /sbin/launchd` and then silence

The CPU spins in `copyinstr` taking the same fault over and over. The
Cortex-A76 has PAN (Privileged Access Never): the kernel enters exception
handlers with PAN set and must clear it around `copyin`/`copyout`, which XNU
only does when the board declares `__ARM_PAN_AVAILABLE__`. The BCM2712
board header declares it now; Apple's A10-and-later boards do the same.
