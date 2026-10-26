# Rapt Pill hydrometer: replacement firmware and interop notes

Reverse-engineering notes, analysis scripts, and a from-scratch replacement
firmware for the Rapt Pill hydrometer (ESP32-S0WD, 4 MB flash, LIS3DH + TMP1075).

Analysis target: app image `rapt-pill-2.0.0.bin`, SHA-256
`b5ac279c9dbab7e0d5f1bb13ea09a59e0f74e2ebff396e5badb89cfea0348862`, 1,345,744 bytes.
**You supply that image yourself** by reading flash from a pill you own; no copy of
Rapt's firmware is distributed here, and none of the scripts below work without it.

- Plan for our own firmware: [notes/roadmap-own-fw.md](notes/roadmap-own-fw.md)
- Our firmware image for the browser flasher: `firmware/webflash/pill_fw.bin`
- Captured REST response payloads: `extract/api/` · Address/name maps: `notes/`

## Not affiliated with Rapt

This is an independent, non-commercial interoperability and ownership project run
on hardware we bought. "Rapt", "KegLand" and "Rapt Pill" are their owners'
trademarks, used here only to say what this work is about. Nothing is endorsed by,
sponsored by, or approved by Rapt or KegLand, and this is not a support channel for
their products.

We publish our own code, our own observations, and interface facts (addresses,
offsets, endpoints, JSON keys, byte layouts). We deliberately do **not** publish:

- Rapt's firmware image, the ELF converted from it, or any decompiled listing of it
- the web UI files embedded in their firmware (HTML/JS) — `scripts/extract_assets.py`
  carves them from an image you supply, into paths that are gitignored
- bulk dumps of strings taken out of their binary, and captures of the firmware's
  own log output (serial console, `/get_log.json`) — those log lines are Rapt's
  text, not ours. Read the flash of a pill you own and capture your own
- flash/NVS dumps from our own pills: they carry live Wi-Fi credentials, cloud
  tokens and BLE bonding keys (see the masking note below)

If you are Rapt or KegLand and want any of this removed, open an issue and it
comes down — that is the whole point of publishing the analysis as notes and
scripts rather than as copies of your binary.

## Masking note

Device-specific values are masked throughout: MACs keep the Espressif OUI and show
`xx:xx:xx` for the NIC half, the LAN reads `192.168.1.0/24`, and NVS Wi-Fi
credentials, cloud `refresh_tok` values and BLE bond keys are `<redacted>`.
`RAPT-Pill-XXXXXX` and `kegland1` are Rapt's own factory defaults, so they stay —
they are what every unprovisioned pill advertises.

## Layout

| file | what |
|---|---|
| `parse_esp.py` | ESP image header + 8 segment table |
| `esp2elf.py` | ESP app image → ELF32 `EM_XTENSA` with 8 `PT_LOAD` at real load addresses |
| `scripts/GhidraExport.java` | headless export: blocks, functions, call graph, strings + owning function |
| `scripts/DiscoverFunctions.java` | recover functions Ghidra's auto-analysis misses (pointer tables) |
| `scripts/extract_assets.py` | carve embedded web assets + TLS root CAs out of `.rodata` |
| `scripts/string_map.py` | string → owning function (literal-pool chaining), writes `notes/string_map.tsv` |
| `scripts/ble_gatt_table.py` | decode the NimBLE GATT attribute table: UUIDs, flags, key size |
| `scripts/http_table.py`, `scripts/handler_table.py` | recover the `esp_http_server` URI → handler table |
| `scripts/xtensa_dis.py`, `scripts/text_listing.py` | objdump listing with literal-slot attribution |
| `notes/names.tsv` | analyst names applied to the Ghidra project (addr → name) |
| `notes/json_keys.txt`, `notes/targets*.txt` | observed JSON keys; function addresses of interest |
| `extract/api/` | REST response payloads captured from a pill (keys and values, not firmware logs) |
| `extract/certs/` | root CAs found in the image (public certificates, from the bundle) |
| `firmware/` | our replacement firmware (ESP-IDF); `python3 firmware/tests/test_wire.py` |
| `firmware/webflash/` | browser flasher: `./firmware/webflash/serve.sh` then open it in Chrome |

Generated, not committed: `out/`, `ghidra/`, `extract/assets/`, `extract/web/`,
`notes/strings_all.txt`, `notes/string_map.tsv`, and any `.elf`/`.bin` you build.

## Reproduce

```bash
python3 esp2elf.py rapt-pill-2.0.0.bin rapt-pill-2.0.0.elf
RAPT_OUT=$PWD/out /opt/ghidra/support/analyzeHeadless $PWD/ghidra rapt200 \
  -import rapt-pill-2.0.0.elf -scriptPath scripts -postScript GhidraExport.java \
  -analysisTimeoutPerFile 5400 -log INFO
python3 scripts/string_map.py                     # then notes/string_map.tsv exists
```

Re-analysis without re-import: `-process rapt-pill-2.0.0.elf -noanalysis -postScript <script>`.

## Prior art (found 2026-10-22)

- `BestEnemies/pill` — teardown: ESP32-S0WD, 4 MB flash, LIS3DH `0x19` +
  TMP1075 `0x48` on SDA 25 / SCL 26, GPIO 18/5 outputs, 23 input, 34 ADC,
  serial header, boot log of the 2021 Azure-era firmware (app at `0x190000`).
- `sairon/rapt-ble` — the 23-byte big-endian BLE advertisement structs
  (v1/v2), which is what Home Assistant parses.
- `kbaggen/SPINDELMATE`, `raptpill2mqtt`, `rapt-mqtt-bridge`,
  `sbaird123/rapt-brewing-hacs` — consumers of the BLE and REST APIs.

## Toolchain notes (2026-10-22)

- Ghidra 12.1.2 (ships the `Xtensa` SLEIGH module) does the disassembly. Some RE
  MCP toolchains whitelist only x86/arm/aarch64/mips/riscv/powerpc for ELF
  admission, so they reject an Xtensa ELF (`file_format_rejected`) and cannot
  decompile ESP32 code.
- `binwalk` signature scanning was done with the shell and is labelled as shell
  evidence, not tool-mediated evidence.
- No ESP encryption: image magic is `0xE9` (plain), not `0xD9`, so the whole app is
  readable from the app image. Secure boot state is not determinable from an app
  image alone.
