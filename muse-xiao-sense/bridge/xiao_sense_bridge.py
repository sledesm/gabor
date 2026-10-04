#!/usr/bin/env python3
"""Bridge a Seeed XIAO nRF52840 Sense to Muse through the Linux gadget SDK.

The XIAO has Bluetooth LE but no Wi-Fi, so it cannot hold its own connection
to Muse. This runs next to the ``musegadget`` service from
github.com/facebookincubator/muse-gadget-sdk (linux/), on a machine with
Bluetooth LE, and:

* keeps a BLE connection to the XIAO running firmware/muse_xiao_sense,
* answers ``read`` and ``led`` requests on a local Unix socket, so Muse can
  use the board through the gadget's ``system.run`` command
  (``xiao_sense_bridge.py read``, ``xiao_sense_bridge.py led red``),
* forwards board events (button, shake, free fall) to Muse with
  ``musegadget send-user-msg``.

Usage:
  xiao_sense_bridge.py run [--address AA:BB:..] [--session-id ID]
  xiao_sense_bridge.py read
  xiao_sense_bridge.py led red|green|blue|white|off|#RRGGBB [--mode blink]
  xiao_sense_bridge.py scan

Needs ``bleak`` (pip install bleak) for ``run`` and ``scan``; ``read`` and
``led`` use only the standard library.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import os
import shutil
import struct
import sys
import time

log = logging.getLogger("xiao-sense-bridge")

# GATT layout, mirrored from firmware/muse_xiao_sense/muse_xiao_sense.ino.
_BASE = "9a3e{:04x}-6b1f-4c3a-9e2d-4d7573655853"
SERVICE_UUID = _BASE.format(0x0000)
STATE_UUID = _BASE.format(0x0001)
LED_UUID = _BASE.format(0x0002)
EVENT_UUID = _BASE.format(0x0003)
NAME_PREFIX = "MuseXiao-"

STATE_FORMAT = "<BBHh3h3hHHIH"
STATE_SIZE = struct.calcsize(STATE_FORMAT)  # 28
EVENT_FORMAT = "<BBHI"

FLAG_IMU_OK = 0x01
FLAG_MIC_OK = 0x02
FLAG_CHARGING = 0x04
FLAG_BUTTON_DOWN = 0x08

EVENTS = {
    1: ("button", "The button on my XIAO sensor was pressed."),
    2: ("button_long", "The button on my XIAO sensor was held down."),
    3: ("shake", "My XIAO sensor was shaken."),
    4: ("free_fall", "My XIAO sensor detected a free fall. It may have been dropped."),
}

LED_MODES = {"solid": 0, "blink": 1, "breathe": 2}
COLOURS = {
    "off": (0, 0, 0),
    "red": (255, 0, 0),
    "green": (0, 255, 0),
    "blue": (0, 0, 255),
    "white": (255, 255, 255),
    "yellow": (255, 160, 0),
    "orange": (255, 64, 0),
    "purple": (160, 0, 255),
    "cyan": (0, 255, 255),
}

DEFAULT_SOCKET = os.environ.get("XIAO_SENSE_SOCKET", "/run/xiao-sense/bridge.sock")
RECONNECT_DELAY_S = 5
STALE_AFTER_S = 5


def decode_state(data: bytes) -> dict:
    """Turn a State characteristic value into a dict with units."""
    if len(data) < STATE_SIZE:
        raise ValueError(f"state is {len(data)} bytes, expected {STATE_SIZE}")
    (version, flags, battery_mv, temp_cc, ax, ay, az, gx, gy, gz,
     mic_rms, mic_peak, uptime_s, event_count) = struct.unpack_from(STATE_FORMAT, data)
    if version != 1:
        raise ValueError(f"unsupported state version {version}")
    state = {
        "battery": {
            "voltage_v": round(battery_mv / 1000, 3) if battery_mv else None,
            "percent": battery_percent(battery_mv),
            "charging": bool(flags & FLAG_CHARGING),
        },
        "button_down": bool(flags & FLAG_BUTTON_DOWN),
        "uptime_s": uptime_s,
        "event_count": event_count,
    }
    if flags & FLAG_IMU_OK:
        state["temperature_c"] = round(temp_cc / 100, 2)
        state["accel_g"] = {"x": ax / 1000, "y": ay / 1000, "z": az / 1000}
        state["gyro_dps"] = {"x": gx / 10, "y": gy / 10, "z": gz / 10}
        state["orientation"] = orientation(ax, ay, az)
    else:
        state["imu"] = "unavailable"
    if flags & FLAG_MIC_OK:
        state["sound"] = {
            "rms": mic_rms,
            "peak": mic_peak,
            "level_dbfs": round(20 * math.log10(mic_rms / 32768), 1) if mic_rms else None,
        }
    else:
        state["sound"] = "unavailable"
    return state


def battery_percent(mv: int) -> int | None:
    if not mv:
        return None
    return max(0, min(100, round((mv - 3300) * 100 / (4150 - 3300))))


def orientation(ax: int, ay: int, az: int) -> str:
    """The board axis pointing up (+z: components facing up), from gravity in mg."""
    axis, value = max((("x", ax), ("y", ay), ("z", az)), key=lambda kv: abs(kv[1]))
    if abs(value) < 700:
        return "tilted"
    return ("+" if value > 0 else "-") + axis + " up"


def decode_event(data: bytes) -> dict:
    kind, _, seq, uptime_ms = struct.unpack_from(EVENT_FORMAT, data)
    name, text = EVENTS.get(kind, (f"event_{kind}", f"My XIAO sensor sent event {kind}."))
    return {"type": name, "seq": seq, "uptime_ms": uptime_ms, "text": text}


def encode_led(colour: str, mode: str = "solid") -> bytes:
    colour = colour.strip().lower()
    if colour in COLOURS:
        rgb = COLOURS[colour]
    else:
        hexval = colour.lstrip("#")
        if len(hexval) != 6:
            raise ValueError(f"unknown colour {colour!r}: use a name or #RRGGBB")
        rgb = tuple(int(hexval[i:i + 2], 16) for i in (0, 2, 4))
    if mode not in LED_MODES:
        raise ValueError(f"unknown mode {mode!r}: use {', '.join(LED_MODES)}")
    return bytes([*rgb, LED_MODES[mode]])


# ---- Daemon -----------------------------------------------------------------


class Bridge:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.client = None
        self.state: dict | None = None
        self.state_at = 0.0
        self.address: str | None = args.address
        self.name: str | None = None

    async def run(self) -> None:
        server = await self._serve()
        async with server:
            while True:
                try:
                    await self._connect_once()
                except Exception as exc:  # BLE errors are many and unspecific
                    log.warning("XIAO connection: %s", exc)
                self.client = None
                await asyncio.sleep(RECONNECT_DELAY_S)

    async def _find(self):
        from bleak import BleakScanner

        if self.address:
            return await BleakScanner.find_device_by_address(self.address, timeout=10)

        def match(device, adv) -> bool:
            name = adv.local_name or device.name or ""
            return name.startswith(NAME_PREFIX) or SERVICE_UUID in adv.service_uuids

        return await BleakScanner.find_device_by_filter(match, timeout=10)

    async def _connect_once(self) -> None:
        from bleak import BleakClient

        device = await self._find()
        if device is None:
            log.info("no XIAO found, retrying")
            return
        disconnected = asyncio.Event()
        async with BleakClient(device, disconnected_callback=lambda _: disconnected.set()) as client:
            self.client = client
            self.name = device.name
            log.info("connected to %s (%s)", device.name, device.address)
            self._on_state(await client.read_gatt_char(STATE_UUID))
            await client.start_notify(STATE_UUID, lambda _, data: self._on_state(data))
            await client.start_notify(EVENT_UUID, lambda _, data: self._on_event(data))
            await disconnected.wait()
            log.info("disconnected from %s", device.name)

    def _on_state(self, data: bytearray) -> None:
        try:
            self.state = decode_state(bytes(data))
            self.state_at = time.time()
        except ValueError as exc:
            log.warning("bad state: %s", exc)

    def _on_event(self, data: bytearray) -> None:
        event = decode_event(bytes(data))
        log.info("event: %s", event["type"])
        if self.args.no_forward:
            return
        asyncio.get_running_loop().create_task(self._forward(event["text"]))

    async def _forward(self, text: str) -> None:
        cmd = [self.args.musegadget, "send-user-msg"]
        if self.args.session_id:
            cmd += ["--session-id", self.args.session_id]
        cmd.append(text)
        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
            _, err = await asyncio.wait_for(proc.communicate(), timeout=100)
            if proc.returncode:
                log.warning("send-user-msg failed: %s", err.decode(errors="replace").strip())
        except (OSError, asyncio.TimeoutError) as exc:
            log.warning("send-user-msg failed: %s", exc)

    # -- Local socket ---------------------------------------------------------

    async def _serve(self):
        path = self.args.socket
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        if os.path.exists(path):
            os.unlink(path)
        server = await asyncio.start_unix_server(self._handle, path=path)
        os.chmod(path, 0o660)
        log.info("listening on %s", path)
        return server

    async def _handle(self, reader, writer) -> None:
        try:
            line = await asyncio.wait_for(reader.readline(), timeout=10)
            reply = await self._dispatch(json.loads(line or b"{}"))
        except Exception as exc:
            reply = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
        writer.write(json.dumps(reply).encode() + b"\n")
        await writer.drain()
        writer.close()

    async def _dispatch(self, request: dict) -> dict:
        connected = self.client is not None and self.client.is_connected
        cmd = request.get("cmd")
        if cmd == "read":
            if not connected:
                return {"ok": False, "error": "the XIAO is not connected"}
            if self.state is None or time.time() - self.state_at > STALE_AFTER_S:
                self._on_state(await self.client.read_gatt_char(STATE_UUID))
            return {"ok": True, "device": self.name, **(self.state or {})}
        if cmd == "led":
            if not connected:
                return {"ok": False, "error": "the XIAO is not connected"}
            payload = encode_led(request.get("colour", "off"), request.get("mode", "solid"))
            await self.client.write_gatt_char(LED_UUID, payload, response=True)
            return {"ok": True}
        if cmd == "status":
            return {"ok": True, "connected": connected, "device": self.name}
        return {"ok": False, "error": f"unknown command {cmd!r}"}


# ---- Client commands ----------------------------------------------------------


def request(socket_path: str, payload: dict) -> dict:
    import socket

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(15)
        sock.connect(socket_path)
        sock.sendall(json.dumps(payload).encode() + b"\n")
        return json.loads(sock.makefile("rb").readline())


def cmd_client(args: argparse.Namespace, payload: dict) -> int:
    try:
        reply = request(args.socket, payload)
    except OSError as exc:
        print(json.dumps({"ok": False, "error": f"bridge not running at {args.socket}: {exc}"}))
        return 1
    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 1


async def scan() -> int:
    from bleak import BleakScanner

    found = await BleakScanner.discover(timeout=8, return_adv=True)
    for device, adv in found.values():
        name = adv.local_name or device.name or ""
        if name.startswith(NAME_PREFIX) or SERVICE_UUID in adv.service_uuids:
            print(f"{device.address}  {name}  rssi {adv.rssi}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="xiao_sense_bridge.py", description=__doc__.split("\n")[0])
    parser.add_argument("--socket", default=DEFAULT_SOCKET, help="bridge socket (default: %(default)s)")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="connect to the XIAO and serve requests")
    run.add_argument("--address", help="the XIAO's BLE address (default: the first MuseXiao-* found)")
    run.add_argument("--session-id", default=os.environ.get("XIAO_SENSE_SESSION_ID"),
                     help="post events to this Muse side chat instead of the main chat")
    run.add_argument("--musegadget", default=os.environ.get("MUSEGADGET") or shutil.which("musegadget")
                     or "/usr/local/bin/musegadget", help="path to the musegadget command")
    run.add_argument("--no-forward", action="store_true", help="don't send events to Muse")

    sub.add_parser("read", help="print the XIAO's sensor readings as JSON")
    led = sub.add_parser("led", help="set the XIAO's RGB LED")
    led.add_argument("colour", help=f"{', '.join(COLOURS)} or #RRGGBB")
    led.add_argument("--mode", choices=list(LED_MODES), default="solid")
    sub.add_parser("status", help="is the bridge connected to the XIAO?")
    sub.add_parser("scan", help="list nearby XIAOs running the firmware")

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    if args.command == "run":
        asyncio.run(Bridge(args).run())
        return 0
    if args.command == "scan":
        return asyncio.run(scan())
    if args.command == "read":
        return cmd_client(args, {"cmd": "read"})
    if args.command == "status":
        return cmd_client(args, {"cmd": "status"})
    if args.command == "led":
        try:
            encode_led(args.colour, args.mode)
        except ValueError as exc:
            parser.error(str(exc))
        return cmd_client(args, {"cmd": "led", "colour": args.colour, "mode": args.mode})
    return 2


if __name__ == "__main__":
    sys.exit(main())
