# 8. Troubleshooting

**`build: the Docker daemon is not reachable`** Start Docker Desktop and
wait for its whale to settle; `maryos doctor` shows "docker daemon: running"
when it is ready. The first `docker run` after a cold start can take a
minute while Docker Desktop's network comes up.

**`DeadlineExceeded: context deadline exceeded` or a hanging `apt-get`
inside Docker** Networking from the builder stalled. The builder forces IPv4
for `apt` and `wget`; if it still hangs, check that a plain container can
reach the mirror: `docker run --rm ubuntu:24.04 bash -c 'exec
3<>/dev/tcp/ports.ubuntu.com/80 && echo ok'`. Docker Desktop's proxy
settings (Settings > Resources > Proxies) and a VPN are the usual suspects.

**`debootstrap` fails with a missing script for `noble`** The builder image
is older than the suite; `docker rmi maryos-builder` and build again.

**The VM does not start: "The process doesn't have the
com.apple.security.virtualization entitlement"** The binary is unsigned
and could not sign itself (`vm run` and the app try; the message names the
executable). Run `scripts/sign.sh` on it, or `make build`, which signs.
`maryos doctor` reports the entitlement of the binary it runs as.

**`maryos vm run` says a VM is already running** Another runner (the app,
another terminal) owns `state/vm/vm/vm.pid`. `maryos vm stop` ends it; a
stale pid file from a crash is removed automatically.

**The window stays black, or shows only the kernel log** The guest is
booting; `state/vm/vm/serial.log` (`maryos vm serial`) shows how far it got.
A login prompt appears on the window's `tty1` and on the serial console
within a few seconds of the root filesystem mounting. If the log stops at
"Waiting for root file system", the initrd could not find
`LABEL=maryos-root`: the image was built without the label (check
`MARYOS.txt` and `distro.conf`).

**No network in the guest** `ip a` should show an address on `enp0s1` (or
similar) from the NAT network. If not, `networkctl` and
`journalctl -u systemd-networkd`; the netplan file must be mode 0600 and
name interfaces `en*` or `eth*`.

**Names do not resolve in the guest, `resolvectl status` shows a
192.168.65.x server** The builder's `resolv.conf` (Docker Desktop's
resolver) leaked into the image as a regular file, and `systemd-resolved`
treats it as a global DNS server. The chroot session borrows the builder's
file and puts the previous one back; `rootfs.sh` then sets the
`stub-resolv.conf` symlink explicitly. `ls -l /etc/resolv.conf` on the
device must show the symlink.

**The Pi shows nothing on HDMI or the UART** First the EEPROM: it must be
recent enough for Ubuntu 24.04; update it with Raspberry Pi Imager. Then
the card: `MARYOS.txt` on the first partition tells you what was written;
`config.txt` must be there with `kernel=vmlinuz`. The UART needs 115200 8N1
on the 3-pin debug header with GND connected.

**`systemd` reports "Found ordering cycle" for `maryos-firstboot.service`**
The unit must not be `Before=` a socket unit: `sockets.target` comes
before `basic.target`, which every ordinary service is `After=`. The
shipped unit is `Before=ssh.service getty.target`; sshd itself only
starts on the first connection, long after the host keys exist.

**The Pi boots but the root is small** `maryos-firstboot` did not run or
failed; `journalctl -u maryos-firstboot` on the device. Running
`/usr/lib/maryos/firstboot` by hand as root is safe. (The first build of
this kit failed here with "partition-number must be a number": `lsblk`
pads its columns with spaces and `growpart` does not trim them.)

**`flash-kernel` complains during `apt upgrade` on the Pi** It must be able
to read `/proc/device-tree/model`; on real hardware it can. Inside a chroot
or a container, set `FLASH_KERNEL_SKIP=true`.

**`maryos flash` refuses the disk** The flasher only writes to whole,
physical, external disks between 4 GB and 2 TB on SD, USB or Thunderbolt;
`maryos list --all` prints the reason for every disk.
