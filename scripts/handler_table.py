#!/usr/bin/env python3
"""Locate the esp_http_server URI registration table and decode uri -> handler pairs.

Pure byte analysis of the app image: find pointers to the known endpoint URI
strings, cluster them into a table by constant stride, then decode each entry.
"""
import struct, sys, collections

b = open(sys.argv[1] if len(sys.argv) > 1 else
         'rapt-pill-2.0.0.bin', 'rb').read()
DROM_ADDR, DROM_OFF = 0x3f400020, 0x20
A2F = lambda a: a - DROM_ADDR + DROM_OFF          # virtual addr -> file offset
F2A = lambda o: o - DROM_OFF + DROM_ADDR
TXT = (0x400d0020, 0x400d0020 + 0xdc538)
IRAM = (0x40080000, 0x400842e8)
IRAM2 = (0x400842e8, 0x400842e8 + 0x1a8c4)

def is_code(a):
    return TXT[0] <= a < TXT[1] or IRAM[0] <= a < IRAM[1] or IRAM2[0] <= a < IRAM2[1]

def word_at(off):
    return struct.unpack_from('<I', b, off)[0] if off + 4 <= len(b) else 0

def strings_at(a):
    o = A2F(a)
    e = b.find(b'\0', o)
    return b[o:e].decode('latin-1') if o >= 0 and e > o else None

# 1. all pointers that point at a printable string inside DROM
ptrs = collections.defaultdict(list)
for off in range(0, len(b) - 4, 4):
    v = word_at(off)
    if 0x3f400020 <= v < 0x3f444908:
        o = A2F(v)
        if o < 0:
            continue
        ch = b[o]
        if 0x20 <= ch < 0x7f:
            ptrs[F2A(off)].append(v)

# 2. cluster pointer sites into tables by stride
sites = sorted(ptrs)
tables, cur = [], [sites[0]] if sites else []
for s in sites[1:]:
    if s - cur[-1] <= 0x40:
        cur.append(s)
    else:
        if len(cur) >= 4:
            tables.append(cur)
        cur = [s]
if len(cur) >= 4:
    tables.append(cur)

for t in tables:
    deltas = collections.Counter(t[i + 1] - t[i] for i in range(len(t) - 1))
    stride, cnt = deltas.most_common(1)[0]
    if cnt < max(4, len(t) // 2):
        continue
    print(f"\n### table @0x{F2A(A2F(t[0])):08x}  stride=0x{stride:x}  entries~{len(t)}")
    for s in t:
        a = F2A(s)
        uri = strings_at(ptrs[s][0])
        ent = []
        for k in range(0, stride, 4):
            v = word_at(A2F(a + k))
            tag = 'uri' if v == ptrs[s][0] else ('handler' if is_code(v) else '')
            ent.append(f"{v:#010x}{(':' + tag) if tag else ''}")
        print(f"  0x{a:08x}  {uri!r:42s} " + ' '.join(ent))
