#!/usr/bin/env bash
# One-command CFW install for the Rapt Pill. Stock firmware stays in ota_0.
#
#   ./flash.sh install  [port]   back up flash, write app to ota_1
#   ./flash.sh boot     [port]   pulse EN: run the slot otadata points at
#   ./flash.sh rollback [port]   boot stock ota_0 again
#   ./flash.sh status   [port]   show otadata and the partition table
#   ./flash.sh restore  [port]   write the saved 4 MB dump back (last resort)
#
# Wiring, once: adapter RXD->E_TXD, TXD->E_RXD, GND->GND, RTS->EN.
#
# Everything that talks to the bootloader (install, status, rollback, restore)
# needs GPIO0 — the strap pin — held low: clip a jumper from it to GND. RTS->EN
# is what puts the chip into the bootloader, and with IO0 held low it stays
# there, so one clip covers the whole install; --after no-reset keeps esptool
# from rebooting into the bootloader over and over.
#
# Then pull the clip and ./flash.sh boot, which pulses EN on RTS without talking
# to the bootloader — a plain reset, so the app slot runs. That is all DTR is
# ever for on a pill: releasing GPIO0 for you. Adapters that have it (DTR->GPIO0)
# skip the clip entirely and esptool enters download mode by itself.
#
# Do NOT wire VCC; power the pill from USB-C.
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)
CMD=${1:-}
if [ -z "$CMD" ]; then sed -n '2,20p' "$0"; exit 2; fi
PORT=${2:-${PORT:-/dev/ttyUSB1}}
BIN=$DIR/build/pill_fw.bin
DUMP=${DUMP:-$DIR/../extract/full-4MB.bin}
IDF=${IDF_PATH:-$HOME/projects/esp-idf}
# 921600 with esptool's compression: ~40 s for the app image. If the Dupont
# wires are dodgy you get "no sync reply"; drop to BAUD=115200.
BAUD=${BAUD:-921600}
# esptool's own interpreter — it is the one with pyserial.
PY=${PY:-$(head -1 "$(readlink -f "$(command -v esptool)")" | cut -c3-)}

otatool() {
    # otatool is an esptool wrapper: it needs esptool's interpreter (pyserial)
    # and IDF_PATH exported, because it shells out to
    # $IDF_PATH/components/esptool_py/esptool/esptool.py. It has no --after of
    # its own; that goes through --esptool-args if you ever need it.
    IDF_PATH=$IDF PYTHONPATH=$IDF/components/partition_table \
        "$PY" $IDF/components/app_update/otatool.py -p "$PORT" -b "$BAUD" "$@"
}

case "$CMD" in
  status)
    otatool read_otadata
    ;;

  install)
    [ -f "$BIN" ] || { echo "no $BIN — run: idf.py build"; exit 1; }
    if [ ! -f "$DUMP" ]; then
      echo "==> backing up the whole flash to $DUMP (about 3 minutes at this baud)"
      esptool --chip esp32 -p "$PORT" -b "$BAUD" read-flash "$DUMP" 0x0 0x400000
      sha256sum "$DUMP"
    fi
    echo "==> writing $BIN to ota_1 (0x190000); stock 2.0.0 stays in ota_0"
    echo "    (needs the bootloader: GPIO0 strapped to GND, pill powered)"
    esptool --chip esp32 -p "$PORT" -b "$BAUD" --connect-attempts 4 --after no-reset \
        write-flash 0x190000 "$BIN"
    echo "==> pointing the bootloader at ota_1"
    otatool switch_ota_partition --name ota_1
    echo "==> done. Pull the GPIO0 jumper, then:  ./flash.sh boot"
    ;;

  boot)
    # A plain EN pulse on RTS, no bootloader handshake: this is the step that
    # works with GPIO0 free, which is why it is not `esptool run`.
    "$PY" -c 'import sys,time,serial; p=serial.Serial(sys.argv[1],115200); p.dtr=False; p.rts=True; time.sleep(0.2); p.rts=False; p.close()' "$PORT"
    echo "==> EN pulsed; the app slot is booting (console on this port at 115200)"
    ;;

  rollback)
    echo "==> booting stock ota_0"
    otatool switch_ota_partition --name ota_0
    echo "==> pull the GPIO0 jumper, then:  ./flash.sh boot"
    ;;

  restore)
    [ -f "$DUMP" ] || { echo "no dump at $DUMP"; exit 1; }
    echo "==> writing the full dump back; this is the factory reset"
    esptool --chip esp32 -p "$PORT" -b "$BAUD" --after no-reset write-flash 0x0 "$DUMP"
    ;;

  *)
    sed -n '2,20p' "$0"; exit 2
    ;;
esac
