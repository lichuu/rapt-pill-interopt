#!/usr/bin/env python3
"""Decompile parts of the Rapt Pill image with Ghidra, driven by PyGhidra.

Why this exists: Ghidra's custom-script provider on this host refuses to build
script bundles ("Failed to get OSGi bundle containing script"), so
`analyzeHeadless -postScript` is out — and the saved project's auto-analysis
never reaches the BLE code (no entry point leads there, so the listing is empty
and a script that iterates instructions finds nothing). PyGhidra drives the same
decompiler through JEP, with no script system involved.

The program is opened from the existing project, and never saved: disassembly
and function creation happen in memory only.

  ~/.venv/bin/python scripts/pyghidra_pseudo.py [--range 0xSTART,0xEND]
      [--addrs 0xA,0xB] [--out out2/pseudo] [--program NAME] [--dry]
"""
import os
import re
import sys

os.environ.setdefault("GHIDRA_INSTALL_DIR", "/opt/ghidra")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

import pyghidra
from pyghidra.api import open_project, program_context, transaction, task_monitor


def arg(key, dflt=None):
    i = sys.argv.index(key) + 1 if key in sys.argv else 0
    return sys.argv[i] if i and i < len(sys.argv) else dflt


def flag(key):
    return key in sys.argv


# Ghidra's default function name (FUN_xxxxxxxx) and this script's own entries.
CALL_RE = re.compile(r"(?:FUN_|entry_0x)([0-9a-f]{6,8})")


rng = arg("--range")
addrs = [a for a in (arg("--addrs", "") or "").split(",") if a.strip()]
out = os.path.expanduser(arg("--out", os.path.join(ROOT, "out2", "pseudo")))


def work(prog):
    # Imported after pyghidra.start(): these come off Ghidra's module loader, so
    # they only resolve once the Ghidra application is up. (The class is
    # DisassembleCommand — a `DisassembleCmd` guess costs a lot of detours.)
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from jpype import JArray, JByte

    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.model.address import AddressSet

    if flag("--dry"):
        print("program:", prog.getName(), "| lang:", prog.getLanguage().getID(),
              "| functions:", prog.getFunctionManager().getFunctionCount(), flush=True)
        print("  class ok:", DisassembleCommand.class_.getName(),
              DecompInterface.class_.getName(), flush=True)
        return
    os.makedirs(out, exist_ok=True)
    monitor = task_monitor()
    spc = prog.getAddressFactory().getDefaultAddressSpace()
    listing, fm = prog.getListing(), prog.getFunctionManager()
    mem = prog.getMemory()
    text = [(b.getStart().getOffset(), b.getEnd().getOffset())
            for b in prog.getMemory().getBlocks() if b.isExecute()]

    def in_text(v):
        return any(lo <= v <= hi for lo, hi in text)

    blocks = [(b, b.getStart().getOffset(), b.getEnd().getOffset())
              for b in prog.getMemory().getBlocks() if b.isExecute()]

    def text_literal_slots():
        for _blk, lo, hi in blocks:
            buf = JArray(JByte)(hi - lo)
            # MemoryBlock has no getByteRange in this API; Memory.getBytes does.
            mem.getBytes(spc.getAddress(lo), buf)
            for i in range(0, len(buf) - 3, 4):
                yield (lo + i, buf[i] & 0xFF | (buf[i+1] & 0xFF) << 8
                       | (buf[i+2] & 0xFF) << 16 | (buf[i+3] & 0xFF) << 24)

    aset = None
    if rng:
        a, b = [int(x, 16) for x in rng.replace("0x", "").split(",")]
        aset = AddressSet(spc.getAddress(a), spc.getAddress(b))
        with transaction(prog, "disassemble " + rng):
            cmd = DisassembleCommand(aset, None, False)
            cmd.applyTo(prog, monitor)
        n = sum(1 for _ in listing.getInstructions(aset, True))
        print(f"disassembled {rng}: {n} instructions", flush=True)

    d, seen, queue = DecompInterface(), set(), []
    d.openProgram(prog)
    cap = int(arg("--cap", "80"))

    def one(v):
        """Create the function at v if Ghidra has none, decompile, return the body."""
        key = f"{v:x}"
        if key in seen:
            return ""
        seen.add(key)
        ad = spc.getAddress(v)
        if fm.getFunctionAt(ad) is None and fm.getFunctionContaining(ad) is None:
            with transaction(prog, f"function {key}"):
                listing.disassemble(ad)
                CreateFunctionCmd(ad, f"entry_0x{key}").applyTo(prog, monitor)
        f = fm.getFunctionAt(ad) or fm.getFunctionContaining(ad)
        if f is None:
            print(f"FAIL {key}: no function", flush=True)
            return ""
        r = d.decompileFunction(f, 120, monitor)
        ok = r.decompileCompleted() and r.getDecompiledFunction() is not None
        body = (r.getDecompiledFunction().getC() if ok
                else f"/* FAILED: {r.getErrorMessage()} */\n")
        open(os.path.join(out, f"{key}_{f.getName()}.c".replace(":", "_")), "w").write(body)
        print(f"{'ok  ' if ok else 'FAIL'} {key} {f.getName()}", flush=True)
        return body if ok else ""

    def seeds_in_range():
        out_, rm = [], prog.getReferenceManager()
        for ins in listing.getInstructions(aset, True):
            for ref in rm.getReferencesFrom(ins.getMinAddress()):
                t = ref.getToAddress()
                if in_text(t.getOffset()):
                    out_.append(t.getOffset())
        return out_

    # Seeds: explicit addresses, plus call targets found in a --range. Then
    # follow calls outward — Ghidra's analysis never reached this code and
    # nothing outside it calls in, so there is no entry point to start from.
    # --globals: find the code that touches a DRAM global by its literal-pool
    # slot. In raw ESP32 text, an l32r loads the *slot*, so Ghidra's references
    # point at the slot (e.g. 0x400d2570), not at the DRAM field (0x3ffc5f58);
    # scan text for slots whose value lands in the range, then take references
    # to those slots and decompile the functions that contain them.
    globs = arg("--globals")
    if globs:
        lo, hi = [int(x, 16) for x in globs.replace("0x", "").split(",")]
        rm = prog.getReferenceManager()
        for v in text_literal_slots():
            if lo <= v[1] <= hi:
                for ref in rm.getReferencesTo(spc.getAddress(v[0])):
                    f = fm.getFunctionContaining(ref.getFromAddress())
                    if f is not None:
                        queue.append(f.getEntryPoint().getOffset())

    for a in addrs:
        queue.append(int(a, 16))
    if aset is not None:
        queue += seeds_in_range()
    while queue and len(seen) < cap:
        for c in CALL_RE.findall(one(queue.pop(0))):
            v = int(c, 16)
            if in_text(v) and f"{v:x}" not in seen:
                queue.append(v)
    print(f"{len(seen)} pseudocode files -> {out}", flush=True)


def main():
    pyghidra.start()
    if True:  # not a context manager in pyghidra 3.1
        proj = open_project(os.path.join(ROOT, "ghidra"), arg("--project", "rapt201"))
        from ghidra.framework.model import ProjectDataUtils
        paths = []
        ProjectDataUtils.descendantFiles(proj.projectData.getFolder("/")) \
            .forEach(lambda f: paths.append(f.getPathname()))
        print("project files:", paths, flush=True)
        path = next((p for p in paths if p.endswith(arg("--program", ".elf"))), paths[0])
        with program_context(proj, path) as prog:
            work(prog)


main()
