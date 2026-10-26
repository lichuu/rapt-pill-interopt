# Rapt Pill CFW

Replacement firmware for the Rapt Pill hydrometer (ESP32-S0WD, 4 MB flash). It
keeps the two interfaces people actually use — the BLE advertisement that Home
Assistant parses, and the LAN HTTP API — and drops the cloud: no AWS IoT, no
MQTT client, no device shadow, no registration, no phone-home. Updates come from
your own LAN instead, from a browser.

The web UI, endpoint shapes and agent notes follow the conventions in
[`../fermentdial`](https://github.com/lichuu/fermentdial), the M5Stack Dial
fermentation controller that already reads these pills over BLE — same theme
tokens, same `/api/status` + `/metrics` + `/firmware` upload layout.

## Build

```bash
export PATH="$HOME/.local/share/mise/installs/python/3.11/bin:$PATH"   # IDF 5.4 rejects 3.14
. ~/projects/esp-idf/export.sh
cd ~/projects/rapt-pill-re/firmware
idf.py set-target esp32 && idf.py build      # -> build/pill_fw.bin
```

Target is ESP-IDF v5.4.1 because the stock 2.0.0 image reports exactly that, so
NVS, OTA and partition formats match the pill as it ships.

## Tests

```bash
cc -I main -o /tmp/tg tests/test_gravity.c -lm && /tmp/tg   # quintic + plane + offset
python3 tests/test_wire.py                                  # vs the sairon/rapt-ble parser
```

The gravity test asserts against a live capture: raw `x=317.029 y=932.686` plus
that pill's `+0.4553°` offset reproduces the `gravity 1.06634` the stock firmware
reported.

## Wire it up

Adapter is any USB-UART at 3.3 V. Power the pill from USB-C — never from the
adapter's VCC; an ESP32 pulls 300-500 mA at boot and a CP2102N regulator sags to
`flash read err, 1000`.

| CP2102N | pill header |
|---|---|
| `RXD` | `E_TXD` |
| `TXD` | `E_RXD` |
| `GND` | `GND` |
| `RTS` | `EN` |
| `DTR` | strap pin (GPIO0) |

The last two rows are optional and are what make flashing one command: with
them esptool pulses download mode itself. Without them, tie GPIO0 to GND, touch
EN to GND, release.

## Flash

```bash
./flash.sh install            # backs up 4 MB, writes ota_1, boots CFW
./flash.sh status             # otadata
./flash.sh rollback           # boot stock ota_0
./flash.sh restore            # write the saved dump back
```

`install` writes only the app, at `0x190000` (`ota_1`). Stock 2.0.0 stays in
`ota_0` at `0x10000`, so a bad build is one `./flash.sh rollback` away, and the
first run takes a byte-exact 4 MB backup before it writes anything. The
bootloader verifies no signature (ABS_DONE_0/1 are 0, flash encryption off),
which is why an unsigned app boots at all.

## What it speaks

BLE: non-connectable advertisement, company `0x4152`, 23-byte body —
`"RAPT"`, version 2, gravity velocity, temperature `K*128`, specific gravity,
XYZ in `g*16`, battery in `V*256`, big-endian. Parsed by `sairon/rapt-ble`,
Home Assistant, SPINDELMATE, raptpill2mqtt.

HTTP (`X-Custom-*` headers, like stock):

| Endpoint | Purpose |
|---|---|
| `GET /` | dashboard: gravity hero, card grid |
| `GET /sensors.json` | `x y z` mg, `tilt`, `gravity`, `temp`, `adc`, `charge`, `charger` |
| `GET /sensors_cal.json` | same payload |
| `GET /static_info.json` | version, mac, AP name, pillType, `provisioningMode` |
| `GET /api/status` | FermentDial-shaped status: `gravity`, `temperatureC`, `gravityVelocity`, `batteryV`, `tilt`, `uptimeSeconds`, `partition` |
| `GET /metrics` | Prometheus text (`pill_gravity`, `pill_temperature_c`, …) |
| `GET /settings` | Wi-Fi form (headless: `POST /set_wifi.json`) |
| `GET \| POST /firmware` | browser upload → other OTA slot |
| `POST /ota_url.json` | install from a URL: `X-Custom-url`, optional `X-Custom-sha256` |
| `GET /ota_status.json` | last update progress, running partition |
| `POST /cal_1p.json` | `X-Custom-dens` (1000 = water) — stock's one-point calibration |
| `GET /get_calibration_state.json` | stock shape: `state`, `expr`, `expr-valid` |
| `POST /set_cal.json` | `X-Custom-angle`, raw angle offset in degrees |
| `POST /set_wifi.json` | `X-Custom-ssid` / `X-Custom-pass`, stored in `nvs.net80211` |
| `POST /reboot.json` | restart |
| `GET /get_settings.json` | stock's seven keys: `interval_wifi`, `interval_ble`, `telem_method`, `grav_vel_time`, `temp_offset`, `low_batt_shutdown`, `smart_mode` |
| `POST /set_interval_wifi.json`, `/set_interval_ble.json` | `X-Custom-interval` — sample period and advert cadence |
| `POST /set_temp_offset.json` | `X-Custom-temp-offset` °C, applied to every published temperature |
| `POST /set_grav_vel_time.json` | `X-Custom-grav-vel-time` s — the gravity-velocity window |
| `POST /set_smart_mode.json`, `/set_low_battery_shutdown.json`, `/set_telem_method.json` | stock headers, stored; the last is cloud-only |
| `GET /grav_vel.json` | stock shape: `valid`, `latest`, `latest_time`, `now_time`, `oldest_time`, `num` |
| `GET /grav_vel_data.csv` | the ring as CSV |
| `POST /cal_2p.json` | `X-Custom-angle1/dens1/angle2/dens2` — mean of the two offsets |
| `POST /clear_calibration.json` | offset and installed model gone |
| `POST /set_calibration_state.json` | `X-Custom-expr` — install a fitted quintic, evaluated live |
| `GET /get_log.json` | the console, as a JSON string (stock's shape) |
| `GET /gesture_log.json` | `[]` — the CFW opens its sessions on boot, nothing to tap |
| `GET /get_dev_buttons.json`, `POST /set_dev_opts.json`, `/set_beta.json`, `/set_test_api.json` | the app's developer switches, stored |
| `GET /ap_time.json`, `POST /poke_ap_timeout.json` | `-1` and a no-op: this AP does not expire |
| `GET /reg-status.json`, `POST /clear-reg.json` | `"local"` — there is no cloud to register with |
| `POST /telem_now.json`, `/upload_log.json`, `/ota_now.json` | stock's offline body, `{"success": false}` |
| `POST /factory_reset.json` | calibration and credentials gone, then reboot |
| `GET \| POST /set_mqtt` | broker, port, prefix, user, pass, enable — form fields or `X-Custom-broker`, `X-Custom-mqtt-port`, `X-Custom-mqtt-prefix`, `X-Custom-mqtt-user`, `X-Custom-mqtt-pass`, `X-Custom-mqtt-enable` |

Missing an `X-Custom` header is not an error: stock answers 200 with the value it
holds, and so does this. Unmatched URIs get stock's `Nothing matches the given URI`.

## Update it

No cables, after the first flash. Open `http://pill.local/firmware` (or the IP),
pick a `pill_fw.bin`, upload. The image goes into the slot that is *not*
running, boots once, and is marked valid only after the sensors, the
advertisement and the HTTP server are up — a build that panics on the way
reboots into the build it came from (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

```bash
curl -F firmware=@build/pill_fw.bin http://192.168.1.12/firmware
curl -X POST --header 'X-Custom-url: http://192.168.1.5:8000/pill_fw.bin' \
     --header 'X-Custom-sha256: <64 hex>' http://192.168.1.12/ota_url.json
curl -s http://192.168.1.12/ota_status.json
```

Stock firmware has no way to take an image from HTTP — `/ota_now.json` only ever
fetches Rapt's API, whose manifest is TLS-validated and checksummed — which is
why the very first install is the serial one below.

Wi-Fi comes up APSTA. A pill that was flashed from stock has no readable
credentials — IDF 5.4's `esp_wifi` cannot read the record IDF 4.3 wrote — so the
first boot advertises `RAPT-Pill-XXXXXX` / `kegland1`, and one browser submit at
`http://172.217.28.1/` provisions it. From then on it is stored in `esp_wifi`'s
own flash record and joined automatically.

Gravity is stock's model, not an approximation of it:

```
tilt    = atan2(y, x) + angle_offset
gravity = (3.11683E-06 a^5 - 0.000786275 a^4 + 0.07871266 a^3
           - 3.916024112 a^2 + 98.73244 a - 8.09818) - 12.3354
```

clamped to 10-75°. The `-12.3354` is a factory constant in the firmware, not
per-device data: on a live pill `atan2(y,x)` = 71.2266, reported tilt 71.6819,
quintic(71.6819) - 12.3354 = 1066.3399 → the reported `gravity` 1.06634.

Stock's calibration (`calibration_run.c`) stores the *ideal* angle for a
density: this pill's NVS has `cal_a1` = 36.4477, `cal_d1` = 1000.0, `cal_state`
= 1, and 36.4477 is exactly where the quintic reads 1000. So a one-point
calibration is an inversion, and `/cal_1p.json` does stock's arithmetic —
sit the pill in water, `curl -X POST --header 'X-Custom-dens: 1000' \
http://<pill>/cal_1p.json`, and the offset (36.4475 minus where it sits) lands
in the live model and in NVS. No reboot. `tests/test_gravity.c` asserts the
inversion against that pill's own numbers.

## Home Assistant

Set a broker on `GET /settings` (or `POST /set_mqtt.json`) and the pill
announces itself: six retained discovery configs under
`homeassistant/sensor/rapt-<mac tail>_*/config`, one device (`mf` “RAPT (CFW)”,
`ids` `rapt-<mac tail>`, `cu` the pill's own URL), state topics under
`rapt/<mac tail>/` — `gravity`, `temp_c`, `temp_f`, `tilt`, `grav_vel`,
`battery` — and a retained `availability` topic with an LWT, so the entities go
unavailable the moment the pill drops. Values are retained, so a fresh HA shows
the last reading immediately.

Nothing is published until a broker is set: a pill that talks to nobody is the
default, and the CFW has no cloud code in it at all.

## First install from a browser

`webflash/serve.sh` serves `webflash/index.html` with the current build beside it — open
`http://localhost:8099/`. Chrome talks to the ROM bootloader over the USB-UART cable:
**Connect** synchronises at the chosen baud, **Flash to ota_1** writes the app to
`0x190000` and then a 32-byte boot sequence to both otadata copies at `0xd000`/`0xd800`,
**Release reset** pulses EN (take the strap clip off first). The boot sequence is just a
sequence number: the bootloader picks the highest valid one, its CRC is
`crc32(0xFFFFFFFF, seq_le32)` — verified against this pill's own otadata — and parity picks
the slot, so `0x10000000` (even) boots `ota_1` and the **Boot stock again** button's
`0x10000001` (odd) points back at `ota_0` without erasing anything. No esptool, no Python,
no command-line flags; the loader is vendored (`esp-web-flasher` + `pako`) so it works offline.

## Not here yet

- Battery: ADC1_CH6 needs the divider ratio and the percentage curve. Reports
  `adc: -112, charge: -1` — the same "nothing to say" values stock uses —
  rather than inventing a voltage.
- `gravity_velocity` over a reboot: the ring is RAM (stock's is too, and its
  `now_time` is uptime), so a battery build that deep sleeps would need it in
  NVS.
- `cal_a1`/`cal_d1` as stock stores them: this build keeps one offset in
  `cal_offset` (milli-degrees) and the installed expression in `cal_expr`.
- Deep sleep between samples: `PILL_BATTERY_MODE=1` compiles the sleep path, untested.
- Light-sleep-aware BLE intervals for real battery life.
- MQTT with Home Assistant discovery, in FermentDial's config format
  (`mf`/`ids`/`via_device`/`exp_aft` plus an `/availability` LWT), so a pill can
  feed HA with no BLE proxy. Next.
- Settings for sample interval and MQTT, in a `net` NVS namespace read by name
  with defaults, the way FermentDial persists config.
