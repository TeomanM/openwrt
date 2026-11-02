#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Wrap an OpenWrt UBI image into a TP-Link Archer AX72 v1 factory image that
# the stock web UI "Firmware Upgrade" page accepts.
#
# Layout (big endian):
#   0x0000  u32     total image size
#   0x0004  u8[16]  MD5(salt + image[0x14:]), same salt as tplink-mkimage-2022.py
#   0x0014  char[]  "fw-type:..." string, rest of the block zero
#   0x1000  0xff padding up to 0x1014
#   0x1014  metadata sections as a linked list, each:
#             char name[32], u32 data offset, u32 next entry offset (0 = last),
#             u32 data size; offsets are relative to 0x1014
#   ...     UBI image (kernel + rootfs volumes), right after the last section
#
# The support-list holds only the Archer AX72 v1 (product_ver 1.0.0) entries
# from stock firmware. soft-version is bumped so the stock anti-downgrade check
# always passes.

import argparse
import hashlib
import struct

SALT = bytes([0x7a, 0x2b, 0x15, 0xed, 0x9b, 0x98, 0x59, 0x6d,
              0xe5, 0x04, 0xab, 0x44, 0xac, 0x2a, 0x9f, 0x4e])

META_OFF = 0x1014
ENTRY_HDR = 0x2c

FW_TYPE = b"fw-type:Proud\n"

SUPPORT_LIST = "SupportList:\n" + "".join(
    "{product_name:Archer AX72,product_ver:1.0.0,special_id:%s}\n" % sid
    for sid in ("55530000", "45550000", "4A500000", "52550000",
                "43410000", "54570000", "53470000", "4B520000"))

SOFT_VERSION = "soft_ver:9.9.9 Build 20991231 rel.99999\nfw_id:\n\n"


def build_image(ubi):
    sections = [("support-list", SUPPORT_LIST.encode()),
                ("soft-version", SOFT_VERSION.encode())]

    meta = bytearray()
    for i, (name, data) in enumerate(sections):
        entry = len(meta)
        last = i == len(sections) - 1
        nxt = 0 if last else entry + ENTRY_HDR + len(data)
        meta += name.encode().ljust(32, b"\0")
        meta += struct.pack(">III", entry + ENTRY_HDR, nxt, len(data))
        meta += data

    img = bytearray(META_OFF)
    img[0x14:0x14 + len(FW_TYPE)] = FW_TYPE
    img[0x1000:META_OFF] = b"\xff" * (META_OFF - 0x1000)
    img += meta + ubi

    struct.pack_into(">I", img, 0, len(img))
    img[4:0x14] = hashlib.md5(SALT + bytes(img[0x14:])).digest()
    return bytes(img)


def main():
    ap = argparse.ArgumentParser(description="TP-Link Archer AX72 v1 factory image")
    ap.add_argument("--rootfs", required=True, help="OpenWrt UBI image")
    ap.add_argument("-o", "--output", required=True, help="factory image output")
    args = ap.parse_args()

    with open(args.rootfs, "rb") as f:
        ubi = f.read()
    with open(args.output, "wb") as f:
        f.write(build_image(ubi))


if __name__ == "__main__":
    main()
