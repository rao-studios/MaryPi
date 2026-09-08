# The MaryPi journey: from a blank SD card to ravynOS on a Raspberry Pi 5

These pages explain, in reading order, how a Mac turns an SD card into
something a Raspberry Pi 5 boots, and how far ravynOS (an XNU/Darwin system)
gets on that card today. They were written while the work was done, so they
say what exists, what is a stand-in, and what is still missing.

Read them top to bottom the first time; each chapter assumes the previous one.

| # | Chapter | What you learn |
|---|---|---|
| 1 | [The goal and the honest levels](01-the-goal.md) | What "boot ravynOS on a Pi 5" means, and the three payload levels MaryPi reports |
| 2 | [Building for arm64 on a Mac](02-toolchain-and-build.md) | The ravynOS build system, host versus target, the four build targets, where outputs land |
| 3 | [Porting the kernel](03-arm64-kernel-port.md) | What the XNU kernel needed to run on a GIC + PL011 board instead of Apple silicon |
| 4 | [The booter](04-the-booter.md) | The UEFI application that stands in for iBoot: device tree, boot arguments, memory layout, loading kexts |
| 5 | [Drivers at boot](05-kexts-at-boot.md) | Why the kernel links its drivers itself, the platform expert, the interrupt controller, virtio storage, the root filesystem |
| 6 | [First steps in userland](06-userland-first-steps.md) | ravyninit: a process 1 without libSystem, and what the real userland needs |
| 7 | [The VM test bed](07-vm-test-bed.md) | Booting the same image under QEMU, reading the window and the serial log, driving the console |
| 8 | [Status and next steps](08-status-and-next-steps.md) | What works, what is a stand-in, known issues, the milestones ahead |
| 9 | [Troubleshooting](09-troubleshooting.md) | The panics and hangs met along the way and what each one means |
| – | [Glossary](glossary.md) | Terms used throughout |

Conventions: paths like `Kernel/xnu/...` are inside the ravynOS checkout;
paths like `Sources/MaryPiKit/...` are inside this repository. Commands are
run from the ravynOS checkout root unless stated otherwise.
