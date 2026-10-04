/* Network access: Wi-Fi through an ESP-AT module, or whatever interface the
 * build has (native_sim's host sockets in tests). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct net_scan_entry {
	char ssid[33];
	int8_t rssi;
	bool secure;
};

int net_init(void);
bool net_has_wifi(void);
/* Interface up and the Muse API host reachable over TCP (5 s timeout). */
bool net_is_online(void);
/* Joins a network; 0 on success. Without Wi-Fi, succeeds if already online. */
int net_wifi_connect(const char *ssid, const char *password, int timeout_ms);
/* Returns entries written, strongest first, deduplicated by SSID. */
int net_wifi_scan(struct net_scan_entry *out, int max);
/* SSID in use, or "" when not on Wi-Fi. */
const char *net_current_ssid(void);

/* Connected TCP (or TLS, verified against the Muse root CA) socket, or <0. */
int net_open(const char *host, uint16_t port, bool tls, int timeout_ms);
