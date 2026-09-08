# 3. Building from source

## Where the build runs

macOS cannot create ext4 filesystems or run `dpkg`, so the build runs in a
Linux environment: an arm64 Ubuntu container under Docker Desktop, which
executes arm64 natively on Apple silicon (no `qemu-user-static`). The same
scripts run as root on an arm64 Linux host without Docker; `build.sh`
decides by checking whether it is root on Linux.

`builder/run-in-docker.sh` builds the `maryos-builder` image from
`builder/Dockerfile` (Ubuntu Noble plus `debootstrap`, `e2fsprogs`,
`dosfstools`, `mtools`, `fdisk`, `zstd`, `python3`) and runs `build.sh` in
it with `--privileged`, which the chroot mounts need. Three mounts:

| Mount | Purpose |
|---|---|
| `linux/` at `/work` | `distro/` and `builder/` are read; nothing else in the checkout is touched |
| `out/` (or `$MARYOS_OUT`) at `/out` | finished images and the VM boot files |
| volumes `maryos-cache` at `/cache`, `maryos-work` at `/build` | apt downloads and the base tarball; rootfs trees and partition images |

Keeping the trees in a volume matters: bind mounts through Docker Desktop
are slow for millions of small files and do not keep sparse files sparse.

## The three stages

`build.sh rootfs`, `build.sh target <pi5|vm>`, `build.sh image <pi5|vm>`;
`build.sh all [pi5|vm|both]` runs them in order. `--dry-run` prints the
stages, `--fresh` ignores the cached base, `--keep` leaves the tree.

**rootfs** (`lib/rootfs.sh`): `debootstrap --arch=arm64 --variant=minbase
noble` with the Ubuntu keyring, then the apt sources in deb822 form
(`noble`, `noble-updates`, `noble-security`), then `apt-get install` of
`base.list`, the overlay, the base hooks. The tree is packed with `tar` and
`zstd` into `cache/rootfs/base-noble-arm64-<key>.tar.zst`, where the key is a
hash of everything that went in.

**target** (`lib/target.sh`): unpack, install the target list, apply the
target overlay and hooks, collect the boot files, run the final hooks. For
the Pi that means copying the raspi `vmlinuz`, `initrd.img`, the BCM2712
device trees and `overlays/` into `/boot/firmware`, the same names Ubuntu
and `flash-kernel` use. For the VM it means extracting the raw kernel
`Image` (next section) and writing `boot.json` with the kernel version and
the command line.

**image** (`lib/image.sh`): described below. The tree is deleted afterwards
unless `--keep`.

## Working in the chroot

`lib/chroot.sh` mounts `/proc`, `/sys`, `/dev`, `/dev/pts` and a tmpfs
`/run` into the tree, bind-mounts the shared apt cache over
`/var/cache/apt/archives` (so downloads survive between builds and never
end up in the image), replaces `resolv.conf` with the builder's for the
session (and puts the previous one back afterwards; the rootfs stage then
sets the `systemd-resolved` stub symlink, so nothing of the builder's
network leaks into the image), installs a `policy-rc.d` that refuses to
start services, and
exports `DEBIAN_FRONTEND=noninteractive` and `FLASH_KERNEL_SKIP=true`
(`flash-kernel` cannot find a machine in a chroot). Everything is undone on
exit, including on failure, by a trap.

## An image without loop devices

The classic way to build a disk image is to attach it as a loop device,
partition it, mount the partitions and copy files in. That needs root and
kernel support, and Docker Desktop's VM is not a good place for it. The
builder never mounts anything:

1. Whatever is under `/boot/firmware` in the tree is moved aside and becomes
   partition 1: `mkfs.vfat -F 32 -C` creates a FAT32 image of
   `BOOT_PARTITION_MIB`, `mcopy -s` copies the files in, and `MARYOS.txt` is
   added with the name, version, target, kernel, date and git sha.
2. `mke2fs -t ext4 -d <tree>` creates partition 2 directly from the
   directory, preserving ownership, modes and extended attributes. Its size
   is the tree plus 30 %, at least `ROOT_MIN_MIB`, rounded to 64 MiB.
3. `truncate` makes a sparse image, `sfdisk` writes an MBR with partition 1
   at 4 MiB (type `c`, bootable) and partition 2 after it (type `83`), and
   `dd conv=notrunc,sparse` drops both filesystem images in.

The result is the smallest image that fits; the root partition grows to the
card or disk on first boot (chapter 6). The VM runner enlarges its own copy
before the first boot for the same reason.

## Ubuntu's kernel and the raw Image

Virtualization.framework's `VZLinuxBootLoader` takes an uncompressed arm64
kernel `Image`. Ubuntu ships `vmlinuz` as either a gzip stream or, for
newer kernels, an EFI "zboot" application: a PE header (`MZ`, then `zimg`
at offset 4, a little-endian payload offset at 8 and size at 12, and the
compression name at 0x18) around a compressed `Image`. `builder/unzboot.py`
recognises a raw `Image`, a gzip stream and a zboot container (gzip, zstd,
xz or lz4 payloads), unpacks it, and refuses anything that does not carry
the arm64 boot magic `ARM\x64` at offset 56. Its tests build synthetic
kernels in each shape.

## Networking inside Docker

Docker Desktop's resolver returns IPv6 addresses first and IPv6 egress from
the builder can stall; the first build attempt here timed out in `apt-get`.
The builder image and the chroot's `apt-get` are pinned to IPv4
(`Acquire::ForceIPv4`, `inet4-only` for `debootstrap`'s `wget`).

## Outputs

```
out/maryos-0.0-pi5.img(.sha256, .txt)     the card image and its manifest
out/maryos-0.0-vm.img(.sha256, .txt)      the VM disk image
out/vm/Image, initrd.img, boot.json         what the VM boots directly
```

`maryos build` and the app run the same script and stream its log; the
`==> stage:` lines drive the step list.
