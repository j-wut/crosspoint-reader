#!/usr/bin/env python3
"""Wrap an ESP32-S3 app image's segments in an ELF so gdb/objdump can disassemble it."""
import struct, sys
src, out = sys.argv[1:3]
d = open(src, "rb").read()
nseg = d[1]; off = 24; segs = []
for _ in range(nseg):
    addr, ln = struct.unpack_from("<II", d, off); off += 8
    segs.append((addr, d[off:off + ln])); off += ln
ehsz, phsz, shsz = 52, 32, 40
data_off = ehsz + phsz * len(segs)
body = b""; ph = b""
for addr, blob in segs:
    flags = 5 if addr >= 0x40000000 and addr < 0x50000000 or addr >= 0x42000000 else 6
    ph += struct.pack("<IIIIIIII", 1, data_off + len(body), addr, addr, len(blob), len(blob), flags, 4)
    body += blob
# Section headers: null + one PROGBITS per segment + shstrtab (so objdump -D works per section)
names = b"\0" + b"".join(b".seg%d\0" % i for i in range(len(segs))) + b".shstrtab\0"
sh_off = data_off + len(body) + len(names)
sh = b"\0" * shsz; p = data_off; noff = 1
for i, (addr, blob) in enumerate(segs):
    exe = 0x40000000 <= addr < 0x44000000
    sh += struct.pack("<IIIIIIIIII", noff, 1, 2 | (4 if exe else 1), addr, p, len(blob), 0, 0, 4, 0)
    noff += len(b".seg%d\0" % i); p += len(blob)
sh += struct.pack("<IIIIIIIIII", noff, 3, 0, 0, data_off + len(body), len(names), 0, 0, 1, 0)
hdr = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, struct.unpack_from("<I", d, 4)[0],
      ehsz, sh_off, 0x300, ehsz, phsz, len(segs), shsz, len(segs) + 2, len(segs) + 1)
open(out, "wb").write(hdr + ph + body + names + sh)
