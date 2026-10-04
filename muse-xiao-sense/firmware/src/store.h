/* Pairing record and Wi-Fi credentials, kept in flash (Zephyr settings). */
#pragma once
#include <stdbool.h>

#define STORE_TOKEN_MAX 1024
#define STORE_URL_MAX 160

struct pairing_record {
	char access_token[STORE_TOKEN_MAX];
	char refresh_token[STORE_TOKEN_MAX];
	char username[96];
	char api_url[STORE_URL_MAX];
	char api_url_v2[STORE_URL_MAX];
	char noise_host[STORE_URL_MAX];
};

struct wifi_record {
	char ssid[33];
	char password[65];
};

int store_init(void);

/* False if the device is not paired. */
bool store_load_pairing(struct pairing_record *out);
bool store_save_pairing(const struct pairing_record *rec);
bool store_save_tokens(const char *access_token, const char *refresh_token);
bool store_clear_pairing(void);
bool store_is_paired(void);

bool store_load_wifi(struct wifi_record *out);
bool store_save_wifi(const struct wifi_record *rec);
