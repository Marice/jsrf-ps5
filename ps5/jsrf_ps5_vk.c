/* JSRF for PS5, the Vulkan build: what jsrf_ps5_gl.c and jsrf_ps5_screen.c are to the OpenGL build.
 *
 * Phase V1 (decision log D34): the game on Mihawk's RADV toolchain with no renderer yet, so there is no screen. The
 * status lines go to the log only; the swapchain that shows them, and the game, comes with the Vulkan device (V2). */
#include <stdio.h>

int jsrf_ps5_status_begin(void)
{
	fprintf(stderr, "jsrf: Vulkan build, phase V1: no screen yet\n");
	return 0;
}

void jsrf_ps5_status_end(void)
{
}

int jsrf_ps5_gl_prepare(void)
{
	return 0;
}

void jsrf_ps5_screen_lines(const char *title, const char *const *lines, const int *pass, int count)
{
	(void)pass;
	fprintf(stderr, "jsrf: screen: %s", title);
	for (int i = 0; i < count; i++)
		if (lines[i]) fprintf(stderr, " | %s", lines[i]);
	fputc('\n', stderr);
}

/* The OpenGL build's present timing (nv2a_gl.c, D13), which the ack thread reports every few seconds: there is no
 * present in V1, so the clock runs and the report says nothing. */
#include <time.h>
double ps5_prof_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
unsigned g_ps5p_frames;
void ps5_present_report(double span_ms)
{
	(void)span_ms;
}

/* C library functions the SDK fork's libc lacks, which the OpenGL SDK supplied (as rush2-ps5's ps5/ps5-shims.c). */
#include <stdint.h>

/* The C locale is the only one in a title: one byte per character (libiconv). */
int ___mb_cur_max(void)
{
	return 1;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* mktime for UTC (win32_compat.c's SystemTimeToFileTime). */
time_t timegm(struct tm *tm)
{
	int64_t year = (int64_t)tm->tm_year + 1900, month = tm->tm_mon;
	year += month / 12;
	month %= 12;
	if (month < 0) {
		month += 12;
		year -= 1;
	}
	int64_t days = days_from_civil(year, (unsigned)month + 1, 1) + (tm->tm_mday - 1);
	return (time_t)(days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec);
}

/* Upstream's window hooks, which its SDL window defines (nv2a_gl.c) and the input and widescreen code ask for weakly:
 * the title converter refuses a weak symbol nothing defines. No keyboard yet; widescreen as the settings default. */
int nv_window_keys(unsigned short *buttons, unsigned char *analog)
{
	(void)buttons;
	(void)analog;
	return 0;
}

int nv_window_pref_widescreen(void)
{
	return 1;
}
