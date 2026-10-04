/*
 * XIAO nRF52840 Sense hardware: LSM6DS3TR-C motion sensor, MSM261 PDM
 * microphone, battery voltage and charge state, the RGB LED (on PWM so it
 * can mix colours) and a button on D1.
 */
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include "board.h"
#include "service.h"

LOG_MODULE_REGISTER(board, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(lsm6ds3tr_c));
static const struct device *const pdm = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const struct device *const mic_power = DEVICE_DT_GET(DT_PATH(msm261d3526hicpm_c_en));
static const struct pwm_dt_spec rgb[3] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(muse_red)),
	PWM_DT_SPEC_GET(DT_NODELABEL(muse_green)),
	PWM_DT_SPEC_GET(DT_NODELABEL(muse_blue)),
};
static const struct adc_dt_spec vbat = ADC_DT_SPEC_GET(USER);
static const struct gpio_dt_spec vbat_enable = GPIO_DT_SPEC_GET(USER, vbat_enable_gpios);
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(USER, charging_gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(USER, button_gpios);

static K_MUTEX_DEFINE(s_imu_lock);
static K_MUTEX_DEFINE(s_mic_lock);
static bool s_imu_ok;
static board_event_cb s_event_cb;

/* ---- Light ---------------------------------------------------------------- */

static volatile enum board_status s_status = BOARD_STATUS_BOOT;
static struct {
	uint8_t r, g, b;
	uint8_t mode; /* 0 solid, 1 blink, 2 breathe */
	bool active;
} s_light;

static void write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t v[3] = {r, g, b};
	for (int i = 0; i < 3; i++) {
		if (pwm_is_ready_dt(&rgb[i])) {
			pwm_set_pulse_dt(&rgb[i], (uint32_t)((uint64_t)rgb[i].period * v[i] / 255));
		}
	}
}

/* 0..255..0 over period_ms, eased. */
static uint32_t breathe(int64_t now, int period_ms)
{
	uint32_t t = now % period_ms, half = period_ms / 2;
	uint32_t tri = t < half ? t : period_ms - t;
	return tri * tri * 255 / (half * half);
}

static void light_thread(void *a, void *b, void *c)
{
	for (;;) {
		int64_t now = k_uptime_get();
		if (s_light.active) {
			uint32_t k = 255;
			if (s_light.mode == 1) {
				k = (now % 1000) < 500 ? 255 : 0;
			} else if (s_light.mode == 2) {
				k = breathe(now, 2000);
			}
			write_rgb(s_light.r * k / 255, s_light.g * k / 255, s_light.b * k / 255);
		} else {
			switch (s_status) {
			case BOARD_STATUS_SETUP: /* breathing orange, like the ESP32 boards */
				write_rgb(breathe(now, 3000) * 200 / 255, breathe(now, 3000) * 60 / 255, 0);
				break;
			case BOARD_STATUS_PAIRING: /* breathing blue */
				write_rgb(0, 0, breathe(now, 1500) * 200 / 255);
				break;
			case BOARD_STATUS_CONNECTING:
				write_rgb((now % 600) < 300 ? 160 : 0, (now % 600) < 300 ? 100 : 0, 0);
				break;
			case BOARD_STATUS_ONLINE: /* a short green blip every 4 s */
				write_rgb(0, (now % 4000) < 80 ? 60 : 0, 0);
				break;
			case BOARD_STATUS_OFFLINE:
				write_rgb((now % 2000) < 150 ? 160 : 0, 0, 0);
				break;
			default:
				write_rgb(30, 30, 30);
				break;
			}
		}
		k_sleep(K_MSEC(20));
	}
}

K_THREAD_DEFINE(board_light, 1024, light_thread, NULL, NULL, NULL, 12, 0, 100);

void board_set_status(enum board_status status)
{
	s_status = status;
}

void board_set_light(uint8_t r, uint8_t g, uint8_t b, const char *mode)
{
	s_light.r = r;
	s_light.g = g;
	s_light.b = b;
	s_light.mode = strcmp(mode, "blink") == 0 ? 1 : strcmp(mode, "breathe") == 0 ? 2 : 0;
	s_light.active = r || g || b;
}

/* ---- Motion ------------------------------------------------------------- */

bool board_has_sensors(void)
{
	return true;
}

int board_read_motion(struct board_motion *out)
{
	memset(out, 0, sizeof(*out));
	if (!s_imu_ok) {
		return -ENODEV;
	}
	struct sensor_value a[3], g[3], t;
	k_mutex_lock(&s_imu_lock, K_FOREVER);
	int rc = sensor_sample_fetch(imu);
	rc = rc ? rc : sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, a);
	rc = rc ? rc : sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, g);
	out->temp_ok = rc == 0 && sensor_channel_get(imu, SENSOR_CHAN_DIE_TEMP, &t) == 0;
	k_mutex_unlock(&s_imu_lock);
	if (rc) {
		return rc;
	}
	for (int i = 0; i < 3; i++) {
		out->accel_g[i] = (float)(sensor_value_to_double(&a[i]) / SENSOR_G * 1000000.0);
		out->gyro_dps[i] = (float)(sensor_value_to_double(&g[i]) * 57.29578);
	}
	if (out->temp_ok) {
		out->temp_c = (float)sensor_value_to_double(&t);
	}
	out->ok = true;
	return 0;
}

static void emit(const char *event)
{
	LOG_INF("event: %s", event);
	if (s_event_cb) {
		s_event_cb(event);
	}
}

/* Free fall: under 0.35 g for 80 ms. Shake: 4 swings past 2.2 g within 1 s. */
static void detect_motion(const struct board_motion *m, int64_t now)
{
	static int64_t fall_since, last_fall, swing_window, last_shake;
	static int swings;
	static bool high;
	float mag = sqrtf(m->accel_g[0] * m->accel_g[0] + m->accel_g[1] * m->accel_g[1] +
			  m->accel_g[2] * m->accel_g[2]);
	if (mag < 0.35f) {
		if (!fall_since) {
			fall_since = now;
		}
		if (now - fall_since >= 80 && now - last_fall > 2000) {
			last_fall = now;
			emit("free_fall");
		}
	} else {
		fall_since = 0;
	}
	if (now - swing_window > 1000) {
		swing_window = now;
		swings = 0;
	}
	if (!high && mag > 2.2f) {
		high = true;
		if (++swings >= 4 && now - last_shake > 3000) {
			last_shake = now;
			swings = 0;
			emit("shake");
		}
	} else if (high && mag < 1.6f) {
		high = false;
	}
}

/* ---- Button ------------------------------------------------------------- */

static void poll_button(int64_t now)
{
	static bool down, long_sent, reset_armed;
	static int64_t since, last_change;
	bool pressed = gpio_pin_get_dt(&button) == 1;
	if (pressed != down && now - last_change > 30) {
		last_change = now;
		down = pressed;
		if (down) {
			since = now;
			long_sent = false;
			reset_armed = false;
		} else if (!long_sent) {
			emit("button");
		}
	}
	if (down && !long_sent && now - since >= 1000) {
		long_sent = true;
		emit("button_long");
	}
	if (down && !reset_armed && now - since >= CONFIG_MUSE_BUTTON_FACTORY_RESET_S * 1000) {
		reset_armed = true;
		service_unpair();
	}
}

static void sense_thread(void *a, void *b, void *c)
{
	struct board_motion m;
	for (;;) {
		int64_t now = k_uptime_get();
		poll_button(now);
		if (s_imu_ok && board_read_motion(&m) == 0) {
			detect_motion(&m, now);
		}
		k_sleep(K_MSEC(20));
	}
}

K_THREAD_DEFINE(board_sense, 2048, sense_thread, NULL, NULL, NULL, 11, 0, 1500);

void board_set_event_cb(board_event_cb cb)
{
	s_event_cb = cb;
}

/* ---- Microphone ------------------------------------------------------------- */

#define PCM_RATE 16000
#define BLOCK_MS 50
#define BLOCK_BYTES (PCM_RATE * 2 * BLOCK_MS / 1000)
K_MEM_SLAB_DEFINE_STATIC(s_pcm_slab, BLOCK_BYTES, 3, 4);

int board_read_sound(int duration_ms, int *rms_out, int *peak_out, float *dbfs_out)
{
	if (!device_is_ready(pdm)) {
		return -ENODEV;
	}
	k_mutex_lock(&s_mic_lock, K_FOREVER);
	regulator_enable(mic_power);
	k_sleep(K_MSEC(20));
	struct pcm_stream_cfg stream = {
		.pcm_rate = PCM_RATE,
		.pcm_width = 16,
		.block_size = BLOCK_BYTES,
		.mem_slab = &s_pcm_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_chan = 1,
			.req_num_streams = 1,
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
		},
	};
	int rc = dmic_configure(pdm, &cfg);
	rc = rc ? rc : dmic_trigger(pdm, DMIC_TRIGGER_START);
	uint64_t sum_sq = 0;
	uint32_t samples = 0;
	int peak = 0;
	/* The first blocks carry the microphone's start-up transient. */
	int skip = 3, blocks = duration_ms / BLOCK_MS;
	for (int i = 0; rc == 0 && i < skip + blocks; i++) {
		void *buf;
		size_t size;
		rc = dmic_read(pdm, 0, &buf, &size, 500);
		if (rc) {
			break;
		}
		if (i >= skip) {
			const int16_t *s = buf;
			int64_t mean = 0;
			for (size_t j = 0; j < size / 2; j++) {
				mean += s[j];
			}
			mean /= (int64_t)MAX(size / 2, 1);
			for (size_t j = 0; j < size / 2; j++) {
				int32_t v = s[j] - (int32_t)mean;
				sum_sq += (uint64_t)((int64_t)v * v);
				peak = MAX(peak, abs(v));
			}
			samples += size / 2;
		}
		k_mem_slab_free(&s_pcm_slab, buf);
	}
	dmic_trigger(pdm, DMIC_TRIGGER_STOP);
	regulator_disable(mic_power);
	k_mutex_unlock(&s_mic_lock);
	if (rc || samples == 0) {
		LOG_WRN("microphone read failed: %d", rc);
		return rc ? rc : -EIO;
	}
	double rms = sqrt((double)sum_sq / samples);
	*rms_out = (int)rms;
	*peak_out = MIN(peak, 32767);
	*dbfs_out = rms > 0 ? (float)(20.0 * log10(rms / 32768.0)) : -120.0f;
	return 0;
}

/* ---- Battery ------------------------------------------------------------- */

int board_read_battery(struct board_battery *out)
{
	memset(out, 0, sizeof(*out));
	if (!adc_is_ready_dt(&vbat)) {
		return -ENODEV;
	}
	int16_t raw;
	struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof(raw)};
	adc_sequence_init_dt(&vbat, &seq);
	int32_t sum = 0;
	for (int i = 0; i < 8; i++) {
		if (adc_read_dt(&vbat, &seq) != 0) {
			return -EIO;
		}
		sum += raw;
	}
	int32_t mv = sum / 8;
	adc_raw_to_millivolts_dt(&vbat, &mv);
	/* VBAT reaches P0.31 through a 1 M / 510 k divider. */
	mv = mv * 1510 / 510;
	out->charging = gpio_pin_get_dt(&charging) == 1;
	/* With no battery the charger output floats around 4.2-4.3 V or reads
	 * near zero; only call it a battery inside a lithium cell's range. */
	out->present = mv >= 2900 && mv <= 4350;
	out->millivolts = mv;
	out->percent = CLAMP((mv - 3300) * 100 / (4150 - 3300), 0, 100);
	return 0;
}

/* ---- Init --------------------------------------------------------------- */

int board_init(void)
{
	/* Keep VBAT_ENABLE asserted (P0.14 low): leaving it high would put the
	 * full battery voltage on P0.31. */
	gpio_pin_configure_dt(&vbat_enable, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&charging, GPIO_INPUT);
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (adc_is_ready_dt(&vbat)) {
		adc_channel_setup_dt(&vbat);
	}
	s_imu_ok = device_is_ready(imu);
	if (s_imu_ok) {
		struct sensor_value odr = {.val1 = 104};
		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
		sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	}
	LOG_INF("motion sensor %s, microphone %s", s_imu_ok ? "ready" : "missing",
		device_is_ready(pdm) ? "ready" : "missing");
	return 0;
}
