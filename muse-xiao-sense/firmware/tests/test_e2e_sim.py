"""End-to-end test of the firmware, built for native_sim, against fakes.

The test plays the three parties the gadget talks to, using the Muse Gadget
SDK's own Python reference implementation wherever one exists:

* the Muse app, pairing over the setup channel with protocol v5 (the SDK's
  build_transcript / derive_session_keys / record encryption),
* the Muse API (/fetch_vms, /device_token/refresh),
* the Muse VM: a WebSocket that runs the SDK's NoiseXXResponder, accepts
  /link-control, checks link.register, invokes commands, receives chat and
  finally unpairs the device.

native_sim has no Bluetooth, so the firmware carries the exact GATT packet
stream over TCP instead (CONFIG_MUSE_SETUP_OVER_TCP). Run:

    pip install -e <muse-gadget-sdk>/linux pytest
    west build -b native_sim/native/64 firmware -d build-sim
    MUSE_SIM_EXE=build-sim/zephyr/zephyr.exe pytest firmware/tests -v
"""

from __future__ import annotations

import asyncio
import json
import os
import socket
import struct
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
import ssl
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from musegadget import pairing as ref
from musegadget.ble_framing import ChunkAssembler, encode_chunks
from musegadget.link_client import MessageDecoder, encode_message
from musegadget.noise import (
    ApplicationResponse, BodyChunk, NoiseFrameDecoder, NoiseXXResponder, ServiceFrame,
    encode_noise_frames,
)
from musegadget.noise.transport import decode_request_envelope, encode_response_envelope

EXE = os.environ.get("MUSE_SIM_EXE", "build-sim/zephyr/zephyr.exe")
SETUP_PORT, API_PORT, VM_PORT = 7700, 18443, 18444
CERTS = Path(__file__).parent / "certs"
API_URL = f"https://localhost:{API_PORT}"
NOISE_HOST = f"localhost:{VM_PORT}"
SDK_TOKEN = "mgst_test_only"  # CONFIG_MUSE_SDK_TOKEN in boards/native_sim_native_64.conf


def server_tls() -> ssl.SSLContext:
    """TLS 1.2 with an RSA certificate for localhost, signed by the test CA
    the native_sim build trusts (like DigiCert for the real endpoints)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(CERTS / "localhost_chain.pem", CERTS / "localhost.key")
    return ctx


# ---- The Muse API -------------------------------------------------------------

class FakeApi:
    def __init__(self) -> None:
        self.access_token = "access-1"
        self.refresh_token = "hatch_refresh:refresh-1"
        self.fetches: list[str] = []
        self.refreshes: list[tuple[str, dict]] = []
        api = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _json(self, status, obj):
                body = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                auth = self.headers.get("Authorization", "")
                api.fetches.append(auth)
                if self.path != "/fetch_vms" or auth != f"Bearer {api.access_token}":
                    return self._json(401, {"error_title": "bad token"})
                self._json(200, {"vm_list": [
                    {"vm_id": "vm-other", "vm_name": "other", "vm_auth_token": "nope",
                     "vm_ws_url": "wss://x"},
                    {"vm_id": "vm 1&x", "vm_name": "home", "vm_auth_token": "vm-bearer",
                     "vm_ws_url": "wss://x", "default": True},
                ]})

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                auth = self.headers.get("Authorization", "")
                api.refreshes.append((auth, body))
                raw = api.refresh_token.rsplit(":", 1)[-1]
                if self.path != "/device_token/refresh" or auth != f"Bearer hatch_refresh:{raw}":
                    return self._json(401, {})
                api.access_token, api.refresh_token = "access-2", "refresh-2"
                self._json(200, {"payload": {"access_token": api.access_token,
                                             "refresh_token": api.refresh_token}})

        self.server = ThreadingHTTPServer(("127.0.0.1", API_PORT), Handler)
        self.server.socket = server_tls().wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


# ---- The Muse VM ------------------------------------------------------------------

class FakeVm:
    """A WebSocket server speaking the VM side, scripted by `script`."""

    def __init__(self, script) -> None:
        self.script = script
        self.connections: list[dict] = []
        self.errors: list[BaseException] = []
        self.done = threading.Event()
        self.listening = threading.Event()
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        assert self.listening.wait(5), "fake VM could not listen"

    def _run(self) -> None:
        from websockets.asyncio.server import serve

        async def handler(ws):
            req = ws.request
            self.connections.append({"path": req.path, "auth": req.headers.get("Authorization")})
            try:
                await self.script(VmSession(ws))
            except BaseException as exc:  # surfaced by the test
                self.errors.append(exc)
            finally:
                self.done.set()

        async def main():
            self.stopped = asyncio.get_running_loop().create_future()
            async with serve(handler, "127.0.0.1", VM_PORT, max_size=None, ssl=server_tls()):
                self.listening.set()
                await self.stopped

        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(main())
        self.loop.close()

    def close(self) -> None:
        if not self.stopped.done():
            self.loop.call_soon_threadsafe(self.stopped.set_result, None)
        self.thread.join(10)


class VmSession:
    def __init__(self, ws) -> None:
        self.ws = ws
        self.decoder = NoiseFrameDecoder()
        self.messages = MessageDecoder()
        self.pending: list[dict] = []
        self.chats: list = []
        self.stream_id = 0

    async def handshake(self) -> None:
        responder = NoiseXXResponder()
        responder.initialize()
        await self.ws.send(responder.read_message1_and_write_message2(await self.ws.recv()))
        responder.read_message3(await self.ws.recv())
        self.send_cipher, self.recv_cipher = responder.split()

    async def next_frame(self) -> ServiceFrame:
        while True:
            raw = await asyncio.wait_for(self.ws.recv(), 30)
            assembled = self.decoder.decode(self.recv_cipher.decrypt_with_ad(b"", raw))
            if assembled is not None:
                return decode_request_envelope(assembled)

    async def send_frame(self, frame: ServiceFrame) -> None:
        for chunk in encode_noise_frames(encode_response_envelope(frame)):
            await self.ws.send(self.send_cipher.encrypt_with_ad(b"", chunk))

    async def accept_control_stream(self) -> ServiceFrame:
        request = await self.next_frame()
        self.stream_id = request.stream_id
        await self.send_frame(ServiceFrame.response(self.stream_id, ApplicationResponse(status=200)))
        return request

    async def next_message(self) -> dict:
        while not self.pending:
            frame = await self.next_frame()
            if frame.stream_id != self.stream_id:
                self.chats.append(frame)
                await self.send_frame(ServiceFrame.response(
                    frame.stream_id, ApplicationResponse(status=200, body=b"{}", end_body=True)))
                continue
            assert frame.kind == "body_chunk", frame.kind
            self.pending.extend(self.messages.feed(frame.value.data))
        return self.pending.pop(0)

    async def send_message(self, message: dict) -> None:
        await self.send_frame(ServiceFrame.body_chunk(self.stream_id, BodyChunk(data=encode_message(message))))

    async def invoke(self, command: str, params: dict | None = None) -> dict:
        invoke_id = f"inv-{command}"
        await self.send_message({"method": "link.invoke", "id": invoke_id, "command": command,
                                 "params": params or {}, "timeout_ms": 10000})
        while True:
            msg = await self.next_message()
            if msg.get("id") == invoke_id:
                return msg

    async def wait_for_chat(self, timeout: float = 15) -> ServiceFrame:
        deadline = time.monotonic() + timeout
        while not self.chats and time.monotonic() < deadline:
            try:
                frame = await asyncio.wait_for(self.next_frame(), 1)
            except asyncio.TimeoutError:
                continue
            self.chats.append(frame)
            await self.send_frame(ServiceFrame.response(
                frame.stream_id, ApplicationResponse(status=200, body=b"{}", end_body=True)))
        return self.chats[0]


# ---- The Muse app ------------------------------------------------------------------

class FakeApp:
    """The phone side of setup, over the TCP stand-in for GATT."""

    def __init__(self) -> None:
        self.sock = socket.create_connection(("127.0.0.1", SETUP_PORT), timeout=30)
        self.assembler = ChunkAssembler()

    def write(self, obj) -> None:
        data = obj if isinstance(obj, bytes) else json.dumps(obj, separators=(",", ":")).encode()
        for packet in encode_chunks(data, 185):
            self.sock.sendall(struct.pack(">H", len(packet)) + packet)

    def _read_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("device closed setup")
            buf += chunk
        return buf

    def read(self) -> bytes:
        while True:
            (n,) = struct.unpack(">H", self._read_exact(2))
            message = self.assembler.feed(self._read_exact(n))
            if message is not None:
                return message

    def read_json(self) -> dict:
        return json.loads(self.read())

    def pair(self, info: dict) -> None:
        key = ec.generate_private_key(ec.SECP256R1())
        mobile_pub = key.public_key().public_bytes(
            serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        mobile_nonce = os.urandom(16)
        self.write({"action": "pairing_client_hello", "version": 5, "pairing_auth": "none",
                    "pairing_policy": "confirm_app",
                    "mobile_pub": ref.b64url_encode(mobile_pub),
                    "mobile_nonce": ref.b64url_encode(mobile_nonce)})
        ready = self.read_json()
        assert ready["type"] == "pairing_ready", ready
        assert ready["node_id"] == info["node_id"] and ready["model"] == "hatch_link"

        # Recompute the transcript the way the app does and check the hash.
        transcript = ref.build_transcript(
            community=True, auth_epoch=0, policy="confirm_app",
            device_id=ready["device_id"], node_id=ready["node_id"], mac=ready["mac"],
            firmware_version=ready["firmware_version"],
            mobile_pub=ref.b64url_encode(mobile_pub), device_pub=ready["device_pub"],
            mobile_nonce=ref.b64url_encode(mobile_nonce), device_nonce=ready["device_nonce"])
        import hashlib
        transcript_hash = hashlib.sha256(transcript.encode()).digest()
        assert ready["transcript_hash"] == ref.b64url_encode(transcript_hash)

        device_pub = ec.EllipticCurvePublicKey.from_encoded_point(
            ec.SECP256R1(), ref.b64url_decode(ready["device_pub"]))
        secret = key.exchange(ec.ECDH(), device_pub)
        tx, rx, session_id = ref.derive_session_keys(
            secret, mobile_nonce, ref.b64url_decode(ready["device_nonce"]), transcript_hash)
        assert ready["session_id"] == ref.b64url_encode(session_id)
        self.tx, self.rx, self.sid = AESGCM(tx), AESGCM(rx), ready["session_id"]
        self.tx_counter = self.rx_counter = 0

    def send_encrypted(self, obj: dict) -> None:
        counter = self.tx_counter
        sealed = self.tx.encrypt(ref.record_nonce(0, counter),
                                 json.dumps(obj, separators=(",", ":")).encode(),
                                 ref.record_aad(self.sid, 0, counter))
        self.tx_counter += 1
        self.write({"action": "pairing_encrypted", "session_id": self.sid,
                    "counter": str(counter), "ciphertext": ref.b64url_encode(sealed[:-16]),
                    "tag": ref.b64url_encode(sealed[-16:])})

    def read_encrypted(self) -> dict:
        env = self.read_json()
        assert env["type"] == "pairing_encrypted", env
        assert env["counter"] == str(self.rx_counter)
        plain = self.rx.decrypt(
            ref.record_nonce(1, self.rx_counter),
            ref.b64url_decode(env["ciphertext"]) + ref.b64url_decode(env["tag"]),
            ref.record_aad(self.sid, 1, self.rx_counter))
        self.rx_counter += 1
        return json.loads(plain)

    def close(self) -> None:
        self.sock.close()


# ---- The device ---------------------------------------------------------------------

class Device:
    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.flash = tmp / "flash.bin"
        self.log_path = tmp / "device.log"
        self.proc = None

    def start(self) -> None:
        self.log = open(self.log_path, "a")
        self.proc = subprocess.Popen([os.path.abspath(EXE), f"--flash={self.flash}"],
                                     stdout=self.log, stderr=subprocess.STDOUT, cwd=self.tmp)

    def wait_log(self, text: str, timeout: float = 30) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text in self.log_path.read_text(errors="replace"):
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        raise AssertionError(f"{text!r} not in device log:\n{self.log_path.read_text()[-4000:]}")

    def wait_exit(self, timeout: float = 15) -> int:
        return self.proc.wait(timeout)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.proc:
            self.log.close()


@pytest.fixture
def world(tmp_path):
    if not Path(EXE).exists():
        pytest.skip(f"build the native_sim firmware first ({EXE})")
    api = FakeApi()
    device = Device(tmp_path)
    yield api, device
    device.stop()
    api.close()


def run_vm(vm: FakeVm, timeout: float = 60) -> None:
    assert vm.done.wait(timeout), "VM session did not finish"
    vm.close()
    if vm.errors:
        raise vm.errors[0]


def test_pair_connect_serve_commands_and_unpair(world):
    api, device = world
    seen: dict = {}

    async def vm_script(s: VmSession) -> None:
        await s.handshake()
        request = await s.accept_control_stream()
        assert (request.kind, request.value.verb, request.value.path) == ("request", "POST", "/link-control")
        register = await s.next_message()
        assert register["method"] == "link.register"
        seen["register"] = register["params"]
        await s.send_message({"type": "res", "id": register["id"], "ok": True})
        for command, params in [("device.health", {}), ("motion.read", {}),
                                ("sound.level", {"duration_ms": 200}),
                                ("light.set", {"color": "purple", "mode": "breathe"}),
                                ("light.set", {"color": "chartreuse"}),
                                ("system.run", {"command": "id"})]:
            seen.setdefault("results", []).append(await s.invoke(command, params))
        chat = await s.wait_for_chat()
        seen["chat"] = chat
        await s.send_message({"type": "evt", "event": "link.unpaired"})
        await asyncio.sleep(1)

    vm = FakeVm(vm_script)
    device.start()
    device.wait_log("setup open on TCP port")

    app = FakeApp()
    # Sensitive commands are refused in plaintext.
    app.write({"action": "wifi_scan"})
    assert app.read() == b"error_encryption_required"
    app.write({"action": "get_device_info"})
    info = app.read_json()
    assert info["type"] == "device_info" and info["pairing_protocol"] == 5
    assert info["node_id"].startswith("homelink-") and info["network_ready"] is True
    assert info["pairing_policy"] == "confirm_app" and info["model"] == "hatch_link"

    app.pair(info)
    app.send_encrypted({"action": "pairing_client_finished"})
    status = app.read_encrypted()
    assert status == {"type": "status", "status": "pairing_confirmed", "sdk_token": SDK_TOKEN}

    app.send_encrypted({"action": "wifi_scan"})
    scan = app.read_encrypted()
    assert scan["type"] == "wifi_scan_result" and scan["networks"][0]["secure"] is False

    app.send_encrypted({"action": "provision_v2", "ssid": scan["networks"][0]["ssid"],
                        "password": "", "access_token": api.access_token,
                        "refresh_token": api.refresh_token, "token_type": "device",
                        "username": "tester", "api_url": "",
                        "api_url_v2": API_URL, "noise_host": NOISE_HOST})
    statuses = [app.read_encrypted()["status"] for _ in range(3)]
    assert statuses == ["wifi_connecting", "wifi_connected", "auth_ok"]
    app.close()

    run_vm(vm)
    # The device rotated its tokens once to report the SDK token, then
    # fetched VMs with the new access token and picked the default VM.
    auth, body = api.refreshes[0]
    assert auth == "Bearer hatch_refresh:refresh-1"
    assert body == {"device_id": info["node_id"], "sdk_token": SDK_TOKEN}
    assert api.fetches[-1] == "Bearer access-2"
    assert vm.connections[0] == {"path": "/v1/noise?vm_id=vm%201%26x", "auth": "Bearer vm-bearer"}

    params = seen["register"]
    assert params["node_id"] == info["node_id"]
    assert (params["platform"], params["device_family"], params["model_id"]) == ("esp32", "link", "esp-link")
    assert set(params["commands_v2"]) == {"device.health", "motion.read", "sound.level", "light.set"}
    for spec in params["commands_v2"].values():
        assert set(spec) == {"description", "required", "optional"}

    health, motion, sound, light, bad_light, unknown = seen["results"]
    assert health["ok"] and health["payload"]["battery_percent"] is None
    assert motion["ok"] and motion["payload"]["accel_g"]["z"] == 1.0
    assert motion["payload"]["orientation"] == "lying flat, chips facing up"
    assert sound["ok"] and sound["payload"]["level_dbfs"] == -40.0
    assert light == {"method": "link.result", "id": "inv-light.set", "ok": True,
                     "payload": {"color": "#a000ff", "mode": "breathe"}}
    assert bad_light["ok"] is False and "colour" in bad_light["error"]
    assert unknown == {"method": "link.result", "id": "inv-system.run", "ok": False,
                       "error": "unsupported command: system.run"}

    chat = seen["chat"]
    assert (chat.kind, chat.value.verb, chat.value.path, chat.value.end_body) == (
        "request", "POST", "/chat/stream", True)
    assert json.loads(chat.value.body) == {
        "message": "The button on my XIAO gadget was pressed.", "output_modality": "text",
        "device_id": info["node_id"]}

    # link.unpaired: the device forgets the pairing and restarts into setup.
    device.wait_exit()
    device.start()
    device.wait_log("not paired: add MuseGadget")


def test_pairing_survives_a_restart(world):
    api, device = world
    registrations = []

    async def vm_script(s: VmSession) -> None:
        await s.handshake()
        await s.accept_control_stream()
        register = await s.next_message()
        registrations.append(register["params"]["node_id"])
        await s.send_message({"type": "res", "id": register["id"], "ok": True})
        await asyncio.sleep(1)

    device.start()
    device.wait_log("setup open on TCP port")
    app = FakeApp()
    app.write({"action": "get_device_info"})
    info = app.read_json()
    app.pair(info)
    app.send_encrypted({"action": "pairing_client_finished"})
    app.read_encrypted()
    app.send_encrypted({"action": "provision_v2", "ssid": "x", "password": "",
                        "access_token": api.access_token, "refresh_token": api.refresh_token,
                        "token_type": "device", "api_url_v2": API_URL,
                        "noise_host": NOISE_HOST})
    assert [app.read_encrypted()["status"] for _ in range(3)][-1] == "auth_ok"
    app.close()
    device.wait_log("device token rotated")
    device.stop()

    # Restarted from the same flash, it connects without setup.
    vm = FakeVm(vm_script)
    device.start()
    device.wait_log("paired; connecting to Muse")
    run_vm(vm)
    assert registrations == [info["node_id"]]


def test_bad_tokens_fail_setup(world):
    api, device = world
    device.start()
    device.wait_log("setup open on TCP port")
    app = FakeApp()
    app.write({"action": "get_device_info"})
    app.pair(app.read_json())
    app.send_encrypted({"action": "pairing_client_finished"})
    app.read_encrypted()
    app.send_encrypted({"action": "provision_v2", "ssid": "x", "password": "",
                        "access_token": "wrong", "refresh_token": "wrong",
                        "token_type": "device", "api_url_v2": API_URL})
    assert [app.read_encrypted()["status"] for _ in range(3)] == [
        "wifi_connecting", "wifi_connected", "auth_failed"]


def test_tampered_record_is_rejected(world):
    api, device = world
    device.start()
    device.wait_log("setup open on TCP port")
    app = FakeApp()
    app.write({"action": "get_device_info"})
    app.pair(app.read_json())
    app.tx = AESGCM(os.urandom(32))  # wrong key: the tag won't verify
    app.send_encrypted({"action": "pairing_client_finished"})
    # As in the SDK, the session is dropped; once pairing has started the
    # error is not sent in plaintext, and the device hangs up.
    with pytest.raises(ConnectionError):
        app.read()
    device.wait_log("setup client disconnected; clearing pairing session")


def test_session_stays_up_across_keepalive_pings(world):
    """Idle past the 20 s WebSocket ping, then the session still serves."""
    api, device = world
    seen = {}

    async def vm_script(s: VmSession) -> None:
        await s.handshake()
        await s.accept_control_stream()
        register = await s.next_message()
        await s.send_message({"type": "res", "id": register["id"], "ok": True})
        await asyncio.sleep(26)
        seen["health"] = await s.invoke("device.health")

    device.start()
    device.wait_log("setup open on TCP port")
    app = FakeApp()
    app.write({"action": "get_device_info"})
    app.pair(app.read_json())
    app.send_encrypted({"action": "pairing_client_finished"})
    app.read_encrypted()
    vm = FakeVm(vm_script)
    app.send_encrypted({"action": "provision_v2", "ssid": "x", "password": "",
                        "access_token": api.access_token, "refresh_token": api.refresh_token,
                        "token_type": "device", "api_url_v2": API_URL, "noise_host": NOISE_HOST})
    assert [app.read_encrypted()["status"] for _ in range(3)][-1] == "auth_ok"
    app.close()
    run_vm(vm, timeout=90)
    assert seen["health"]["ok"] is True
    assert "no traffic from the VM" not in device.log_path.read_text()
