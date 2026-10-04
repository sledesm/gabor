/*
 * The commands this gadget offers Muse in link.register (commands_v2), in the
 * shape the SDK's firmware uses: description, required and optional params.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include "board.h"
#include "commands.h"
#include "net.h"

static cJSON *param(const char *type, const char *description)
{
	cJSON *p = cJSON_CreateObject();
	cJSON_AddStringToObject(p, "type", type);
	cJSON_AddStringToObject(p, "description", description);
	return p;
}

static void add_command(cJSON *commands, const char *name, const char *description,
			cJSON *required, cJSON *optional)
{
	cJSON *c = cJSON_AddObjectToObject(commands, name);
	cJSON_AddStringToObject(c, "description", description);
	cJSON_AddItemToObject(c, "required", required ? required : cJSON_CreateObject());
	cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
}

cJSON *commands_specs(void)
{
	cJSON *commands = cJSON_CreateObject();
	add_command(commands, "device.health",
		    "Report basic Link health, including battery level (percent), voltage, and "
		    "whether it is charging or on USB power; each is null when the device has no "
		    "battery or can't measure it",
		    NULL, NULL);
	if (board_has_sensors()) {
		add_command(commands, "motion.read",
			    "Read the XIAO's motion sensor: acceleration in g and rotation in degrees "
			    "per second on x, y and z, which way up the board is, and the sensor "
			    "chip's temperature in degrees Celsius (it runs a few degrees above the "
			    "room).",
			    NULL, NULL);
		cJSON *opt = cJSON_CreateObject();
		cJSON_AddItemToObject(opt, "duration_ms",
				      param("integer", "How long to listen, 100 to 5000 ms. Default 1000."));
		add_command(commands, "sound.level",
			    "Listen with the XIAO's microphone and report how loud it is: RMS and "
			    "peak level (0-32767) and the level in dBFS (0 is the loudest the "
			    "microphone can measure; a quiet room is around -60, speech -30 to -20). "
			    "No audio is kept or sent.",
			    NULL, opt);
	}
	cJSON *req = cJSON_CreateObject();
	cJSON_AddItemToObject(req, "color",
			      param("string", "A colour name (red, green, blue, white, yellow, orange, "
					      "purple, cyan, pink) or #RRGGBB, or off to give the light "
					      "back to the status pattern."));
	cJSON *opt = cJSON_CreateObject();
	cJSON_AddItemToObject(opt, "mode", param("string", "solid (default), blink or breathe."));
	add_command(commands, "light.set", "Set the XIAO's RGB status light.", req, opt);
	return commands;
}

static cJSON *ok(cJSON *payload)
{
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", true);
	cJSON_AddItemToObject(r, "payload", payload ? payload : cJSON_CreateObject());
	return r;
}

static cJSON *error(const char *message)
{
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", false);
	cJSON_AddStringToObject(r, "error", message);
	return r;
}

static double round_to(double v, double step)
{
	return round(v / step) * step;
}

static cJSON *device_health(void)
{
	cJSON *p = cJSON_CreateObject();
	struct board_battery bat;
	bool have = board_read_battery(&bat) == 0 && bat.present;
	if (have) {
		cJSON_AddNumberToObject(p, "battery_percent", bat.percent);
		cJSON_AddNumberToObject(p, "battery_voltage", round_to(bat.millivolts / 1000.0, 0.01));
		cJSON_AddBoolToObject(p, "charging", bat.charging);
	} else {
		cJSON_AddNullToObject(p, "battery_percent");
		cJSON_AddNullToObject(p, "battery_voltage");
		cJSON_AddNullToObject(p, "charging");
	}
	cJSON_AddNumberToObject(p, "uptime_s", (double)(k_uptime_get() / 1000));
	cJSON_AddStringToObject(p, "version", CONFIG_MUSE_FW_VERSION);
	cJSON_AddStringToObject(p, "board", "Seeed XIAO nRF52840 Sense");
	const char *ssid = net_current_ssid();
	if (ssid && *ssid) {
		cJSON_AddStringToObject(p, "network_ssid", ssid);
	}
	return ok(p);
}

static const char *orientation(const float a[3])
{
	int axis = 0;
	for (int i = 1; i < 3; i++) {
		if (fabsf(a[i]) > fabsf(a[axis])) {
			axis = i;
		}
	}
	if (fabsf(a[axis]) < 0.7f) {
		return "tilted";
	}
	static const char *const NAMES[3][2] = {
		{"-x up", "+x up"}, {"-y up", "+y up"}, {"lying flat, chips facing down",
							"lying flat, chips facing up"}};
	return NAMES[axis][a[axis] > 0];
}

static cJSON *motion_read(void)
{
	struct board_motion m;
	if (board_read_motion(&m) != 0 || !m.ok) {
		return error("the motion sensor is not responding");
	}
	cJSON *p = cJSON_CreateObject();
	cJSON *a = cJSON_AddObjectToObject(p, "accel_g");
	cJSON *g = cJSON_AddObjectToObject(p, "gyro_dps");
	static const char *const AX[] = {"x", "y", "z"};
	for (int i = 0; i < 3; i++) {
		cJSON_AddNumberToObject(a, AX[i], round_to(m.accel_g[i], 0.001));
		cJSON_AddNumberToObject(g, AX[i], round_to(m.gyro_dps[i], 0.1));
	}
	cJSON_AddStringToObject(p, "orientation", orientation(m.accel_g));
	float mag = sqrtf(m.accel_g[0] * m.accel_g[0] + m.accel_g[1] * m.accel_g[1] +
			  m.accel_g[2] * m.accel_g[2]);
	cJSON_AddBoolToObject(p, "moving", fabsf(mag - 1.0f) > 0.08f ||
					       fabsf(m.gyro_dps[0]) + fabsf(m.gyro_dps[1]) +
						       fabsf(m.gyro_dps[2]) > 15.0f);
	if (m.temp_ok) {
		cJSON_AddNumberToObject(p, "temperature_c", round_to(m.temp_c, 0.1));
	} else {
		cJSON_AddNullToObject(p, "temperature_c");
	}
	return ok(p);
}

static cJSON *sound_level(const cJSON *params)
{
	int duration = 1000;
	const cJSON *d = cJSON_GetObjectItem(params, "duration_ms");
	if (cJSON_IsNumber(d)) {
		duration = CLAMP((int)d->valuedouble, 100, 5000);
	}
	int rms = 0, peak = 0;
	float dbfs = 0;
	if (board_read_sound(duration, &rms, &peak, &dbfs) != 0) {
		return error("the microphone is not responding");
	}
	cJSON *p = cJSON_CreateObject();
	cJSON_AddNumberToObject(p, "duration_ms", duration);
	cJSON_AddNumberToObject(p, "rms", rms);
	cJSON_AddNumberToObject(p, "peak", peak);
	if (rms > 0) {
		cJSON_AddNumberToObject(p, "level_dbfs", round_to(dbfs, 0.1));
	} else {
		cJSON_AddNullToObject(p, "level_dbfs");
	}
	return ok(p);
}

static bool parse_color(const char *s, uint8_t rgb[3])
{
	static const struct {
		const char *name;
		uint8_t r, g, b;
	} NAMED[] = {
		{"off", 0, 0, 0},       {"red", 255, 0, 0},       {"green", 0, 255, 0},
		{"blue", 0, 0, 255},    {"white", 255, 255, 255}, {"yellow", 255, 160, 0},
		{"orange", 255, 64, 0}, {"purple", 160, 0, 255},  {"cyan", 0, 255, 255},
		{"pink", 255, 40, 120}, {"magenta", 255, 0, 255},
	};
	for (size_t i = 0; i < ARRAY_SIZE(NAMED); i++) {
		if (strcasecmp(s, NAMED[i].name) == 0) {
			rgb[0] = NAMED[i].r;
			rgb[1] = NAMED[i].g;
			rgb[2] = NAMED[i].b;
			return true;
		}
	}
	if (s[0] == '#') {
		s++;
	}
	if (strlen(s) != 6) {
		return false;
	}
	char *end;
	unsigned long v = strtoul(s, &end, 16);
	if (*end) {
		return false;
	}
	rgb[0] = v >> 16;
	rgb[1] = v >> 8;
	rgb[2] = v;
	return true;
}

static cJSON *light_set(const cJSON *params)
{
	const char *color = cJSON_GetStringValue(cJSON_GetObjectItem(params, "color"));
	const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(params, "mode"));
	uint8_t rgb[3];
	if (!color || !parse_color(color, rgb)) {
		return error("color must be a colour name or #RRGGBB");
	}
	if (!mode) {
		mode = "solid";
	}
	if (strcmp(mode, "solid") && strcmp(mode, "blink") && strcmp(mode, "breathe")) {
		return error("mode must be solid, blink or breathe");
	}
	board_set_light(rgb[0], rgb[1], rgb[2], mode);
	cJSON *p = cJSON_CreateObject();
	char hex[8];
	snprintf(hex, sizeof(hex), "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
	cJSON_AddStringToObject(p, "color", hex);
	cJSON_AddStringToObject(p, "mode", mode);
	return ok(p);
}

cJSON *commands_run(const char *command, const cJSON *params)
{
	if (strcmp(command, "device.health") == 0) {
		return device_health();
	}
	if (board_has_sensors() && strcmp(command, "motion.read") == 0) {
		return motion_read();
	}
	if (board_has_sensors() && strcmp(command, "sound.level") == 0) {
		return sound_level(params);
	}
	if (strcmp(command, "light.set") == 0) {
		return light_set(params);
	}
	char msg[96];
	snprintf(msg, sizeof(msg), "unsupported command: %s", command);
	return error(msg);
}
