#!/usr/bin/env python3
"""Whole-image annotated Xtensa listing, per function.

Why: disassembling the text segment as one sequential stream derails at the first
literal island — Xtensa is variable length, so once objdump reads a data word as
an instruction it never resynchronises. The full-stream pass produced 466k lines in
which only 3 l32r loads into the BLE lock struct appeared, while the same loads are
obvious when the slice starts at a real `entry` prologue.

So: ask Ghidra for the function boundaries, objdump each function's range, annotate
every literal-pool load with the value it fetches and the string at that target
(same annotation as scripts/xtensa_dis.py).

  ~/.venv/bin/python scripts/text_listing.py [--bin PATH]
        [--project ghidra] [--name rapt201] [--file /NAME.elf] [--seeds s,e;s,e]

Ghidra's own literal-slot resolution is unreliable in this image: that is why the listing comes from objdump. Only the *boundaries* come from
Ghidra, and functions auto-analysis never discovered are absent — extend --seeds
with `start,end` pairs for those.
"""
import argparse
import os
import re
import struct
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("--bin", default=os.path.expanduser("rapt-pill-2.0.0.bin"))
ap.add_argument("--project", default="ghidra", help="directory that holds the project")
ap.add_argument("--name", default="rapt201", help="project name inside that directory")
ap.add_argument("--file", default="/rapt-pill-2.0.0-f.elf", help="path inside the project")
ap.add_argument("--out", default="out2/text_dis.txt")
ap.add_argument("--seeds", default="", help="extra ranges as start,end;start,end")
a = ap.parse_args()

SEG = [(0x3F400020, 0x20, 280808), (0x3FFBDB60, 0x46568, 22464),
       (0x400D0020, 0x50020, 902456)]
DATA = open(os.path.expanduser(a.bin), "rb").read()


def off(v):
    for lo, o, ln in SEG:
        if lo <= v < lo + ln:
            return v - lo + o


def word(v):
    o = off(v)
    return struct.unpack_from("<I", DATA, o)[0] if o is not None else None


def string(v, n=52):
    o = off(v)
    if o is None:
        return ""
    b = DATA[o:o + n].split(b"\0")[0]
    return b.decode("latin1", "replace") if sum(c >= 0x20 for c in b) > len(b) // 2 else ""


OBJDUMP = os.path.expanduser(
    "~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/"
    "xtensa-esp32-elf-objdump")
LIT = re.compile(r"0x([0-9a-f]{5,8})\b")


def dump(start, end, out):
    o0, o1 = off(start), off(end)
    if o0 is None or o1 is None or o1 <= o0:
        return
    open("/tmp/fn_slice.bin", "wb").write(DATA[o0:o1])
    listing = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "--architecture", "xtensa",
         f"--adjust-vma={start:#x}", "/tmp/fn_slice.bin"],
        capture_output=True, text=True).stdout
    for line in listing.splitlines():
        if not re.match(r"^\s*[0-9a-f]+:", line):
            continue
        addr, _, rest = line.partition(":")
        notes = []
        for m in LIT.finditer(rest):
            v = int(m.group(1), 16)
            w = word(v)
            if w is None:
                continue
            s = string(w) if 0x3F400020 <= w < 0x3F400020 + 280808 else ""
            notes.append(f"[{v:#x}]={w:#x}" + (f' "{s}"' if s else ""))
        out.write(f"{addr}:{rest}\t" + "  ".join(dict.fromkeys(notes)) + "\n")


import pyghidra  # noqa: E402
from pyghidra import api  # noqa: E402

pyghidra.start()
proj_dir = a.project if os.path.isabs(a.project) else os.path.abspath(a.project)
with api.open_project(proj_dir, a.name) as pp:
    with api.program_context(pp, a.file) as prog:
        fnm = prog.getFunctionManager()
        ranges = [tuple(int(x, 16) for x in pair.split(","))
                  for pair in a.seeds.split(";") if pair.strip()]
        with open("/tmp/fns.txt", "w") as f:
            for fn in fnm.getFunctions(True):
                body = fn.getBody()
                lo, hi = body.getMinAddress(), body.getMaxAddress()
                if lo is None or hi is None:
                    continue
                f.write(f"{lo.getOffset():x},{hi.getOffset():x}\n")
        ranges += [tuple(int(x, 16) for x in line.split(","))
                   for line in open("/tmp/fns.txt")]

with open(a.out, "w") as out:
    for s, e in sorted(set(ranges)):
        dump(s, e + 4, out)
print(f"{a.out}: {sum(1 for _ in open(a.out))} lines, {len(set(ranges))} functions")
