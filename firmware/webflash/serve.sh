#!/usr/bin/env bash
# Serve the Web Serial installer with the current build beside it.
# Web Serial only works in a secure context, so use http://localhost (or https).
set -euo pipefail
cd "$(dirname "$0")"
[ -f ../build/pill_fw.bin ] && cp -f ../build/pill_fw.bin pill_fw.bin && echo "staged pill_fw.bin"
[ -f esp-web-flasher.mjs ] || {
  curl -sL -o esp-web-flasher.mjs 'https://cdn.jsdelivr.net/npm/esp-web-flasher@5.1.5/+esm'
  curl -sL -o pako.mjs 'https://cdn.jsdelivr.net/npm/pako@2.0.4/dist/pako.esm.mjs/+esm'
  sed -i 's#/npm/pako@2.0.4/dist/pako.esm.mjs/+esm#./pako.mjs#' esp-web-flasher.mjs
  echo "vendored the loader"
}
echo "open http://localhost:${PORT:-8099}/"
exec python3 -m http.server "${PORT:-8099}" -b 127.0.0.1
