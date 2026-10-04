/*
 * Flash storage under the "muse/" settings subtree. Every field is its own
 * key; "muse/paired" is written last and erased first, so a half-written
 * record never reads as a pairing.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include "store.h"

LOG_MODULE_REGISTER(store, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(s_lock);

struct field {
	const char *key;
	size_t offset;
	size_t size;
	bool wifi;
};

#define PF(name) {#name, offsetof(struct pairing_record, name), \
		   sizeof(((struct pairing_record *)0)->name), false}
#define WF(name) {"wifi_" #name, offsetof(struct wifi_record, name), \
		   sizeof(((struct wifi_record *)0)->name), true}

static const struct field FIELDS[] = {
	PF(access_token), PF(refresh_token), PF(username),
	PF(api_url), PF(api_url_v2), PF(noise_host),
	WF(ssid), WF(password),
};

static struct pairing_record s_pairing;
static struct wifi_record s_wifi;
static bool s_paired;

static char *field_ptr(const struct field *f)
{
	return (f->wifi ? (char *)&s_wifi : (char *)&s_pairing) + f->offset;
}

static int h_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "paired") == 0) {
		uint8_t v = 0;
		if (read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
			s_paired = v != 0;
		}
		return 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(FIELDS); i++) {
		const struct field *f = &FIELDS[i];
		if (strcmp(name, f->key) != 0) {
			continue;
		}
		char *dst = field_ptr(f);
		memset(dst, 0, f->size);
		if (len < f->size) {
			ssize_t n = read_cb(cb_arg, dst, len);
			if (n < 0) {
				memset(dst, 0, f->size);
			}
		}
		return 0;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(muse, "muse", NULL, h_set, NULL, NULL);

int store_init(void)
{
	int err = settings_subsys_init();
	if (err) {
		LOG_ERR("settings init failed: %d", err);
		return err;
	}
	return settings_load_subtree("muse");
}

static bool save_str(const char *key, const char *value)
{
	char path[32];
	snprintk(path, sizeof(path), "muse/%s", key);
	return settings_save_one(path, value, strlen(value)) == 0;
}

static bool save_paired(bool on)
{
	uint8_t v = on;
	if (!on) {
		return settings_delete("muse/paired") == 0;
	}
	return settings_save_one("muse/paired", &v, sizeof(v)) == 0;
}

bool store_load_pairing(struct pairing_record *out)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = s_paired && s_pairing.access_token[0] && s_pairing.refresh_token[0];
	if (ok && out) {
		*out = s_pairing;
	}
	k_mutex_unlock(&s_lock);
	return ok;
}

bool store_is_paired(void)
{
	return store_load_pairing(NULL);
}

bool store_save_pairing(const struct pairing_record *rec)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = save_paired(false);
	s_paired = false;
	s_pairing = *rec;
	for (size_t i = 0; ok && i < ARRAY_SIZE(FIELDS); i++) {
		if (!FIELDS[i].wifi) {
			ok = save_str(FIELDS[i].key, field_ptr(&FIELDS[i]));
		}
	}
	ok = ok && save_paired(true);
	s_paired = ok;
	k_mutex_unlock(&s_lock);
	return ok;
}

bool store_save_tokens(const char *access_token, const char *refresh_token)
{
	if (strlen(access_token) >= STORE_TOKEN_MAX || strlen(refresh_token) >= STORE_TOKEN_MAX) {
		return false;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	strcpy(s_pairing.access_token, access_token);
	strcpy(s_pairing.refresh_token, refresh_token);
	/* The refresh token is the one that matters: it is single use. */
	bool ok = save_str("refresh_token", refresh_token) && save_str("access_token", access_token);
	k_mutex_unlock(&s_lock);
	return ok;
}

bool store_clear_pairing(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = save_paired(false);
	s_paired = false;
	for (size_t i = 0; i < ARRAY_SIZE(FIELDS); i++) {
		if (!FIELDS[i].wifi) {
			char path[32];
			snprintk(path, sizeof(path), "muse/%s", FIELDS[i].key);
			settings_delete(path);
		}
	}
	memset(&s_pairing, 0, sizeof(s_pairing));
	k_mutex_unlock(&s_lock);
	return ok;
}

bool store_load_wifi(struct wifi_record *out)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = s_wifi.ssid[0] != '\0';
	if (ok) {
		*out = s_wifi;
	}
	k_mutex_unlock(&s_lock);
	return ok;
}

bool store_save_wifi(const struct wifi_record *rec)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_wifi = *rec;
	bool ok = save_str("wifi_ssid", rec->ssid) && save_str("wifi_password", rec->password);
	k_mutex_unlock(&s_lock);
	return ok;
}
