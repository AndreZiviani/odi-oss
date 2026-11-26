#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
"""Append the CRC trailer `nv slotcheck` reads to a squashfs image.

    image/rootfs-crc.py build/rootfs.img

squashfs has no checksum of its own. The 16 bytes this appends sit right
after the filesystem, which mksquashfs pads to 4 KiB, where the kernel never
reads: "odrc", the length covered, the CRC32 of those bytes and the CRC32 of
the first 12 trailer bytes, little-endian. src/nv/slot.h documents the
layout and what the check does with it.
"""
import struct
import sys
import zlib

PAD = 4096


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: rootfs-crc.py rootfs.img")
    path = sys.argv[1]
    with open(path, "rb") as f:
        fs = f.read()
    if len(fs) < 96 or fs[:4] != b"hsqs":
        sys.exit("rootfs-crc: %s is not a squashfs image" % path)
    used = struct.unpack_from("<Q", fs, 40)[0]
    end = (used + PAD - 1) // PAD * PAD
    # Run twice, or on an image mksquashfs did not pad: the trailer would
    # not sit where nv looks for it.
    if len(fs) != end:
        sys.exit("rootfs-crc: %s is %d bytes, not bytes_used %d padded to %d"
                 % (path, len(fs), used, end))
    head = b"odrc" + struct.pack("<II", end, zlib.crc32(fs))
    with open(path, "ab") as f:
        f.write(head + struct.pack("<I", zlib.crc32(head)))


if __name__ == "__main__":
    main()
