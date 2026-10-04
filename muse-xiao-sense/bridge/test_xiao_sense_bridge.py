"""Tests for xiao_sense_bridge.py that need no Bluetooth: python3 -m unittest."""

import argparse
import asyncio
import os
import struct
import tempfile
import unittest

import xiao_sense_bridge as b


def pack_state(**kw):
    f = dict(version=1, flags=b.FLAG_IMU_OK | b.FLAG_MIC_OK, battery_mv=3900, temp_cc=2512,
             accel=(10, -20, 1000), gyro=(15, -5, 0), mic_rms=3277, mic_peak=12000,
             uptime_s=42, event_count=3)
    f.update(kw)
    return struct.pack(b.STATE_FORMAT, f["version"], f["flags"], f["battery_mv"], f["temp_cc"],
                       *f["accel"], *f["gyro"], f["mic_rms"], f["mic_peak"], f["uptime_s"],
                       f["event_count"])


class DecodeTest(unittest.TestCase):
    def test_state_matches_firmware_size(self):
        self.assertEqual(b.STATE_SIZE, 28)  # static_assert(sizeof(State) == 28) in the sketch

    def test_decode_state(self):
        s = b.decode_state(pack_state())
        self.assertEqual(s["battery"], {"voltage_v": 3.9, "percent": 71, "charging": False})
        self.assertEqual(s["temperature_c"], 25.12)
        self.assertEqual(s["accel_g"], {"x": 0.01, "y": -0.02, "z": 1.0})
        self.assertEqual(s["gyro_dps"], {"x": 1.5, "y": -0.5, "z": 0.0})
        self.assertEqual(s["orientation"], "+z up")
        self.assertEqual(s["sound"]["level_dbfs"], -20.0)
        self.assertEqual(s["uptime_s"], 42)

    def test_decode_state_without_sensors(self):
        s = b.decode_state(pack_state(flags=b.FLAG_CHARGING, battery_mv=0, mic_rms=0))
        self.assertEqual(s["imu"], "unavailable")
        self.assertEqual(s["sound"], "unavailable")
        self.assertEqual(s["battery"], {"voltage_v": None, "percent": None, "charging": True})

    def test_decode_state_rejects_short_and_new_versions(self):
        with self.assertRaises(ValueError):
            b.decode_state(pack_state()[:20])
        with self.assertRaises(ValueError):
            b.decode_state(pack_state(version=2))

    def test_decode_event(self):
        e = b.decode_event(struct.pack(b.EVENT_FORMAT, 3, 0, 7, 1234))
        self.assertEqual((e["type"], e["seq"], e["uptime_ms"]), ("shake", 7, 1234))

    def test_encode_led(self):
        self.assertEqual(b.encode_led("red"), bytes([255, 0, 0, 0]))
        self.assertEqual(b.encode_led("#10Ff00", "breathe"), bytes([0x10, 0xFF, 0, 2]))
        for bad in (("pink", "solid"), ("#123", "solid"), ("red", "strobe")):
            with self.assertRaises(ValueError):
                b.encode_led(*bad)


class FakeClient:
    is_connected = True

    def __init__(self):
        self.writes = []

    async def read_gatt_char(self, uuid):
        assert uuid == b.STATE_UUID
        return bytearray(pack_state())

    async def write_gatt_char(self, uuid, data, response):
        self.writes.append((uuid, bytes(data)))


class SocketTest(unittest.TestCase):
    def test_read_and_led_over_socket(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as tmp:
                args = argparse.Namespace(socket=os.path.join(tmp, "bridge.sock"))
                bridge = b.Bridge(argparse.Namespace(address=None, **vars(args)))
                server = await bridge._serve()
                async with server:
                    loop = asyncio.get_running_loop()
                    call = lambda p: loop.run_in_executor(None, b.request, args.socket, p)

                    self.assertFalse((await call({"cmd": "read"}))["ok"])

                    bridge.client = FakeClient()
                    bridge.name = "MuseXiao-ABCD"
                    reply = await call({"cmd": "read"})
                    self.assertTrue(reply["ok"])
                    self.assertEqual(reply["device"], "MuseXiao-ABCD")
                    self.assertEqual(reply["temperature_c"], 25.12)

                    reply = await call({"cmd": "led", "colour": "blue", "mode": "blink"})
                    self.assertTrue(reply["ok"])
                    self.assertEqual(bridge.client.writes, [(b.LED_UUID, bytes([0, 0, 255, 1]))])

                    reply = await call({"cmd": "led", "colour": "nope"})
                    self.assertFalse(reply["ok"])

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
