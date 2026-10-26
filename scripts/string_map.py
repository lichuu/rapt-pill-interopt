#!/usr/bin/env python3
"""Build a complete string -> function map from Ghidra's reference export.

Reads raw bytes straight out of the app image (Ghidra only defines a string
where auto-analysis created a string data type), then attributes every
instruction reference that lands inside a printable string to its function.

Usage: string_map.py <regex> [-n N]     (no args = write the map only)
Writes notes/string_map.tsv; prints matching rows.
"""
import bisect, collections, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'rapt-pill-2.0.0.bin')
SEG = [(0x3f400020, 0x20, 0x448e8), (0x3ff80278, 0x44910, 0x1c50),
       (0x3ffbdb60, 0x46568, 0x57c0), (0x40080000, 0x4bd30, 0x42e8),
       (0x400d0020, 0x50020, 0xdc538), (0x400842e8, 0x12c560, 0x1a8c4),
       (0x400c0000, 0x146e2c, 0x274), (0x50000000, 0x1470a8, 0x1800)]
raw = open(BIN, 'rb').read()
by_addr = {}
for va, off, ln in SEG:
    by_addr[va] = raw[off:off + ln]
    SEG.sort(key=lambda s: s[0])
starts = sorted(by_addr)

def read(a, n=4096):
    for i, s in enumerate(starts):
        if s <= a:
            nxt = starts[i + 1] if i + 1 < len(starts) else 1 << 64
            if a < nxt:
                d = by_addr[s]
                o = a - s
                return d[o:o + n] if o < len(d) else b''
    return b''

def cstr(a, maxlen=400):
    d = read(a, maxlen + 1)
    i = d.find(b'\0')
    if i is None or i < 4:
        return None
    s = d[:i]
    if not all(0x20 <= c < 0x7f for c in s):
        return None
    return s.decode('latin-1')

fns = []
for l in open(f'{ROOT}/out/functions.tsv').read().splitlines()[1:]:
    a, n, c = l.split('\t')
    fns.append((int(a, 16), n))
fns.sort()
fstarts = [f[0] for f in fns]

def owner(a):
    i = bisect.bisect_right(fstarts, a) - 1
    return fns[i] if i >= 0 else (0, '?')

def word(a):
    d = read(a, 4)
    return struct.unpack('<I', d)[0] if len(d) == 4 else 0

owners = collections.defaultdict(list)
for l in open(f'{ROOT}/out/refs.tsv').read().splitlines()[1:]:
    fr, to = l.split('\t')
    fr, to = int(fr, 16), int(to, 16)
    # hop 1: reference lands directly on a string
    targets = []
    if 0x3f400020 <= to < 0x3f444908:
        targets.append(to)
    # hop 2: reference lands on a literal-pool slot in text; the word there is
    # the string address
    else:
        v = word(to)
        if 0x3f400020 <= v < 0x3f444908:
            targets.append(v)
    for t in targets:
        s = cstr(t)
        if not s:
            continue
        o = owner(fr)
        lst = owners[(t, s)]
        if o not in lst:
            lst.append(o)

with open(f'{ROOT}/notes/string_map.tsv', 'w') as f:
    f.write('addr\towner\tstring\n')
    for (a, s), ol in sorted(owners.items()):
        for o in ol:
            f.write(f'{a:08x}\t{o[0]:08x}:{o[1]}\t{s[:180]}\n')
print(f'{len(owners)} strings with at least one code reference')

if len(sys.argv) > 1:
    pat = re.compile(sys.argv[1], re.I)
    n = int(sys.argv[sys.argv.index('-n') + 1]) if '-n' in sys.argv else 25
    hits = [((a, s), ol) for (a, s), ol in sorted(owners.items()) if pat.search(s)]
    print(f'{len(hits)} matching strings')
    for (a, s), ol in hits[:n]:
        o = ' '.join(f'{x[0]:x}:{x[1]}' for x in ol[:2])
        print(f'  0x{a:08x} {o:32s} {s[:80]}')
