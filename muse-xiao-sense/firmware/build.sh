#!/usr/bin/env bash
# Build the Muse gadget firmware for the XIAO nRF52840 Sense.
#   firmware/build.sh [mgst_your_sdk_token]
# Output: build/xiao/zephyr/zephyr.uf2 (drag it onto the XIAO-SENSE drive).
# Needs a west workspace (see README.md) and the Zephyr SDK.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
token="${1:-${MUSE_SDK_TOKEN:-}}"
extra=()
if [[ -n "$token" ]]; then
  extra=(-- "-DCONFIG_MUSE_SDK_TOKEN=\"$token\"")
else
  echo "warning: no SDK token; pass mgst_... or set MUSE_SDK_TOKEN" >&2
fi
west build -p auto -b xiao_ble/nrf52840/sense "$here" -d "$here/../build/xiao" "${extra[@]}"
echo "Built $here/../build/xiao/zephyr/zephyr.uf2"
