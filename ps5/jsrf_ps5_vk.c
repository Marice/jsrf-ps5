/* JSRF for PS5, the Vulkan build: what jsrf_ps5_gl.c and jsrf_ps5_screen.c are to the OpenGL build.
 *
 * The game on Mihawk's RADV toolchain (decision log D34) with the Vulkan renderer (D36); the status screen and the
 * start menu are jsrf_ps5_screen.c's and jsrf_ps5_menu.c's, as in the OpenGL build (D37). */
#include <stdio.h>
#include <stdlib.h>

/* The status screen and the start menu draw through the Vulkan device (jsrf_ps5_screen.c, D37), which opens the
 * display when it first shows something. */
int jsrf_ps5_status_begin(void)
{
	/* RADV reads its options through our getenv (ps5-posix.c): its error messages into the log. */
	setenv("MESA_DEBUG", "1", 0);
	return 0;
}

void jsrf_ps5_status_end(void)
{
}

int jsrf_ps5_gl_prepare(void)
{
	return 0;
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

/* Imports that only libScePosixForWebKit's stub defines, whose entries a title gets as null (as radv-link.sh notes
 * for strcasestr): the first console run of this build called fnmatch through 0 in NtQueryDirectoryFile (D34). The
 * OpenGL SDK carried its own; these are ours. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fold(int c, int flags)
{
	return (flags & FNM_CASEFOLD) ? tolower((unsigned char)c) : (unsigned char)c;
}

/* One bracket expression at *p against c; *p moves past it. -1 when the bracket is not closed (then '[' is literal). */
static int match_bracket(const char **p, int c, int flags)
{
	const char *s = *p + 1;
	int negate = 0, hit = 0;
	if (*s == '!' || *s == '^') { negate = 1; s++; }
	const char *first = s;
	while (*s && (*s != ']' || s == first)) {
		int lo = *s, hi;
		if (lo == '\\' && !(flags & FNM_NOESCAPE) && s[1]) lo = *++s;
		s++;
		hi = lo;
		if (*s == '-' && s[1] && s[1] != ']') {
			s++;
			hi = *s;
			if (hi == '\\' && !(flags & FNM_NOESCAPE) && s[1]) hi = *++s;
			s++;
		}
		if (fold(lo, flags) <= fold(c, flags) && fold(c, flags) <= fold(hi, flags)) hit = 1;
	}
	if (*s != ']') return -1;
	*p = s + 1;
	return hit != negate;
}

static int match_here(const char *p, const char *s, const char *start, int flags)
{
	for (;;) {
		int c = *p;
		if (c == '\0') return *s == '\0';
		if (*s == '.' && (flags & FNM_PERIOD) &&
		    (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')) && c != '.')
			return 0;
		if (c == '*') {
			while (*p == '*') p++;
			if (*p == '\0') return !(flags & FNM_PATHNAME) || strchr(s, '/') == NULL;
			for (; *s; s++) {
				if (match_here(p, s, start, flags & ~FNM_PERIOD)) return 1;
				if (*s == '/' && (flags & FNM_PATHNAME)) return 0;
			}
			return match_here(p, s, start, flags);
		}
		if (*s == '\0') return 0;
		if (c == '?') {
			if (*s == '/' && (flags & FNM_PATHNAME)) return 0;
			p++;
			s++;
			continue;
		}
		if (c == '[' && !(*s == '/' && (flags & FNM_PATHNAME))) {
			const char *q = p;
			int r = match_bracket(&q, *s, flags);
			if (r == 0) return 0;
			if (r == 1) { p = q; s++; continue; }
		}
		if (c == '\\' && !(flags & FNM_NOESCAPE) && p[1]) c = *++p;
		if (fold(c, flags) != fold(*s, flags)) return 0;
		p++;
		s++;
	}
}

int fnmatch(const char *pattern, const char *string, int flags)
{
	return match_here(pattern, string, string, flags) ? 0 : FNM_NOMATCH;
}

char *strcasestr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);
	for (; *haystack; haystack++)
		if (strncasecmp(haystack, needle, n) == 0) return (char *)haystack;
	return n == 0 ? (char *)haystack : NULL;
}

int mkstemp(char *template)
{
	static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(template);
	if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		for (int i = 0; i < 6; i++) template[len - 6 + i] = letters[arc4random_uniform(sizeof letters - 1)];
		int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST) return fd;
	}
	errno = EEXIST;
	return -1;
}

/* Anti-aliasing as the window would have it: off (the Vulkan device has neither SMAA nor MSAA yet). */
int nv_window_pref_smaa(void)
{
	return 0;
}

int nv_window_pref_msaa(void)
{
	return 0;
}
