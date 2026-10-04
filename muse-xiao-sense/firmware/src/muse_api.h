/* The Muse device API: leased VM lookup and device token rotation. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct vm_info {
	char vm_id[96];
	char vm_name[96];
	char vm_auth_token[1024];
};

/* api_url_v2 if it is an allowed URL, else the default API base. */
const char *muse_api_root(const char *api_url_v2);
bool muse_api_url_allowed(const char *url);

/* Picks the default VM (or the first). Returns 0 if one was found;
 * *http_status is the HTTP status, or 0 for a transport failure. */
int muse_api_fetch_vm(const char *access_token, const char *root, struct vm_info *out,
		      int *http_status);

/* Rotates the token pair. Returns 0 with both tokens written on success. */
int muse_api_refresh(const char *refresh_token, const char *device_id, const char *root,
		     const char *sdk_token, char *access_out, char *refresh_out, size_t cap,
		     int *http_status);
