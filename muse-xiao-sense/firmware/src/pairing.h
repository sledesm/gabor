/*
 * Device side of Muse Gadget BLE pairing, protocol version 5, community mode
 * ("pairing_auth": "none", policy "confirm_app"), ported from the SDK's
 * linux/src/musegadget/pairing.py. The wire format, transcript, key schedule
 * and record encryption match the SDK so the Muse apps pair unchanged.
 *
 * Methods that advance the handshake return a nonzero generation; deferred
 * work holds on to it so an abandoned attempt can't act on a newer one.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define PAIRING_ERROR_INVALID_HELLO "error_pairing_invalid_hello"
#define PAIRING_ERROR_DECRYPT "error_pairing_decrypt"

void pairing_init(const char *node_id, const char *device_id, const char *mac,
		  const char *firmware_version, const char *sdk_token);
void pairing_reset(void);

/* Adds the pairing fields of get_device_info to obj. */
void pairing_add_device_info(cJSON *obj);

/* pairing_client_hello -> pairing_ready (caller frees), or NULL and *err. */
cJSON *pairing_handle_hello(const cJSON *hello, const char **err);

/* Opens a pairing_encrypted record; returns malloc'd plaintext or NULL. */
char *pairing_decrypt(const cJSON *envelope);

uint32_t pairing_handle_client_finished(const cJSON *command);
bool pairing_confirmed(void);
uint32_t pairing_mark_provisioning(void);
bool pairing_extend_provisioning(uint32_t generation);
bool pairing_is_provisioning(uint32_t generation);
/* Runs commit() under the pairing lock if the session is still valid. */
bool pairing_commit_provisioning(uint32_t generation, bool (*commit)(void *), void *arg);

/* Seals a device-to-mobile record; returns the envelope JSON (caller frees)
 * or NULL with no session or a stale generation. */
char *pairing_encrypt_json(const char *plaintext, uint32_t generation);
char *pairing_encrypt_status(const char *status, uint32_t generation);
