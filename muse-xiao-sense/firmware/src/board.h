/* The board's sensors, light and button. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum board_status {
	BOARD_STATUS_BOOT,
	BOARD_STATUS_SETUP,      /* breathing orange: ready for the Muse app */
	BOARD_STATUS_PAIRING,    /* breathing blue: the app is pairing */
	BOARD_STATUS_CONNECTING, /* blinking yellow */
	BOARD_STATUS_ONLINE,     /* a green blip every few seconds */
	BOARD_STATUS_OFFLINE,    /* blinking red */
};

struct board_motion {
	bool ok;
	float accel_g[3];
	float gyro_dps[3];
	float temp_c;
	bool temp_ok;
};

struct board_battery {
	bool present;
	int millivolts;
	int percent;
	bool charging;
};

int board_init(void);
void board_set_status(enum board_status status);

/* Muse can take over the light; mode is "solid", "blink" or "breathe".
 * Off hands it back to the status pattern. */
void board_set_light(uint8_t r, uint8_t g, uint8_t b, const char *mode);

int board_read_motion(struct board_motion *out);
/* Records for duration_ms and reports RMS and peak (0..32767) and dBFS. */
int board_read_sound(int duration_ms, int *rms, int *peak, float *dbfs);
int board_read_battery(struct board_battery *out);
bool board_has_sensors(void);

/* Events: button click / long press, shake, free fall. */
typedef void (*board_event_cb)(const char *event);
void board_set_event_cb(board_event_cb cb);
