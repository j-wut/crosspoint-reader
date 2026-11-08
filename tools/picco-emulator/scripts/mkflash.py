#!/usr/bin/env python3
"""Build a 16 MB Picco flash image: bootloader @0, partition table @0x8000, app @0xE0000."""
import hashlib, struct, sys
boot, app, out = sys.argv[1:4]
parts = [  # name, type, subtype, offset, size  (stock layout minus the vendor safeboot app)
    ("nvs", 1, 0x02, 0x9000, 0xC000),
    ("otadata", 1, 0x00, 0x15000, 0x2000),
    ("app", 0, 0x10, 0xE0000, 0xE70000),
    ("spiffs", 1, 0x82, 0xF50000, 0x80000),
    ("coredump", 1, 0x03, 0xFD0000, 0x10000),
]
tbl = b"".join(struct.pack("<HBBII16sI", 0x50AA, t, s, o, z, n.encode(), 0) for n, t, s, o, z in parts)
tbl += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(tbl).digest()
tbl += b"\xff" * (0xC00 - len(tbl))
img = bytearray(b"\xff" * (16 << 20))
b = open(boot, "rb").read(); img[0:len(b)] = b
img[0x8000:0x8000 + len(tbl)] = tbl
a = open(app, "rb").read(); img[0xE0000:0xE0000 + len(a)] = a
open(out, "wb").write(img)
