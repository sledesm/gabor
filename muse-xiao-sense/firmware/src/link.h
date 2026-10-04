/*
 * One control session with a Muse VM, ported from the SDK's
 * linux/src/musegadget/link_client.py and built on the SDK's noise_core:
 * a WebSocket to /v1/noise with the VM bearer, the Noise XX handshake, then a
 * long-lived POST /link-control stream of length-prefixed JSON messages.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum link_outcome {
	LINK_CLOSED,        /* connection ended; reconnect normally */
	LINK_AUTH_REJECTED, /* the edge refused the VM bearer; re-fetch VMs */
	LINK_FORBIDDEN,     /* authenticated but not allowed right now */
	LINK_UNPAIRED,      /* the Muse removed this device */
};

/* Runs a session until it ends. *registered_ms is the uptime at which the
 * Muse accepted link.register, or 0. */
enum link_outcome link_run(const char *noise_host, const char *vm_id, const char *vm_auth_token,
			   int64_t *registered_ms);

bool link_is_registered(void);

/* Posts a message to the Muse as coming from this device (POST /chat/stream),
 * like `musegadget send-user-msg`. Any thread; false if not connected or the
 * queue is full. */
bool link_send_chat(const char *message);

#ifdef __cplusplus
}
#endif
