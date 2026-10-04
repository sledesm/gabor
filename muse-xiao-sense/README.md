# Muse gadget: Seeed XIAO nRF52840 Sense

Firmware that makes a [Seeed XIAO nRF52840 Sense](https://wiki.seeedstudio.com/XIAO_BLE/)
into a Muse gadget, built to work with the
[Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk).

## Why there's a bridge

A Muse gadget keeps its own encrypted connection to Muse over Wi-Fi
(a Noise session over `wss://`). The SDK's firmware is written for ESP32
chips, which have Wi-Fi. The nRF52840 has Bluetooth LE only, so it can't run
that firmware or reach Muse by itself.

So the XIAO acts as a BLE sensor, and a machine running the SDK's
[Linux gadget](https://github.com/facebookincubator/muse-gadget-sdk/tree/main/linux)
(a Raspberry Pi, for example) connects it to Muse:

```
XIAO nRF52840 Sense ──BLE──▶ xiao_sense_bridge.py ──▶ musegadget (Linux SDK) ──wss──▶ Muse
   IMU, mic, battery,          read / led on a           system.run, send-user-msg
   RGB LED, button             local socket
```

- Muse **reads the sensors** and **sets the LED** by running
  `xiao_sense_bridge.py read` or `xiao_sense_bridge.py led ...` through the
  gadget's existing `system.run` command. The SDK needs no changes.
- **Events** (button press, long press, shake, free fall) reach Muse as
  messages, via `musegadget send-user-msg`, the same way the SDK's
  `examples/pebble_ring_bridge.py` works.

## What's here

| Path | What it is |
|---|---|
| `dist/muse_xiao_sense.uf2` | Prebuilt firmware. Drag it onto the XIAO's USB drive. |
| `dist/muse_xiao_sense_dfu.zip` | The same firmware as a DFU package, for `adafruit-nrfutil`. |
| `firmware/muse_xiao_sense/` | The Arduino sketch. |
| `firmware/build.sh` | Rebuilds `dist/` with `arduino-cli`. |
| `bridge/xiao_sense_bridge.py` | The bridge daemon and its command-line client. |
| `bridge/xiao-sense-bridge.service` | A systemd unit for the bridge. |
| `bridge/test_xiao_sense_bridge.py` | Tests for the bridge, no Bluetooth needed. |

## 1. Flash the XIAO

1. Plug the XIAO into your computer over USB-C.
2. Double-tap the tiny **RESET** button. A USB drive called `XIAO-SENSE`
   appears and the LED pulses green.
3. Copy `dist/muse_xiao_sense.uf2` onto the drive. It reboots by itself.

The LED then blips **blue** every two seconds while it waits for the bridge,
and **green** once the bridge is connected. The board advertises as
`MuseXiao-XXXX`. The USB serial port (115200 baud) logs what it's doing.

To build it yourself instead, install
[`arduino-cli`](https://arduino.github.io/arduino-cli/) and
`pip install adafruit-nrfutil`, then run `firmware/build.sh`. It uses the
**Seeed nRF52 Boards** core 1.1.13 (board *Seeed XIAO nRF52840 Sense*, not the
mbed-enabled one) and the *Seeed Arduino LSM6DS3* library 2.0.7. You can also
open the sketch in the Arduino IDE with those selected.

### Optional button

The XIAO's only button is RESET. To send button events, wire a push button
between **D1** and **GND**. A click sends `button`, holding it for a second
sends `button_long`.

## 2. Set up the Linux gadget

On the machine that will bridge the XIAO (it needs Bluetooth LE and has to be
within range of the XIAO), install the Muse Linux gadget and pair it with the
Muse app, as its
[README](https://github.com/facebookincubator/muse-gadget-sdk/tree/main/linux)
describes:

```sh
curl -fsSL https://raw.githubusercontent.com/facebookincubator/muse-gadget-sdk/main/linux/install.sh -o install.sh
bash install.sh --sdk-token mgst_…
```

## 3. Run the bridge

```sh
sudo mkdir -p /opt/xiao-sense
sudo cp bridge/xiao_sense_bridge.py /opt/xiao-sense/
sudo python3 -m venv /opt/xiao-sense/venv
sudo /opt/xiao-sense/venv/bin/pip install bleak
```

Edit `User=` in `bridge/xiao-sense-bridge.service` to the account the
`musegadget` service runs commands as (the account you installed it for). That
account needs to be in the musegadget socket's group to send events, and it's
the one Muse's commands run as, so it can use the bridge socket. Then:

```sh
sudo cp bridge/xiao-sense-bridge.service /etc/systemd/system/
sudo systemctl enable --now xiao-sense-bridge
journalctl -u xiao-sense-bridge -f     # "connected to MuseXiao-XXXX"
```

Check it:

```sh
/opt/xiao-sense/xiao_sense_bridge.py read
/opt/xiao-sense/xiao_sense_bridge.py led purple --mode breathe
/opt/xiao-sense/xiao_sense_bridge.py led off
```

`read` and `led` need only the system Python. With more than one XIAO around,
pin the bridge to one with `run --address AA:BB:CC:DD:EE:FF`
(`xiao_sense_bridge.py scan` lists them).

## 4. Use it from Muse

Tell Muse about it once, for example:

> My Pi has a XIAO sensor. Run `/opt/xiao-sense/xiao_sense_bridge.py read`
> to get its readings (battery, motion, temperature, sound level) and
> `/opt/xiao-sense/xiao_sense_bridge.py led COLOUR [--mode blink|breathe]` to set
> its light. When I say the sensor was shaken or pressed, that came from it.

Then ask things like:

> How loud is it in the room right now?

> Is the XIAO's battery low?

> Turn the XIAO's light red and make it blink.

Events arrive in your main Muse chat. To keep them in their own side chat,
set `XIAO_SENSE_SESSION_ID` in the service file to any UUID.

`read` prints JSON like this:

```json
{
  "ok": true,
  "device": "MuseXiao-3F2A",
  "battery": {"voltage_v": 3.912, "percent": 72, "charging": false},
  "button_down": false,
  "uptime_s": 812,
  "event_count": 4,
  "temperature_c": 27.31,
  "accel_g": {"x": 0.012, "y": -0.031, "z": 0.998},
  "gyro_dps": {"x": 0.4, "y": -0.2, "z": 0.1},
  "orientation": "+z up",
  "sound": {"rms": 412, "peak": 3120, "level_dbfs": -38.0}
}
```

`orientation` names the board axis facing up; `+z up` is lying flat with
the components on top. The temperature is the IMU chip's, which runs a
little above the room's.

## BLE interface

One primary service, `9a3e0000-6b1f-4c3a-9e2d-4d7573655853`, plus the
standard Battery (0x180F) and Device Information (0x180A) services. All
values are little-endian.

| Characteristic | UUID | Access | Value |
|---|---|---|---|
| State | `9a3e0001-…` | read, notify (2 Hz) | 28 bytes, below |
| LED | `9a3e0002-…` | write | `r, g, b[, mode]`; mode 0 solid, 1 blink, 2 breathe. All zero hands the LED back to the status blips. |
| Event | `9a3e0003-…` | notify | `u8 type, u8 0, u16 seq, u32 uptime_ms`; type 1 button, 2 long press, 3 shake, 4 free fall |

State:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version (1) |
| 1 | u8 | flags: 0x01 IMU ok, 0x02 mic ok, 0x04 charging, 0x08 button down |
| 2 | u16 | battery, mV (0 with no battery) |
| 4 | i16 | temperature, 0.01 °C |
| 6 | i16 ×3 | acceleration x, y, z, milli-g |
| 12 | i16 ×3 | rotation x, y, z, 0.1 °/s |
| 18 | u16 | sound RMS over the last 500 ms, 0–32767 |
| 20 | u16 | sound peak over the last 500 ms |
| 22 | u32 | uptime, s |
| 26 | u16 | events sent since boot |

## Tests

```sh
cd bridge && python3 -m unittest -v
```

They cover decoding the state and events, encoding LED commands, and the
bridge socket with a fake BLE connection. The firmware was compiled but has
not been run on hardware yet, so treat thresholds like shake sensitivity as
starting points.
