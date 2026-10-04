/* Stand-in board for native_sim: steady fake readings, light logged. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "board.h"

LOG_MODULE_REGISTER(board, LOG_LEVEL_INF);

int board_init(void)
{
	return 0;
}

void board_set_status(enum board_status status)
{
	static enum board_status last = -1;
	if (status != last) {
		LOG_INF("status light: %d", status);
		last = status;
	}
}

void board_set_light(uint8_t r, uint8_t g, uint8_t b, const char *mode)
{
	LOG_INF("light: #%02x%02x%02x %s", r, g, b, mode);
}

bool board_has_sensors(void)
{
	return true;
}

int board_read_motion(struct board_motion *out)
{
	memset(out, 0, sizeof(*out));
	out->ok = true;
	out->accel_g[2] = 1.0f;
	out->temp_ok = true;
	out->temp_c = 24.5f;
	return 0;
}

int board_read_sound(int duration_ms, int *rms, int *peak, float *dbfs)
{
	k_sleep(K_MSEC(duration_ms));
	*rms = 328;
	*peak = 2000;
	*dbfs = -40.0f;
	return 0;
}

int board_read_battery(struct board_battery *out)
{
	memset(out, 0, sizeof(*out));
	return 0;
}

static board_event_cb s_event_cb;

void board_set_event_cb(board_event_cb cb)
{
	s_event_cb = cb;
}

/* A "button press" every 5 s, so the test sees device-originated chat. */
static void sim_events(void *a, void *b, void *c)
{
	for (;;) {
		k_sleep(K_SECONDS(5));
		if (s_event_cb) {
			s_event_cb("button");
		}
	}
}

K_THREAD_DEFINE(sim_events_thread, 2048, sim_events, NULL, NULL, NULL, 10, 0, 0);
