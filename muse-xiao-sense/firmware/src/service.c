/*
 * Port of the SDK's linux/src/musegadget/service.py: each round fetches the
 * leased VMs with the device token (which also yields a fresh per-VM bearer),
 * connects to the default VM and serves commands until the connection ends.
 * Failures back off exponentially; a session that stayed up a while resets
 * the backoff. The device token is rotated before it ages out, and at once if
 * the API rejects it.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include "board.h"
#include "identity.h"
#include "link.h"
#include "muse_api.h"
#include "mem.h"
#include "net.h"
#include "service.h"
#include "setup.h"
#include "store.h"

LOG_MODULE_REGISTER(service, LOG_LEVEL_INF);

#define BACKOFF_BASE_MS 2000
#define BACKOFF_MAX_MS 60000
#define AUTH_BACKOFF_MIN_MS 15000
#define HEALTHY_SESSION_MS 30000
#define UNPAIRED_POLL_MS 30000
#define TOKEN_REFRESH_AGE_MS (3LL * 3600 * 1000)
#define TOKEN_RETRY_MS 300000
#define WIFI_TIMEOUT_MS 30000

static K_SEM_DEFINE(s_wake, 0, 1);
static struct pairing_record s_rec;
static struct vm_info s_vm;
static char s_access[STORE_TOKEN_MAX], s_refresh[STORE_TOKEN_MAX];
/* No real-time clock, so token age counts from boot; a token that expired
 * while the board was off draws a 401, which forces a refresh. */
static int64_t s_token_saved_ms;
static int64_t s_last_refresh_attempt = INT64_MIN / 2;
static bool s_sdk_token_reported;

struct backoff {
	int failures;
	int floor_ms;
};

static int next_delay(struct backoff *b)
{
	int delay = BACKOFF_BASE_MS << MIN(b->failures, 5);
	delay = MIN(delay, BACKOFF_MAX_MS);
	b->failures++;
	return MAX(delay, b->floor_ms);
}

static void nap(int ms)
{
	k_sem_take(&s_wake, K_MSEC(ms));
}

void service_kick(void)
{
	k_sem_give(&s_wake);
}

void service_unpair(void)
{
	LOG_WRN("forgetting the pairing; setup opens again after restart");
	store_clear_pairing();
	board_set_status(BOARD_STATUS_SETUP);
	k_sleep(K_MSEC(500));
	sys_reboot(SYS_REBOOT_COLD);
}

/* Returns false when a due refresh failed and the old token should not be
 * used yet. */
static bool maybe_refresh(bool force)
{
	int64_t now = k_uptime_get();
	bool report_due = CONFIG_MUSE_SDK_TOKEN[0] && !s_sdk_token_reported;
	bool due = force || now - s_token_saved_ms >= TOKEN_REFRESH_AGE_MS;
	if (!due && !report_due) {
		return true;
	}
	if (!force && now - s_last_refresh_attempt < TOKEN_RETRY_MS) {
		return true;
	}
	s_last_refresh_attempt = now;
	s_sdk_token_reported = true;
	int status = 0;
	int rc = muse_api_refresh(s_rec.refresh_token, identity_get()->node_id,
				  muse_api_root(s_rec.api_url_v2), CONFIG_MUSE_SDK_TOKEN, s_access,
				  s_refresh, sizeof(s_access), &status);
	if (rc == 0) {
		if (!store_save_tokens(s_access, s_refresh)) {
			LOG_ERR("could not save the rotated tokens");
		}
		strcpy(s_rec.access_token, s_access);
		strcpy(s_rec.refresh_token, s_refresh);
		s_token_saved_ms = k_uptime_get();
		LOG_INF("device token rotated");
		return true;
	}
	if (!due) {
		/* Only reporting the SDK token: never unpair over this. */
		return true;
	}
	if (status == 401) {
		LOG_ERR("pairing revoked by Muse");
		service_unpair();
	}
	return !force;
}

static bool ensure_network(void)
{
	if (net_is_online()) {
		return true;
	}
	if (!net_has_wifi()) {
		return false;
	}
	struct wifi_record wifi;
	if (!store_load_wifi(&wifi)) {
		LOG_WRN("no saved Wi-Fi network");
		return false;
	}
	board_set_status(BOARD_STATUS_CONNECTING);
	return net_wifi_connect(wifi.ssid, wifi.password, WIFI_TIMEOUT_MS) == 0;
}

static void service_thread(void *a, void *b, void *c)
{
	struct backoff backoff = {0};
	for (;;) {
		if (!store_load_pairing(&s_rec)) {
			setup_run_job(K_MSEC(UNPAIRED_POLL_MS));
			continue;
		}
		if (!ensure_network()) {
			board_set_status(BOARD_STATUS_OFFLINE);
			nap(next_delay(&backoff));
			continue;
		}
		board_set_status(BOARD_STATUS_CONNECTING);
		if (!maybe_refresh(false)) {
			nap(TOKEN_RETRY_MS);
			continue;
		}
		int status = 0;
		const char *root = muse_api_root(s_rec.api_url_v2);
		if (muse_api_fetch_vm(s_rec.access_token, root, &s_vm, &status) != 0) {
			if (status == 401) {
				LOG_WRN("device token rejected by the API; refreshing");
				if (!maybe_refresh(true)) {
					nap(TOKEN_RETRY_MS);
				}
				continue;
			}
			nap(next_delay(&backoff));
			continue;
		}
		const char *host = s_rec.noise_host[0] ? s_rec.noise_host : CONFIG_MUSE_NOISE_HOST;
		const char *vm_id = s_vm.vm_id[0] ? s_vm.vm_id : s_vm.vm_name;
		LOG_INF("connecting to %s", s_vm.vm_name[0] ? s_vm.vm_name : vm_id);
		int64_t started = k_uptime_get(), registered_ms = 0;
		board_set_status(BOARD_STATUS_CONNECTING);
		enum link_outcome outcome = link_run(host, vm_id, s_vm.vm_auth_token, &registered_ms);
		board_set_status(BOARD_STATUS_OFFLINE);
		LOG_INF("session ended (%d) after %llds", outcome,
			(long long)((k_uptime_get() - started) / 1000));
		mem_log("after session");
		if (outcome == LINK_UNPAIRED) {
			service_unpair();
			continue;
		}
		if (registered_ms && k_uptime_get() - registered_ms >= HEALTHY_SESSION_MS) {
			backoff.failures = 0;
			backoff.floor_ms = 0;
		}
		if (outcome == LINK_AUTH_REJECTED || outcome == LINK_FORBIDDEN) {
			backoff.floor_ms = AUTH_BACKOFF_MIN_MS;
		}
		int delay = next_delay(&backoff);
		LOG_INF("reconnecting in %ds", delay / 1000);
		nap(delay);
	}
}

K_THREAD_DEFINE(service, 10240, service_thread, NULL, NULL, NULL, 8, 0, 1000);

void service_start(void)
{
	/* The thread starts on its own; nothing else to do yet. */
}
