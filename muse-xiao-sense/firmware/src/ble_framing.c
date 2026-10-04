#include <string.h>
#include "ble_framing.h"

size_t framing_packet_size(uint16_t mtu)
{
	size_t notify_max = mtu > 3 ? (size_t)mtu - 3 : 20;
	return notify_max < FRAMING_MAX_PACKET ? notify_max : FRAMING_MAX_PACKET;
}

void framing_reset(struct framing_assembler *a)
{
	a->len = 0;
	a->total = 0;
	a->next = 0;
}

bool framing_feed(struct framing_assembler *a, const uint8_t *pkt, size_t len,
		  const uint8_t **msg, size_t *msg_len)
{
	if (len < FRAMING_HEADER || pkt[0] != FRAMING_MAGIC) {
		*msg = pkt;
		*msg_len = len;
		return true;
	}
	uint8_t index = pkt[1], total = pkt[2];
	const uint8_t *frag = pkt + FRAMING_HEADER;
	size_t frag_len = len - FRAMING_HEADER;
	if (total == 0) {
		framing_reset(a);
		return false;
	}
	if (index == 0 || total != a->total) {
		framing_reset(a);
		a->total = total;
	}
	if (index != a->next || index >= a->total) {
		framing_reset(a);
		return false;
	}
	if (a->len + frag_len > sizeof(a->buf)) {
		framing_reset(a);
		return false;
	}
	memcpy(a->buf + a->len, frag, frag_len);
	a->len += frag_len;
	a->next = index + 1;
	if (a->next < a->total) {
		return false;
	}
	*msg = a->buf;
	*msg_len = a->len;
	a->total = 0;
	a->next = 0;
	/* len is cleared by the caller's next framing_reset or first chunk. */
	return true;
}
