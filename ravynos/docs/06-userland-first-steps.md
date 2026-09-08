# 6. First steps in userland

Once root is mounted, `bsd_init` execs process 1. XNU's DEVELOPMENT kernels
try `/usr/local/sbin/launchd.debug`, then `.development`, then
`/sbin/launchd`, and panic `Process 1 exec of ... failed` if none works.

## Why not launchd

`launchd` is dynamically linked: it needs `dyld`, `libSystem` and the
libraries below it (libc, libpthread, libdispatch, libxpc, ...), all built
for arm64 against this kernel. ravynOS has that userland for x86_64 only.
Building it for arm64 is a project of its own (chapter 8), and the kernel
had no way to prove it could run *any* process until something stood in.

## ravyninit

`Kernel/ravyninit/` is a ~400-line C program with no libraries at all:

- `start.S` is the entry point: the kernel starts a static executable at
  the `LC_UNIXTHREAD` address with `sp` pointing at `argc`; the stub
  aligns the stack and calls `ravyninit_main`.
- system calls are made directly: number in `x16`, arguments in `x0..x5`,
  `svc #0x80`; the carry flag reports an error and `x0` then holds `errno`.
- it opens `/dev/console`, prints a banner, and runs a small shell:
  `help`, `uname` (sysctl `kern.*`), `mem` (`hw.memsize`, `hw.ncpu`), `pid`,
  `ls [dir]` (`getdirentries64`), `cat <file>`, `write <file> <text>`,
  `mkdir`, `sync`, `echo`, `reboot`, `halt`.

It is linked with `-static -e _start` into a plain Mach-O executable
(`__PAGEZERO`, `__TEXT`, `__DATA`, `__LINKEDIT`, an `LC_UNIXTHREAD`
with an `ARM_THREAD_STATE64`), which XNU loads without `dyld`. The
BCM2712 kernel is built without `config_enforce_signed_code`, so the
unsigned binary may run.

MaryPi installs it as `/sbin/launchd` on the root partition whenever the
card has a storage stack but no real launchd (`marypi payload` shows
`Init program:`), together with the directories a Darwin root expects
(`/dev` for devfs, `/etc`, `/tmp`, `/var`, ...). The root partition also
carries `README-ravynos-bringup.txt` saying exactly that.

## What it looks like

The first boot that reached it printed, on the serial line and the screen:

```
load_init_program: attempting to load /sbin/launchd

ravynOS init: userland is alive on arm64 (process 1)
This is ravyninit, a freestanding stand-in for launchd. Type 'help'.
ravynOS#
```

A session over the serial port, unedited apart from trimming `ls /dev`:

```
ravynOS# uname
kern.ostype: Darwin
kern.osrelease: 19.6.0
kern.version: Darwin Kernel Version 19.6.0: Mon Sep  7 21:17:51 PDT 2026; ritesh:xnu/DEVELOPMENT_ARM64_BCM2712
hw.machine: linux,dummy-virt
ravynOS# mem
hw.memsize: 2086666240
hw.ncpu: 1
ravynOS# ls /
./  ../  .fseventsd/  .HFS+ Private Directory Data/  .journal  .journal_info_block
bin/  dev/  etc  private/  README-ravynos-bringup.txt  sbin/  System/  tmp  Users/  usr/  var  Volumes/
ravynOS# cat /etc/motd
ravynOS arm64 bring-up
ravynOS# mkdir /tmp/marypi
/tmp/marypi: errno 30
ravynOS# ls /dev
console  tty  null  zero  klog  ...  ptmx  random  urandom  disk0  rdisk0  disk0s2  rdisk0s2  disk0s1  rdisk0s1
```

`errno 30` is `EROFS`: XNU mounts the root read-only and leaves remounting
it read-write to launchd, which ravyninit does not do yet (`mount` is not
one of its commands).

## Talking to it

The shell reads the console. In the VM, run
`swift run marypi vm run --display serial` so your terminal is the serial
port; on a Pi 5, the 3-pin debug UART. The prompt is `ravynOS# `.

## What the real userland needs

In dependency order, all cross-compiled for `arm64-apple-darwin19` against
the arm64 SDK the kernel build installs:

1. `dyld` (has arm64 support upstream),
2. `libSystem` and its members: libplatform, libpthread, libc (Libc), libm,
   libdispatch, libxpc/liblaunch, libunwind, libc++,
3. `launchd`, `/bin/sh`, the base BSD tools,
4. Cocoa-level frameworks, which ravynOS already builds for x86_64.

Each of those has some arm64-specific assembly (syscall stubs, setjmp,
atomics, string routines) that Apple's sources carry; the work is mostly
build-system plumbing plus the same "host is not target" fixes the kernel
build needed.
