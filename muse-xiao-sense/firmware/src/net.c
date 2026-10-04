#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#if defined(CONFIG_WIFI)
#include <zephyr/net/wifi_mgmt.h>
#endif
#include "muse_api.h"
#include "net.h"

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

#define CA_TAG 1
#define ONLINE_TIMEOUT_MS 5000

static const unsigned char ROOT_CA[] = {
#include "muse_root_ca.der.inc"
};

static char s_ssid[33];

int net_init(void)
{
	int rc = tls_credential_add(CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, ROOT_CA, sizeof(ROOT_CA));
	if (rc && rc != -EEXIST) {
		LOG_ERR("adding the root CA failed: %d", rc);
	}
	return rc == -EEXIST ? 0 : rc;
}

bool net_has_wifi(void)
{
	return IS_ENABLED(CONFIG_MUSE_NET_WIFI);
}

const char *net_current_ssid(void)
{
	return s_ssid;
}

/* ---- Sockets ------------------------------------------------------------ */

static int wait_writable(int fd, int timeout_ms)
{
	struct zsock_pollfd p = {.fd = fd, .events = ZSOCK_POLLOUT};
	int rc = zsock_poll(&p, 1, timeout_ms);
	if (rc <= 0) {
		return rc == 0 ? -ETIMEDOUT : -errno;
	}
	return (p.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) ? -ECONNREFUSED : 0;
}

int net_open(const char *host, uint16_t port, bool tls, int timeout_ms)
{
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%u", port);
	struct zsock_addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res = NULL;
	int rc = zsock_getaddrinfo(host, port_text, &hints, &res);
	if (rc != 0 || !res) {
		LOG_WRN("DNS lookup of %s failed: %d", host, rc);
		return -EHOSTUNREACH;
	}
	int fd = zsock_socket(res->ai_family, SOCK_STREAM, tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
	if (fd < 0) {
		rc = -errno;
		goto out;
	}
	if (tls) {
		sec_tag_t tags[] = {CA_TAG};
		int verify = TLS_PEER_VERIFY_REQUIRED;
		if (zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) ||
		    zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, host, strlen(host)) ||
		    zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify))) {
			rc = -errno;
			goto fail;
		}
	}
	struct zsock_timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
	zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (zsock_connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		rc = -errno;
		if (rc == -EINPROGRESS) {
			rc = wait_writable(fd, timeout_ms);
		}
		if (rc) {
			LOG_WRN("connect to %s:%u failed: %d", host, port, rc);
			goto fail;
		}
	}
	rc = fd;
	goto out;
fail:
	zsock_close(fd);
out:
	zsock_freeaddrinfo(res);
	return rc;
}

static bool host_of(const char *url, char *host, size_t size, uint16_t *port)
{
	const char *p = strstr(url, "://");
	bool https = strncmp(url, "https://", 8) == 0;
	p = p ? p + 3 : url;
	size_t n = strcspn(p, ":/");
	if (n == 0 || n >= size) {
		return false;
	}
	memcpy(host, p, n);
	host[n] = '\0';
	*port = p[n] == ':' ? (uint16_t)atoi(p + n + 1) : (https ? 443 : 80);
	return true;
}

bool net_is_online(void)
{
	struct net_if *iface = net_if_get_default();
	if (!iface || !net_if_is_up(iface)) {
		return false;
	}
#if defined(CONFIG_MUSE_NET_WIFI)
	if (!s_ssid[0]) {
		return false;
	}
#endif
	char host[96];
	uint16_t port;
	if (!host_of(muse_api_root(""), host, sizeof(host), &port)) {
		return false;
	}
	int fd = net_open(host, port, false, ONLINE_TIMEOUT_MS);
	if (fd < 0) {
		return false;
	}
	zsock_close(fd);
	return true;
}

/* ---- Wi-Fi ---------------------------------------------------------------- */

#if defined(CONFIG_MUSE_NET_WIFI)

static struct net_mgmt_event_callback s_wifi_cb;
static K_SEM_DEFINE(s_connect_sem, 0, 1);
static K_SEM_DEFINE(s_scan_sem, 0, 1);
static int s_connect_status;
static struct net_scan_entry *s_scan_out;
static int s_scan_max;
static int s_scan_count;

static void add_scan_result(const struct wifi_scan_result *r)
{
	if (!s_scan_out || r->ssid_length == 0) {
		return;
	}
	for (int i = 0; i < s_scan_count; i++) {
		if (strcmp(s_scan_out[i].ssid, (const char *)r->ssid) == 0) {
			if (r->rssi > s_scan_out[i].rssi) {
				s_scan_out[i].rssi = r->rssi;
			}
			return;
		}
	}
	if (s_scan_count >= s_scan_max) {
		return;
	}
	struct net_scan_entry *e = &s_scan_out[s_scan_count++];
	size_t n = MIN(r->ssid_length, sizeof(e->ssid) - 1);
	memcpy(e->ssid, r->ssid, n);
	e->ssid[n] = '\0';
	e->rssi = r->rssi;
	e->secure = r->security != WIFI_SECURITY_TYPE_NONE;
}

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st = cb->info;
		s_connect_status = st ? st->status : -1;
		k_sem_give(&s_connect_sem);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Wi-Fi disconnected");
		s_ssid[0] = '\0';
		break;
	case NET_EVENT_WIFI_SCAN_RESULT:
		add_scan_result(cb->info);
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		k_sem_give(&s_scan_sem);
		break;
	}
}

static void wifi_register(void)
{
	static bool done;
	if (done) {
		return;
	}
	net_mgmt_init_event_callback(&s_wifi_cb, wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&s_wifi_cb);
	done = true;
}

int net_wifi_connect(const char *ssid, const char *password, int timeout_ms)
{
	wifi_register();
	struct net_if *iface = net_if_get_default();
	size_t ssid_len = strlen(ssid), psk_len = strlen(password);
	if (ssid_len == 0 || ssid_len > 32 || psk_len > 64) {
		return -EINVAL;
	}
	if (s_ssid[0]) {
		net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
		s_ssid[0] = '\0';
		k_sleep(K_MSEC(500));
	}
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = (uint8_t)ssid_len,
		.psk = (const uint8_t *)password,
		.psk_length = (uint8_t)psk_len,
		.security = psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
		.timeout = timeout_ms / 1000,
	};
	k_sem_reset(&s_connect_sem);
	LOG_INF("joining Wi-Fi \"%s\"", ssid);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
	if (rc) {
		LOG_WRN("Wi-Fi connect request failed: %d", rc);
		return rc;
	}
	if (k_sem_take(&s_connect_sem, K_MSEC(timeout_ms + 5000)) != 0 || s_connect_status != 0) {
		LOG_WRN("Wi-Fi join failed (%d)", s_connect_status);
		return -ECONNREFUSED;
	}
	snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
	/* The module needs a moment for DHCP after reporting the join. */
	for (int i = 0; i < 20 && !net_is_online(); i++) {
		k_sleep(K_MSEC(500));
	}
	LOG_INF("Wi-Fi connected to \"%s\"", ssid);
	return 0;
}

int net_wifi_scan(struct net_scan_entry *out, int max)
{
	wifi_register();
	s_scan_out = out;
	s_scan_max = max;
	s_scan_count = 0;
	k_sem_reset(&s_scan_sem);
	if (net_mgmt(NET_REQUEST_WIFI_SCAN, net_if_get_default(), NULL, 0) != 0) {
		s_scan_out = NULL;
		return 0;
	}
	k_sem_take(&s_scan_sem, K_SECONDS(15));
	s_scan_out = NULL;
	/* Strongest first. */
	for (int i = 1; i < s_scan_count; i++) {
		struct net_scan_entry e = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].rssi < e.rssi) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = e;
	}
	LOG_INF("Wi-Fi scan: %d networks", s_scan_count);
	return s_scan_count;
}

#else /* !CONFIG_MUSE_NET_WIFI */

int net_wifi_connect(const char *ssid, const char *password, int timeout_ms)
{
	ARG_UNUSED(ssid);
	ARG_UNUSED(password);
	ARG_UNUSED(timeout_ms);
	return net_is_online() ? 0 : -ENETUNREACH;
}

int net_wifi_scan(struct net_scan_entry *out, int max)
{
	ARG_UNUSED(out);
	ARG_UNUSED(max);
	return 0;
}

#endif
