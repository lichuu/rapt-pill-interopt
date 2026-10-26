import struct, sys, hashlib
p = "rapt-pill-2.0.0.bin"
b = open(p,"rb").read()
print("size", len(b), "sha256", hashlib.sha256(b).hexdigest())
magic, segs, spi_mode, spi_speed, entry, wp, fsize, auto = struct.unpack("<BBBBIBBB", b[:11])
chip, minrev, maxrev, minrevf, maxrevf = struct.unpack("<HHHHH", b[12:22])
appdesc = struct.unpack("<I", b[24:28])[0]
CHIP={0:"ESP32",2:"ESP32-S2",3:"ESP32-S3(beta)",5:"ESP32-C3",9:"ESP32-S3",12:"ESP32-C2",13:"ESP32-C6",16:"ESP32-H2",17:"ESP32-P4"}
print(f"magic=0x{magic:x} segments={segs} spi_mode={spi_mode} spi_speed={spi_speed} entry=0x{entry:08x}")
print(f"wp={wp} flash_size_byte={fsize} autodetect={auto} chip={chip}({CHIP.get(chip,'?')}) minrev={minrev} maxrev={maxrev}")
print(f"app_desc_drom=0x{appdesc:08x}")
off=24
tot=0
for i in range(segs):
    addr, ln = struct.unpack("<II", b[off:off+8]); off+=8
    print(f"seg{i}: load=0x{addr:08x} len={ln} (0x{ln:x}) filedata@0x{off:x}")
    d=b[off:off+ln]; off+=ln; tot+=ln
    region = ("DROM" if 0x3f400000<=addr<0x3f800000 else "IRAM" if 0x40080000<=addr<0x400c0000 else
              "DRAM" if 0x3ffae000<=addr<0x40000000 else "RTC" if addr<0x3f400000 and addr>=0x3ff80000 else "?")
    print(f"     class={region} head={d[:16].hex()}")
print("segments end @0x%x  segbytes=%d  trailing=%d" % (off, tot, len(b)-off))
print("trailing 64:", b[off:off+64].hex())
print("tail 64:", b[-64:].hex())
