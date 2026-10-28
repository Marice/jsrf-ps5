/* JSRF for PS5 - the start menu (decision log D35): before the game, on the status screen, the output resolution,
 * the picture's scale and widescreen, kept in the title folder's settings file between starts.
 *
 * It counts down five seconds and starts with the last choices; X opens the rows. The engine's own keys (scale,
 * widescreen) are upstream's, which nv2a_gl.c reads when the renderer starts; ps5_output is ours (upstream's reader
 * passes over keys it does not know). The pad is read straight from scePad, and closed again before the engine, whose
 * SDL opens it for the game.
 *
 * The Vulkan build (D37) has no output-resolution row: it renders at the scale and fills the 4K display itself. Its
 * engine has no SDL window to read the settings file, so the choices go to it through RECOMP_GL_SCALE and
 * RECOMP_WIDESCREEN. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(JSRF_PS5_RADV)
static int jsrf_ps5_display_mode(int height)
{
	(void)height; /* the Vulkan build's display is 4K */
	return 0;
}
#else
int jsrf_ps5_display_mode(int height); /* jsrf_ps5_gl.c */
#endif
void jsrf_ps5_screen_menu(const char *title, const char *subtitle, const char *const *labels,
                          const char *const *values, int count, int selected, const char *footer);

/* ---- the settings file: lines of key=value, as upstream's sdl_cfg_get/sdl_cfg_set (nv2a_gl.c) ---- */

static int cfg_get(const char *path, const char *key, int def)
{
	char line[256];
	size_t kl = strlen(key);
	int v = def;
	FILE *f = fopen(path, "r");
	if (!f)
		return def;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == '=')
			v = atoi(line + kl + 1);
	fclose(f);
	return v;
}

/* The keys given are written; every other line is kept as it was. */
static void cfg_put(const char *path, const char *const *keys, const int *values, int count)
{
	char tmp[600], line[256];
	int done[8] = {0};
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *out = fopen(tmp, "w");
	if (!out)
		return;
	FILE *in = fopen(path, "r");
	if (in) {
		while (fgets(line, sizeof line, in)) {
			int i;
			for (i = 0; i < count; i++) {
				size_t kl = strlen(keys[i]);
				if (!strncmp(line, keys[i], kl) && line[kl] == '=')
					break;
			}
			if (i == count)
				fputs(line, out);
			else if (!done[i]++)
				fprintf(out, "%s=%d\n", keys[i], values[i]);
		}
		fclose(in);
	}
	for (int i = 0; i < count; i++)
		if (!done[i])
			fprintf(out, "%s=%d\n", keys[i], values[i]);
	fclose(out);
	if (rename(tmp, path) != 0) {
		remove(path);
		rename(tmp, path);
	}
}

/* ---- the pad (scePadRead's sample, as a2-racer-2-ps5's ps5/a2_ps5_input.c) ---- */

typedef struct {
	uint32_t buttons;
	uint8_t rest[116];
} pad_sample;
_Static_assert(sizeof(pad_sample) == 120, "scePadRead sample layout");

enum {
	PAD_OPTIONS = 0x8, PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
	PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_INTERCEPTED = 0x80000000u,
};

int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int32_t *);
int sceUserServiceGetForegroundUser(int32_t *);
int scePadInit(void);
int scePadOpen(int32_t user, int type, int index, const void *parameters);
int scePadGetHandle(int32_t user, int type, int index);
int scePadRead(int handle, pad_sample *samples, int count);
int scePadClose(int handle);

static int g_pad = -1, g_pad_ours;

static void pad_open(void)
{
	int32_t user = -1;
	sceUserServiceInitialize(NULL); /* already initialised is fine */
	if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceGetForegroundUser(&user) != 0) {
		fprintf(stderr, "jsrf: menu: no user, so no pad: the countdown starts the game\n");
		return;
	}
	scePadInit();
	g_pad = scePadOpen(user, 0, 0, NULL);
	g_pad_ours = g_pad >= 0;
	if (g_pad < 0)
		g_pad = scePadGetHandle(user, 0, 0);
	fprintf(stderr, "jsrf: menu: pad handle %d (%s)\n", g_pad, g_pad_ours ? "opened here" : "already open");
}

/* The buttons held now (0 without a pad). */
static uint32_t pad_buttons(void)
{
	static uint32_t held;
	pad_sample samples[16];
	if (g_pad < 0)
		return 0;
	int n = scePadRead(g_pad, samples, 16);
	if (n > 0)
		held = samples[n - 1].buttons & ~PAD_INTERCEPTED;
	return held;
}

/* ---- the menu ---- */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const int kOutputs[] = {1080, 1440, 2160};
#if defined(JSRF_PS5_RADV)
enum { ROW_OUTPUT = -1, ROW_SCALE, ROW_WIDE, ROW_SMAA, ROW_CAMERA, ROW_CANS, ROW_DEBUG, ROW_START, ROWS };
#else
enum { ROW_OUTPUT, ROW_SCALE, ROW_WIDE, ROW_START, ROWS, ROW_CAMERA = -2, ROW_DEBUG = -3, ROW_SMAA = -4, ROW_CANS = -5 };
#endif

/* The most the scale row offers: 6x in the Vulkan build (3840x2880, in widescreen 3840x2160, the 4K screen pixel
 * for pixel, as upstream's own menus since 0.5.7); 4x in the OpenGL build, where every draw is already the limit. */
#if defined(JSRF_PS5_RADV)
#define SCALE_MAX 6
#else
#define SCALE_MAX 4
#endif

static int g_output = 2160, g_scale = 4, g_wide = 1, g_camera = 1;
/* DEBUG MODE: the test modes on the touchpad (jsrf_ps5_vk.c) and their start values; off for playing. */
int jsrf_ps5_debug;
/* SMAA over the finished frame (the backend's target_smaa, D71): upstream's key smaa=, off unless chosen. */
int jsrf_ps5_smaa;
/* INFINITE CANS: the spray cans kept at the character's most (jsrf_ps5_game.c, D72); not saved by the game. */
int jsrf_ps5_infinite_cans;

/* Before the status screen opens: the display in the kept mode, so the menu is already shown in it. */
void jsrf_ps5_menu_prepare(const char *settings)
{
	g_output = cfg_get(settings, "ps5_output", 2160);
	if (g_output != 1080 && g_output != 1440)
		g_output = 2160;
#if defined(JSRF_PS5_RADV)
	/* The test modes (D41): the backend's modes to start with, which the touchpad changes in the game. */
	{
		extern uint32_t jsrf_vk_modes;
		jsrf_vk_modes = (uint32_t)cfg_get(settings, "vk_modes", 0);
	}
	/* vk_replay=1: the captured draws in /app0/vkcap/replay.txt are drawn again before the menu (D43). */
	if (cfg_get(settings, "vk_replay", 0))
		setenv("JSRF_VK_REPLAY", "/app0/vkcap/replay.txt", 1);
#endif
	g_scale = cfg_get(settings, "scale", 4);
	if (g_scale < 1 || g_scale > SCALE_MAX)
		g_scale = 4;
	g_wide = cfg_get(settings, "widescreen", 1) != 0;
	/* upstream's key for the camera on the right stick (0.5.8, recomp_manual.c), on unless turned off */
	g_camera = cfg_get(settings, "rstick_camera", 1) != 0;
	jsrf_ps5_debug = cfg_get(settings, "debug_mode", 0) != 0;
	jsrf_ps5_smaa = cfg_get(settings, "smaa", 0) != 0;
	jsrf_ps5_infinite_cans = cfg_get(settings, "infinite_cans", 0) != 0;
	jsrf_ps5_display_mode(g_output);
	fprintf(stderr, "jsrf: menu: kept choices: output %dp, scale %d, widescreen %s, right-stick camera %s\n",
	        g_output, g_scale, g_wide ? "on" : "off", g_camera ? "on" : "off");
}

/* The menu; returns when the game is to start, with the choices written to the settings file. */
void jsrf_ps5_menu(const char *settings)
{
#if defined(JSRF_PS5_RADV)
	static const char *const labels[ROWS] = {"SCALE", "WIDESCREEN", "SMAA", "CAMERA", "INFINITE CANS", "DEBUG MODE", "START GAME"};
#else
	static const char *const labels[ROWS] = {"RESOLUTION", "SCALE", "WIDESCREEN", "START GAME"};
#endif
	char output[32], scale[32];
	const char *values[ROWS];
	char footer[96];
	int selected = ROW_START, open = 0;
	uint32_t before = 0;
	const double start = now_s();

#if defined(JSRF_PS5_RADV)
	{
		void nv2a_vk_replay(const char *list); /* src/nv2a_vk/nv2a_vk.c */
		const char *list = getenv("JSRF_VK_REPLAY");
		if (list && *list)
			nv2a_vk_replay(list);
	}
#endif
	pad_open();
	before = pad_buttons(); /* a button already held does not count as a press */
	for (;;) {
		uint32_t held = pad_buttons(), pressed = held & ~before;
		before = held;
		if (!open) {
			double left = 5.0 - (now_s() - start);
			if (left <= 0.0)
				break;
			if (pressed & PAD_CROSS) {
				open = 1;
				pressed = 0;
			} else if (pressed & PAD_OPTIONS) {
				break;
			}
			snprintf(footer, sizeof footer, "STARTING IN %d  -  PRESS X FOR SETTINGS", (int)left + 1);
		} else {
			if (pressed & PAD_UP)
				selected = (selected + ROWS - 1) % ROWS;
			if (pressed & PAD_DOWN)
				selected = (selected + 1) % ROWS;
			int step = (pressed & PAD_RIGHT) ? 1 : (pressed & PAD_LEFT) ? -1 : 0;
			if ((pressed & PAD_CROSS) && selected != ROW_START)
				step = 1;
			if ((pressed & PAD_OPTIONS) || ((pressed & PAD_CROSS) && selected == ROW_START))
				break;
			if (step && selected == ROW_OUTPUT) {
				int i = 0;
				while (kOutputs[i] != g_output)
					i++;
				int next = kOutputs[(i + 3 + step) % 3];
				if (jsrf_ps5_display_mode(next) == 0) {
					g_output = next;
				} else {
					fprintf(stderr, "jsrf: menu: %dp refused, back to %dp\n", next, g_output);
					jsrf_ps5_display_mode(g_output);
				}
			} else if (step && selected == ROW_SCALE) {
				g_scale = (g_scale - 1 + SCALE_MAX + step) % SCALE_MAX + 1;
			} else if (step && selected == ROW_WIDE) {
				g_wide = !g_wide;
			} else if (step && selected == ROW_SMAA) {
				jsrf_ps5_smaa = !jsrf_ps5_smaa;
			} else if (step && selected == ROW_CAMERA) {
				g_camera = !g_camera;
			} else if (step && selected == ROW_CANS) {
				jsrf_ps5_infinite_cans = !jsrf_ps5_infinite_cans;
			} else if (step && selected == ROW_DEBUG) {
				jsrf_ps5_debug = !jsrf_ps5_debug;
			}
			snprintf(footer, sizeof footer, "UP/DOWN CHOOSE  -  LEFT/RIGHT CHANGE  -  X ON START GAME");
		}
		snprintf(output, sizeof output, "%dP", g_output);
		/* the Xbox's 640x480 times the scale; wider in 16:9 */
		snprintf(scale, sizeof scale, "%dX  (%dX%d)", g_scale, (g_wide ? 853 : 640) * g_scale, 480 * g_scale);
#if !defined(JSRF_PS5_RADV)
		values[ROW_OUTPUT] = output;
#endif
		values[ROW_SCALE] = scale;
		values[ROW_WIDE] = g_wide ? "ON" : "OFF";
#if defined(JSRF_PS5_RADV)
		values[ROW_SMAA] = jsrf_ps5_smaa ? "ON" : "OFF";
		values[ROW_CAMERA] = g_camera ? "RIGHT STICK" : "ORIGINAL";
		values[ROW_CANS] = jsrf_ps5_infinite_cans ? "ON" : "OFF";
		values[ROW_DEBUG] = jsrf_ps5_debug ? "ON" : "OFF";
#endif
		values[ROW_START] = NULL;
		jsrf_ps5_screen_menu("JET SET RADIO FUTURE", "PS5 SETTINGS", labels, values, ROWS, open ? selected : -1,
		                     footer);
		usleep(8000);
	}
	static const char *const keys[] = {"ps5_output", "scale", "widescreen", "rstick_camera", "debug_mode", "smaa", "infinite_cans"};
	const int kept[] = {g_output, g_scale, g_wide, g_camera, jsrf_ps5_debug, jsrf_ps5_smaa, jsrf_ps5_infinite_cans};
	cfg_put(settings, keys, kept, 7);
	fprintf(stderr, "jsrf: menu: starting with output %dp, scale %d, widescreen %s, SMAA %s, right-stick camera %s, "
	        "infinite cans %s, debug %s\n", g_output, g_scale, g_wide ? "on" : "off", jsrf_ps5_smaa ? "on" : "off",
	        g_camera ? "on" : "off", jsrf_ps5_infinite_cans ? "on" : "off", jsrf_ps5_debug ? "on" : "off");
#if defined(JSRF_PS5_RADV)
	snprintf(scale, sizeof scale, "%d", g_scale);
	setenv("RECOMP_GL_SCALE", scale, 1);
	setenv("RECOMP_WIDESCREEN", g_wide ? "1" : "0", 1);
	/* the engine has no window layer to ask (nv_window_pref_rstick_camera is upstream's weak "off"): it reads this */
	setenv("RECOMP_RSTICK_CAMERA", g_camera ? "1" : "0", 1);
	/* the engine's development reports only with DEBUG MODE on (D67): the sound ring once a second (D28), and the
	   first kernel calls (upstream's budget, 200 by default) */
	if (jsrf_ps5_debug)
		setenv("RECOMP_APU_STATS", "1", 0);
	else
		setenv("RECOMP_KERNEL_LOG_BUDGET", "0", 0);
	if (!jsrf_ps5_debug) {
		extern uint32_t jsrf_vk_modes;
		jsrf_vk_modes = 0; /* the settings file's vk_modes only with DEBUG MODE on */
	}
#endif
	if (g_pad_ours)
		scePadClose(g_pad); /* the game's SDL opens it */
	g_pad = -1;
}
