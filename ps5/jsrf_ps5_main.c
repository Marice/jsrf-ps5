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
#include <sched.h>
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

int jsrf_ps5_status_begin(void); /* jsrf_ps5_vk.c */
void jsrf_ps5_status_end(void);
int jsrf_ps5_gl_prepare(void);
int __real_fflush(FILE *stream);
void jsrf_ps5_screen_lines(const char *title, const char *const *lines, const int *pass, int count); /* screen */
void jsrf_ps5_menu_prepare(const char *settings); /* jsrf_ps5_menu.c */
void jsrf_ps5_menu(const char *settings);

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
	{"RECOMP_APU_STATS", "1"}, /* dev builds: the sound ring once a second (decision log D28) */


	{"JSRF_GAME_DIR", GAME_DIR}, {"JSRF_SAVE_DIR", SAVE_DIR}, {"JSRF_SETTINGS_FILE", SETTINGS_FILE},
	{"TMPDIR", TITLE_DIR},
};

__attribute__((constructor(102))) static void ps5_env(void)
{
	for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
		setenv(k_env[i][0], k_env[i][1], 0);
}

/* ---- the status screen, until the game's renderer starts ---- */

static void status3(const char *line1, const char *line2, const char *line3, int failed)
{
	const char *lines[3] = {line1, line2 ? line2 : "", line3 ? line3 : ""};
	const int pass[3] = {!failed, !failed, !failed};
	jsrf_ps5_screen_lines("JET SET RADIO FUTURE", lines, pass, line3 ? 3 : 2);
}

static void status(const char *line1, const char *line2, int failed)
{
	status3(line1, line2, NULL, failed);
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
#if defined(JSRF_PS5_RADV)
/* RADV's link recipe binds opendir and its kin to the platform's own, which can list a folder; the --wrap names then
   leave __real_opendir at address 0 (the first import with no game/ jumped there). */
#define __real_opendir opendir
#define __real_readdir readdir
#define __real_closedir closedir
#else
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);
#endif

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
		/* about 3,200 files, 2.5 GB: about ten minutes on the console's own storage */
		status3("UNPACKING YOUR DISC IMAGE, ONCE", line, "GRAB SOME SNACKS, THIS TAKES ABOUT 10 MINUTES...", 0);
		if (percent % 10 == 0) {
			fprintf(stderr, "jsrf: unpacking %s\n", line);
			fflush(stderr); /* the log is buffered: the progress is in it as it happens */
		}
	}
	return 0;
}

/* ---- the game's folder listing ----
   A title may not read folders here (opendir is refused), and the game asks for its files in other cases than the
   disc has them (stage01.bin for Stage01.bin): upstream finds them by listing the folder (kernel_path.c), the bug
   upstream's 0.1.1 fixed on Linux, which left New Game on "Now Loading" for good. ps5-posix.c answers a refused
   opendir from dirs.txt in the title folder, so the title writes one for game/ from the disc image (decision log
   D15): lines of folder, name and d or f, the folder relative to the title folder. */
#define DIRS_FILE TITLE_DIR "/dirs.txt"
#define DIRS_PART TITLE_DIR "/dirs.txt.part"

struct listing {
	FILE *out;
	const char *from; /* the folder the paths start with, replaced by "game" */
	unsigned lines;
};

static void list_entry(void *ctx, const char *path, int is_dir)
{
	struct listing *l = ctx;
	size_t n = strlen(l->from);
	if (strncmp(path, l->from, n) != 0)
		return;
	const char *rel = path + n; /* "/Media/Stage/stg00_03.dat" */
	const char *slash = strrchr(rel, '/');
	if (!slash)
		return;
	fprintf(l->out, "game%.*s\t%s\t%c\n", (int)(slash - rel), rel, slash + 1, is_dir ? 'd' : 'f');
	l->lines++;
}

/* dirs.txt for game/ from the disc image; 0 when written. */
static int write_listing(const char *iso)
{
	char err[512];
	struct listing l = {fopen(DIRS_PART, "w"), "/LIST", 0};
	if (!l.out)
		return -1;
	fprintf(l.out, "\tgame\td\n");
	int r = xiso_list(iso, "/LIST", list_entry, &l, err, sizeof err);
	fclose(l.out);
	if (r != 0 || rename(DIRS_PART, DIRS_FILE) != 0) {
		fprintf(stderr, "jsrf: the game's folder listing: %s\n", r ? err : strerror(errno));
		unlink(DIRS_PART);
		return -1;
	}
	fprintf(stderr, "jsrf: the game's folder listing written: %u entries\n", l.lines + 1);
	fflush(stderr);
	return 0;
}

/* game/ from the disc image in dump/, through game.unpacking/ so a cut-off unpack is never taken for the game. */
static int import_disc(void)
{
	char iso[512], why[640], err[512];
	if (find_iso(iso, sizeof iso) != 0)
		return stay_with("NO DISC IMAGE FOUND", "PUT YOUR JSRF (USA) .ISO IN THE TITLE FOLDER'S DUMP FOLDER");
	fprintf(stderr, "jsrf: disc image %s\n", iso);
	fflush(stderr);
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
	if (write_listing(iso) != 0)
		return stay_with("COULD NOT LIST THE GAME'S FOLDERS", "SEE JSRF.LOG IN THE TITLE FOLDER");
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

int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);

static void log_flexible(const char *when)
{
	size_t free_size = 0;
	sceKernelAvailableFlexibleMemorySize(&free_size);
	fprintf(stderr, "jsrf: flexible memory %zu MiB free %s\n", free_size >> 20, when);
	fflush(stderr);
}

/* How long the short waits the engine leans on really take here (decision log D8): Sleep(1) in its timer thread,
   usleep(20) on a critical section's hand-over, sched_yield; 50 of each, the mean and the longest. */
static double clock_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void *wake_after_3s(void *cv)
{
	struct timespec ts = {3, 0};
	nanosleep(&ts, NULL);
	pthread_cond_broadcast(cv);
	return NULL;
}

static void measure_waits(void)
{
	static const char *const names[3] = {"nanosleep(1 ms)", "usleep(20)", "sched_yield"};
	for (int k = 0; k < 3; k++) {
		double sum = 0.0, worst = 0.0;
		for (int i = 0; i < 50; i++) {
			double t0 = clock_ms();
			if (k == 0) {
				struct timespec ts = {0, 1000000};
				nanosleep(&ts, NULL);
			} else if (k == 1) {
				usleep(20);
			} else {
				sched_yield();
			}
			double d = clock_ms() - t0;
			sum += d;
			if (d > worst)
				worst = d;
		}
		fprintf(stderr, "jsrf: wait %-16s %.3f ms on average, %.3f ms at most\n", names[k], sum / 50, worst);
	}
	/* pthread_cond_timedwait with a 200 us deadline on CLOCK_REALTIME, as the engine's ack thread and its Win32
	   waits use it (decision log D9) */
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	pthread_t waker; /* wakes the wait after 3 s, should it never time out */
	pthread_create(&waker, NULL, wake_after_3s, &cv);
	pthread_detach(waker);
	double sum = 0.0, worst = 0.0;
	for (int i = 0; i < 20; i++) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 200000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec++;
		}
		double t0 = clock_ms();
		pthread_mutex_lock(&mu);
		pthread_cond_timedwait(&cv, &mu, &ts);
		pthread_mutex_unlock(&mu);
		double d = clock_ms() - t0;
		sum += d;
		if (d > worst)
			worst = d;
		if (d > 2000.0)
			break; /* it does not time out: enough */
	}
	fprintf(stderr, "jsrf: wait %-16s %.3f ms on average, %.3f ms at most\n", "cond 200 us", sum / 20, worst);
	fflush(stderr);
}

int nv_ps5_main(int argc, char **argv, int (*host)(void));
int nv_ps5_main(int argc, char **argv, int (*host)(void))
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "jsrf: JSRF Unleashed %s for PS5\n", JSRF_VERSION);
	log_flexible("at the start");
	jsrf_ps5_menu_prepare(SETTINGS_FILE); /* the kept output mode, before the display opens (D35) */
	if (jsrf_ps5_status_begin() != 0)
		return 1;
	if (!exists(GAME_DIR "/default.xbe")) {
		status("LOOKING FOR YOUR DISC IMAGE", NULL, 0);
		if (import_disc() != 0)
			return 1;
	}
	if (!exists(DIRS_FILE)) {
		/* unpacked by a build before the listing: made now, from the image if it is still in dump/ */
		char iso[512];
		status("LISTING THE GAME'S FOLDERS, ONCE", NULL, 0);
		if (find_iso(iso, sizeof iso) != 0 || write_listing(iso) != 0)
			return stay_with("THE GAME NEEDS ITS DISC IMAGE ONCE MORE",
			                 "PUT THE .ISO BACK IN DUMP/ (OR REMOVE GAME/ TO UNPACK AGAIN)");
	}
	char why[640];
	if (jsrf_xbe_check_folder(GAME_DIR, why, sizeof why) != JSRF_XBE_OK) {
		fprintf(stderr, "jsrf: game/: %s\n", why);
		return stay_with("THE GAME FOLDER IS NOT THE USA RELEASE", "REMOVE GAME/ AND PUT THE RIGHT .ISO IN DUMP/");
	}
	/* The PS5's defaults, on the first start (decision log D30): widescreen 16:9 (upstream's wider view, not a stretch)
	   and the picture made at 4x the Xbox's (2560x1920, wider in 16:9) for the 2160p output (D31). The keys are
	   upstream's (nv2a_gl.c sdl_cfg_get); F9/F10 set them on a PC, which a DualSense does not have. A file that is there
	   is left as it is. */
	if (!exists(SETTINGS_FILE)) {
		FILE *f = fopen(SETTINGS_FILE, "w");
		if (f) {
			fputs("scale=4\nwidescreen=1\n", f);
			fclose(f);
			fprintf(stderr, "jsrf: settings written: scale 4, widescreen\n");
		}
	}
	jsrf_ps5_menu(SETTINGS_FILE); /* resolution, scale, widescreen; five seconds, X to change them (D35) */
	status("STARTING", NULL, 0);
	log_flexible("before the engine");
	measure_waits();
	{
		extern void ps5_log_main_cores(void);
		ps5_log_main_cores();
	}
	/* The status screen's context lets go of the surface on this thread: the renderer makes its own current on its
	   thread, and EGL refused that while this one was still current here (0x3009, decision log D5). */
	jsrf_ps5_status_end();
	{
		extern void jsrf_prof_register(int number); /* the thread profiler's (D45): the main thread is -2 */
		jsrf_prof_register(-2);
	}
	if (jsrf_ps5_gl_prepare() != 0)
		return stay_with("NO OPENGL CONTEXT FOR THE RENDERER", "SEE JSRF.LOG IN THE TITLE FOLDER");
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
	__real_fflush(NULL); /* fflush is held to once a second (ps5-posix.c): the log out now */
	_exit(call.rc); /* as A2 (its D10): no second teardown */
}
