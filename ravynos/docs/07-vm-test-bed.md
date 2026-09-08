# 7. The VM test bed

Hardware iteration is slow: write a card, move it, watch a serial line.
The same image boots under QEMU's `virt` machine with EDK2 (UEFI) firmware,
which offers exactly what the kernel speaks: a GIC (v2 or v3), a PL011 and
the generic timer. MaryPi ships the VM setup in `vm/` and drives it from the
CLI and the app.

## Running it

```sh
swift run marypi vm run                       # build the image, boot it in a window
swift run marypi vm run --detach              # same, keep the terminal
swift run marypi vm run --display serial      # serial console on this terminal (Ctrl-C stops)
swift run marypi vm status | serial | stop
vm/run.sh run --image path/to.img             # any host with a POSIX shell and QEMU
```

Profiles live in `vm/profiles/*.conf`: `qemu-virt` (GICv2, like the Pi 5,
software emulation on macOS) and `qemu-virt-gicv3` (hardware acceleration
where the host allows it). State per profile goes to `vm/state/<profile>/`:
the disk image, the UEFI variable store, `serial.log`, the QEMU pid and a
QMP control socket.

The disk QEMU sees is a `virtio-blk-device` on the virtio-mmio transport,
which is why chapter 5 has a virtio block driver: on QEMU it plays the role
the SD controller plays on the Pi.

## Reading the window and the log

The window shows EDK2 first (Esc or F2 during its countdown opens the
firmware menus), then the booter's text, then the kernel's console because
the kernel mirrors it onto the framebuffer. `serial.log` holds the same
text plus everything `kprintf` writes, which is more; when something goes
wrong, read the log.

A window that stops updating is usually not a hang. After a panic the
kernel waits for a debugger on the serial line ("Waiting for remote
debugger connection"), and the window keeps showing the panic. `vm stop`
ends the session.

## Driving the console

In window mode the serial port is a log file, so the guest gets no input.
Use `--display serial` to attach your terminal to the serial port; that is
how you type at `ravyninit`'s prompt (chapter 6). The window's USB keyboard
is not usable yet because the kernel has no HID stack.

## The parity test

`vm/run.sh` (for hosts without Swift) and the Swift launcher build the same
QEMU command line; `Tests/MaryPiKitTests/RunShParityTests.swift` runs the
shell script in dry-run mode and compares the argument lists so the two
cannot drift apart.
