/* The commands Muse can invoke on this gadget. */
#pragma once
#include "cJSON.h"

/* The commands_v2 object for link.register (caller owns it). */
cJSON *commands_specs(void);

/* Runs one command; returns {"ok": true, "payload": {...}} or
 * {"ok": false, "error": "..."} (caller owns it). */
cJSON *commands_run(const char *command, const cJSON *params);
