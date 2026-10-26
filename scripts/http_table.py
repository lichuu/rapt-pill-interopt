#!/usr/bin/env python3
"""Decode the pill's HTTP endpoint table.

For every URI string ("/sensors.json", ...) find the pointer(s) that reference
it in the image, then print the surrounding entry with any code pointers next
to it. Those code pointers are the esp_http_server handler functions.
"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'rapt-pill-2.0.0.bin'
b = open(path, 'rb').read()
DROM_ADDR, DROM_OFF = 0x3f400020, 0x20
A2F = lambda a: a - DROM_ADDR + DROM_OFF
F2A = lambda o: o - DROM_OFF + DROM_ADDR
CODE = [(0x400d0020, 0x400d0020 + 0xdc538), (0x40080000, 0x400842e8),
        (0x400842e8, 0x400842e8 + 0x1a8c4)]
is_code = lambda a: any(s <= a < e for s, e in CODE)

def rd32(off):
    return struct.unpack_from('<I', b, off)[0] if 0 <= off and off + 4 <= len(b) else 0

def cstr(a):
    o = A2F(a)
    if o < 0 or o >= len(b):
        return None
    e = b.find(b'\0', o)
    if e < 0:
        return None
    s = b[o:e]
    return s.decode('latin-1') if all(0x20 <= c < 0x7f for c in s) else None

# 1. endpoint URI strings: taken from Ghidra's string export (evidence, not guessing)
uris = []
for line in open('./out/strings.tsv').read().splitlines()[1:]:
    a, fn, s = line.split('\t', 2)
    s = s.strip()
    if s.startswith('/') and (s.endswith('.json') or s.endswith('.html') or s == '/'):
        uris.append((int(a, 16), s))
print(f"{len(uris)} endpoint URI strings")

# 2. pointer sites for each
for a, s in uris:
    pat = struct.pack('<I', a)
    sites, i = [], 0
    while True:
        i = b.find(pat, i)
        if i < 0:
            break
        if i % 4 == 0:
            sites.append(F2A(i))
        i += 1
    rows = []
    for site in sites:
        fields = []
        for k in range(-8, 20, 4):
            v = rd32(A2F(site + k))
            tag = 'uri' if v == a else ('HANDLER' if is_code(v) else ('str:' + cstr(v)[:22] if cstr(v) else ''))
            fields.append(f"{k:+d}={v:#010x}{' ' + tag if tag else ''}")
        rows.append((site, fields))
    print(f"\n{s}  (str@0x{a:08x})  {len(sites)} pointer site(s)")
    for site, fields in rows:
        print(f"   site 0x{site:08x}: " + '  '.join(fields))
