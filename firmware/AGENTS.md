# Agent Notes

ESP-IDF firmware for the Rapt Pill (ESP32-S0WD, classic ESP32, Xtensa, 4 MB
flash). ESP-IDF **v5.4.1** at `~/projects/esp-idf` — the stock 2.0.0 image
reports exactly that, so NVS, OTA and partition formats match as shipped. Do not
bump the target IDF version casually: partition and NVS compatibility is the
whole flashing story.

## Build And Verify

`export.sh` picks a Python venv by version, and the system Python 3.14 is not
one it installed, so put 3.11 first on PATH in every shell:

```sh
export PATH="$HOME/.local/share/mise/installs/python/3.11/bin:$PATH"
. ~/projects/esp-idf/export.sh
cd ~/projects/rapt-pill-re/firmware
idf.py build                       # -> build/pill_fw.bin
```

`idf.py … | tail` masks the exit status — judge a build by
`build/pill_fw.bin` and by grepping the log:

```sh
grep -aoE '[a-z_]+\.(c|h):[0-9]+:[0-9]+: error: .*' /tmp/pillbuild.log | sort -u
```

Host tests, no hardware needed:

```sh
cc -I main -o /tmp/tg tests/test_gravity.c -lm && /tmp/tg
python3 tests/test_wire.py
```

## Compatibility Contract

These two are the reason the CFW exists. Nothing may change them:

1. **The BLE advertisement** (`main/pill_wire.h`): non-connectable, company id
   `0x4152`, 23-byte body — `"RAPT"`, version 2, reserved, gravity-velocity
   valid, `f32` gravity velocity, `u16` temperature `K*128`, `f32` gravity
   `*1000`, `i16` x/y/z `g*16`, `i16` battery `V*256`, all big-endian.
   `tests/rapt_ble_parser_reference.py` is the vendored `sairon/rapt-ble`
   parser; the wire test asserts against it. Home Assistant, raptpill2mqtt,
   SPINDELMATE and FermentDial's `decodeRapt` all read these bytes.
2. **Stock JSON endpoints**: `/sensors.json`, `/sensors_cal.json`,
   `/static_info.json` keep stock field names, units and the stock "no data"
   sentinels (`adc: -112`, `charge: -1`). New surfaces go on new paths
   (`/api/status`, `/metrics`), never into the stock payloads.

The gravity model is the stock quintic in `main/gravity.h`, clamped to 10-75°,
plus the device's `+angle_offset`; `tilt = atan2(y, x) + offset`. Tests assert
against a live capture, so they encode the real device, not an idea.

## Flashing

Only ever write the app, to the slot that is not running:

```sh
./flash.sh install     # 4 MB backup first, app to ota_1 @ 0x190000, otadata -> ota_1
./flash.sh rollback    # boot stock ota_0
./flash.sh restore     # write the saved 4 MB dump back
```

Never write the bootloader, partition table, otadata by hand (use
`otatool.py`), NVS, or eFuses. The pill is powered from USB-C, never from the
adapter's VCC.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` means an OTA-installed build stays
`PENDING_VERIFY` until `pill_ota_confirm_boot()` runs, so a build that dies
early reverts. Keep that call where the sensors, advertisement and HTTP server
are already up.

## Conventions

- Web pages are C string literals in `main/web_theme.h` / `main/pill_net.c`.
  HTML attributes use single quotes, the CSS uses none: C has no raw strings.
- The theme tokens mirror FermentDial's `webui/src/shared/theme.css` (teal
  `--bg #0d1b1e`, `--card #132428`, `--accent #b0d8f8`, hero + card grid +
  status pills). Change the tokens in both repos or neither, so the pill and the
  Dial keep looking like siblings.
- Settings are additive NVS reads by name with a default, no version stamp and
  no migration code, and never reinterpret an existing key — notably
  `nvs.net80211` (`ssid`, `password`) belongs to `esp_wifi` and the Rapt app
  writes it.
- `check git status --short` in reviews; new source files sit untracked and
  invisible to `git diff`.
- Comments state what the reverse-engineered firmware does, with the address
  (`accel_init_lis3dh @ 0x400e0520`) so the claim is checkable in
  `out2/functions.tsv`.

## Layout

`../extract/` holds the 4 MB flash dump, NVS slice, live API captures and
extracted web assets. `../out2/` is the Ghidra export of the 2.0.0 image with
correct per-segment flags. `../notes/` has the findings and the protocol notes.
Analysis is Ghidra 12.1.2 (Xtensa SLEIGH); REA cannot admit Xtensa ELF.
