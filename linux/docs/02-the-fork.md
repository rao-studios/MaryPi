# 2. The fork: what is in `distro/`

Everything that makes MaryOS different from a minimal Ubuntu is in one
directory, as data. The builder reads it; nothing else does.

## distro.conf

`KEY=VALUE` lines, sourced by the builder (sh) and parsed by MaryOSKit.
The name, id, version and codename drive every use: image file names,
`os-release` (`VERSION_ID`, `VERSION_CODENAME`, `PRETTY_NAME`), the login
banner, the manifest on the boot partition. MaryOS starts at version 0.0,
codename `bonnie` (written "Bonnie" through `DISTRO_CODENAME_PRETTY`);
these are MaryOS's numbers, not Ubuntu's. The Ubuntu base is
`BASE_SUITE` (noble) from `BASE_MIRROR` (ports.ubuntu.com, where arm64
lives). The first user, its password, the locale and timezone are here too,
as are the partition labels and sizes. Change the id and version and every
artifact follows.

## packages/

One package per line, `#` comments. `base.list` is what every target gets on
top of a `debootstrap --variant=minbase`: systemd and udev, filesystem
tools, netplan and ssh, sudo, locales, editors. It deliberately names
packages rather than pulling `ubuntu-minimal` or `ubuntu-server`, so the
image contains what the list says; in particular there is no snapd.
`pi5.list` adds the raspi kernel, Raspberry Pi firmware and `flash-kernel`,
Wi-Fi and Bluetooth; `vm.list` adds the generic kernel. `desktop.list` is
an empty placeholder for a desktop flavour.

## overlay/, overlay-pi5/, overlay-vm/

Files copied verbatim onto the rootfs, root-owned, modes kept. The shared
overlay carries the netplan default (DHCP on every wired interface) and the
first-boot service with its script. `overlay-pi5/boot/firmware/` holds
`config.txt` and `usercfg.txt`; whatever ends up under `/boot/firmware` in
the tree becomes the content of the FAT partition.

## hooks/

Shell scripts run inside the chroot with the `distro.conf` values exported,
in name order:

| Stage | Directory | What the shipped hooks do |
|---|---|---|
| base (after `base.list`) | `hooks/` | `10-locale-tz` generates the locale and sets the timezone; `20-user` creates the first user with sudo; `30-branding` writes `os-release`, `lsb-release`, hostname, hosts, issue and motd; `40-services` enables networkd, resolved, timesyncd and the first-boot service; `50-fstab` writes `/etc/fstab` by label |
| target (after `<target>.list`) | `hooks/pi5/`, `hooks/vm/` | `pi5/20-boot-config` writes `cmdline.txt` from the root label; `vm/10-shared-folder` adds the virtiofs share to `fstab` |
| final (end of every target build) | `hooks/final/` | `90-cleanup` removes package lists, temporary files, ssh host keys, the machine id and logs |

Hooks are ordinary `sh -e` scripts. To add behaviour, add a file; to change
behaviour, edit one. The base stage's output is cached by the content of
`distro.conf`, `packages/base.list`, `overlay/` and the base hooks, so a
change there costs a full bootstrap; a change to a target's list or hooks
costs a target build only.

## Branding that survives upgrades

`/etc/os-release` is a symlink to `/usr/lib/os-release`, which belongs to
Ubuntu's `base-files` package: the next `apt upgrade` of `base-files` would
put Ubuntu's version back. The branding hook registers a dpkg diversion, so
`base-files` installs its copy as `/usr/lib/os-release.ubuntu` forever and
MaryOS's stays in place. `/etc/lsb-release` is a conffile, which dpkg leaves
alone once it differs from the package's version.

`ID=maryos` with `ID_LIKE="ubuntu debian"` and `UBUNTU_CODENAME=noble` is
what third-party installers and `apt-add-repository`-style tools look at;
most read `ID_LIKE` and `UBUNTU_CODENAME` and keep working.
`VERSION_CODENAME` is MaryOS's (`bonnie`) and so is what `lsb_release -c`
prints, since it reads `os-release`; a script that builds an apt suite name
from `lsb_release -cs` (some vendor install scripts do) gets `bonnie` and
needs `UBUNTU_CODENAME` from `/etc/os-release` instead. That is the same
trade-off Linux Mint makes, and the price of having a codename of one's own. Tools that insist on
`ID=ubuntu` are the first place a fork shows.

## What is not changed

The apt archive is Ubuntu's: updates and security fixes flow from
ports.ubuntu.com exactly as for Ubuntu. There is no MaryOS repository yet,
no rebuilt package and no MaryOS kernel; chapter 7 lists those as the next
milestones. The builder is arranged so a MaryOS repository becomes one more
`Types: deb` stanza and a rebuilt kernel one more package in a list.
