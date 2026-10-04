/* Port of the SDK's linux/src/musegadget/muse_api.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "cJSON.h"
#include "mem.h"
#include "https.h"
#include "muse_api.h"

LOG_MODULE_REGISTER(muse_api, LOG_LEVEL_INF);

#define FETCH_PATH "/fetch_vms"
#define REFRESH_PATH "/device_token/refresh"
#define RESP_CAP 6144
#define USER_AGENT "User-Agent: musegadget-xiao/" CONFIG_MUSE_FW_VERSION \
	" (Seeed XIAO nRF52840 Sense; Zephyr)\r\n"

static char s_url[256];
static char s_auth[1100];
static K_MUTEX_DEFINE(s_api_lock);

bool muse_api_url_allowed(const char *url)
{
	if (!url || !*url) {
		return false;
	}
	return strncmp(url, "https://", 8) == 0 ||
	       (IS_ENABLED(CONFIG_MUSE_ALLOW_PLAINTEXT) && strncmp(url, "http://", 7) == 0);
}

const char *muse_api_root(const char *api_url_v2)
{
	/* api_url is ignored: only older firmware reads it. */
	return muse_api_url_allowed(api_url_v2) ? api_url_v2 : CONFIG_MUSE_API_BASE;
}

static void join_url(const char *root, const char *path)
{
	size_t n = strlen(root);
	while (n && root[n - 1] == '/') {
		n--;
	}
	snprintf(s_url, sizeof(s_url), "%.*s%s", (int)n, root, path);
}

static bool copy_str(char *dst, size_t size, const cJSON *item)
{
	if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) {
		return false;
	}
	strcpy(dst, item->valuestring);
	return true;
}

int muse_api_fetch_vm(const char *access_token, const char *root, struct vm_info *out,
		      int *http_status)
{
	k_mutex_lock(&s_api_lock, K_FOREVER);
	/* Allocated per call: this memory serves the Noise session later. */
	char *s_resp = mem_alloc(RESP_CAP);
	int rc = -ENOENT;
	if (!s_resp) {
		k_mutex_unlock(&s_api_lock);
		return -ENOMEM;
	}
	join_url(root, FETCH_PATH);
	snprintf(s_auth, sizeof(s_auth), "Authorization: Bearer %s\r\n", access_token);
	const char *headers[] = {s_auth, "X-API-Version: 1.0.0\r\n", USER_AGENT, NULL};
	int status = https_request("GET", s_url, headers, NULL, s_resp, RESP_CAP);
	*http_status = status > 0 ? status : 0;
	if (status != 200) {
		LOG_ERR("VM fetch failed: %d", status);
		goto out;
	}
	cJSON *data = cJSON_Parse(s_resp);
	if (!cJSON_IsObject(data)) {
		LOG_ERR("VM fetch: unexpected response");
		cJSON_Delete(data);
		goto out;
	}
	if (cJSON_GetObjectItem(data, "error_title") || cJSON_GetObjectItem(data, "backend_error_code")) {
		LOG_ERR("VM fetch error: %s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "error_title")));
		cJSON_Delete(data);
		goto out;
	}
	const cJSON *list = cJSON_GetObjectItem(data, "vm_list");
	const cJSON *entry, *first = NULL, *def = NULL;
	int count = 0;
	cJSON_ArrayForEach(entry, list) {
		const cJSON *url = cJSON_GetObjectItem(entry, "vm_ws_url");
		if (!cJSON_IsString(url)) {
			url = cJSON_GetObjectItem(entry, "vm_url");
		}
		if (!cJSON_IsString(url) || !cJSON_IsString(cJSON_GetObjectItem(entry, "vm_auth_token"))) {
			continue;
		}
		count++;
		first = first ? first : entry;
		if (!def && cJSON_IsTrue(cJSON_GetObjectItem(entry, "default"))) {
			def = entry;
		}
	}
	const cJSON *chosen = def ? def : first;
	LOG_INF("VM fetch: %d VMs", count);
	if (chosen) {
		memset(out, 0, sizeof(*out));
		copy_str(out->vm_id, sizeof(out->vm_id), cJSON_GetObjectItem(chosen, "vm_id"));
		copy_str(out->vm_name, sizeof(out->vm_name), cJSON_GetObjectItem(chosen, "vm_name"));
		if (copy_str(out->vm_auth_token, sizeof(out->vm_auth_token),
			     cJSON_GetObjectItem(chosen, "vm_auth_token")) &&
		    (out->vm_id[0] || out->vm_name[0])) {
			rc = 0;
		}
	}
	cJSON_Delete(data);
out:
	mem_free(s_resp);
	k_mutex_unlock(&s_api_lock);
	return rc;
}

int muse_api_refresh(const char *refresh_token, const char *device_id, const char *root,
		     const char *sdk_token, char *access_out, char *refresh_out, size_t cap,
		     int *http_status)
{
	k_mutex_lock(&s_api_lock, K_FOREVER);
	char *s_resp = mem_alloc(RESP_CAP);
	if (!s_resp) {
		k_mutex_unlock(&s_api_lock);
		return -ENOMEM;
	}
	join_url(root, REFRESH_PATH);
	/* Apps hand over refresh tokens that may already carry the
	 * hatch_refresh: prefix; doubling it makes the server reject it. The
	 * access token is never presented, as on the ESP32. */
	const char *raw = strrchr(refresh_token, ':');
	raw = raw ? raw + 1 : refresh_token;
	snprintf(s_auth, sizeof(s_auth), "Authorization: Bearer hatch_refresh:%s\r\n", raw);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "device_id", device_id);
	if (sdk_token && *sdk_token) {
		cJSON_AddStringToObject(body, "sdk_token", sdk_token);
	}
	char *body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	const char *headers[] = {s_auth, USER_AGENT, NULL};
	int status = body_text ? https_request("POST", s_url, headers, body_text, s_resp,
					       RESP_CAP) : -ENOMEM;
	mem_free(body_text);
	*http_status = status > 0 ? status : 0;
	int rc = -EIO;
	if (status == 200) {
		cJSON *data = cJSON_Parse(s_resp);
		cJSON *payload = cJSON_GetObjectItem(data, "payload");
		cJSON *tokens = cJSON_IsObject(payload) ? payload : data;
		if (copy_str(access_out, cap, cJSON_GetObjectItem(tokens, "access_token")) &&
		    copy_str(refresh_out, cap, cJSON_GetObjectItem(tokens, "refresh_token")) &&
		    access_out[0] && refresh_out[0]) {
			rc = 0;
		} else {
			LOG_WRN("token refresh response missing tokens");
		}
		cJSON_Delete(data);
	} else if (status == 401) {
		LOG_WRN("token refresh rejected: device must be paired again");
	} else {
		LOG_WRN("token refresh failed: %d", status);
	}
	mem_free(s_resp);
	k_mutex_unlock(&s_api_lock);
	return rc;
}
