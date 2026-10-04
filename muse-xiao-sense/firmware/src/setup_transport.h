/* Where setup runs: BLE on hardware, a TCP socket in the simulator test. */
#pragma once

/* Starts advertising (or listening) for the Muse app. */
int setup_transport_start(const char *name);
/* Disconnects and stops advertising after delay_ms. */
void setup_transport_stop(int delay_ms);
