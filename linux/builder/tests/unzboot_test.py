#!/usr/bin/env python3
"""Tests for builder/unzboot.py: raw, gzip and zboot inputs.  Run with
`python3 builder/tests/unzboot_test.py` (no dependencies)."""
import gzip
import importlib.util
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location("unzboot", os.path.join(HERE, "..", "unzboot.py"))
unzboot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(unzboot)


def fake_image(size=8192) -> bytes:
    body = bytearray(os.urandom(size))
    body[56:60] = b"ARM\x64"
    return bytes(body)


def zboot(payload: bytes, kind=b"gzip") -> bytes:
    header = bytearray(64)
    header[0:2] = b"MZ"
    header[4:8] = b"zimg"
    struct.pack_into("<II", header, 8, len(header), len(payload))
    header[0x18:0x18 + len(kind) + 1] = kind + b"\0"
    return bytes(header) + payload + struct.pack("<I", 0)


class UnzbootTests(unittest.TestCase):
    def setUp(self):
        self.image = fake_image()

    def test_raw_image_passes_through(self):
        self.assertEqual(unzboot.extract(self.image), self.image)

    def test_gzip_stream(self):
        self.assertEqual(unzboot.extract(gzip.compress(self.image)), self.image)

    def test_gzip_with_trailing_garbage(self):
        self.assertEqual(unzboot.extract(gzip.compress(self.image) + b"\0" * 100), self.image)

    def test_zboot_gzip(self):
        self.assertEqual(unzboot.extract(zboot(gzip.compress(self.image))), self.image)

    def test_zboot_unknown_compression_fails(self):
        with self.assertRaises(SystemExit):
            unzboot.extract(zboot(self.image, kind=b"bzip2"))

    def test_garbage_fails(self):
        with self.assertRaises(SystemExit):
            unzboot.extract(b"not a kernel at all" * 10)

    def test_main_writes_file_and_checks_magic(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "vmlinuz")
            dst = os.path.join(tmp, "Image")
            with open(src, "wb") as handle:
                handle.write(zboot(gzip.compress(self.image)))
            self.assertEqual(unzboot.main(["unzboot", src, dst]), 0)
            with open(dst, "rb") as handle:
                self.assertEqual(handle.read(), self.image)
            with open(src, "wb") as handle:
                handle.write(gzip.compress(b"x" * 4096))
            with self.assertRaises(SystemExit):
                unzboot.main(["unzboot", src, dst])


if __name__ == "__main__":
    unittest.main(verbosity=1)
