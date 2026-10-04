#include <string.h>
#include "b64url.h"

static const char ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
	size_t need = (len / 3) * 4 + ((len % 3) ? (len % 3) + 1 : 0);
	if (need + 1 > out_size) {
		return -1;
	}
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		size_t rem = len - i;
		if (rem > 1) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (rem > 2) {
			v |= in[i + 2];
		}
		out[o++] = ALPHABET[(v >> 18) & 63];
		out[o++] = ALPHABET[(v >> 12) & 63];
		if (rem > 1) {
			out[o++] = ALPHABET[(v >> 6) & 63];
		}
		if (rem > 2) {
			out[o++] = ALPHABET[v & 63];
		}
	}
	out[o] = '\0';
	return (int)o;
}

static int value_of(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '-') {
		return 62;
	}
	if (c == '_') {
		return 63;
	}
	return -1;
}

int b64url_decode(const char *text, size_t max_chars, uint8_t *out, size_t out_size)
{
	if (!text) {
		return -1;
	}
	size_t len = strlen(text);
	if (len == 0 || len > max_chars || len % 4 == 1) {
		return -1;
	}
	size_t need = len / 4 * 3 + (len % 4 ? len % 4 - 1 : 0);
	if (need > out_size) {
		return -1;
	}
	uint32_t acc = 0;
	int bits = 0;
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		int v = value_of(text[i]);
		if (v < 0) {
			return -1;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)o;
}
