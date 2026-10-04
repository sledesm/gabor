#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "ble_framing.h"
#include "cJSON.h"
#include "mem.h"
#include "identity.h"
#include "muse_api.h"
#include "net.h"
#include "pairing.h"
#include "setup.h"
#include "store.h"

LOG_MODULE_REGISTER(setup, LOG_LEVEL_INF);

#define DISCONNECT_AFTER_ERROR_MS 300
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define MAX_SCAN 16

static const char *const SENSITIVE_ACTIONS[] = {
	"provision", "provision_v2", "wifi_scan", "ota", "device.ota",
	"unpair", "set_wifi", "set_auth",
};
static const char *const PLAINTEXT_STATUSES[] = {
	"error_encryption_required", "error_pairing_invalid_hello",
	"error_pairing_unavailable", "error_pairing_decrypt",
};

struct inbox_msg {
	void *fifo_reserved;
	size_t len;
	char data[];
};

static const struct setup_transport *s_t;
static void (*s_on_complete)(void);
static struct framing_assembler s_asm;
static K_FIFO_DEFINE(s_inbox);
static K_MUTEX_DEFINE(s_tx_lock);
static K_MUTEX_DEFINE(s_state_lock);
static bool s_plaintext_blocked;
static bool s_provisioning;

/* One provisioning job at a time, run on its own thread. */
static struct {
	char ssid[33];
	char password[65];
	struct pairing_record rec;
	uint32_t generation;
} s_job;
static K_SEM_DEFINE(s_job_sem, 0, 1);

static bool in_list(const char *s, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(s, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* ---- Sending ------------------------------------------------------------ */

static void send_locked(const char *text)
{
	size_t len = strlen(text);
	size_t packet = framing_packet_size(s_t->mtu());
	size_t usable = packet - FRAMING_HEADER;
	size_t total = len ? (len + usable - 1) / usable : 1;
	if (total > FRAMING_MAX_CHUNKS) {
		LOG_ERR("message too long for BLE (%u bytes)", (unsigned)len);
		return;
	}
	uint8_t buf[FRAMING_MAX_PACKET];
	for (size_t i = 0; i < total; i++) {
		size_t off = i * usable;
		size_t n = MIN(usable, len - off);
		buf[0] = FRAMING_MAGIC;
		buf[1] = (uint8_t)i;
		buf[2] = (uint8_t)total;
		memcpy(buf + FRAMING_HEADER, text + off, n);
		if (s_t->send(buf, FRAMING_HEADER + n) < 0) {
			LOG_WRN("notify failed");
			return;
		}
	}
}

static void send_json(cJSON *obj)
{
	char *text = cJSON_PrintUnformatted(obj);
	if (!text) {
		return;
	}
	k_mutex_lock(&s_tx_lock, K_FOREVER);
	send_locked(text);
	k_mutex_unlock(&s_tx_lock);
	LOG_INF("TX %s", cJSON_GetStringValue(cJSON_GetObjectItem(obj, "type")));
	mem_free(text);
}

static void send_encrypted_json(cJSON *obj, uint32_t generation)
{
	char *plain = cJSON_PrintUnformatted(obj);
	if (!plain) {
		return;
	}
	k_mutex_lock(&s_tx_lock, K_FOREVER);
	char *env = pairing_encrypt_json(plain, generation);
	if (env) {
		send_locked(env);
	}
	k_mutex_unlock(&s_tx_lock);
	LOG_INF("TX %s%s", cJSON_GetStringValue(cJSON_GetObjectItem(obj, "type")),
		env ? " (encrypted)" : " suppressed: no session");
	mem_free(env);
	mem_free(plain);
}

static void send_status(const char *status, uint32_t generation)
{
	k_mutex_lock(&s_tx_lock, K_FOREVER);
	char *env = pairing_encrypt_status(status, generation);
	if (env) {
		send_locked(env);
		k_mutex_unlock(&s_tx_lock);
		LOG_INF("TX status (encrypted): %s", status);
		mem_free(env);
		return;
	}
	k_mutex_lock(&s_state_lock, K_FOREVER);
	bool blocked = s_plaintext_blocked;
	k_mutex_unlock(&s_state_lock);
	if (generation || blocked ||
	    !in_list(status, PLAINTEXT_STATUSES, ARRAY_SIZE(PLAINTEXT_STATUSES))) {
		k_mutex_unlock(&s_tx_lock);
		LOG_INF("TX status suppressed: %s", status);
		return;
	}
	s_t->send((const uint8_t *)status, strlen(status));
	k_mutex_unlock(&s_tx_lock);
	LOG_INF("TX status: %s", status);
}

/* ---- Commands ----------------------------------------------------------- */

static void send_device_info(void)
{
	const struct identity *id = identity_get();
	cJSON *info = cJSON_CreateObject();
	cJSON_AddStringToObject(info, "type", "device_info");
	cJSON_AddStringToObject(info, "node_id", id->node_id);
	cJSON_AddStringToObject(info, "version", CONFIG_MUSE_FW_VERSION);
	pairing_add_device_info(info);
	cJSON_AddStringToObject(info, "build_sha", "");
	cJSON_AddBoolToObject(info, "network_ready", net_is_online());
	send_json(info);
	cJSON_Delete(info);
}

static void handle_wifi_scan(void)
{
	static struct net_scan_entry entries[MAX_SCAN];
	int n = 0;
	if (net_has_wifi()) {
		n = net_wifi_scan(entries, MAX_SCAN);
	} else if (net_is_online()) {
		/* Already online: offer the current connection, marked open so the
		 * app skips the password, as the SDK's Linux gadget does. */
		strcpy(entries[0].ssid, "Use current connection");
		entries[0].rssi = -40;
		entries[0].secure = false;
		n = 1;
	}
	cJSON *msg = cJSON_CreateObject();
	cJSON_AddStringToObject(msg, "type", "wifi_scan_result");
	cJSON *arr = cJSON_AddArrayToObject(msg, "networks");
	for (int i = 0; i < n; i++) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "ssid", entries[i].ssid);
		cJSON_AddNumberToObject(e, "rssi", entries[i].rssi);
		cJSON_AddBoolToObject(e, "secure", entries[i].secure);
		cJSON_AddItemToArray(arr, e);
	}
	send_encrypted_json(msg, 0);
	cJSON_Delete(msg);
}

static const char *text_of(const cJSON *cmd, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(cmd, key);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static bool copy_field(char *dst, size_t size, const char *src)
{
	if (strlen(src) >= size) {
		return false;
	}
	strcpy(dst, src);
	return true;
}

static void handle_provision(const cJSON *cmd)
{
	const char *token_type = text_of(cmd, "token_type");
	if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cmd, "ssid")) ||
	    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cmd, "password")) ||
	    !*text_of(cmd, "access_token") || !*text_of(cmd, "refresh_token") ||
	    strcmp(token_type, "device") != 0) {
		send_status("error_missing_credentials", 0);
		return;
	}
	k_mutex_lock(&s_state_lock, K_FOREVER);
	if (s_provisioning) {
		k_mutex_unlock(&s_state_lock);
		send_status("error_operation_in_progress", 0);
		return;
	}
	uint32_t generation = pairing_mark_provisioning();
	if (!generation) {
		k_mutex_unlock(&s_state_lock);
		send_status("error_pairing_confirm_required", 0);
		return;
	}
	memset(&s_job, 0, sizeof(s_job));
	const char *api_url = text_of(cmd, "api_url");
	const char *api_url_v2 = text_of(cmd, "api_url_v2");
	bool ok = copy_field(s_job.ssid, sizeof(s_job.ssid), text_of(cmd, "ssid")) &&
		  copy_field(s_job.password, sizeof(s_job.password), text_of(cmd, "password")) &&
		  copy_field(s_job.rec.access_token, STORE_TOKEN_MAX, text_of(cmd, "access_token")) &&
		  copy_field(s_job.rec.refresh_token, STORE_TOKEN_MAX, text_of(cmd, "refresh_token")) &&
		  copy_field(s_job.rec.username, sizeof(s_job.rec.username), text_of(cmd, "username")) &&
		  copy_field(s_job.rec.api_url, STORE_URL_MAX,
			     muse_api_url_allowed(api_url) ? api_url : "") &&
		  copy_field(s_job.rec.api_url_v2, STORE_URL_MAX,
			     muse_api_url_allowed(api_url_v2) ? api_url_v2 : "") &&
		  copy_field(s_job.rec.noise_host, STORE_URL_MAX, text_of(cmd, "noise_host"));
	if (!ok) {
		k_mutex_unlock(&s_state_lock);
		send_status("error_missing_credentials", generation);
		return;
	}
	s_job.generation = generation;
	s_provisioning = true;
	k_mutex_unlock(&s_state_lock);
	k_sem_give(&s_job_sem);
}

static void handle_message(const char *raw, size_t len, bool decrypted);

static void handle_record(const cJSON *envelope)
{
	char *plain = pairing_decrypt(envelope);
	if (!plain) {
		send_status(PAIRING_ERROR_DECRYPT, 0);
		s_t->disconnect(DISCONNECT_AFTER_ERROR_MS);
		return;
	}
	handle_message(plain, strlen(plain), true);
	mem_free(plain);
}

static void handle_message(const char *raw, size_t len, bool decrypted)
{
	cJSON *cmd = cJSON_ParseWithLength(raw, len);
	if (!cJSON_IsObject(cmd)) {
		LOG_WRN("invalid command JSON (%u bytes)", (unsigned)len);
		send_status("error_invalid_command", 0);
		cJSON_Delete(cmd);
		return;
	}
	const char *action = text_of(cmd, "action");
	LOG_INF("RX action: %s%s", *action ? action : "?", decrypted ? " (encrypted)" : "");

	k_mutex_lock(&s_state_lock, K_FOREVER);
	bool blocked = s_plaintext_blocked;
	k_mutex_unlock(&s_state_lock);
	bool sensitive = in_list(action, SENSITIVE_ACTIONS, ARRAY_SIZE(SENSITIVE_ACTIONS));

	if (!decrypted && strcmp(action, "pairing_client_hello") == 0) {
		const char *err = NULL;
		cJSON *ready = pairing_handle_hello(cmd, &err);
		if (!ready) {
			send_status(err, 0);
		} else {
			k_mutex_lock(&s_state_lock, K_FOREVER);
			s_plaintext_blocked = true;
			k_mutex_unlock(&s_state_lock);
			send_json(ready);
			cJSON_Delete(ready);
		}
	} else if (!decrypted && strcmp(action, "pairing_encrypted") == 0) {
		handle_record(cmd);
	} else if (strcmp(action, "get_device_info") == 0) {
		/* Public metadata, fine in plaintext at any point. */
		send_device_info();
	} else if (!decrypted && blocked) {
		LOG_WRN("plaintext command ignored after pairing started: %s", action);
	} else if (!decrypted && sensitive) {
		send_status("error_encryption_required", 0);
	} else if (decrypted && strcmp(action, "pairing_client_finished") == 0) {
		uint32_t generation = pairing_handle_client_finished(cmd);
		if (!generation) {
			send_status(PAIRING_ERROR_DECRYPT, 0);
			s_t->disconnect(DISCONNECT_AFTER_ERROR_MS);
		} else {
			LOG_INF("pairing confirmed (app consent)");
			send_status("pairing_confirmed", generation);
		}
	} else if (decrypted && sensitive && !pairing_confirmed()) {
		send_status("error_pairing_confirm_required", 0);
	} else if (decrypted && strcmp(action, "wifi_scan") == 0) {
		handle_wifi_scan();
	} else if (decrypted && strcmp(action, "provision_v2") == 0) {
		handle_provision(cmd);
	} else {
		send_status("error_unknown_action", 0);
	}
	cJSON_Delete(cmd);
}

/* ---- Provisioning --------------------------------------------------------- */

static bool commit_job(void *arg)
{
	ARG_UNUSED(arg);
	if (net_has_wifi()) {
		struct wifi_record wifi;
		strcpy(wifi.ssid, s_job.ssid);
		strcpy(wifi.password, s_job.password);
		if (!store_save_wifi(&wifi)) {
			return false;
		}
	}
	return store_save_pairing(&s_job.rec);
}

static void run_provision(void)
{
	uint32_t gen = s_job.generation;
	static struct vm_info vm;

	send_status("wifi_connecting", gen);
	int rc = net_has_wifi()
		? net_wifi_connect(s_job.ssid, s_job.password, WIFI_CONNECT_TIMEOUT_MS)
		: (net_is_online() ? 0 : -1);
	if (rc != 0 || !net_is_online()) {
		/* Stay in provisioning so the app can retry. */
		pairing_extend_provisioning(gen);
		send_status("wifi_failed", gen);
		return;
	}
	send_status("wifi_connected", gen);

	int status = 0;
	if (muse_api_fetch_vm(s_job.rec.access_token, muse_api_root(s_job.rec.api_url_v2), &vm,
			      &status) != 0) {
		LOG_WRN("device token check failed (HTTP %d)", status);
		send_status("auth_failed", gen);
		s_t->disconnect(500);
		return;
	}
	if (!pairing_commit_provisioning(gen, commit_job, NULL)) {
		send_status("error_storage", gen);
		s_t->disconnect(500);
		return;
	}
	send_status("auth_ok", gen);
	LOG_INF("setup complete");
	mem_log("setup");
	if (s_on_complete) {
		s_on_complete();
	}
}

/* Runs on the service thread, which has nothing else to do until the device
 * is paired; that saves a TLS-sized stack. */
void setup_run_job(k_timeout_t wait)
{
	if (k_sem_take(&s_job_sem, wait) != 0) {
		return;
	}
	run_provision();
	memset(&s_job, 0, sizeof(s_job));
	k_mutex_lock(&s_state_lock, K_FOREVER);
	s_provisioning = false;
	k_mutex_unlock(&s_state_lock);
}

/* ---- Worker -------------------------------------------------------------- */

static void worker_thread(void *a, void *b, void *c)
{
	for (;;) {
		struct inbox_msg *m = k_fifo_get(&s_inbox, K_FOREVER);
		handle_message(m->data, m->len, false);
		mem_free(m);
	}
}

K_THREAD_DEFINE(setup_worker, 5120, worker_thread, NULL, NULL, NULL, 7, 0, 0);

void setup_init(const struct setup_transport *transport, void (*on_complete)(void))
{
	s_t = transport;
	if (on_complete) {
		s_on_complete = on_complete;
	}
	framing_reset(&s_asm);
}

void setup_set_complete_cb(void (*on_complete)(void))
{
	s_on_complete = on_complete;
}

void setup_on_write(const uint8_t *pkt, size_t len)
{
	const uint8_t *msg;
	size_t msg_len;
	if (!framing_feed(&s_asm, pkt, len, &msg, &msg_len)) {
		return;
	}
	struct inbox_msg *m = mem_alloc(sizeof(*m) + msg_len + 1);
	if (!m) {
		LOG_ERR("no memory for a %u-byte setup message", (unsigned)msg_len);
		return;
	}
	m->len = msg_len;
	memcpy(m->data, msg, msg_len);
	m->data[msg_len] = '\0';
	k_fifo_put(&s_inbox, m);
}

void setup_on_disconnect(void)
{
	LOG_INF("setup client disconnected; clearing pairing session");
	framing_reset(&s_asm);
	k_mutex_lock(&s_state_lock, K_FOREVER);
	s_plaintext_blocked = false;
	k_mutex_unlock(&s_state_lock);
	pairing_reset();
}
