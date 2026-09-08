#!/usr/bin/env python3
"""Turn Ubuntu's arm64 vmlinuz into the raw kernel Image that
Virtualization.framework's VZLinuxBootLoader boots.

    unzboot.py <vmlinuz> <Image>

Accepts, in order of detection: a raw arm64 Image (copied), a gzip stream
(decompressed), or an EFI zboot application ("MZ" at 0, "zimg" at 4, u32 LE
payload offset at 8, u32 LE payload size at 12, compression name at 0x18)
whose payload is gzip, zstd, xz/lzma or lz4. The output must carry the arm64
boot magic "ARM\\x64" at offset 56, otherwise the run fails.
"""
import lzma
import struct
import subprocess
import sys
import zlib

ARM64_MAGIC = b"ARM\x64"
ARM64_MAGIC_OFFSET = 56
ZBOOT_MAGIC = b"zimg"


def is_arm64_image(data: bytes) -> bool:
    return len(data) > ARM64_MAGIC_OFFSET + 4 and data[ARM64_MAGIC_OFFSET:ARM64_MAGIC_OFFSET + 4] == ARM64_MAGIC


def gunzip(data: bytes) -> bytes:
    # zlib with a 16+ window accepts a gzip member and ignores trailing bytes.
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    return d.decompress(data) + d.flush()


def run_tool(argv, data: bytes) -> bytes:
    try:
        return subprocess.run(argv, input=data, capture_output=True, check=True).stdout
    except FileNotFoundError:
        raise SystemExit(f"unzboot: {argv[0]} is needed to unpack this kernel and is not installed")
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"unzboot: {argv[0]} failed: {error.stderr.decode(errors='replace').strip()}")


def decompress(kind: str, payload: bytes) -> bytes:
    if kind == "gzip":
        return gunzip(payload)
    if kind == "zstd":
        return run_tool(["zstd", "-d", "-c"], payload)
    if kind in ("xz", "lzma"):
        return lzma.decompress(payload)
    if kind == "lz4":
        return run_tool(["lz4", "-d", "-c"], payload)
    raise SystemExit(f"unzboot: unsupported zboot compression {kind!r}")


def zboot_payload(data: bytes):
    """(compression name, payload) for an EFI zboot image, or None."""
    if len(data) < 0x40 or data[:2] != b"MZ" or data[4:8] != ZBOOT_MAGIC:
        return None
    offset, size = struct.unpack_from("<II", data, 8)
    kind = data[0x18:0x28].split(b"\0", 1)[0].decode("ascii", errors="replace")
    if offset + size > len(data):
        raise SystemExit("unzboot: zboot header points past the end of the file")
    return kind, data[offset:offset + size]


def extract(data: bytes) -> bytes:
    if is_arm64_image(data):
        return data
    if data[:2] == b"\x1f\x8b":
        return gunzip(data)
    zboot = zboot_payload(data)
    if zboot is not None:
        kind, payload = zboot
        return decompress(kind, payload)
    raise SystemExit("unzboot: not a raw Image, a gzip stream or an EFI zboot image")


def main(argv) -> int:
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    source, destination = argv[1], argv[2]
    with open(source, "rb") as handle:
        data = handle.read()
    image = extract(data)
    if not is_arm64_image(image):
        raise SystemExit(f"unzboot: {source} did not unpack to an arm64 Image (no ARM\\x64 magic at offset 56)")
    with open(destination, "wb") as handle:
        handle.write(image)
    print(f"unzboot: {source} -> {destination} ({len(image)} bytes)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
