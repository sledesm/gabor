/*
 * Chunked framing for setup messages larger than one BLE packet, as in the
 * SDK: 0xFE, chunk index, total chunks, then a fragment. A packet that does
 * not start with 0xFE is a whole message.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAMING_MAGIC 0xFE
#define FRAMING_HEADER 3
#define FRAMING_MAX_PACKET 160
#define FRAMING_MAX_CHUNKS 255
#define FRAMING_MAX_MESSAGE 6144
#define FRAMING_DEFAULT_MTU 23

/* Usable notification size for an ATT MTU, capped like the SDK. */
size_t framing_packet_size(uint16_t mtu);

struct framing_assembler {
	uint8_t buf[FRAMING_MAX_MESSAGE];
	size_t len;
	uint8_t total;
	uint8_t next;
};

void framing_reset(struct framing_assembler *a);

/* Feeds one write. Returns true with msg and msg_len set when a message is
 * complete; *msg points into the packet (unchunked) or the assembler. */
bool framing_feed(struct framing_assembler *a, const uint8_t *pkt, size_t len,
		  const uint8_t **msg, size_t *msg_len);
