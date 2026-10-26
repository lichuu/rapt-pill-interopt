#!/usr/bin/env python3
"""Parity check: our encoder vs the community parser's struct format.

The format string is read out of the vendored rapt-ble parser (MIT) rather than
imported, so this runs with no Home Assistant dependencies.
"""
import re, struct
from pathlib import Path

here = Path(__file__).parent
src = (here / "rapt_ble_parser_reference.py").read_text()
fmt = re.search(r'unpack\("(>Bx\?fHfhhhh)"', src).group(1)
assert re.search(r'len\(data\) != 23', src), "vendored parser no longer expects 23 bytes"


def build(gravity, gv, temp_c, ax, ay, az, volts, gv_valid=True):
    """Mirror of pill_adv_build() in main/pill_wire.h."""
    return (b"PT" + struct.pack(">BB", 2, 0) + bytes([1 if gv_valid else 0])
            + struct.pack(">fHfhhhh", gv, round((temp_c + 273.15) * 128), gravity * 1000.0,
                          round(ax * 16), round(ay * 16), round(az * 16), round(volts * 256)))


body = build(1.048, -3.5, 21.5, 0.12, -0.03, 0.99, 3.72)
assert len(body) == 23, len(body)
v, gv_valid, gv, t, g, x, y, z, b = struct.unpack(fmt, body[2:])
assert v == 2 and gv_valid
assert abs(g / 1000.0 - 1.048) < 1e-4, g
assert abs(t / 128.0 - 273.15 - 21.5) < 0.01, t
assert abs(gv + 3.5) < 1e-4
assert abs(x / 16.0 - 0.12) <= 1 / 32 and abs(z / 16.0 - 0.99) <= 1 / 32  # 1/16 g LSB
assert abs(b / 256.0 - 3.72) <= 1 / 512  # 1/256 V LSB
print("wire format OK:", body.hex())
print(f"  decoded: gravity={g/1000:.4f} temp={t/128.0-273.15:.2f}C "
      f"gv={gv:.2f}/day x/y/z={x/16:.2f}/{y/16:.2f}/{z/16:.2f}g volts={b/256:.2f}")
