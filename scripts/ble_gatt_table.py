#!/usr/bin/env python3
"""Decode the stock Rapt Pill's NimBLE GATT schema out of the app image.

The schema is const data, so this is a read of the binary, not of a device.
Layouts, from the IDF NimBLE headers plus what the image actually contains:

  ble_uuid16_t   { u8 type=0x10; u8 value[2] (LE) }                     3 bytes
  ble_uuid128_t  { u8 type=0x80; u8 value[16] (LE) }                    17 bytes, packed
  ble_gatt_svc_def  { u8 type(1=primary,2=secondary,0=end); pad;
                      ble_uuid_t *uuid; svc_def **includes;
                      ble_gatt_chr_def *characteristics }               16 bytes
  ble_gatt_chr_def  { ble_uuid_t *uuid; access_cb *; void *arg;
                      ble_gatt_dsc_def *descriptors; u32 flags;
                      u16 *val_handle; u8 min_key_size }                28 bytes

Two things cost me a whole pass and are worth writing down:

  * The characteristic arrays live in **.dram0.data** (`val_handle` is written
    at registration, so the compiler put the whole array in writable memory).
    A DROM-only carve finds the service table and *nothing* else, which reads
    like "10 groups of 2 mystery UUIDs". They are not characteristics.
  * NimBLE has **no `val_max_size`**: registration imposes no write-length
    bound, so the bound is always whatever the access callback checks.

Access gate: READ_ENC/WRITE_ENC make NimBLE refuse
the op on a plaintext link; min_key_size adds a key-size floor; on top of that
the app refuses control operations before owner authentication.

  scripts/ble_gatt_table.py [bin] [--json]
"""
import json
import os
import struct
import sys

BIN = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-")
                         else "rapt-pill-2.0.0.bin")
data = open(BIN, "rb").read()

# segments from parse_esp.py: (vaddr, file offset, length, name)
SEG = [(0x3F400020, 0x20, 280808, "DROM"),
       (0x3FFBDB60, 0x46568, 22464, "DRAM"),
       (0x400D0020, 0x50020, 902456, "TEXT")]
SEG[0] = SEG[0]


def seg(v):
    for lo, o, ln, n in SEG:
        if lo <= v < lo + ln:
            return (v - lo + o, n)
    return (None, None)


def at(v, n):
    o, _ = seg(v)
    return data[o:o + n] if o is not None else b""


def w(v):
    b = at(v, 4)
    return struct.unpack("<I", b)[0] if len(b) == 4 else 0


def txt(v, n=48):
    b = at(v, n).split(b"\0")[0]
    return b.decode("latin1", "replace") if b and all(c >= 0x20 for c in b) else ""


def uuid(v):
    """Decode a ble_uuid_t; return a display string, or '' if v isn't one."""
    if not v:
        return "NULL"
    t = at(v, 1)
    if not t:
        return ""
    t = t[0]
    if t == 0x10:
        # 16-bit UUIDs need manual confirmation: 0x10 is a common data byte, so
        # type-byte matching alone produces plausible-looking SIG UUIDs.
        b = at(v + 1, 2)
        return f"{b[1]:02x}{b[0]:02x}?"
    if t == 0x80:
        u = at(v + 1, 16)[::-1].hex()
        return f"{u[0:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:32]}"
    return ""


FLAGS = [(0x0001, "BROADCAST"), (0x0002, "READ"), (0x0004, "WRITE_NO_RSP"),
         (0x0008, "WRITE"), (0x0010, "NOTIFY"), (0x0020, "INDICATE"),
         (0x0040, "SIGN_WRITE"), (0x0080, "RELIABLE_WRITE"), (0x0100, "AUX_WRITE"),
         (0x0200, "READ_ENC"), (0x0400, "READ_AUTHEN"), (0x0800, "READ_AUTHOR"),
         (0x1000, "WRITE_ENC"), (0x2000, "WRITE_AUTHEN"), (0x4000, "WRITE_AUTHOR")]
WRITE_BITS = 0x0004 | 0x0008 | 0x0040
ENC_BITS = 0x0040 | 0x0200 | 0x0400 | 0x0800 | 0x1000 | 0x2000 | 0x4000


def names(f):
    return "|".join(n for b, n in FLAGS if f & b)


def services():
    """Every 16-byte svc_def whose type is primary/secondary, whose uuid field
    decodes, and whose characteristics field points into DRAM (the writable
    chr arrays)."""
    dlo, dhi = 0x3FFBDB60, 0x3FFBDB60 + 22464
    out = []
    for v in range(0x3F400020, 0x3F400020 + 280808 - 16, 4):
        t = data[v - 0x3F400000]
        if t not in (1, 2) or not uuid(w(v + 4)):
            continue
        c = w(v + 12)
        uu = uuid(w(v + 4))
        if not (dlo <= c < dhi) or not uu or uu.endswith("?"):
            continue
        out.append((v, t, uu, c))
    return out


def chars(c):
    out = []
    for _ in range(32):
        u, cb, arg, dsc, flags, val_handle = struct.unpack("<6I", at(c, 24))
        if u == 0:                       # .uuid = NULL ends the array
            break
        if not uuid(u):
            break
        ksz = at(c + 24, 1)[0]
        out.append({"addr": c, "uuid": uuid(u), "access_cb": cb, "arg": arg,
                    "descriptors": dsc, "flags": flags, "names": names(flags),
                    "val_handle": val_handle, "min_key_size": ksz})
        c += 28
    return out


rows = []
for v, t, u, c in services():
    ch = chars(c)
    rows.append({"svc": hex(v), "type": t, "uuid": u, "chr_table": hex(c),
                 "characteristics": ch})

if "--json" in sys.argv:
    print(json.dumps(rows, indent=1))
    sys.exit(0)

print(f"{BIN}")
n = 0
for r in rows:
    print(f"\nSERVICE {r['uuid']}  @{r['svc']}  chr table {r['chr_table']} (DRAM)")
    for c in r["characteristics"]:
        n += 1
        wr = c["flags"] & WRITE_BITS
        gate = ("no write" if not wr
                else "ENCRYPTED" if c["flags"] & ENC_BITS else "*** PLAINTEXT WRITE ***")
        print(f"  {c['uuid']}  flags={c['flags']:#07x} ksz={c['min_key_size']:2d} "
              f"{gate:<18} {c['names']}  cb={c['access_cb']:#x}")
print(f"\n{len(rows)} services, {n} characteristics; "
      f"plaintext writes: {sum(1 for r in rows for c in r['characteristics'] if (c['flags'] & WRITE_BITS) and not (c['flags'] & ENC_BITS))}")
