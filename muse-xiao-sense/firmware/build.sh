#!/usr/bin/env bash
# Build the XIAO nRF52840 Sense firmware into ../dist/muse_xiao_sense.uf2.
# Needs arduino-cli and adafruit-nrfutil (pip install adafruit-nrfutil) on PATH.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../dist"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT

fqbn=Seeeduino:nrf52:xiaonRF52840Sense
index=https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json

arduino-cli core update-index --additional-urls "$index"
arduino-cli core install Seeeduino:nrf52@1.1.13 --additional-urls "$index"
arduino-cli lib install "Seeed Arduino LSM6DS3@2.0.7"

arduino-cli compile --fqbn "$fqbn" --additional-urls "$index" \
  --output-dir "$build" "$here/muse_xiao_sense"

uf2conv="$(find ~/.arduino15/packages/Seeeduino/hardware/nrf52 -name uf2conv.py | head -1)"
mkdir -p "$out"
python3 "$uf2conv" -c -f 0xADA52840 -o "$out/muse_xiao_sense.uf2" "$build/muse_xiao_sense.ino.hex"
cp "$build/muse_xiao_sense.ino.zip" "$out/muse_xiao_sense_dfu.zip"
echo "Built $out/muse_xiao_sense.uf2"
