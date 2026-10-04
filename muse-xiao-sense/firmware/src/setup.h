/*
 * BLE setup commands: the protocol behind the setup GATT characteristics,
 * ported from the SDK's linux/src/musegadget/ble_setup.py. Independent of the
 * radio: a transport delivers writes and sends notifications.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

struct setup_transport {
	/* Sends one notification; blocks until queued. */
	int (*send)(const uint8_t *pkt, size_t len);
	uint16_t (*mtu)(void);
	void (*disconnect)(int delay_ms);
};

void setup_init(const struct setup_transport *transport, void (*on_complete)(void));
/* Called once provisioning has finished and the pairing is saved. */
void setup_set_complete_cb(void (*on_complete)(void));
/* Called by the transport, from any thread. */
void setup_on_write(const uint8_t *pkt, size_t len);
void setup_on_disconnect(void);

/* Waits up to `wait` for a provision_v2 request and carries it out (Wi-Fi,
 * token check, saving the pairing). Called by the service thread. */
void setup_run_job(k_timeout_t wait);
