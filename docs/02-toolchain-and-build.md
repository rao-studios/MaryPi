# 2. Building for arm64 on a Mac

## Host and target

The ravynOS tree is driven by BSD make (`bmake`) from the checkout root and
writes everything into the sibling directory `../build`. Two architectures
are involved:

- the **host** is the Mac doing the building (an Apple silicon Mac here);
- the **target** is what the output runs on: `x86_64` by default, `arm64`
  for the Raspberry Pi 5.

The root `Makefile` splits the two: `TARGET_ARCH=arm64` selects
`MachOArch=arm64`, `CpuArch=aarch64`, the kernel configuration `ARM64` with
machine configuration `BCM2712`, and a separate staging root
`../build/sysroot-arm64` so an arm64 build never disturbs the x86_64 system.
The toolchain (clang, ld64, cctools) is built once for the host with both
X86 and AArch64 backends and lives under
`../build/Developer/Platforms/ravynOS.platform/Developer/Toolchains`.

Everything must be run from the checkout root so the exported variables
match:

```sh
bmake TARGET_ARCH=arm64 KERNEL_CONFIGS=DEVELOPMENT GMAKE_JOBS=10 kernel   # XNU
bmake TARGET_ARCH=arm64 KERNEL_CONFIGS=DEVELOPMENT booter                 # bootaa64.efi
bmake TARGET_ARCH=arm64 KERNEL_CONFIGS=DEVELOPMENT kexts                  # drivers
bmake TARGET_ARCH=arm64 KERNEL_CONFIGS=DEVELOPMENT ravyninit              # process 1 stand-in
```

Running `bmake` inside a subdirectory does not work: the root Makefile is
what exports `SYSROOT_DIR`, `MachOArch`, `MACHINE_CONFIGS` and friends.

## What each target produces

| Target | Output | Notes |
|---|---|---|
| `kernel` | `../build/sysroot-arm64/System/Library/Kernels/kernel` (a copy of `kernel.development.bcm2712`) and `System/Library/Extensions/System.kext` | ~4–5 minutes. XNU's own GNU-make build runs underneath; `all` and `install` are run as two separate invocations because their timestamps race. |
| `booter` | `../build/booter/bootaa64.efi` | A freestanding PE/COFF EFI application, built with `--target=aarch64-unknown-windows` and `lld-link`, so no EFI SDK is needed. Seconds. |
| `kexts` | `../build/sysroot-arm64/System/Library/Extensions/*.kext` | Also builds `libkmod.a` for arm64 into the SDK. About a minute; `hfs` is the big one. |
| `ravyninit` | `../build/sysroot-arm64/sbin/ravyninit` | A static arm64 Mach-O with no libSystem. Seconds. |

MaryPi reads these paths (see `Sources/MaryPiKit/Payload/BuildTree.swift`)
and never builds anything itself.

## Things the build had to learn

The x86_64 build assumed host equals target. Making arm64 work meant:

- the kernel's runtime libraries (`libcc_kext.a`, `libfirehose_kernel.a`)
  are cross-built per architecture into `usr/local/lib/kernel/arm64`;
- kexts are compiled with an explicit `-target arm64-apple-darwin19` and
  linked against the arm64 `libkmod.a` (`BSD/share/mk/rvn.kext.mk`);
- kexts must search the kernel's own `pexpert` and `iokit` headers before
  the userland copies under `System.framework/PrivateHeaders`, or
  `machine_routines.h` fails to compile;
- the kernel is configured with `CONFIG_EMBEDDED`, which removes the
  reserved vtable slots from libkern classes, so kexts are built with
  `-DAPPLE_KEXT_VTABLE_PADDING=0` to get identical class layouts (chapter 5
  explains why this matters);
- `plktool`, the tool that prelinks kexts into a kernelcache, only builds on
  Linux and only for x86_64. Instead of porting it, the arm64 boot loads
  kexts separately (chapter 5).
