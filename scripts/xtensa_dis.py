#!/usr/bin/env python3
"""Disassemble a slice of the stock app image with the ESP toolchain's objdump
and annotate every Xtensa literal pool load with the value it fetches.

Why not Ghidra: the 400d0020-400d3xxx region is never disassembled by the saved
project (entry-point-driven discovery never reaches it), and Ghidra 12.1.2's Java
script provider stopped building script bundles on this host ("Failed to get OSGi
bundle containing script"), so -postScript no longer runs. objdump is already
installed, is the authoritative Xtensa disassembler, and needs no project:

  xtensa-esp32-elf-objdump -D -b binary --adjust-vma=<addr>

Annotated output turns a bare `l32r a2, 0x400d262c` into the thing the code
actually loaded, which is how you read a call's argument out of a listing:

  l32r a2, 0x400d262c   [0x400d262c] = 0x3f4382f4  "...."

  scripts/xtensa_dis.py <start_hex> <end_hex> [bin]
"""
import os
import re
import struct
import subprocess
import sys

BIN = "rapt-pill-2.0.0.bin"
if len(sys.argv) > 3:
    BIN = os.path.expanduser(sys.argv[3])
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
OBJDUMP = os.path.expanduser(
    "~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/"
    "xtensa-esp32-elf-objdump")

data = open(BIN, "rb").read()
# seg4 flash text: 0x400d0020 @ file 0x50020  -> off = vaddr - 0x400d0020 + 0x50020
TEXT = (0x400D0020, 0x50020, 902456)
# seg0 DROM: 0x3f400020 @ 0x20 -> off = vaddr - 0x3f400000
SEG = [(0x3F400020, 0x20, 280808), (0x3FFBDB60, 0x46568, 22464), TEXT]


def off(v):
    for lo, o, ln in SEG:
        if lo <= v < lo + ln:
            return v - lo + o


def word(v):
    o = off(v)
    return struct.unpack("<I", data[o:o + 4])[0] if o is not None and o + 4 <= len(data) else None


def string(v, n=52):
    o = off(v)
    if o is None:
        return ""
    b = data[o:o + n].split(b"\0")[0]
    return b.decode("latin1", "replace") if sum(c >= 0x20 for c in b) > len(b) // 2 else ""


# Xtensa is variable length: objdump from a mid-instruction address produces
# `ill` garbage for a while. Back up to the nearest `entry a1, N` prologue
# (encoding 0x36 0x41 0xNN) so the listing is aligned.
o0 = off(start)
for cand in range(o0, max(o0 - 0x400, 0), -1):
    if data[cand] == 0x36 and data[cand + 1] == 0x41:
        start = start - (o0 - cand)
        break
# Say so: the caller asked for a window and got one that starts earlier.
sys.stderr.write(f"# listing starts at {start:#x}\n")
blob = data[off(start):off(end)]
open("/tmp/xtensa_slice.bin", "wb").write(blob)
listing = subprocess.run(
    # `-b binary` needs an architecture and this objdump's only machine name is
    # plain "xtensa" (there is no "xtensa:esp32"; `-m xtensa:esp32` fails).
    [OBJDUMP, "-D", "-b", "binary", "--architecture", "xtensa",
     f"--adjust-vma={start:#x}", "/tmp/xtensa_slice.bin"],
    capture_output=True, text=True).stdout

lit = re.compile(r"0x([0-9a-f]{5,8})\b")
for line in listing.splitlines():
    if not re.match(r"^\s*[0-9a-f]+:", line):
        continue
    addr, _, rest = line.partition(":")
    notes = []
    for m in lit.finditer(rest):
        v = int(m.group(1), 16)
        o = off(v)
        if o is None:
            continue
        w = word(v)
        if w is None:
            continue
        # literal pool slot: report the value and, if it points at text/rodata,
        # what lives there
        s = string(w) if 0x3F400020 <= w < 0x3F400020 + 280808 else ""
        notes.append(f"[{v:#x}]={w:#x}" + (f' "{s}"' if s else ""))
    print(f"{addr}: {rest.strip()}" + ("   " + "; ".join(notes) if notes else ""))
