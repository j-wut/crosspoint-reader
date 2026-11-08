#!/usr/bin/env python3
"""Find 32-bit literal references to the DROM address of a string in the app image."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
segs = []; off = 24
for _ in range(d[1]):
    a, l = struct.unpack_from('<II', d, off); off += 8; segs.append((a, off, l)); off += l
def f2v(fo):
    for a, o, l in segs:
        if o <= fo < o + l: return a + fo - o
for needle in sys.argv[2:]:
    fo = d.find(needle.encode() + b'\0')
    while fo != -1:
        if d[fo - 1] == 0:
            v = f2v(fo); print(needle, 'at', hex(v))
            for a, o, l in segs:
                i = d.find(struct.pack('<I', v), o, o + l)
                while i != -1:
                    print('   literal at', hex(a + i - o)); i = d.find(struct.pack('<I', v), i + 1, o + l)
        fo = d.find(needle.encode() + b'\0', fo + 1)
