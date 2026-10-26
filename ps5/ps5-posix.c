/* POSIX calls a PS5 title does not survive, replaced at link time.
 *
 * A native title has no environment, no working directory and no argv, and
 * the C library's own versions of these do not fail politely: they take the
 * process down. Game code calls them everywhere (getenv alone appears 57
 * times in SuperTuxKart), so wrapping them once is the only way through.
 *
 * Linked with --wrap, so every call reaches these instead, including the
 * calls made from inside the bundled libraries and the C++ runtime.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The Vulkan build (decision log D34): the platform layer of the SDK fork wraps
   pthread_create (thread stacks in direct memory) and fflush itself, and a name
   is wrapped once. Ours run first under their own names, which the rest of the
   build calls (ps5-vk.cmake), and hand on to the platform's: pthread_create here
   is the platform's wrap, __real_fflush still libc's (the linker's --wrap). */
#if defined(JSRF_PS5_RADV)
#undef pthread_create
#undef fflush
int fflush(FILE* stream); /* stdio.h declared it under the other name */
#define __wrap_pthread_create jsrf_pthread_create
#define __real_pthread_create pthread_create
#define __wrap_fflush jsrf_fflush
#endif

/* The title folder. It is writable when the title runs from ShadowMountPlus,
   which is where the game keeps its log, and it is the only folder a title
   can count on being able to reach. */
static const char kAppRoot[] = "/app0";

/* A title has no environment of its own (the C library's ends with nothing in
   it), but JSRF Unleashed is configured through one: the launchers set
   RECOMP_* and JSRF_* variables, and the engine reads them with getenv
   (jsrf-ps5 decision log D3). So the environment is kept here, in the process:
   setenv and putenv store, getenv finds, unsetenv removes. A value handed out
   by getenv is never freed, so a pointer a thread holds stays valid. HOME is
   the title folder unless set. */
#include <pthread.h>

#define ENV_MAX 128
static struct { char* name; char* value; } g_env[ENV_MAX];
static int g_env_count = 0;
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;

static int env_find(const char* name, size_t n)
{
	for (int i = 0; i < g_env_count; i++)
		if (strlen(g_env[i].name) == n && strncmp(g_env[i].name, name, n) == 0) return i;
	return -1;
}

static int env_set(const char* name, size_t n, const char* value, int overwrite)
{
	if (name == NULL || n == 0 || memchr(name, '=', n) != NULL || value == NULL) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&g_env_lock);
	int i = env_find(name, n);
	if (i >= 0 && !overwrite) {
		pthread_mutex_unlock(&g_env_lock);
		return 0;
	}
	char* copy = strdup(value);
	if (copy == NULL || (i < 0 && g_env_count == ENV_MAX)) {
		pthread_mutex_unlock(&g_env_lock);
		free(copy);
		errno = ENOMEM;
		return -1;
	}
	if (i < 0) {
		i = g_env_count++;
		g_env[i].name = strndup(name, n);
	}
	g_env[i].value = copy;   /* the old value is left: a reader may hold it */
	pthread_mutex_unlock(&g_env_lock);
	return 0;
}

char* __wrap_getenv(const char* name)
{
	if (name == NULL) return NULL;
	pthread_mutex_lock(&g_env_lock);
	int i = env_find(name, strlen(name));
	char* value = i >= 0 ? g_env[i].value : NULL;
	pthread_mutex_unlock(&g_env_lock);
	if (value == NULL && strcmp(name, "HOME") == 0) return (char*)kAppRoot;
	return value;
}

int __wrap_setenv(const char* name, const char* value, int overwrite)
{
	return env_set(name, name ? strlen(name) : 0, value, overwrite);
}

int __wrap_putenv(char* string)
{
	const char* eq = string ? strchr(string, '=') : NULL;
	if (eq == NULL) {
		errno = EINVAL;
		return -1;
	}
	return env_set(string, (size_t)(eq - string), eq + 1, 1);
}

int __wrap_unsetenv(const char* name)
{
	if (name == NULL) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&g_env_lock);
	int i = env_find(name, strlen(name));
	if (i >= 0) g_env[i] = g_env[--g_env_count];   /* name and value left for readers */
	pthread_mutex_unlock(&g_env_lock);
	return 0;
}

/* A title has no working directory of its own, and the C library's getcwd
   ends the process. Irrlicht still relies on one: it lists a model folder by
   changing into it and listing ".". So the working directory is kept here,
   starting at the title folder; chdir moves it when the folder exists, and
   relative paths in this file resolve against it. */
static char g_cwd[PATH_MAX] = "/app0";

static int normalise_path(const char* path, char* out, size_t size);

char* __wrap_getcwd(char* buf, size_t size)
{
	size_t len = strlen(g_cwd) + 1;
	if (buf == NULL) {
		buf = malloc(size > len ? size : len);
		if (buf == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	} else if (size < len) {
		errno = ERANGE;
		return NULL;
	}
	memcpy(buf, g_cwd, len);
	return buf;
}

int __wrap_chdir(const char* path)
{
	if (path == NULL || path[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	char target[PATH_MAX];
	if (normalise_path(path, target, sizeof(target)) != 0) return -1;
	struct stat info;
	if (stat(target, &info) != 0) return -1;
	if (!S_ISDIR(info.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	strcpy(g_cwd, target);
	return 0;
}

/* access() is where the game stopped: it is the first thing Irrlicht asks of
   the file system, and the title process ended inside it. stat() is known to
   work in a title (OliSe Player and the log above rely on it), so existence is
   answered with that. The permission bits are not checked: the sandbox, not
   the mode, decides what a title may do, and an open or a write reports that
   itself. */
int __wrap_access(const char* path, int mode)
{
	(void)mode;
	struct stat info;
	if (path == NULL || path[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	return stat(path, &info) == 0 ? 0 : -1;
}

/* Copies path into out, resolving "." and ".." by text alone. A relative path
   is taken against the working directory kept above. */
static int normalise_path(const char* path, char* out, size_t size)
{
	size_t len = 0;
	if (path[0] != '/') {
		len = strlen(g_cwd);
		if (len + 1 > size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(out, g_cwd, len);
		if (len == 1) len = 0;   /* "/" itself: the parts add their own slash */
	}
	out[len] = '\0';

	const char* p = path;
	while (*p) {
		while (*p == '/') p++;
		const char* end = p;
		while (*end && *end != '/') end++;
		size_t part = (size_t)(end - p);

		if (part == 0 || (part == 1 && p[0] == '.')) {
			/* nothing to add */
		} else if (part == 2 && p[0] == '.' && p[1] == '.') {
			while (len > 0 && out[len - 1] != '/') len--;
			if (len > 0) len--;      /* the slash before it */
			out[len] = '\0';
		} else {
			if (len + 1 + part + 1 > size) {
				errno = ENAMETOOLONG;
				return -1;
			}
			out[len++] = '/';
			memcpy(out + len, p, part);
			len += part;
			out[len] = '\0';
		}
		p = end;
	}
	if (len == 0) {
		out[0] = '/';
		out[1] = '\0';
	}
	return 0;
}

/* realpath() needs the working directory for a relative path and readlink for
   every component, and a title has neither. Paths on the console have no
   symbolic links a game needs to follow, so the path is normalised as text
   and checked for existence, which is what callers rely on it for. */
char* __wrap_realpath(const char* path, char* resolved)
{
	if (path == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (path[0] == '\0') {
		errno = ENOENT;
		return NULL;
	}

	char buf[PATH_MAX];
	if (normalise_path(path, buf, sizeof(buf)) != 0) return NULL;

	struct stat info;
	if (stat(buf, &info) != 0) return NULL;

	if (resolved == NULL) {
		resolved = malloc(strlen(buf) + 1);
		if (resolved == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	}
	strcpy(resolved, buf);
	return resolved;
}

/* The C library's readlink goes straight to the kernel, the way its mmap did,
   and a title is not allowed that: the console ends the process with an
   illegal function call. The shader preprocessor (mcpp) asks it of every file
   it opens. Answering "not a link" is what those files are anyway. */
ssize_t __wrap_readlink(const char* path, char* buf, size_t size)
{
	(void)path; (void)buf; (void)size;
	errno = EINVAL;
	return -1;
}

/* Same story: a direct kernel call in the C library. libc++ asks it for the
   longest path before reading the working directory, which OpenAL does when
   it looks for HRTF data. */
long __wrap_pathconf(const char* path, int name)
{
	(void)path;
	switch (name) {
	case _PC_PATH_MAX: return PATH_MAX;
	case _PC_NAME_MAX: return NAME_MAX;
	default:
		errno = EINVAL;
		return -1;
	}
}

/* There is no shell to run a command in. The game only uses this to open a
   web link, and it logs a non-zero answer. */
int __wrap_system(const char* command)
{
	if (command == NULL) return 0;   /* "is there a shell": no */
	errno = ENOSYS;
	return -1;
}

/* --- Directory listings ---------------------------------------------------

   A title may open and stat a file but may not read a directory: opendir is
   refused with EPERM. The game finds its shaders, karts and tracks by listing
   folders, so build.sh writes dirs.txt into the title folder, one line per
   entry: the folder relative to the title root, the name, and d or f. When
   the real opendir is refused, the listing comes from there instead.

   Folders the game creates while running (add-ons, replays) are not in the
   file; they try the real opendir first and otherwise list as empty. */

DIR* __real_opendir(const char* path);
struct dirent* __real_readdir(DIR* dir);
int __real_closedir(DIR* dir);

/* The places the title folder shows up under, longest first so a prefix of
   another does not match early. */
static const char* const kTitleRoots[] = {
	"/mnt/sandbox/PPSA69429_000/app0",
	"/system_ex/app/PPSA69429",
	"/app0",
};

static char* g_manifest = NULL;      /* the whole file, lines split in place */
static size_t g_manifest_size = 0;
static int g_manifest_tried = 0;

static void load_manifest(void)
{
	if (g_manifest_tried) return;
	g_manifest_tried = 1;
	for (size_t i = 0; i < sizeof(kTitleRoots) / sizeof(kTitleRoots[0]); i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/dirs.txt", kTitleRoots[i]);
		FILE* f = fopen(path, "rb");
		if (f == NULL) continue;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size > 0) {
			g_manifest = malloc((size_t)size + 1);
			if (g_manifest != NULL &&
			    fread(g_manifest, 1, (size_t)size, f) == (size_t)size) {
				g_manifest[size] = '\0';
				g_manifest_size = (size_t)size;
			} else {
				free(g_manifest);
				g_manifest = NULL;
			}
		}
		fclose(f);
		fprintf(stderr, "jsrf: directory manifest %s: %ld bytes\n", path, size);
		return;
	}
	fprintf(stderr, "jsrf: no dirs.txt, folders will list as empty\n");
}

/* The folder relative to the title root ("" for the root itself), or NULL
   when the path lies outside the title folder. */
static const char* relative_to_title(const char* normalised)
{
	for (size_t i = 0; i < sizeof(kTitleRoots) / sizeof(kTitleRoots[0]); i++) {
		size_t n = strlen(kTitleRoots[i]);
		if (strncmp(normalised, kTitleRoots[i], n) != 0) continue;
		if (normalised[n] == '\0') return normalised + n;
		if (normalised[n] == '/') return normalised + n + 1;
	}
	return NULL;
}

struct ps5_dir {
	struct ps5_dir* next;      /* every open listing, to tell them from real ones */
	struct dirent entry;
	char* entries;             /* "name\0type" pairs, type 'd' or 'f' */
	size_t length;
	size_t pos;
};

static struct ps5_dir* g_open_dirs = NULL;

static struct ps5_dir* find_listing(DIR* dir)
{
	for (struct ps5_dir* d = g_open_dirs; d != NULL; d = d->next)
		if ((DIR*)d == dir) return d;
	return NULL;
}

/* Collects the manifest lines for one folder. NULL when the folder is not
   in it, so the caller can report that it does not exist. */
static struct ps5_dir* listing_from_manifest(const char* folder)
{
	load_manifest();
	if (g_manifest == NULL) return NULL;

	size_t folder_len = strlen(folder);
	struct ps5_dir* d = calloc(1, sizeof(*d));
	if (d == NULL) return NULL;
	size_t capacity = 0;
	int found = 0;

	const char* line = g_manifest;
	const char* end = g_manifest + g_manifest_size;
	while (line < end) {
		const char* eol = memchr(line, '\n', (size_t)(end - line));
		if (eol == NULL) eol = end;
		const char* tab1 = memchr(line, '\t', (size_t)(eol - line));
		const char* tab2 = tab1 ? memchr(tab1 + 1, '\t', (size_t)(eol - tab1 - 1)) : NULL;
		if (tab2 != NULL && (size_t)(tab1 - line) == folder_len &&
		    strncmp(line, folder, folder_len) == 0) {
			size_t name_len = (size_t)(tab2 - tab1 - 1);
			size_t need = d->length + name_len + 2;
			if (need > capacity) {
				size_t grown = capacity ? capacity * 2 : 1024;
				while (grown < need) grown *= 2;
				char* bigger = realloc(d->entries, grown);
				if (bigger == NULL) break;
				d->entries = bigger;
				capacity = grown;
			}
			memcpy(d->entries + d->length, tab1 + 1, name_len);
			d->entries[d->length + name_len] = '\0';
			d->entries[d->length + name_len + 1] = tab2[1];
			d->length += name_len + 2;
			found = 1;
		}
		/* A folder with no entries still has a line naming it as a child of
		   its parent; an empty listing is right for it. */
		line = eol + 1;
	}
	if (!found) {
		/* Not listed as anyone's folder either: it is not there. */
		const char* slash = strrchr(folder, '/');
		char parent[PATH_MAX];
		const char* name = folder;
		parent[0] = '\0';
		if (slash != NULL) {
			size_t n = (size_t)(slash - folder);
			memcpy(parent, folder, n);
			parent[n] = '\0';
			name = slash + 1;
		}
		char needle[PATH_MAX + 8];
		int len = snprintf(needle, sizeof(needle), "%s\t%s\td", parent, name);
		int exists = folder[0] == '\0';
		for (const char* p = g_manifest; !exists && p < end; ) {
			const char* e = memchr(p, '\n', (size_t)(end - p));
			if (e == NULL) e = end;
			if (e - p == len && strncmp(p, needle, (size_t)len) == 0) exists = 1;
			p = e + 1;
		}
		if (!exists) {
			free(d->entries);
			free(d);
			return NULL;
		}
	}
	return d;
}

DIR* __wrap_opendir(const char* path)
{
	DIR* real = __real_opendir(path);
	if (real != NULL) return real;
	int refused = errno;

	char normalised[PATH_MAX];
	if (path == NULL || normalise_path(path, normalised, sizeof(normalised)) != 0) {
		errno = refused;
		return NULL;
	}
	const char* folder = relative_to_title(normalised);
	if (folder == NULL) {
		errno = refused;
		return NULL;
	}
	struct ps5_dir* d = listing_from_manifest(folder);
	if (d == NULL) {
		errno = ENOENT;
		return NULL;
	}
	d->next = g_open_dirs;
	g_open_dirs = d;
	return (DIR*)d;
}

struct dirent* __wrap_readdir(DIR* dir)
{
	struct ps5_dir* d = find_listing(dir);
	if (d == NULL) return __real_readdir(dir);
	if (d->pos >= d->length) return NULL;

	const char* name = d->entries + d->pos;
	size_t name_len = strlen(name);
	char type = name[name_len + 1];
	d->pos += name_len + 2;

	memset(&d->entry, 0, sizeof(d->entry));
	if (name_len > MAXNAMLEN) name_len = MAXNAMLEN;
	memcpy(d->entry.d_name, name, name_len);
	d->entry.d_name[name_len] = '\0';
	d->entry.d_namlen = (__uint8_t)name_len;
	d->entry.d_reclen = (__uint16_t)sizeof(d->entry);
	d->entry.d_type = type == 'd' ? DT_DIR : DT_REG;
	d->entry.d_fileno = 1;   /* zero would mean a deleted entry */
	return &d->entry;
}

int __wrap_closedir(DIR* dir)
{
	struct ps5_dir** link = &g_open_dirs;
	while (*link != NULL && (DIR*)*link != dir) link = &(*link)->next;
	if (*link == NULL) return __real_closedir(dir);

	struct ps5_dir* d = *link;
	*link = d->next;
	free(d->entries);
	free(d);
	return 0;
}

/* --- Seeing why the process ended -------------------------------------------

   The OpenGL SDK ends the process with _Exit on a GPU error, and writes the
   reason to the kernel log, which nothing here can read. Mesa's shader
   compiler uses abort and assert. None of these run exit handlers, so the
   game's log just stops. These copy the message into the log and add the
   return addresses found on the stack; tools/addr2func.sh turns them into
   function names using the reference address printed alongside. */

int __real_sceKernelDebugOutText(int channel, const char* text);
void __real__Exit(int status) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));
void __real___assert(const char* func, const char* file, int line,
                     const char* expression) __attribute__((noreturn));

int __wrap_sceKernelDebugOutText(int channel, const char* text)
{
	if (text != NULL) fprintf(stderr, "klog: %s", text);
	return __real_sceKernelDebugOutText(channel, text);
}

/* Prints the code addresses found in the caller's stack. Without frame
   pointers a real backtrace is not available; a word that points into the
   executable is very likely a return address, and the first few are the
   chain of callers. */
/* True when an address lies in the game's own code. The bounds come from
   the linker; "reference - 64 MiB" wrapped below zero and matched nothing. */
extern const char __executable_start[] __attribute__((weak));
extern const char etext[] __attribute__((weak));
static int is_game_code(const char* value)
{
	const char* low = __executable_start ? __executable_start : (const char*)0x400000;
	const char* high = etext ? etext : low + 0x8000000;
	return value >= low && value < high;
}

static void log_stack(const char* why) __attribute__((noinline));
static void log_stack(const char* why)
{
	const char* reference = (const char*)(void*)&log_stack;
	fprintf(stderr, "jsrf: %s; reference log_stack=%p\n", why, (void*)reference);
	void** sp = (void**)__builtin_frame_address(0);
	int printed = 0;
	for (int i = 0; i < 2048 && printed < 24; i++) {
		const char* value = (const char*)sp[i];
		if (is_game_code(value)) {
			fprintf(stderr, "jsrf:   stack[%d] %p\n", i, (void*)value);
			printed++;
		}
	}
}

int __real_fflush(FILE* stream);

void __wrap__Exit(int status)
{
	char why[64];
	snprintf(why, sizeof(why), "_Exit(%d)", status);
	log_stack(why);
	__real_fflush(stderr); /* a crash or an exit: the log out now */
	__real__Exit(status);
}

void __wrap_abort(void)
{
	log_stack("abort()");
	__real_fflush(stderr); /* a crash or an exit: the log out now */
	__real_abort();
}

void __wrap___assert(const char* func, const char* file, int line,
                     const char* expression)
{
	fprintf(stderr, "jsrf: assertion %s failed in %s (%s:%d)\n",
	        expression ? expression : "?", func ? func : "?",
	        file ? file : "?", line);
	log_stack("assert");
	__real_fflush(stderr); /* a crash or an exit: the log out now */
	__real___assert(func, file, line, expression);
}

/* A fault in the GPU driver or the shader compiler ends the process without a
   word. This catches the signal on a stack of its own, so a stack overflow is
   reported too, and logs where the fault happened: the instruction (rip), the
   address it touched, and the code addresses on the faulting stack. */
/* The highest stack address a thread's fault report may read: a little above
   the first frame we own on that thread (the trampoline below, or the
   constructor that installs the handlers on the main thread). Reading on
   past the top of a stack faulted inside the handler, before the report was
   written (decision log D36). */
static __thread uintptr_t t_stack_top;

static void log_fault(int signal_number, siginfo_t* info, void* context)
{
	const ucontext_t* uc = (const ucontext_t*)context;
	const char* rip = (const char*)uc->uc_mcontext.mc_rip;
	void** rsp = (void**)uc->uc_mcontext.mc_rsp;
	const char* reference = (const char*)(void*)&log_stack;

	fprintf(stderr, "jsrf: signal %d at rip %p, address %p, rsp %p\n",
	        signal_number, (void*)rip, info ? info->si_addr : NULL, (void*)rsp);
	fprintf(stderr, "jsrf: fault; reference log_stack=%p\n", (void*)reference);
	fprintf(stderr, "jsrf:   stack[-1] %p\n", (void*)rip);
	__real_fflush(stderr); /* the lines above reach the file even if the scan below faults */
	/* The context layout on this console is not proven: a stall sample once
	   reported rsp 0, and reading from there faulted inside this handler. The
	   stack is only read when the pointer is plausible. */
	if ((uintptr_t)rsp < 0x10000 || ((uintptr_t)rsp & 7) != 0) {
		fprintf(stderr, "jsrf:   no usable stack pointer\n");
		__real_fflush(stderr); /* a crash or an exit: the log out now */
		signal(signal_number, SIG_DFL);
		raise(signal_number);
		return;
	}
	/* Up to the stack's top when it is known, else only to the end of the
	   page rsp is on. */
	const uintptr_t limit = t_stack_top > (uintptr_t)rsp
		? t_stack_top : ((uintptr_t)rsp | 0x3fff) + 1;
	int printed = 0;
	for (int i = 0; i < 4096 && printed < 32 && (uintptr_t)&rsp[i + 1] <= limit; i++) {
		const char* value = (const char*)rsp[i];
		if (is_game_code(value)) {
			fprintf(stderr, "jsrf:   stack[%d] %p\n", i, (void*)value);
			printed++;
		}
	}
	__real_fflush(stderr); /* a crash or an exit: the log out now */
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

__attribute__((constructor(102)))
static void install_fault_handlers(void)
{
	static char alternate_stack[256 * 1024];
	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = alternate_stack;
	ss.ss_size = sizeof(alternate_stack);
	if (sigaltstack(&ss, NULL) != 0)
		fprintf(stderr, "jsrf: sigaltstack failed (errno %d)\n", errno);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = log_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		if (sigaction(signals[i], &action, NULL) != 0)
			fprintf(stderr, "jsrf: sigaction %d failed (errno %d)\n", signals[i], errno);
	char here;
	t_stack_top = (uintptr_t)&here + 64;
	fprintf(stderr, "jsrf: fault handlers installed, main stack near %p\n", (void*)&here);

	/* Only a little above this frame: start-up has used little of the stack
	   so far, and reading past its real top would fault in the handler. */

}

/* --- Thread stacks --------------------------------------------------------

   Threads created without a stack size get the console's default, which is
   small. Mesa compiles shaders on threads like that, and its compiler
   recurses deeply on a large shader; ProsperoEden gives its own GPU thread
   1 MiB for the same reason. Every thread gets at least kMinThreadStack
   here, plus a signal stack of its own: an alternate stack is per thread,
   and without one a stack overflow cannot be reported at all. */

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start)(void*), void* arg);

static const size_t kMinThreadStack = 2u * 1024u * 1024u;
static const size_t kSignalStack = 64u * 1024u;

struct thread_start {
	void* (*start)(void*);
	void* arg;
	size_t stack;
	int number;
};

/* Which cores a thread may run on (jsrf-ps5 decision log D12). JSRF Unleashed
   has guest threads that spin until another thread (the emulated GPU) clears a
   bit; on the console that other thread hardly ran while one spun, as if they
   shared a core. Each new thread is logged with its cores, its priority and
   where it runs, and given every core the process may use when it had fewer. */
#include <pthread_np.h>
#include <sys/cpuset.h>
int pthread_getprio(pthread_t thread);
int sceKernelGetCurrentCpu(void);

static unsigned long long mask_bits(const cpuset_t* set)
{
	unsigned long long bits = 0;
	for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, set)) bits |= 1ull << cpu;
	return bits;
}

static void spread_over_cores(int number)
{
	cpuset_t mine, process;
	CPU_ZERO(&mine);
	CPU_ZERO(&process);
	int r1 = pthread_getaffinity_np(pthread_self(), sizeof(mine), &mine);
	int r2 = cpuset_getaffinity(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, sizeof(process), &process);
	unsigned long long before = mask_bits(&mine), all = mask_bits(&process);
	int widened = 0;
	if (r1 == 0 && r2 == 0 && all && (before & all) != all)
		widened = pthread_setaffinity_np(pthread_self(), sizeof(process), &process) == 0;
	cpuset_t after_set;
	CPU_ZERO(&after_set);
	pthread_getaffinity_np(pthread_self(), sizeof(after_set), &after_set);
	fprintf(stderr, "jsrf: thread %d cores 0x%llx (process 0x%llx; %d/%d)%s now 0x%llx, priority %d, on core %d\n",
	        number, before, all, r1, r2, widened ? ", widened:" : ",", mask_bits(&after_set),
	        pthread_getprio(pthread_self()), sceKernelGetCurrentCpu());
}

void ps5_log_main_cores(void)
{
	spread_over_cores(0);
}

void jsrf_prof_register(int number); /* below */

static void* thread_trampoline(void* opaque)
{
	struct thread_start launch = *(struct thread_start*)opaque;
	free(opaque);

	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = malloc(kSignalStack);
	ss.ss_size = kSignalStack;
	if (ss.ss_sp != NULL) sigaltstack(&ss, NULL);

	char here;
	t_stack_top = (uintptr_t)&here + 64;
	fprintf(stderr, "jsrf: thread %d started, stack %zu KiB near %p\n",
	        launch.number, launch.stack / 1024, (void*)&here);
	spread_over_cores(launch.number);
	jsrf_prof_register(launch.number); /* the profiler's (D45) */
	return launch.start(launch.arg);
}

int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelConfiguredFlexibleMemorySize(size_t* size);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start)(void*), void* arg)
{
	static int threads_created = 0;
	struct thread_start* launch = malloc(sizeof(*launch));
	if (launch == NULL) return __real_pthread_create(thread, attr, start, arg);
	launch->start = start;
	launch->arg = arg;
	launch->number = ++threads_created;

	pthread_attr_t own;
	pthread_attr_t* use = (pthread_attr_t*)attr;
	size_t size = 0;
	if (use == NULL) {
		pthread_attr_init(&own);
		use = &own;
	}
	pthread_attr_getstacksize(use, &size);
	if (size < kMinThreadStack) {
		pthread_attr_setstacksize(use, kMinThreadStack);
		size = kMinThreadStack;
	}
	launch->stack = size;

	int result = __real_pthread_create(thread, use, thread_trampoline, launch);
	if (use == &own) pthread_attr_destroy(&own);
	if (result != 0) {
		free(launch);
		/* Where the stack would have come from: the title's flexible memory (jsrf-ps5 decision log D4). */
		size_t flexible = 0, configured = 0;
		sceKernelAvailableFlexibleMemorySize(&flexible);
		sceKernelConfiguredFlexibleMemorySize(&configured);
		fprintf(stderr, "jsrf: pthread_create failed (%d): stack %zu KiB, called from %p (this function at %p); "
		        "flexible memory %zu of %zu MiB free\n", result, size / 1024, __builtin_return_address(0),
		        (void*)&__wrap_pthread_create, flexible >> 20, configured >> 20);
		fflush(stderr);
	}
	return result;
}

/* --- The log: written in blocks ------------------------------------------

   stderr is the log in the title folder, fully buffered (ps5-support.c):
   each write to the title folder is slow, and upstream flushes stderr after
   many of its lines (249 calls), which turned every line into a write
   (jsrf-ps5 decision log D7). A flush of stderr or stdout here writes the
   buffer out at most five times a second; any other stream is flushed as asked.
   A crash, abort or exit flushes for real (__real_fflush, above). */
int __real_fflush(FILE* stream);
int __wrap_fflush(FILE* stream)
{
	if (stream != NULL && stream != stderr && stream != stdout) return __real_fflush(stream);
	static long long last_ms;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long long ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	long long before = __atomic_load_n(&last_ms, __ATOMIC_RELAXED);
	if (ms - before < 200) return 0; /* five times a second: at most 0.2 s of log lost to a kill (D16) */
	if (!__atomic_compare_exchange_n(&last_ms, &before, ms, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 0;
	return __real_fflush(stream);
}

/* --- Devices ------------------------------------------------------------

   OpenSSL, started by curl with its built-in engines, opens /dev/crypto on a
   BSD kernel and probes it with ioctl calls. A title is not meant to talk to
   kernel devices, and curl_global_init is where the process disappeared
   without a signal. /dev/crypto is reported missing, so OpenSSL keeps to its
   own software ciphers, as it does on any machine without the device. Other
   device opens and every ioctl are logged, so the next refusal has a name. */

int __real_open(const char* path, int flags, ...);
int __real__open(const char* path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);

static int refuse_device(const char* path)
{
	if (path == NULL || strncmp(path, "/dev/", 5) != 0) return 0;
	fprintf(stderr, "jsrf: open %s\n", path);
	if (strcmp(path, "/dev/crypto") == 0) {
		errno = ENOENT;
		return 1;
	}
	return 0;
}

int __wrap_open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	if (refuse_device(path)) return -1;
	return __real_open(path, flags, mode);
}

int __wrap__open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	if (refuse_device(path)) return -1;
	return __real__open(path, flags, mode);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	va_start(args, request);
	void* argument = va_arg(args, void*);
	va_end(args);
	fprintf(stderr, "jsrf: ioctl(%d, 0x%lx)\n", fd, request);
	return __real_ioctl(fd, request, argument);
}

/* --- printf ---------------------------------------------------------------

   The OpenGL SDK prints diagnostics through printf: a line for every texture
   whose mipmaps it builds, and a profiling report every 10,000 draws. The
   system library's printf sends them to neither the stdout variable nor
   descriptor 1, and writing every one to the log made start-up crawl. So the
   first lines go to the log, to show what is printed, and the rest are only
   counted; the count is reported with the frame rate. */

static const unsigned kPrintedToLog = 40;
static unsigned g_printed_lines;

unsigned ps5_printed_lines(void)
{
	return __atomic_load_n(&g_printed_lines, __ATOMIC_RELAXED);
}

static int print_line(const char* format, va_list args)
{
	unsigned line = __atomic_fetch_add(&g_printed_lines, 1, __ATOMIC_RELAXED);
	/* The SDK's profiling report (CPU cycles per driver phase, cache bytes
	   written back, mapping callers) comes every 10,000 draws and is what
	   shows where the frame time goes, so it is always kept. */
	if (line < kPrintedToLog ||
	    strncmp(format, "[ps5-driver", 11) == 0 ||
	    strncmp(format, "[ps5-cpu-flush", 14) == 0 ||
	    strncmp(format, "[ps5-map", 8) == 0 ||
	    strncmp(format, "[ps5-cache", 10) == 0)
		return vfprintf(stderr, format, args);
	return 0;
}

int __wrap_printf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int written = print_line(format, args);
	va_end(args);
	return written;
}

int __wrap_vprintf(const char* format, va_list args)
{
	return print_line(format, args);
}

int __wrap_puts(const char* text)
{
	unsigned line = __atomic_fetch_add(&g_printed_lines, 1, __ATOMIC_RELAXED);
	if (line < kPrintedToLog) {
		fputs(text, stderr);
		fputc('\n', stderr);
	}
	return 1;
}

int __wrap_putchar(int c)
{
	return c;   /* single characters are not worth a line in the log */
}

/* --- Who creates descriptors ---------------------------------------------

   The main thread was found blocked in a one-byte read on descriptor 11 or
   12: one thread waiting for another's signal. These log who creates pipes
   and sockets, with their numbers, so the descriptor can be traced. */

int __real_pipe(int fds[2]);
int __wrap_pipe(int fds[2])
{
	int r = __real_pipe(fds);
	fprintf(stderr, "jsrf: pipe -> %d, %d (caller %p; reference log_stack=%p)\n",
	        r == 0 ? fds[0] : -1, r == 0 ? fds[1] : -1,
	        __builtin_return_address(0), (void*)&log_stack);
	return r;
}

int __real_socketpair(int domain, int type, int protocol, int fds[2]);
int __wrap_socketpair(int domain, int type, int protocol, int fds[2])
{
	int r = __real_socketpair(domain, type, protocol, fds);
	fprintf(stderr, "jsrf: socketpair -> %d, %d (caller %p; reference log_stack=%p)\n",
	        r == 0 ? fds[0] : -1, r == 0 ? fds[1] : -1,
	        __builtin_return_address(0), (void*)&log_stack);
	return r;
}

int __real_socket(int domain, int type, int protocol);
int __wrap_socket(int domain, int type, int protocol)
{
	int r = __real_socket(domain, type, protocol);
	fprintf(stderr, "jsrf: socket(%d, %d) -> %d (caller %p; reference log_stack=%p)\n",
	        domain, type, r, __builtin_return_address(0), (void*)&log_stack);
	return r;
}

/* --- lstat ----------------------------------------------------------------

   A title's lstat answers EPERM for every path, even /app0/game, where stat
   works (jsrf-ps5 decision log D28). JSRF Unleashed's case-insensitive file
   lookup (kernel_path.c resolve_case) starts with lstat and gives up on any
   error but ENOENT, so files the game asks for in another case than the disc
   has them (TypeA.bin, typea.bin) were never found and New Game hung. A title
   folder has no symbolic links, so lstat is stat here. */
int __wrap_lstat(const char* path, struct stat* info)
{
	return stat(path, info);
}


/* --- A sampling profiler for the game's threads (decision log D45) ---------

   Once switched on (jsrf_prof_set), a thread of its own sends every thread
   that registered (each thread started through thread_trampoline, and the
   render thread) a signal about every millisecond; the handler counts the
   instruction it interrupted, per thread, and for an instruction in the
   system's libraries (from 0x800000000) also the first return address on the
   stack in the game's own code: who called into them. Every ten seconds,
   per thread with samples, its share and its most counted addresses go to
   the log (llvm-symbolizer names them against the ELF). All in memory. */
#include <ucontext.h>

#define PROF_THREADS 48
#define PROF_SLOTS 2048
typedef struct { uintptr_t pc; uint32_t n; } prof_bin;
static struct {
	pthread_t thread;
	int number;          /* thread_trampoline's number; -1 the render thread */
	volatile uint32_t total, sys;
	prof_bin pcs[PROF_SLOTS], callers[PROF_SLOTS];
} g_prof[PROF_THREADS];
static volatile int g_prof_count, g_prof_on, g_prof_started;

void jsrf_prof_register(int number)
{
	int i = __atomic_fetch_add(&g_prof_count, 1, __ATOMIC_SEQ_CST);
	if (i >= PROF_THREADS) return;
	g_prof[i].thread = pthread_self();
	g_prof[i].number = number;
}

void jsrf_ps5_render_thread(void)
{
	jsrf_prof_register(-1);
}

static void prof_count(prof_bin* bins, uintptr_t pc)
{
	uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) & (PROF_SLOTS - 1);
	for (int i = 0; i < 16; i++) {
		uint32_t k = (h + (uint32_t)i) & (PROF_SLOTS - 1);
		if (bins[k].pc == pc || bins[k].pc == 0) {
			bins[k].pc = pc;
			bins[k].n++;
			return;
		}
	}
}

static void prof_sample(int signal_number, siginfo_t* info, void* context)
{
	(void)signal_number;
	(void)info;
	pthread_t self = pthread_self();
	int t, n = g_prof_count < PROF_THREADS ? g_prof_count : PROF_THREADS;
	for (t = 0; t < n && g_prof[t].thread != self; t++)
		;
	if (t == n) return;
	const ucontext_t* uc = (const ucontext_t*)context;
	const uintptr_t pc = (uintptr_t)uc->uc_mcontext.mc_rip;
	g_prof[t].total++;
	prof_count(g_prof[t].pcs, pc);
	if (pc >= 0x800000000ull) {
		const uintptr_t* sp = (const uintptr_t*)(uintptr_t)uc->uc_mcontext.mc_rsp;
		uintptr_t caller = 0;
		for (int i = 0; i < 64 && sp; i++)
			if (is_game_code((const char*)sp[i])) { caller = sp[i]; break; }
		g_prof[t].sys++;
		prof_count(g_prof[t].callers, caller);
	}
}

static void prof_top(const char* what, int number, const prof_bin* bins, uint32_t total, int want)
{
	prof_bin top[16];
	memset(top, 0, sizeof top);
	if (want > 16) want = 16;
	for (int k = 0; k < PROF_SLOTS; k++) {
		const uint32_t n = bins[k].n;
		if (n <= top[want - 1].n) continue;
		int j = want - 1;
		while (j > 0 && top[j - 1].n < n) { top[j] = top[j - 1]; j--; }
		top[j] = bins[k];
	}
	for (int j = 0; j < want && top[j].n; j++)
		fprintf(stderr, "  [PROF] t%d %s %p %u %.1f%%\n", number, what, (void*)top[j].pc, top[j].n,
		        100.0 * top[j].n / total);
}

static void* prof_thread(void* unused)
{
	(void)unused;
	unsigned ticks = 0;
	for (;;) {
		usleep(1000);
		if (!g_prof_on) continue;
		int n = g_prof_count < PROF_THREADS ? g_prof_count : PROF_THREADS;
		for (int t = 0; t < n; t++)
			pthread_kill(g_prof[t].thread, SIGUSR2);
		if (++ticks < 10000) continue;
		ticks = 0;
		fprintf(stderr, "  [PROF] ---- %d threads, ten seconds ----\n", n);
		for (int t = 0; t < n; t++) {
			const uint32_t total = g_prof[t].total, sys = g_prof[t].sys;
			if (!total) continue;
			fprintf(stderr, "  [PROF] t%d: %u samples, %u (%.0f%%) in the system's libraries\n", g_prof[t].number,
			        total, sys, 100.0 * sys / total);
			/* the busy ones in detail: a thread mostly waiting shows only its line above */
			if (total - sys > total / 5 || g_prof[t].number < 0) {
				prof_top("at", g_prof[t].number, g_prof[t].pcs, total, 12);
				prof_top("from", g_prof[t].number, g_prof[t].callers, total, 8);
			}
		}
		for (int t = 0; t < n; t++) {
			memset(g_prof[t].pcs, 0, sizeof g_prof[t].pcs);
			memset(g_prof[t].callers, 0, sizeof g_prof[t].callers);
			g_prof[t].total = g_prof[t].sys = 0;
		}
	}
	return NULL;
}

void jsrf_prof_set(int on)
{
	if (on && !g_prof_started) {
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_sigaction = prof_sample;
		action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
		sigemptyset(&action.sa_mask);
		sigaction(SIGUSR2, &action, NULL);
		pthread_t t;
		if (__real_pthread_create(&t, NULL, prof_thread, NULL) == 0) {
			pthread_detach(t);
			g_prof_started = 1;
			fprintf(stderr, "jsrf: thread profiler started\n");
		}
	}
	if (on != g_prof_on)
		fprintf(stderr, "jsrf: thread profiler %s\n", on ? "on" : "off");
	g_prof_on = on;
}
