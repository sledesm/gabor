# Muse gadget firmware for the Seeed XIAO nRF52840 Sense

A port of the [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
device firmware to the **Seeed XIAO nRF52840 Sense**, on Zephyr.

The XIAO is the gadget. It speaks the same protocols as the SDK's ESP32
firmware, so the Muse app treats it the same way:

- it advertises as **`MuseGadgetXXXXXX`** with the SDK's setup GATT service,
- it pairs with the Muse app using **pairing protocol v5** (community mode,
  P-256 ECDH, HKDF-SHA256, AES-256-GCM records), and gets its Wi-Fi network
  and device tokens from the app,
- it holds its **own encrypted Noise XX session** to your Muse VM
  (`wss://…/v1/noise`, then `/link-control`), registers its commands and
  serves `link.invoke`, and posts messages with `/chat/stream`,
- it rotates its device token and reconnects with backoff, like the SDK's
  service loop.

The Noise client is the SDK's own `noise_core` (C++, unchanged). Pairing, setup,
the device API and the link session are ported from the SDK's reference code
(`linux/src/musegadget/` and `esp32/main/`).

## The one extra part: a Wi-Fi chip

A Muse gadget needs an IP connection to reach Muse, and the nRF52840 has
Bluetooth but no Wi-Fi. So the XIAO gets a Wi-Fi coprocessor on its UART: any
ESP32 or ESP8266 running Espressif's stock **ESP-AT** firmware. It acts only as
a network interface (like an Ethernet chip): Zephyr's ESP-AT driver drives it
from the XIAO, and TLS, the Noise session, the pairing keys and the tokens all
stay on the XIAO. There is no computer or bridge in the loop.

A **Seeed XIAO ESP32-C3** is the natural partner: same size, and it can sit
under the Sense.

### Wiring (XIAO nRF52840 Sense ↔ XIAO ESP32-C3)

| XIAO nRF52840 Sense | XIAO ESP32-C3 (ESP-AT) | |
|---|---|---|
| D6 (TX, P1.11) | D4 (GPIO6, AT RX) | UART, 115200 8N1 |
| D7 (RX, P1.12) | D5 (GPIO7, AT TX) | |
| GND | GND | |
| 5V | 5V | power both from the nRF's USB-C |
| D1 (optional) | — | push button to GND |

Flash the ESP32-C3 once with Espressif's prebuilt ESP-AT firmware for ESP32-C3
(v2.x–v4.x, from the [ESP-AT releases](https://github.com/espressif/esp-at/releases),
`esptool.py write_flash 0 factory_*.bin`). Its AT port is UART1 on GPIO6 (RX)
and GPIO7 (TX). The XIAO doesn't use hardware flow control: if your AT build
has it on, turn it off once from a serial terminal on that port with
`AT+UART_DEF=115200,8,1,0,0`.

Other ESP-AT modules (ESP-01/ESP8266 with AT 2.x, an ESP32 dev board) work the
same way: their AT TX to D7, AT RX to D6.

## Build

You need the [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html)
(0.17.x) and `west` (`pip install west`). From this directory:

```sh
west init -l firmware              # this directory becomes the west workspace
west update --narrow -o=--depth=1  # Zephyr 4.2 plus the modules it uses
pip install -r zephyr/scripts/requirements-base.txt
firmware/build.sh mgst_…           # your SDK token from gadgets.muse.ai
```

The firmware lands in `build/xiao/zephyr/zephyr.uf2`. The SDK token is
compiled in (`CONFIG_MUSE_SDK_TOKEN`); every gadget needs one to pair. Never
commit it.

`dist/muse_xiao_sense.uf2` is a prebuilt image **without** an SDK token, for
trying it out; it logs a warning, and will stop pairing once Muse requires
tokens.

## Flash

1. Plug the XIAO in over USB-C and double-tap its **RESET** button. A drive
   called `XIAO-SENSE` appears.
2. Copy the `.uf2` onto it. The board restarts into the firmware.

The log is on the XIAO's USB serial port (115200 baud). To go back to Seeed's
firmware, double-tap RESET and copy theirs.

## Set it up with Muse

As with the SDK's ESP32 boards:

1. The light **breathes orange**: ready for setup.
2. In the Muse app, turn on **Settings > Devices > Developer mode**, then add
   a device (**Settings > Devices > Add Device**). Pick `MuseGadgetXXXXXX`
   (the same digits as `homelink-xxxxxx` in the log).
3. While the app pairs, the light **breathes blue**. Choose your Wi-Fi network
   when asked; the list comes from the ESP-AT module's scan.
4. The light **blinks yellow** while it joins Wi-Fi and connects, then shows a
   short **green blip** every few seconds once Muse has registered it.
   **Red blinks** mean it's offline and retrying.

Pairing, the network and the tokens are kept in flash, so it reconnects by
itself after a restart. To set it up again, remove it in the Muse app (the
gadget forgets the pairing and restarts into setup), or hold the D1 button for
5 seconds.

## What Muse can do with it

| Command | What it does |
|---|---|
| `device.health` | Battery level, voltage and charging, uptime, version, Wi-Fi network |
| `motion.read` | Acceleration (g) and rotation (°/s) from the LSM6DS3TR-C, which way up the board is, whether it's moving, chip temperature |
| `sound.level` | Listens with the PDM microphone for 0.1–5 s and reports RMS, peak and dBFS. No audio is kept or sent |
| `light.set` | Sets the RGB LED: a colour name or `#RRGGBB`, `solid`, `blink` or `breathe`; `off` gives it back to the status pattern |

The gadget also tells Muse when something happens, as a message from the
device (like `musegadget send-user-msg`): the D1 button is clicked or held, the
board is shaken, or it detects a free fall.

Ask things like *"How loud is it in the office?"*, *"Is my XIAO's battery
low?"*, *"Turn the XIAO light purple and make it breathe."*

## How it's built

| Path | What it is |
|---|---|
| `firmware/src/ble_gatt.c` | Setup GATT service and advertisement (same UUIDs, `0xFFFF` paired flag, name in scan response) |
| `firmware/src/ble_framing.c`, `setup.c` | Chunked framing and the setup commands (`get_device_info`, `pairing_*`, `wifi_scan`, `provision_v2`), from `ble_setup.py` |
| `firmware/src/pairing.c` | Pairing v5: transcript, key schedule, record encryption, generations and timeouts, from `pairing.py` |
| `firmware/src/net.c`, `https.c`, `muse_api.c` | Wi-Fi join/scan, TLS sockets pinned to DigiCert Global Root G2 (the root of `api.muse.ai` and `hatch.metaaivm.com`), `/fetch_vms` and `/device_token/refresh` |
| `firmware/src/link.cpp` | The Noise link session on the SDK's `noise_core`: upgrade, handshake, `link.register`, invokes, chat, keepalive |
| `firmware/src/service.c` | Reconnect loop, token rotation, unpairing, from `service.py` |
| `firmware/src/commands.c`, `board_xiao.c` | Commands and the XIAO hardware (IMU, PDM mic, battery ADC and charge pin, PWM RGB LED, D1 button, shake and free-fall detection) |
| `firmware/src/mem.c` | One fixed 100 KB heap for cJSON, mbedTLS and the session buffers, with peak-use logging |
| `firmware/lib/noise_core` | The SDK's Noise core, unchanged (Apache-2.0, Meta) |
| `firmware/lib/cjson` | cJSON 1.7.18 (MIT) |

It registers with `platform: esp32`, `device_family: link`,
`model_id: esp-link` (`CONFIG_MUSE_REGISTER_*`): the XIAO uses the SDK ESP32
firmware's pairing model (`hatch_link`) and control protocol, so it presents
the profile Muse already knows for it.

Memory: the nRF52840 has 256 KB of RAM and no PSRAM, so the session uses the
buffer sizes of the SDK's smallest ESP32 profile (M5Stack Cardputer). A live
session (TLS plus Noise) peaks at about 78 KB of the 100 KB heap, measured in
the test below on a 64-bit host, which overstates the 32-bit chip.

## Tests

`firmware/tests/test_e2e_sim.py` runs the real firmware, built for Zephyr's
`native_sim`, against three fakes built from the SDK's own Python reference
code:

- **the Muse app**: pairs with protocol v5 using the SDK's `build_transcript`,
  `derive_session_keys` and record encryption, then scans and provisions,
- **the Muse API** over TLS,
- **a Muse VM** over TLS that runs the SDK's `NoiseXXResponder`.

native_sim has no radio, so the setup packets go over TCP instead of GATT,
and a test CA stands in for DigiCert. Everything else (pairing, TLS, Noise,
storage, the service loop, the heap budget) is the code that runs on the XIAO.
It checks: pairing and provisioning, token rotation with the SDK token,
default-VM choice, `link.register` and its commands, every command's result,
a chat message from a button press, `link.unpaired` resetting to setup, the
pairing surviving a restart, bad tokens failing setup, a tampered record being
rejected, and the session staying up across keepalive pings.

```sh
pip install -e <muse-gadget-sdk>/linux pytest
west build -b native_sim/native/64 firmware -d build/sim
MUSE_SIM_EXE=build/sim/zephyr/zephyr.exe pytest firmware/tests -v
```

## Status

- Builds for `xiao_ble/nrf52840/sense` (446 KB flash, 96% of RAM statically
  assigned, most of it the gadget heap) and passes the end-to-end test above.
- **Not yet run on a physical board.** The parts the simulator can't cover,
  the Bluetooth radio, the ESP-AT link and the sensor drivers, are built from
  Zephyr's drivers but haven't met real hardware. The shake and free-fall
  thresholds are starting points.
- Not ported from the ESP32 firmware: OTA updates, the home-network tunnel,
  displays and voice.
