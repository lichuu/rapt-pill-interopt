# Writing our own firmware for the Rapt Pill

## Decide the target first

| option | what you ship | work | what you keep |
|---|---|---|---|
| **A. No reflash** | expression + local HTTP polling | ~0 | Rapt cloud, warranty, OTA |
| **B. Fork-compatible fw** | your ESP-IDF app that keeps the Rapt wire formats | medium | Rapt app + Home Assistant keep working |
| **C. Fully local fw** | your own BLE/MQTT, no AWS | medium | nothing from Rapt cloud |

`~/projects/fermentdial` and `~/projects/ha` suggest **B** is the useful one:
keep the BLE advertisement format and the local HTTP API, drop AWS. That is a
strict subset of what the RE has already recovered, so B is the plan below; C is
B minus §2 of the recovered protocol.

Option A is not a toy: the shipped firmware already compiles a user expression
for gravity (`angle`, `pow`, `sqrt`, `log10`, …, ≤255 chars) and serves
`/sensors.json` on the LAN. A cron on your HA box polling `/sensors.json` gets
data today.

## Step 1 — dump the whole flash (do this before touching anything)

The file we have is *only* the app image. Everything that makes a custom image
bootable and everything that stores your device's identity is elsewhere.

Wire the PCB's serial header (the teardown shows it unsoldered; 3.3 V USB-UART,
GPIO0 low for ROM download) then:

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 flash_id
esptool --chip esp32 read_flash 0x0 0x400000 ~/projects/rapt-pill-re/extract/full-4MB.bin
espefuse --chip esp32 --port /dev/ttyUSB0 summary   # secure boot / encryption bits
```

What the dump answers, with no disassembly at all:

- partition table at `0x8000` → `nvs`, `otadata`, `phy_init`, `ota_0`, `ota_1`
  offsets and sizes (2021 fw booted an app at `0x190000`);
- NVS contents → namespace names, your saved Wi-Fi credentials, the AWS thing
  name/cert, calibration values, settings keys, all typed;
- `otadata` → which slot is live, so you know what to write;
- eFuses → whether flash encryption/secure boot are on. If `FLASH_CRYPT_CNT`
  is set, a hand-written image must be encrypted the same way, which changes
  the whole plan (then `espsecure` + a matching key, which you do not have —
  in that case the practical path is an OTA update, not a raw flash write).

Keep `full-4MB.bin` as the restore image.

## Step 2 — pin/driver confirmation in Ghidra

The teardown gives SDA 25 / SCL 26, LIS3DH @ `0x19`, TMP1075 @ `0x48`, LED 18,
output 5, button 23, ADC 34 (2021 board, Azure-era firmware). Confirm on the
2.0.0 image — rev ≥2.1 boards are known to differ.

```bash
cd ~/projects/rapt-pill-re
# 1. string -> function map (literal-pool chaining)
python3 scripts/string_map.py 'LIS3DH|accel|tmp1075|i2c' -n 25
# 2. edit notes/targets.txt with the addresses it prints, then:
RAPT_OUT=$PWD/out RAPT_TARGETS=$PWD/notes/targets.txt \
  /opt/ghidra/support/analyzeHeadless $PWD/ghidra rapt200 \
  -process rapt-pill-2.0.0.elf -noanalysis -scriptPath $PWD/scripts \
  -postScript AnnotateDecompile.java
```

Look for the integer arguments to `i2c_param_config(port, sda, scl, clk, cfg)`
and `i2c_master_write_byte(dev<<1, …)` in the drivers, and the
`adc1_config_channel_atten` / GPIO calls for battery and LED.
`notes/names.tsv` (`addr<TAB>name`) renames functions in the saved project so
later pseudocode reads as `accel_init_peripherals` instead of `FUN_400df2b4`.

## Step 3 — the protocol to reproduce (in priority order)

1. **BLE advertisement, 23 bytes, big-endian** — v2 layout, byte-for-byte in
   `firmware/tests/test_wire.py` and `rapt_ble_parser_reference.py`. Company id
   `0x4152`, prefix `"RAPT"`. Emit v2 and
   `rapt-ble` / Home Assistant / SPINDELMATE all keep working.
2. **Local HTTP API** — the endpoint set you recover with `scripts/http_table.py`
   (captured samples in `extract/api/`), with the
   `X-Custom-*` header convention, and serve 4 pages of your own that keep the
   same form fields, so the existing phone flow is unchanged. Their pages are
   Rapt's UI: carve them from your own image for reference, do not ship them.
3. **NVS keys** — reuse the same namespace/keys so a settings backup from the
   original firmware restores into yours (read them from the flash dump).
4. **Wi-Fi provisioning** — the original uses `esp32-wifi-manager` (captive
   portal, AP password string `kegland1` in the older fw). Same component, no
   work.
5. **AWS shadow** — only for option "keep the cloud": `$aws/things/<thing>/shadow/…`
   with `state.reported` / `state.desired` + `$aws/things/<thing>/shadow/get/accepted`,
   broker `a2l0zcy7vb2970-ats.iot.ap-southeast-2.amazonaws.com:8883`, mutual TLS
   with the cert/key already in NVS. If you keep `aws_cert`/`aws_key` in NVS and
   speak shadows, the Rapt app keeps working with your firmware. The cloud may
   refuse unknown `firmwareVersion` values (`/v1/devices/firmwareVersionCheck-wifi`),
   which only affects OTA.

## Step 4 — build environment

```bash
git clone --depth 1 --branch v5.4.1 https://github.com/espressif/esp-idf && cd esp-idf && ./install.sh esp32
. export.sh
```

v5.4.1 is exactly what 2.0.0 was built with (`esp_app_desc`), so NVS/OTA/partition
formats and the Wi-Fi driver blobs match. Project skeleton to write:

```
main/idf_component.yml      esp-wifi-manager (tgx/esp32-wifi-manager), NimBLE is in IDF
main/app_main.c             board pins, sensor init, sampling task, deep sleep
main/accel_lis3dh.c         I2C @0x19, WHO_AM_I 0x33, ODR + range, axis inversion
main/temp_tmp1075.c         I2C @0x48, 12-bit, °C = reg>>4
main/gravity_expr.c         start with a fixed linear fit, then the expression VM
main/ble_advert.c           23-byte v2 advertisement, RAPT-Pill-%02X%02X%02X
main/http_api.c             esp_http_server, the endpoint list above
main/settings_nvs.c         same namespace/keys as stock
partitions.csv              copied from the flash dump
sdkconfig                   single core, 160 MHz, brownout on, deep-sleep wake sources
```

Battery: the stock firmware charges-calibrates a coulomb counter in software
("Charge the battery fully to calibrate the charge measurement system", `adc`
on GPIO34). Copy the shape of that: voltage→% LUT plus a full-charge anchor.

Deep sleep is the reason this device runs for months: sample, publish,
`esp_deep_sleep_start()`; keep a RTC-fast struct (`0x50000000` segment exists in
the image) for the gravity-velocity window; wake sources = timer + button GPIO.

## Step 5 — flash it without bricking

```bash
# slot-based, keeps the stock app in the other slot
esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x<ota_x_offset> build/my-app.bin
python3 - <<'EOF'   # flip otadata to that slot
import struct; open('otadata.bin','wb').write(struct.pack('<II',0x6F36050A,0)*2)
EOF
```

Safer: serve the image through the stock OTA path (`/ota_now.json` →
`download-automation`, or the `firmwareVersionCheck` endpoint if you stand up a
mock API Gateway) — the bootloader validates the image and rolls back, and you
never touch the partition table, `nvs`, `otadata`, or the bootloader.
Keep `full-4MB.bin` for the factory restore.

## Step 6 — features worth adding on top

- LAN-only MQTT (`rapt/-pill/state`) into `~/projects/ha`, plus the stock BLE
  advert so nothing regresses.
- Ferment-dial style alarm bands (`gravity_velocity` threshold → HA service call)
  — the pill already computes `gravity_velocity` and its age.
- Tap/shake event stream (`gesture_log`, `click_events`) as a generic wireless
  button.
- A `/telem.json?n=…` endpoint exposing the raw sample ring the diagnostics page
  already draws from.
- Keep the expression: it is the cheapest user-facing extensibility point and it
  is already what the "custom calibration" page drives.

## Standing caveats

- Flashing voids Rapt support and replaces their OTA path; keep the stock image.
- Nothing here is a licence opinion: check Rapt's terms for your region before
  redistributing a derivative of their web assets (the HTML/JS carved here is
  their copyrighted UI; use it for interop study, write your own pages).
- The RE evidence is static. None of it was observed
  running; serial console + a BLE sniffer are the cheap way to confirm.
