#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/random/random.h>

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>

extern "C" {
#include "board.h"
#include "cJSON.h"
#include "commands.h"
#include "identity.h"
#include "net.h"
}
#include "link.h"
#include "mem.h"

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

using namespace musegadgets::noise::core;

namespace {

constexpr const char *NOISE_PATH = "/v1/noise";
constexpr const char *CONTROL_PATH = "/link-control";
constexpr const char *CHAT_PATH = "/chat/stream";
constexpr const char *APP_ID = "musegadget";
constexpr int64_t CONTROL_STREAM_ID = 1;
constexpr int CONNECT_TIMEOUT_MS = 20000;
constexpr int HANDSHAKE_TIMEOUT_MS = 20000;
constexpr int PING_INTERVAL_MS = 20000;
constexpr int SILENCE_LIMIT_MS = 3 * PING_INTERVAL_MS;
constexpr int POLL_MS = 100;

/* Buffer sizes follow the SDK's ESP32 firmware for its smallest boards
 * (noise_control.cpp, CARDPUTER_CONTROL_SESSION): this session carries only
 * control JSON and small chat requests, never voice or the tunnel. */
constexpr size_t WS_TX_SIZE = 4 * 1024;
constexpr size_t SVC_SCRATCH = 10 * 1024;
constexpr size_t WS_RX_SIZE = SVC_SCRATCH + 64;
constexpr size_t OUT_SVC_SCRATCH = 3 * 1024;
constexpr size_t OUT_ENV_SCRATCH = 3 * 1024;
constexpr size_t BODY_CHUNK_MAX = 2048;
constexpr size_t CONTROL_INBOX_MAX = 6 * 1024;
constexpr size_t MAX_HEADERS = 16;

uint8_t ws_tmp[1536]; /* HTTP upgrade buffer, then the WebSocket's receive buffer */

struct Buffers {
	uint8_t ws_tx[WS_TX_SIZE];
	uint8_t ws_rx[WS_RX_SIZE];
	uint8_t out_svc[OUT_SVC_SCRATCH];
	uint8_t out_env[OUT_ENV_SCRATCH];
	uint8_t tf[SVC_SCRATCH];
	uint8_t sr[SVC_SCRATCH];
	uint8_t inbox[CONTROL_INBOX_MAX];
};

volatile bool s_registered;
K_MSGQ_DEFINE(s_chat_q, sizeof(char *), 4, 4);

int s_upgrade_status;

int on_headers_complete(struct http_parser *parser)
{
	s_upgrade_status = parser->status_code;
	return 0;
}

const struct http_parser_settings UPGRADE_CB = [] {
	struct http_parser_settings s = {};
	s.on_headers_complete = on_headers_complete;
	return s;
}();

void url_encode(const char *in, char *out, size_t size)
{
	static const char SAFE[] = "-_.!~*'()";
	size_t o = 0;
	for (; *in && o + 4 < size; in++) {
		unsigned char c = static_cast<unsigned char>(*in);
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    strchr(SAFE, c)) {
			out[o++] = static_cast<char>(c);
		} else {
			o += snprintf(out + o, size - o, "%%%02X", c);
		}
	}
	out[o] = '\0';
}

void make_uuid(char out[37])
{
	uint8_t b[16];
	sys_rand_get(b, sizeof(b));
	b[6] = (b[6] & 0x0F) | 0x40;
	b[8] = (b[8] & 0x3F) | 0x80;
	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
		 b[13], b[14], b[15]);
}

class Session {
public:
	Session(int ws, Buffers &buf) : ws_(ws), buf_(buf), session_(crypto_) {}

	enum link_outcome run(int64_t *registered_ms);

private:
	bool send_ws(const uint8_t *data, size_t len)
	{
		int rc = websocket_send_msg(ws_, data, len, WEBSOCKET_OPCODE_DATA_BINARY, true, true,
					    CONNECT_TIMEOUT_MS);
		if (rc < 0) {
			LOG_WRN("WebSocket send failed: %d", rc);
			return false;
		}
		return true;
	}

	/* Receives one whole WebSocket message into ws_rx. Returns its length,
	 * 0 on timeout, or <0 when the connection is gone. */
	int recv_ws(int timeout_ms);
	bool handshake();
	bool drain();
	bool send_message(cJSON *msg);
	bool open_control_stream();
	bool send_chat(const char *message);
	bool on_frame(const DecodedServiceFrame &frame, enum link_outcome *outcome);
	bool on_control_data(const uint8_t *data, size_t len, enum link_outcome *outcome);
	bool handle_message(cJSON *msg, enum link_outcome *outcome);
	bool invoke(cJSON *msg);

	int ws_;
	Buffers &buf_;
	PsaCryptoBackend crypto_;
	ClientSession session_;
	int64_t next_stream_id_ = CONTROL_STREAM_ID + 1;
	char register_id_[37] = {};
	size_t inbox_len_ = 0;
	int64_t last_rx_ = 0;
	int64_t registered_ms_ = 0;
};

int Session::recv_ws(int timeout_ms)
{
	size_t got = 0;
	int64_t deadline = k_uptime_get() + timeout_ms;
	for (;;) {
		uint32_t type = 0;
		uint64_t remaining = 0;
		int wait = static_cast<int>(MAX(0, deadline - k_uptime_get()));
		if (got) {
			wait = CONNECT_TIMEOUT_MS; /* finish a message once started */
		}
		int rc = websocket_recv_msg(ws_, buf_.ws_rx + got, sizeof(buf_.ws_rx) - got, &type,
					    &remaining, wait);
		if (rc == -EAGAIN || rc == -ETIMEDOUT) {
			if (got == 0) {
				return 0;
			}
			continue;
		}
		if (rc < 0) {
			return rc;
		}
		last_rx_ = k_uptime_get();
		if (type & WEBSOCKET_FLAG_CLOSE) {
			LOG_INF("VM closed the WebSocket");
			return -ENOTCONN;
		}
		if (type & WEBSOCKET_FLAG_PING) {
			websocket_send_msg(ws_, buf_.ws_rx + got, rc, WEBSOCKET_OPCODE_PONG, true, true,
					   CONNECT_TIMEOUT_MS);
			continue;
		}
		if (type & WEBSOCKET_FLAG_PONG) {
			continue;
		}
		if (type & WEBSOCKET_FLAG_TEXT) {
			LOG_WRN("ignoring text frame on Noise connection");
			continue;
		}
		got += rc;
		if (remaining == 0 && (type & WEBSOCKET_FLAG_FINAL)) {
			return static_cast<int>(got);
		}
		if (got >= sizeof(buf_.ws_rx)) {
			LOG_ERR("inbound WebSocket message too large");
			return -EMSGSIZE;
		}
	}
}

bool Session::handshake()
{
	auto m1 = session_.WriteHandshakeMessage1(ByteSpan(buf_.ws_tx, sizeof(buf_.ws_tx)));
	if (!m1.ok() || !send_ws(buf_.ws_tx, m1.size())) {
		return false;
	}
	int n = recv_ws(HANDSHAKE_TIMEOUT_MS);
	if (n <= 0) {
		LOG_WRN("no Noise handshake reply: %d", n);
		return false;
	}
	uint8_t extra[256];
	size_t extra_len = 0;
	if (!session_.ReadHandshakeMessage2(ConstByteSpan(buf_.ws_rx, n), ByteSpan(extra, sizeof(extra)),
					    extra_len).ok()) {
		LOG_WRN("bad Noise handshake message 2");
		return false;
	}
	/* The bearer already authenticated us at the upgrade; message 3 carries
	 * an empty payload. */
	auto m3 = session_.WriteHandshakeMessage3(ConstByteSpan(), ByteSpan(buf_.ws_tx, sizeof(buf_.ws_tx)));
	if (!m3.ok() || !send_ws(buf_.ws_tx, m3.size())) {
		return false;
	}
	LOG_INF("Noise session established");
	return true;
}

bool Session::drain()
{
	while (session_.HasOutboundWebSocketPayload()) {
		auto r = session_.WriteNextOutboundWebSocketPayload(ByteSpan(buf_.ws_tx, sizeof(buf_.ws_tx)));
		if (!r.ok() || !send_ws(buf_.ws_tx, r.size())) {
			LOG_WRN("outbound frame failed");
			return false;
		}
	}
	return true;
}

bool Session::send_message(cJSON *msg)
{
	char *json = cJSON_PrintUnformatted(msg);
	if (!json) {
		return false;
	}
	size_t len = strlen(json);
	/* Each message is its length as a little-endian u32, then the JSON; the
	 * VM reassembles across body chunks by that prefix. */
	uint8_t prefix[4] = {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
			     static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24)};
	bool ok = true;
	size_t off = 0;
	bool first = true;
	while (ok && (first || off < len)) {
		static uint8_t chunk[BODY_CHUNK_MAX];
		size_t n = 0;
		if (first) {
			memcpy(chunk, prefix, 4);
			n = 4;
			first = false;
		}
		size_t take = MIN(sizeof(chunk) - n, len - off);
		memcpy(chunk + n, json + off, take);
		n += take;
		off += take;
		BodyChunkView view{ConstByteSpan(chunk, n), false};
		ok = session_.StartOutboundBodyChunk(ServiceType::Daemon, CONTROL_STREAM_ID, view,
						     ByteSpan(buf_.out_svc, sizeof(buf_.out_svc)),
						     ByteSpan(buf_.out_env, sizeof(buf_.out_env))).ok() &&
		     drain();
	}
	mem_free(json);
	return ok;
}

bool Session::open_control_stream()
{
	ApplicationRequestView req{};
	req.verb = "POST";
	req.path = CONTROL_PATH;
	req.end_body = false;
	if (!session_.StartOutboundApplicationRequest(ServiceType::Daemon, CONTROL_STREAM_ID, req,
						      ByteSpan(buf_.out_svc, sizeof(buf_.out_svc)),
						      ByteSpan(buf_.out_env, sizeof(buf_.out_env))).ok() ||
	    !drain()) {
		return false;
	}

	const struct identity *id = identity_get();
	make_uuid(register_id_);
	cJSON *msg = cJSON_CreateObject();
	cJSON_AddStringToObject(msg, "type", "req");
	cJSON_AddStringToObject(msg, "id", register_id_);
	cJSON_AddStringToObject(msg, "method", "link.register");
	cJSON *params = cJSON_AddObjectToObject(msg, "params");
	cJSON_AddStringToObject(params, "node_id", id->node_id);
	cJSON_AddStringToObject(params, "display_name", CONFIG_MUSE_DISPLAY_NAME);
	cJSON_AddStringToObject(params, "platform", CONFIG_MUSE_REGISTER_PLATFORM);
	cJSON_AddStringToObject(params, "version", CONFIG_MUSE_FW_VERSION);
	cJSON_AddStringToObject(params, "device_family", "link");
	cJSON_AddStringToObject(params, "model_id", CONFIG_MUSE_REGISTER_MODEL_ID);
	cJSON_AddBoolToObject(params, "is_wakeup_supported", false);
	const char *ssid = net_current_ssid();
	if (ssid && *ssid) {
		cJSON *metadata = cJSON_AddObjectToObject(params, "metadata");
		cJSON_AddStringToObject(metadata, "network_ssid", ssid);
	}
	cJSON_AddItemToObject(params, "commands_v2", commands_specs());
	bool ok = send_message(msg);
	cJSON_Delete(msg);
	LOG_INF("sent link.register as %s", id->node_id);
	return ok;
}

bool Session::send_chat(const char *message)
{
	const struct identity *id = identity_get();
	cJSON *body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "message", message);
	cJSON_AddStringToObject(body, "output_modality", "text");
	cJSON_AddStringToObject(body, "device_id", id->node_id);
	char *json = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	if (!json) {
		return false;
	}
	char request_id[37];
	make_uuid(request_id);
	HeaderView headers[] = {
		{"Content-Type", "application/json"},
		{"x-request-id", request_id},
		{"x-app-id", APP_ID},
	};
	ApplicationRequestView req{};
	req.verb = "POST";
	req.path = CHAT_PATH;
	req.headers = Span<const HeaderView>(headers, 3);
	req.body = ConstByteSpan(reinterpret_cast<const uint8_t *>(json), strlen(json));
	req.end_body = true;
	int64_t stream = next_stream_id_++;
	bool ok = session_.StartOutboundApplicationRequest(ServiceType::Daemon, stream, req,
							   ByteSpan(buf_.out_svc, sizeof(buf_.out_svc)),
							   ByteSpan(buf_.out_env, sizeof(buf_.out_env))).ok() &&
		  drain();
	LOG_INF("chat message sent on stream %lld (%u chars)", (long long)stream,
		(unsigned)strlen(message));
	mem_free(json);
	return ok;
}

bool Session::invoke(cJSON *msg)
{
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
	const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "command"));
	const cJSON *params = cJSON_GetObjectItem(msg, "params");
	if (!id || !*id) {
		return true;
	}
	LOG_INF("invoke %s", command ? command : "?");
	cJSON *result = commands_run(command ? command : "", cJSON_IsObject(params) ? params : nullptr);
	cJSON *reply = cJSON_CreateObject();
	cJSON_AddStringToObject(reply, "method", "link.result");
	cJSON_AddStringToObject(reply, "id", id);
	cJSON *item;
	while ((item = result->child) != nullptr) {
		cJSON_AddItemToObject(reply, item->string, cJSON_DetachItemViaPointer(result, item));
	}
	cJSON_Delete(result);
	bool ok = send_message(reply);
	cJSON_Delete(reply);
	return ok;
}

bool Session::handle_message(cJSON *msg, enum link_outcome *outcome)
{
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
	cJSON *method = cJSON_GetObjectItem(msg, "method");
	if (id && strcmp(id, register_id_) == 0 && (!method || cJSON_IsNull(method))) {
		cJSON *error = cJSON_GetObjectItem(msg, "error");
		if (error && !cJSON_IsNull(error)) {
			char *text = cJSON_PrintUnformatted(error);
			LOG_ERR("link.register rejected: %s", text ? text : "?");
			mem_free(text);
		} else {
			registered_ms_ = k_uptime_get();
			s_registered = true;
			board_set_status(BOARD_STATUS_ONLINE);
			LOG_INF("registered with the Muse");
			mem_log("registered");
		}
		return true;
	}
	const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "event"));
	if (event && (strcmp(event, "link.unpaired") == 0 || strcmp(event, "node.unpaired") == 0)) {
		LOG_WRN("the Muse removed this device");
		*outcome = LINK_UNPAIRED;
		return false;
	}
	const char *m = cJSON_GetStringValue(method);
	if (m && strcmp(m, "link.invoke") == 0) {
		return invoke(msg);
	}
	return true;
}

bool Session::on_control_data(const uint8_t *data, size_t len, enum link_outcome *outcome)
{
	if (inbox_len_ + len > sizeof(buf_.inbox)) {
		LOG_ERR("control message too large");
		return false;
	}
	memcpy(buf_.inbox + inbox_len_, data, len);
	inbox_len_ += len;
	size_t off = 0;
	bool keep_going = true;
	while (keep_going && inbox_len_ - off >= 4) {
		const uint8_t *p = buf_.inbox + off;
		uint32_t n = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
		if (n > sizeof(buf_.inbox) - 4) {
			LOG_ERR("inbound control message too large: %u", n);
			return false;
		}
		if (inbox_len_ - off - 4 < n) {
			break;
		}
		off += 4 + n;
		if (n == 0) {
			continue; /* keepalive */
		}
		cJSON *msg = cJSON_ParseWithLength(reinterpret_cast<const char *>(p + 4), n);
		if (cJSON_IsObject(msg)) {
			keep_going = handle_message(msg, outcome);
		} else {
			LOG_WRN("dropping malformed control message (%u bytes)", n);
		}
		cJSON_Delete(msg);
	}
	memmove(buf_.inbox, buf_.inbox + off, inbox_len_ - off);
	inbox_len_ -= off;
	return keep_going;
}

bool Session::on_frame(const DecodedServiceFrame &frame, enum link_outcome *outcome)
{
	if (frame.stream_id != CONTROL_STREAM_ID) {
		/* Replies to our chat requests: only the status matters. */
		if (frame.kind == ServiceFrameKind::Response) {
			LOG_INF("stream %lld: HTTP %d", (long long)frame.stream_id, frame.response.status);
		} else if (frame.kind == ServiceFrameKind::Reset) {
			LOG_WRN("stream %lld reset", (long long)frame.stream_id);
		}
		return true;
	}
	ConstByteSpan data;
	bool ended = false;
	switch (frame.kind) {
	case ServiceFrameKind::Reset:
		LOG_WRN("control stream reset");
		*outcome = LINK_CLOSED;
		return false;
	case ServiceFrameKind::Response:
		if (frame.response.status >= 400) {
			LOG_WRN("/link-control refused: HTTP %d", frame.response.status);
			*outcome = frame.response.status == 403 ? LINK_FORBIDDEN : LINK_CLOSED;
			return false;
		}
		data = frame.response.body;
		ended = frame.response.end_body;
		break;
	case ServiceFrameKind::BodyChunk:
		data = frame.body_chunk.data;
		ended = frame.body_chunk.end_body;
		break;
	default:
		return true;
	}
	if (data.size() && !on_control_data(data.data(), data.size(), outcome)) {
		return false;
	}
	if (ended) {
		LOG_INF("control stream ended by VM");
		*outcome = LINK_CLOSED;
		return false;
	}
	return true;
}

enum link_outcome Session::run(int64_t *registered_ms)
{
	enum link_outcome outcome = LINK_CLOSED;
	if (!handshake() || !open_control_stream()) {
		return LINK_CLOSED;
	}
	last_rx_ = k_uptime_get();
	int64_t last_ping = k_uptime_get();
	HeaderView headers[MAX_HEADERS];
	for (;;) {
		char *chat = nullptr;
		if (s_registered && k_msgq_get(&s_chat_q, &chat, K_NO_WAIT) == 0) {
			bool ok = send_chat(chat);
			mem_free(chat);
			if (!ok) {
				break;
			}
		}
		int64_t now = k_uptime_get();
		if (now - last_ping >= PING_INTERVAL_MS) {
			last_ping = now;
			websocket_send_msg(ws_, nullptr, 0, WEBSOCKET_OPCODE_PING, true, true, 5000);
		}
		if (now - last_rx_ > SILENCE_LIMIT_MS) {
			LOG_WRN("no traffic from the VM; reconnecting");
			break;
		}
		int n = recv_ws(POLL_MS);
		if (n == 0) {
			continue;
		}
		if (n < 0) {
			LOG_INF("control connection closed: %d", n);
			break;
		}
		auto res = session_.ProcessInboundWebSocketPayload(
			ConstByteSpan(buf_.ws_rx, n), ByteSpan(buf_.tf, sizeof(buf_.tf)),
			ByteSpan(buf_.sr, sizeof(buf_.sr)), Span<HeaderView>(headers, MAX_HEADERS));
		if (!res.ok() || res.frame_status == InboundFrameStatus::Violation) {
			LOG_WRN("inbound frame rejected");
			break;
		}
		if (res.frame_status == InboundFrameStatus::Complete && !on_frame(res.frame, &outcome)) {
			break;
		}
	}
	*registered_ms = registered_ms_;
	return outcome;
}

} // namespace

extern "C" bool link_is_registered(void)
{
	return s_registered;
}

extern "C" bool link_send_chat(const char *message)
{
	if (!s_registered || !message || !*message) {
		return false;
	}
	size_t len = strlen(message);
	char *copy = static_cast<char *>(mem_alloc(len + 1));
	if (!copy) {
		return false;
	}
	memcpy(copy, message, len + 1);
	if (k_msgq_put(&s_chat_q, &copy, K_NO_WAIT) != 0) {
		mem_free(copy);
		return false;
	}
	return true;
}

static bool split_host(const char *noise_host, char *host, size_t size, uint16_t *port, bool *tls)
{
	*tls = true;
	const char *p = noise_host;
	if (strncmp(p, "wss://", 6) == 0) {
		p += 6;
	} else if (IS_ENABLED(CONFIG_MUSE_ALLOW_PLAINTEXT) && strncmp(p, "ws://", 5) == 0) {
		p += 5;
		*tls = false;
	}
	size_t n = strcspn(p, ":/");
	if (n == 0 || n >= size) {
		return false;
	}
	memcpy(host, p, n);
	host[n] = '\0';
	*port = p[n] == ':' ? static_cast<uint16_t>(atoi(p + n + 1)) : (*tls ? 443 : 80);
	return true;
}

extern "C" enum link_outcome link_run(const char *noise_host, const char *vm_id,
				      const char *vm_auth_token, int64_t *registered_ms)
{
	*registered_ms = 0;
	s_registered = false;
	char host[96];
	uint16_t port;
	bool tls;
	if (!split_host(noise_host, host, sizeof(host), &port, &tls)) {
		LOG_ERR("bad noise host: %s", noise_host);
		return LINK_CLOSED;
	}
	int sock = net_open(host, port, tls, CONNECT_TIMEOUT_MS);
	if (sock < 0) {
		return LINK_CLOSED;
	}

	static char url[200], vm_enc[160], auth[1100];
	url_encode(vm_id, vm_enc, sizeof(vm_enc));
	snprintf(url, sizeof(url), "%s?vm_id=%s", NOISE_PATH, vm_enc);
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", vm_auth_token);
	const char *headers[] = {
		auth,
		"User-Agent: musegadget-xiao/" CONFIG_MUSE_FW_VERSION " (Seeed XIAO nRF52840 Sense; Zephyr)\r\n",
		nullptr,
	};
	struct websocket_request req = {};
	req.host = host;
	req.url = url;
	req.optional_headers = headers;
	req.http_cb = &UPGRADE_CB;
	req.tmp_buf = ws_tmp;
	req.tmp_buf_len = sizeof(ws_tmp);
	s_upgrade_status = 0;
	int ws = websocket_connect(sock, &req, CONNECT_TIMEOUT_MS, nullptr);
	if (ws < 0) {
		LOG_WRN("VM refused connection: HTTP %d (%d)", s_upgrade_status, ws);
		zsock_close(sock);
		if (s_upgrade_status == 401) {
			return LINK_AUTH_REJECTED;
		}
		return s_upgrade_status == 403 ? LINK_FORBIDDEN : LINK_CLOSED;
	}
	LOG_INF("WebSocket upgraded to %s", NOISE_PATH);

	auto *buf = static_cast<Buffers *>(mem_alloc(sizeof(Buffers)));
	enum link_outcome outcome = LINK_CLOSED;
	if (!buf) {
		LOG_ERR("no memory for session buffers (%u bytes)", (unsigned)sizeof(Buffers));
	} else {
		/* Session holds crypto state; keep it off the thread stack. */
		static uint8_t session_mem[sizeof(Session)] __aligned(8);
		auto *session = new (session_mem) Session(ws, *buf);
		outcome = session->run(registered_ms);
		session->~Session();
		mem_free(buf);
	}
	s_registered = false;
	char *pending;
	while (k_msgq_get(&s_chat_q, &pending, K_NO_WAIT) == 0) {
		mem_free(pending);
	}
	/* Closing the WebSocket leaves the TCP/TLS socket under it open. */
	websocket_disconnect(ws);
	zsock_close(sock);
	return outcome;
}
