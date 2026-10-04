/*
 * Device identity. The apps expect the node id ("homelink-" + six hex
 * digits) and the BLE name ("MuseGadget" + the same digits, upper case) to
 * end alike. The MAC-shaped value comes from the nRF52840's factory device
 * id rather than a radio address, marked locally administered like the SDK's
 * Linux gadget does.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <zephyr/drivers/hwinfo.h>
#include "identity.h"

static struct identity s_id;
static bool s_ready;

const struct identity *identity_get(void)
{
	if (s_ready) {
		return &s_id;
	}
	uint8_t raw[8] = {0};
	ssize_t n = hwinfo_get_device_id(raw, sizeof(raw));
	if (n < 6) {
		memset(raw, 0x5a, sizeof(raw));
	}
	uint8_t o[6];
	memcpy(o, raw, 6);
	o[0] = (o[0] & 0xFC) | 0x02; /* unicast, locally administered */
	snprintf(s_id.mac, sizeof(s_id.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
		 o[0], o[1], o[2], o[3], o[4], o[5]);
	snprintf(s_id.node_id, sizeof(s_id.node_id), "homelink-%02x%02x%02x", o[3], o[4], o[5]);
	snprintf(s_id.device_id, sizeof(s_id.device_id), "hatch-link:%s", s_id.mac);
	snprintf(s_id.ble_name, sizeof(s_id.ble_name), "MuseGadget%02X%02X%02X", o[3], o[4], o[5]);
	s_ready = true;
	return &s_id;
}
