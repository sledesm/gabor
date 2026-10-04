/* Keeps a paired device connected to its Muse. */
#pragma once
#include <stdbool.h>

void service_start(void);
/* Wakes the loop, e.g. right after setup finished. */
void service_kick(void);
/* Forgets the pairing (button hold or the Muse unpairing us). */
void service_unpair(void);
