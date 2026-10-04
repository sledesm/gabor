/* One HTTP(S) request, response body collected into a caller buffer. */
#pragma once
#include <stddef.h>

/* headers: "Name: value\r\n" strings ending with NULL. Returns the HTTP
 * status, or <0 for a transport failure. The body is NUL-terminated. */
int https_request(const char *method, const char *url, const char *const *headers,
		  const char *body, char *resp, size_t resp_cap);

/* Splits scheme://host[:port]/path; false if malformed or not allowed. */
int https_split_url(const char *url, char *host, size_t host_size, unsigned short *port,
		    const char **path, int *tls);
