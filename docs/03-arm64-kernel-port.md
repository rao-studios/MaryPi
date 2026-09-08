# 3. Porting the kernel

XNU's arm64 code assumes an Apple SoC: Apple's interrupt controller (AIC),
Apple's UART, Apple's timer wiring, and a boot loader (iBoot) that hands over
an Apple-style device tree. The Raspberry Pi 5 and QEMU's `virt` machine have
a standard ARM GIC, a PL011 UART and UEFI. The port adds one "board" to XNU,
`BCM2712`, and teaches the platform expert (`pexpert`) about that hardware.

## The board configuration

`Kernel/xnu/config/MASTER.arm64.bcm2712` is the kernel configuration; the
board header is `Kernel/xnu/pexpert/pexpert/arm64/BCM2712.h` with the
selector in `pexpert/arm64/board_config.h`. The important knobs:

- `config_embedded`: the arm64 kernel family XNU ships for devices. This
  choice has consequences for kexts (chapter 5).
- `config_kxld` and `config_kext_basement`: added so the kernel can link
  kexts at boot (chapter 5).
- `PL011_UART`, `PE_GIC`, `ARM_ARCH_TIMER`: the hardware drivers below.
- No `config_enforce_signed_code`: unsigned binaries may run, which the
  bring-up userland relies on.
- No `config_dtrace` and no `interrupt_masked_debug`: the first deadlocks
  against kexts linked at boot, the second panics under QEMU's slow
  emulation (chapter 9 has both stories).
- `__ARM_PAN_AVAILABLE__`: the Cortex-A76 implements PAN, so the kernel
  must clear it around user-memory copies like it does on Apple SoCs.

## Serial console

`pexpert/arm/pe_serial.c` gained a PL011 driver. The booter names the UART
in the device tree (`/arm-io/pl011` with `reg`), pexpert maps it, and kernel
flags `serial=3` make the serial port the console. The kernel flags MaryPi
writes are `-v serial=3 debug=0x8 cpus=1` (plus `rd=disk0s2` once a storage
stack is on board).

## Interrupts: a generic GIC layer

`pexpert/arm/pe_gic.c` is a version-neutral layer with a handler table and
the top-level IRQ dispatcher; `pe_gicv2.c` and `pe_gicv3.c` provide the
register-level operations (`struct pe_gic_ops`). The booter writes
`gic-version` and the register windows into the `/arm-io/interrupt-controller`
node, so one kernel handles the Pi 5 (GIC-400, v2) and QEMU with either GIC.

Because the timer must tick long before IOKit exists, pexpert installs the
GIC dispatcher as the kernel's interrupt handler itself
(`ml_install_interrupt_handler` with a NULL nub). The generic timer's PPI
(30 for the physical timer, 27 for the virtual one) is routed to
`ml_arm_generic_timer_irq()`; everything else goes to whoever registered
for that interrupt ID, which is how the IOKit interrupt controller in
chapter 5 plugs in.

## Timer

XNU drives its decrementer from the EL1 physical timer (CNTP). Apple's
hypervisor only delivers the virtual timer, so `ml_timer_select_virtual()`
can switch to CNTV, either with the boot-arg `vtimer=1` or automatically
when the CPU identifies as Apple (that is what QEMU's `-accel hvf -cpu host`
looks like). On TCG both work; under HVF the interrupt still does not arrive
(chapter 8, known issues).

## Screen

The booter passes the UEFI framebuffer in `boot_args.Video`. With `-v` the
booter asks for text mode, so the kernel's video console draws the boot
log. Since the console itself is the serial port, `serial_console.c`
mirrors every character to the framebuffer on the BCM2712 configuration
(`vcmirror=0` turns that off). One early bug is worth remembering: once the
VM system is up, the video console must be acquired with a NULL video
descriptor, because `PE_state.video` holds a physical address and the
kernel would treat it as virtual (chapter 9).

## Where the kernel stops without drivers

With all of the above, XNU runs `kernel_bootstrap`, brings up the VM,
scheduler and IPC, starts IOKit, and panics
`Unable to find driver for this platform: "qemu-virt"`. That panic comes
from `IOPanicPlatform`, the IOKit class that matches when no platform expert
does. Getting past it is chapter 5.
