# 6. First boot and userland

## maryos-firstboot

The image is built as small as it can be and knows nothing about the device
it will run on. `maryos-firstboot.service` runs once, after local
filesystems are mounted and before getty and ssh, and its script
(`/usr/lib/maryos/firstboot`) does three things on the console:

1. Finds the device behind `/` (`findmnt`, `lsblk`), runs `growpart` on the
   partition and `resize2fs` on the filesystem. On a 64 GB card the root
   goes from about 2.5 GB to the whole card; in the VM it fills the sparse
   disk. `growpart` reporting "no change" is fine.
2. Creates the ssh host keys with `ssh-keygen -A`; the build removes them so
   no two devices share a key.
3. Writes `/var/lib/maryos/firstboot-done`, which the unit's
   `ConditionPathExists` checks, so it never runs again.

`/etc/machine-id` is empty in the image; systemd generates one on the first
boot. There is no cloud-init: the first user exists already and the network
is configured statically to DHCP, which keeps the image small and the boot
fast.

## The user

`DEFAULT_USER` from `distro.conf` (`mary`) is created at build time with a
home directory, bash, and membership of `sudo`, `adm`, `dialout`, `video`,
`plugdev` and the Pi's `gpio`, `i2c` and `spi` groups when they exist. Its
password is `DEFAULT_PASSWORD` from the same file, which is in the
repository; change it on first login (`passwd`) or in `distro.conf` before
building. `root` has no password, like Ubuntu.

## Networking

`netplan` with the `networkd` renderer: every `en*` and `eth*` interface
asks for DHCP over IPv4 and IPv6 and is `optional`, so a missing cable does
not delay boot. `systemd-resolved` owns `/etc/resolv.conf`,
`systemd-timesyncd` sets the clock. Wi-Fi on the Pi has its firmware and
`wpasupplicant` installed; add a `wifis:` section to
`/etc/netplan/01-maryos.yaml` on the device, or in the overlay for every
device. In the VM, the NAT network gives the guest an address from the Mac.

`openssh-server` is installed and socket-activated; password logins are on
(Ubuntu's default), so `ssh mary@maryos.local` works once the host keys
exist. Consider keys and `PasswordAuthentication no` in
`/etc/ssh/sshd_config.d/` as the first hardening step.

## Identity on the device

```
$ cat /etc/os-release
PRETTY_NAME="MaryOS 0.0 (Liquid Platinum)"
NAME="MaryOS"
VERSION_ID="0.0"
VERSION_CODENAME=liquid-platinum
ID=maryos
ID_LIKE="ubuntu debian"
UBUNTU_CODENAME=noble
$ hostname
maryos
$ cat /boot/firmware/MARYOS.txt     # on the Pi; the VM's boot partition has the same file
```

## Where to go from here in distro/

- More packages: add to `packages/base.list` (both targets) or a target list.
- A file on every device: put it in `overlay/` with the path it should have.
- Something computed from `distro.conf` or needing a command: a hook.
- A desktop: fill `packages/desktop.list`, add a `desktop` target in the
  builder (a copy of the `vm` or `pi5` target with that list), and a hook
  that enables the display manager.
