#!/usr/bin/env python3
"""Pulse EN on RTS and capture the pill's console.

    console.py [port] [seconds] [watch]      defaults /dev/ttyUSB1, 20 s
    "watch" reads without touching EN, so the pill keeps running

GPIO0 must be free, or the chip comes back into the bootloader and prints
nothing. Stock 2.0.0 keeps its console off UART0, so this is a CFW thing.
"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
watch = len(sys.argv) > 3 and sys.argv[3] == "watch"

p = serial.Serial(port, 115200, timeout=0.1)
p.dtr = False
p.rts = False           # opening the port asserts RTS; release it or EN stays low
if not watch:
    p.rts = True        # EN low: hold in reset
    time.sleep(0.2)
p.rts = False           # EN high: run
t0 = time.time()
while time.time() - t0 < secs:
    b = p.read(256)
    if b:
        sys.stdout.write(b.decode("utf-8", "replace"))
        sys.stdout.flush()
p.close()
