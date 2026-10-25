/* JSRF for PS5 - the title's start (decision log D1, D3): the game's files from the player's disc image when they
 * are not there yet, the environment the desktop launchers set, then the engine (port/main.c host_main) on a thread
 * with a large stack, as port/android/jsrf_android.c does on Android.
 *
 * Folders, all in the title folder (/app0, writable under ShadowMountPlus):
 *   dump/      the player's disc image (.iso), unpacked once into game/
 *   game/      the game's files (default.xbe and the rest), as on every other platform
 *   save/      the saves (upstream: JSRF_SAVE_DIR)
 *   settings   the engine's stored choices (upstream: JSRF_SETTINGS_FILE) */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ios/xiso.h"
#include "xbe_check.h"

#define TITLE_DIR "/app0"
#define DUMP_DIR TITLE_DIR "/dump"
#define GAME_DIR TITLE_DIR "/game"
#define UNPACK_DIR TITLE_DIR "/game.unpacking"
#define SAVE_DIR TITLE_DIR "/save"
#define SETTINGS_FILE TITLE_DIR "/settings"
#define ISO_NAME "JSRF - Jet Set Radio Future (USA).iso"

int jsrf_ps5_status_begin(void);                                                          /* jsrf_ps5_gl.c */
void jsrf_ps5_screen_lines(const char *title, const char *const *lines, const int *pass, int count); /* screen */

/* The desktop and Android launchers' settings (port/android/jsrf_android.c k_env), without Android's own SDL ones;
   SDL's video is the offscreen driver, the picture goes through EGL (jsrf_ps5_gl.c). Set before the runtime's own
   constructors read them; anything already set wins. */
static const char *const k_env[][2] = {
	{"RECOMP_USB", "1"}, {"RECOMP_GL", "1"}, {"RECOMP_PB_EXEC", "1"},
	{"RECOMP_VBLANK", "1"}, {"RECOMP_AC97_READY_NOTRAP", "1"},
	{"RECOMP_TRACE_BUDGET", "0"}, {"RECOMP_GL_READBACK", "0"}, {"RECOMP_FPS", "60"},
	{"RECOMP_ABI_RESTORE", "1"}, {"RECOMP_LOCK_IDLE", "1"},
	{"RECOMP_GL_CURTAIN", "skip"}, {"RECOMP_MUTE", "0"}, {"RECOMP_WINDOW", "1"},
	{"RECOMP_FULLSCREEN", "1"}, {"RECOMP_WINDOW_W", "1920"}, {"RECOMP_WINDOW_H", "1080"},
	{"SDL_VIDEODRIVER", "offscreen"},
	{"JSRF_GAME_DIR", GAME_DIR}, {"JSRF_SAVE_DIR", SAVE_DIR}, {"JSRF_SETTINGS_FILE", SETTINGS_FILE},
	{"TMPDIR", TITLE_DIR},
};

__attribute__((constructor(102))) static void ps5_env(void)
{
	for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
		setenv(k_env[i][0], k_env[i][1], 0);
}

/* ---- the status screen, until the game's renderer starts ---- */

static void status(const char *line1, const char *line2, int failed)
{
	const char *lines[2] = {line1, line2 ? line2 : ""};
	const int pass[2] = {!failed, !failed};
	jsrf_ps5_screen_lines("JET SET RADIO FUTURE", lines, pass, 2);
}

/* Stays on a message the player has to act on, until the title is closed (the PS button). */
static int stay_with(const char *line1, const char *line2)
{
	fprintf(stderr, "jsrf: %s %s\n", line1, line2 ? line2 : "");
	fflush(stderr);
	for (;;) {
		status(line1, line2, 1);
		struct timespec wait = {0, 250 * 1000 * 1000};
		nanosleep(&wait, NULL);
	}
	return 1;
}

static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* The first .iso in dump/; the usual name when the folder cannot be listed. */
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);

static int find_iso(char *out, size_t cap)
{
	DIR *d = __real_opendir(DUMP_DIR);
	if (d) {
		struct dirent *e;
		while ((e = __real_readdir(d)) != NULL) {
			size_t n = strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".iso") == 0 && e->d_name[0] != '.') {
				snprintf(out, cap, "%s/%s", DUMP_DIR, e->d_name);
				__real_closedir(d);
				return 0;
			}
		}
		__real_closedir(d);
	}
	snprintf(out, cap, "%s/%s", DUMP_DIR, ISO_NAME);
	return exists(out) ? 0 : -1;
}

static int on_progress(void *ctx, uint64_t done, uint64_t total)
{
	(void)ctx;
	static int shown = -1;
	int percent = total ? (int)(done * 100 / total) : 0;
	if (percent != shown) {
		shown = percent;
		char line[64];
		snprintf(line, sizeof line, "%d%% (%llu OF %llu MB)", percent, (unsigned long long)(done >> 20),
		         (unsigned long long)(total >> 20));
		status("UNPACKING YOUR DISC IMAGE, ONCE", line, 0);
		if (percent % 10 == 0)
			fprintf(stderr, "jsrf: unpacking %s\n", line);
	}
	return 0;
}

/* game/ from the disc image in dump/, through game.unpacking/ so a cut-off unpack is never taken for the game. */
static int import_disc(void)
{
	char iso[512], why[640], err[512];
	if (find_iso(iso, sizeof iso) != 0)
		return stay_with("NO DISC IMAGE FOUND", "PUT YOUR JSRF (USA) .ISO IN THE TITLE FOLDER'S DUMP FOLDER");
	fprintf(stderr, "jsrf: disc image %s\n", iso);
	status("CHECKING YOUR DISC IMAGE", NULL, 0);
	int r = jsrf_xbe_check_iso(iso, why, sizeof why);
	if (r != JSRF_XBE_OK) {
		fprintf(stderr, "jsrf: the disc image is not the one the engine runs (%d): %s\n", r, why);
		return stay_with("THIS IS NOT THE STANDALONE US DISC", "JSRF UNLEASHED RUNS ONLY THE USA (NTSC) RELEASE");
	}
	mkdir(UNPACK_DIR, 0755);
	if (xiso_extract(iso, UNPACK_DIR, on_progress, NULL, err, sizeof err) != 0) {
		fprintf(stderr, "jsrf: unpacking failed: %s\n", err);
		return stay_with("UNPACKING FAILED", "SEE JSRF.LOG IN THE TITLE FOLDER");
	}
	if (rename(UNPACK_DIR, GAME_DIR) != 0) {
		fprintf(stderr, "jsrf: %s -> %s: %s\n", UNPACK_DIR, GAME_DIR, strerror(errno));
		return stay_with("UNPACKING FAILED", "SEE JSRF.LOG IN THE TITLE FOLDER");
	}
	fprintf(stderr, "jsrf: the disc image is unpacked; it may be removed from dump/\n");
	return 0;
}

/* ---- the engine ---- */

struct host_call {
	int (*host)(void);
	int rc;
};

static void *host_thread(void *p)
{
	struct host_call *c = p;
	c->rc = c->host();
	return NULL;
}

int nv_ps5_main(int argc, char **argv, int (*host)(void));
int nv_ps5_main(int argc, char **argv, int (*host)(void))
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "jsrf: JSRF Unleashed %s for PS5\n", JSRF_VERSION);
	if (jsrf_ps5_status_begin() != 0)
		return 1;
	if (!exists(GAME_DIR "/default.xbe")) {
		status("LOOKING FOR YOUR DISC IMAGE", NULL, 0);
		if (import_disc() != 0)
			return 1;
	}
	char why[640];
	if (jsrf_xbe_check_folder(GAME_DIR, why, sizeof why) != JSRF_XBE_OK) {
		fprintf(stderr, "jsrf: game/: %s\n", why);
		return stay_with("THE GAME FOLDER IS NOT THE USA RELEASE", "REMOVE GAME/ AND PUT THE RIGHT .ISO IN DUMP/");
	}
	status("STARTING", NULL, 0);
	mkdir(SAVE_DIR, 0755);
	chdir(GAME_DIR);

	struct host_call call = {host, 1};
	pthread_attr_t a;
	pthread_t t;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 16u << 20);
	if (pthread_create(&t, &a, host_thread, &call) != 0) {
		fprintf(stderr, "jsrf: no thread for the engine\n");
		return 1;
	}
	pthread_attr_destroy(&a);
	pthread_join(t, NULL);
	fprintf(stderr, "jsrf: the engine returned %d\n", call.rc);
	fflush(NULL);
	_exit(call.rc); /* as A2 (its D10): no second teardown */
}
