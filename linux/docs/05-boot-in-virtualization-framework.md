# 5. Booting in Virtualization.framework

## Direct kernel boot

Apple's Virtualization.framework runs Linux guests natively on Apple
silicon. Its `VZLinuxBootLoader` takes a kernel and an initrd from files
on the host and a command line, so no firmware or bootloader lives in the
image: the VM boots `out/vm/Image` (the raw kernel extracted from Ubuntu's
`vmlinuz`, chapter 3) with `out/vm/initrd.img` and
`console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait`. The
initramfs finds the root by label on the virtio disk exactly as on the Pi.

This is why the VM image has an empty boot partition: nothing reads it. The
layout is kept identical anyway so `fstab`, labels and the first-boot
service are the same code on both targets.

## Devices

`VMConfigurationBuilder` turns a `VMSpec` into the configuration:

| Device | Purpose |
|---|---|
| `VZVirtioBlockDeviceConfiguration` on a disk image attachment | the root disk, `state/vm/vm/disk.img` |
| `VZVirtioNetworkDeviceConfiguration` with `VZNATNetworkDeviceAttachment` | outbound networking through the Mac; the MAC address is generated once per state directory and kept, so the DHCP lease survives |
| `VZVirtioConsoleDeviceSerialPortConfiguration` with a file-handle attachment | `hvc0`; output to `serial.log`, optionally also to your terminal |
| `VZVirtioGraphicsDeviceConfiguration` (1280×800 scanout), `VZUSBKeyboardConfiguration`, `VZUSBScreenCoordinatePointingDeviceConfiguration` | the window; `tty1` gets a getty there, so the window is a usable console |
| `VZVirtioFileSystemDeviceConfiguration` per share | virtiofs; `out/` is always shared as `maryos-out` and mounted at `/mnt/maryos-out` in the guest |
| entropy, memory balloon | the usual |

Ubuntu's generic kernel carries all of these as modules and the initramfs
includes them (`MODULES=most`).

## The disk

`maryos vm run` never boots `out/maryos-0.0-vm.img` itself. It clones it
into `state/vm/vm/disk.img` (an APFS clone, instant, sharing blocks with
the original), then extends the clone to `VM_DISK_GIB` with `truncate`, so
the file is sparse and the guest sees a bigger disk than the image. The
first boot grows the root partition into that space, the same as on a card.
`--fresh` clones again; `maryos vm reset` deletes the clone; the kernel,
initrd and `boot.json` are copied alongside so the state stays consistent
with the image it came from even after a rebuild.

## Window, console, log

Without `--headless` the VM has a window; the guest's `tty1` is there and
keys go to it. With or without a window, everything on `hvc0` is appended to
`state/vm/vm/serial.log` with a session header. `maryos vm serial` follows
the log; `maryos vm run --console` puts your terminal in raw mode and
attaches it to `hvc0`, with Ctrl-] as the key that stops the VM (Ctrl-C goes
to the guest in that mode).

Stopping is graceful: the runner calls `requestStop()`, which the guest sees
as an ACPI power button; systemd powers off, the framework reports
`guestDidStop`, and the runner exits. After 20 seconds it forces the machine
off. `maryos vm stop` from another terminal sends the runner `SIGTERM`,
which triggers the same sequence.

## The entitlement

Virtualization.framework refuses processes without
`com.apple.security.virtualization`: `VZVirtualMachineConfiguration.validate()`
already fails with "The process doesn't have the entitlement". A plain
`swift build` produces an unsigned binary. Rather than making everyone
remember a signing step, the CLI's `vm run` and the app's launch check
their own executable (`codesign -d --entitlements`) and, when the
entitlement is missing, ad-hoc sign it in place with the entitlements
embedded in `SelfEntitlement` and re-exec themselves with the same
arguments. `codesign` writes a new file and renames it over the old one, so
the running process is unaffected, and SwiftPM does not relink a binary
whose signature changed, so this happens once per build; a marker in the
environment stops a failed signing from looping. `scripts/sign.sh` does the
same ahead of time (`make build` calls it) and `scripts/bundle.sh` signs
`MaryOS.app`. Ad-hoc signing is enough on the Mac that builds it.
`maryos doctor` reports the running binary's entitlement.

The unit tests do not have the entitlement either, so they check the
assembled configuration rather than a validated one.

## The CLI and the main thread

Two details of the framework shape the CLI. First, a `VZVirtualMachine` is
bound to one dispatch queue and delivers its delegate calls there; the
runner uses the main queue. Second, a window needs AppKit's event loop.
Both only work when the real main thread drives the main run loop at top
level: an `async` main runs inside a dispatch block, and a nested
`NSApp.run()` (or run loop) started from inside such a block does not
drain further main-queue work, which deadlocks MainActor code and the
framework's callbacks. So the CLI's commands are synchronous; async work
runs through `runBlocking`, which starts it on the main actor and pumps the
main run loop until it finishes, and a windowed `vm run` calls `NSApp.run()`
directly from the command. The SwiftUI app has no such problem: it is an
ordinary `NSApplicationMain` process.
