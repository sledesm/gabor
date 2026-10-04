/* Unpadded base64url, with the same strictness as the SDK's pairing code. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Writes a NUL-terminated string; returns its length, or -1 if out is too small. */
int b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_size);

/* Rejects padding, '+', '/', bad lengths and anything over max_chars.
 * Returns the decoded length, or -1. */
int b64url_decode(const char *text, size_t max_chars, uint8_t *out, size_t out_size);
