/* Stable device identity, in the Muse Gadget naming conventions. */
#pragma once

struct identity {
	char mac[18];        /* "02:xx:xx:xx:xx:xx", locally administered */
	char node_id[16];    /* "homelink-xxxxxx" */
	char device_id[32];  /* "hatch-link:<mac>" */
	char ble_name[17];   /* "MuseGadgetXXXXXX" */
};

/* Derived from the chip's factory device id, so it survives reflashing. */
const struct identity *identity_get(void);
