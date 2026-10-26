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
enum { ROW_OUTPUT = -1, ROW_SCALE, ROW_WIDE, ROW_START, ROWS };
#else
enum { ROW_OUTPUT, ROW_SCALE, ROW_WIDE, ROW_START, ROWS };
#endif

/* The most the scale row offers: 6x in the Vulkan build (3840x2880, in widescreen 3840x2160, the 4K screen pixel
 * for pixel, as upstream's own menus since 0.5.7); 4x in the OpenGL build, where every draw is already the limit. */
#if defined(JSRF_PS5_RADV)
#define SCALE_MAX 6
#else
#define SCALE_MAX 4
#endif

static int g_output = 2160, g_scale = 4, g_wide = 1;

/* Before the status screen opens: the display in the kept mode, so the menu is already shown in it. */
void jsrf_ps5_menu_prepare(const char *settings)
{
	g_output = cfg_get(settings, "ps5_output", 2160);
	if (g_output != 1080 && g_output != 1440)
		g_output = 2160;
	g_scale = cfg_get(settings, "scale", 4);
	if (g_scale < 1 || g_scale > SCALE_MAX)
		g_scale = 4;
	g_wide = cfg_get(settings, "widescreen", 1) != 0;
	jsrf_ps5_display_mode(g_output);
	fprintf(stderr, "jsrf: menu: kept choices: output %dp, scale %d, widescreen %s\n", g_output, g_scale,
	        g_wide ? "on" : "off");
}

/* The menu; returns when the game is to start, with the choices written to the settings file. */
void jsrf_ps5_menu(const char *settings)
{
#if defined(JSRF_PS5_RADV)
	static const char *const labels[ROWS] = {"SCALE", "WIDESCREEN", "START GAME"};
#else
	static const char *const labels[ROWS] = {"RESOLUTION", "SCALE", "WIDESCREEN", "START GAME"};
#endif
	char output[32], scale[32];
	const char *values[ROWS];
	char footer[96];
	int selected = ROW_START, open = 0;
	uint32_t before = 0;
	const double start = now_s();

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
		values[ROW_START] = NULL;
		jsrf_ps5_screen_menu("JET SET RADIO FUTURE", "PS5 SETTINGS", labels, values, ROWS, open ? selected : -1,
		                     footer);
		usleep(8000);
	}
	static const char *const keys[] = {"ps5_output", "scale", "widescreen"};
	const int kept[] = {g_output, g_scale, g_wide};
	cfg_put(settings, keys, kept, 3);
	fprintf(stderr, "jsrf: menu: starting with output %dp, scale %d, widescreen %s\n", g_output, g_scale,
	        g_wide ? "on" : "off");
#if defined(JSRF_PS5_RADV)
	snprintf(scale, sizeof scale, "%d", g_scale);
	setenv("RECOMP_GL_SCALE", scale, 1);
	setenv("RECOMP_WIDESCREEN", g_wide ? "1" : "0", 1);
#endif
	if (g_pad_ours)
		scePadClose(g_pad); /* the game's SDL opens it */
	g_pad = -1;
}
