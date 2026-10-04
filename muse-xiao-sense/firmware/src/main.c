/*
 * Muse gadget firmware for the Seeed XIAO nRF52840 Sense.
 *
 * A port of the Muse Gadget SDK (github.com/facebookincubator/muse-gadget-sdk)
 * to Zephyr: the XIAO pairs with the Muse app over BLE as MuseGadgetXXXXXX,
 * joins Wi-Fi, and holds its own encrypted Noise session to the Muse VM,
 * serving commands for its motion sensor, microphone, battery and light.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "board.h"
#include "identity.h"
#include "link.h"
#include "net.h"
#include "pairing.h"
#include "service.h"
#include "setup.h"
#include "setup_transport.h"
#include "store.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void on_setup_complete(void)
{
	setup_transport_stop(1500);
	service_kick();
}

static void on_board_event(const char *event)
{
	static const struct {
		const char *event;
		const char *text;
	} MESSAGES[] = {
		{"button", "The button on my XIAO gadget was pressed."},
		{"button_long", "The button on my XIAO gadget was held down."},
		{"shake", "My XIAO gadget was shaken."},
		{"free_fall", "My XIAO gadget detected a free fall. It may have been dropped."},
	};
	for (size_t i = 0; i < ARRAY_SIZE(MESSAGES); i++) {
		if (strcmp(event, MESSAGES[i].event) == 0) {
			if (!link_send_chat(MESSAGES[i].text)) {
				LOG_INF("not connected to Muse; %s not sent", event);
			}
			return;
		}
	}
}

int main(void)
{
	const struct identity *id = identity_get();
	LOG_INF("Muse gadget %s for XIAO nRF52840 Sense: %s (%s)", CONFIG_MUSE_FW_VERSION,
		id->ble_name, id->node_id);
	if (!CONFIG_MUSE_SDK_TOKEN[0]) {
		LOG_WRN("no SDK token: set CONFIG_MUSE_SDK_TOKEN; gadgets without one will stop pairing");
	}

	board_init();
	board_set_status(BOARD_STATUS_BOOT);
	if (store_init() != 0) {
		LOG_ERR("flash storage unavailable");
	}
	net_init();
	pairing_init(id->node_id, id->device_id, id->mac, CONFIG_MUSE_FW_VERSION,
		     CONFIG_MUSE_SDK_TOKEN);
	board_set_event_cb(on_board_event);

	if (store_is_paired()) {
		LOG_INF("paired; connecting to Muse");
	} else {
		LOG_INF("not paired: add %s in the Muse app (Settings > Devices, Developer mode)",
			id->ble_name);
		board_set_status(BOARD_STATUS_SETUP);
		setup_transport_start(id->ble_name);
		setup_set_complete_cb(on_setup_complete);
	}
	service_start();
	return 0;
}
