import struct, sys
src, dst = sys.argv[1], sys.argv[2]
b = open(src,'rb').read()
segs, entry, off = b[1], struct.unpack_from("<I", b, 4)[0], 24
S = []
for _ in range(segs):
    addr, ln = struct.unpack_from("<II", b, off); off += 8
    S.append((addr, ln, off)); off += ln
E, PH = 52, 32
base = (E + PH*len(S) + 15)//16*16
out = bytearray(b"\x7fELF\x01\x01\x01\x00" + b"\0"*8)
out += struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, entry, base, 0, 0, 52, 32, len(S), 32, 0, 0)
out += b"\0"*(base - len(out))
def flags(addr):
    """ESP32 memory map. Ghidra derives per-block read/write/exec from p_flags,
    and its pointer-table analysis only sweeps blocks marked non-executable, so
    getting these right is what recovers the handler/task-entry tables."""
    if 0x40000000 <= addr < 0x40100000:
        return 7 if 0x40070000 <= addr < 0x400D0000 else 5   # IRAM RWX, flash text RX
    if 0x3F400000 <= addr < 0x3F800000:
        return 4                                             # DROM: read-only data
    return 6                                                 # DRAM / RTC fast: read-write

blob, phs = bytearray(), b""
for addr, ln, f in S:
    phs += struct.pack("<IIIIIIII", 1, base+len(blob), addr, addr, ln, ln, flags(addr), 1)
    blob += b[f:f+ln]
open(dst,'wb').write(bytes(out+phs+blob))
print(f"{dst}: EM_XTENSA(94) entry=0x{entry:08x} phnum={len(S)}")
for a,l,f in S: print(f"  PT_LOAD 0x{a:08x} +0x{l:x} flags=0x{flags(a):X} (file 0x{f:x})")
