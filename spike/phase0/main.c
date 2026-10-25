/* JSRF for PS5 - phase 0: can a PS5 title give JSRF Unleashed's runtime what it takes from its host (decision log D1)?
 *
 *   1  a 4 GB PROT_NONE reservation (src/platform/win32_compat.c win32_reserve_address_space)
 *   2  64 MB of guest RAM mapped as shared views at fixed offsets inside it: the base, a mirror 64 MB up and the
 *      0xF0000000 aperture (src/kernel/xbox_memory_layout.c), with three backings, since the PS5 has no memfd_create:
 *      shm_open(SHM_ANON), an unlinked file in the title folder, and direct memory mapped twice
 *   3  an OpenGL 3.3 core context with #version 330 core shaders (src/nv2a_gl), GL_EXT_depth_clamp,
 *      GL_ARB_get_program_binary
 *   4  pthread_kill(SIGUSR2) reaching a handler on another thread (W32_SUSPEND_SIGNAL, win32_compat.c)
 * Each check logs PASS or FAIL with what it measured to jsrf.log in the title folder; a summary stays on screen. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void a2ps5_screen_lines(const char *title, const char *const *lines, const int *pass, int count); /* screen.c */

#define GUEST_RAM (64u * 1024u * 1024u)
#define SPAN (4ull * 1024u * 1024u * 1024u)
#define APERTURE 0xF0000000ull

static EGLDisplay g_display;
static EGLSurface g_surface;
static EGLConfig g_config;
static EGLContext g_screen_context;
static int g_width, g_height;

#define MAX_LINES 24
static char g_text[MAX_LINES][96];
static const char *g_lines[MAX_LINES];
static int g_pass[MAX_LINES];
static int g_count, g_failures;

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One check's result: the log gets the detail, the screen a short line. */
static void result(const char *check, int pass, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void result(const char *check, int pass, const char *fmt, ...)
{
	char detail[400] = "";
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof detail, fmt, ap);
		va_end(ap);
	}
	fprintf(stderr, "jsrf: check %-30s %s %s\n", check, pass ? "PASS" : "FAIL", detail);
	fflush(stderr);
	if (!pass)
		g_failures++;
	if (g_count < MAX_LINES) {
		snprintf(g_text[g_count], sizeof g_text[0], "%s %s", pass ? "PASS" : "FAIL", check);
		g_lines[g_count] = g_text[g_count];
		g_pass[g_count] = pass;
		g_count++;
	}
}

/* ---- a write that should fault: caught, so the check can go on ---- */

static sigjmp_buf g_fault_jump;
static volatile sig_atomic_t g_expect_fault;

static void on_fault(int sig)
{
	if (g_expect_fault)
		siglongjmp(g_fault_jump, sig);
	_exit(3);
}

/* Writes one byte at p; returns the signal it raised, or 0. */
static int write_faults(volatile uint8_t *p)
{
	struct sigaction sa, old_segv, old_bus;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_fault;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_segv);
	sigaction(SIGBUS, &sa, &old_bus);
	int sig = sigsetjmp(g_fault_jump, 1);
	if (sig == 0) {
		g_expect_fault = 1;
		*p = 0x5a;
	}
	g_expect_fault = 0;
	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);
	return sig;
}

/* ---- 1 and 2: the guest address space ---- */

static uint8_t *reserve_span(void)
{
	/* FreeBSD has no MAP_NORESERVE (upstream passes it on Linux): a PROT_NONE mapping commits nothing anyway. */
	void *p = mmap(NULL, SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	int e = errno;
	result("1 reserve 4 GB", p != MAP_FAILED, "(at %p; errno %d)", p == MAP_FAILED ? NULL : p, p == MAP_FAILED ? e : 0);
	return p == MAP_FAILED ? NULL : p;
}

/* Puts one view of the backing at span + offset; 0 on success, else errno. */
typedef int (*map_view_fn)(uint8_t *at, void *backing);

static int map_fd_view(uint8_t *at, void *backing)
{
	int fd = (int)(intptr_t)backing;
	void *p = mmap(at, GUEST_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (p == MAP_FAILED)
		return errno ? errno : -1;
	return p == at ? 0 : -2;
}

int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                      int32_t memory_type, int64_t *physical);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical,
                                 size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t length);
int64_t sceKernelGetDirectMemorySize(void);
#define SCE_KERNEL_MAP_FIXED 0x10
#define SCE_KERNEL_WB_ONION 0

static int map_direct_view(uint8_t *at, void *backing)
{
	int64_t physical = *(int64_t *)backing;
	void *address = at;
	int32_t r = sceKernelMapDirectMemory(&address, GUEST_RAM, PROT_READ | PROT_WRITE, SCE_KERNEL_MAP_FIXED, physical,
	                                     0);
	if (r != 0)
		return r;
	return address == at ? 0 : -2;
}

/* Maps the base, a mirror 64 MB up and the aperture; checks that they alias, that a read-only page faults, and
   times a memset per view. */
static void check_views(const char *name, uint8_t *span, map_view_fn map_view, void *backing)
{
	static const uint64_t offsets[3] = {0, GUEST_RAM, APERTURE};
	static const char *const labels[3] = {"base", "mirror +64MB", "aperture +0xF0000000"};
	char check[64];
	for (int i = 0; i < 3; i++) {
		int r = map_view(span + offsets[i], backing);
		snprintf(check, sizeof check, "2%s map %s", name, labels[i]);
		result(check, r == 0, "(at %p; result %d)", (void *)(span + offsets[i]), r);
		if (r != 0)
			return;
	}
	double times[3];
	for (int i = 0; i < 3; i++) {
		double t0 = now_seconds();
		memset(span + offsets[i], 0x11 * (i + 1), GUEST_RAM);
		times[i] = (now_seconds() - t0) * 1000.0;
	}
	/* the last memset wrote 0x33 everywhere; every view must see it, and a write at the base shows at the others */
	volatile uint8_t *base = span, *mirror = span + GUEST_RAM, *aperture = span + APERTURE;
	base[0x448] = 0xa5;
	mirror[0x1000] = 0x5a;
	int alias = base[0x10] == 0x33 && mirror[0x448] == 0xa5 && aperture[0x448] == 0xa5 && aperture[0x1000] == 0x5a &&
	            base[GUEST_RAM - 1] == 0x33;
	snprintf(check, sizeof check, "2%s views alias", name);
	result(check, alias, "(base %02x mirror %02x aperture %02x/%02x; memset 64 MB: %.1f / %.1f / %.1f ms)", base[0x10],
	       mirror[0x448], aperture[0x448], aperture[0x1000], times[0], times[1], times[2]);
	/* .rdata is made read-only in the guest (xbox_memory_layout.c): one page of the base view */
	int pr = mprotect(span + 0x10000, 0x4000, PROT_READ);
	int e = errno;
	int sig = pr == 0 ? write_faults(span + 0x10000) : 0;
	mprotect(span + 0x10000, 0x4000, PROT_READ | PROT_WRITE);
	snprintf(check, sizeof check, "2%s read-only page", name);
	result(check, pr == 0 && sig != 0, "(mprotect %d errno %d; write raised signal %d)", pr, pr ? e : 0, sig);
}

/* Puts the span back to PROT_NONE between backings. */
static void clear_span(uint8_t *span)
{
	mmap(span, SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

static void check_memory(void)
{
	uint8_t *span = reserve_span();
	if (!span)
		return;

	/* a: POSIX shared memory without a name (FreeBSD's SHM_ANON) */
	int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
	int e = errno;
	int ok = fd >= 0 && ftruncate(fd, GUEST_RAM) == 0;
	result("2a shm_open(SHM_ANON)", ok, "(fd %d; errno %d)", fd, ok ? 0 : (fd < 0 ? e : errno));
	if (ok)
		check_views("a", span, map_fd_view, (void *)(intptr_t)fd);
	if (fd >= 0)
		close(fd);
	clear_span(span);

	/* b: a file in the title folder, unlinked once open (upstream's fallback where shm_open is refused) */
	const char *path = "/app0/jsrf-phase0.ram";
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	e = errno;
	ok = fd >= 0 && ftruncate(fd, GUEST_RAM) == 0;
	result("2b file in the title folder", ok, "(fd %d; errno %d)", fd, ok ? 0 : (fd < 0 ? e : errno));
	if (fd >= 0)
		unlink(path);
	if (ok)
		check_views("b", span, map_fd_view, (void *)(intptr_t)fd);
	if (fd >= 0)
		close(fd);
	clear_span(span);

	/* c: direct memory, one physical range mapped at each view */
	int64_t physical = 0;
	int32_t r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), GUEST_RAM, 2 * 1024 * 1024,
	                                          SCE_KERNEL_WB_ONION, &physical);
	result("2c direct memory", r == 0, "(result 0x%x; physical 0x%llx)", (unsigned)r, (unsigned long long)physical);
	if (r == 0) {
		check_views("c", span, map_direct_view, &physical);
		clear_span(span);
		sceKernelReleaseDirectMemory(physical, GUEST_RAM);
	}
	munmap(span, SPAN);
}

/* ---- 3: OpenGL 3.3 core ---- */

static int open_display(void)
{
	g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
		return 0;
	const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8, EGL_NONE,
	};
	EGLint count = 0;
	if (!eglChooseConfig(g_display, attributes, &g_config, 1, &count) || count < 1)
		return 0;
	g_surface = eglCreateWindowSurface(g_display, g_config, 0, NULL);
	const EGLint compat[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
	                         EGL_NONE};
	g_screen_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, compat);
	if (g_surface == EGL_NO_SURFACE || g_screen_context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(g_display, g_surface, g_surface, g_screen_context))
		return 0;
	eglQuerySurface(g_display, g_surface, EGL_WIDTH, &g_width);
	eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &g_height);
	fprintf(stderr, "jsrf: egl %d.%d, surface %dx%d\n", major, minor, g_width, g_height);
	return 1;
}

static GLuint compile(GLenum type, const char *source, char *log, size_t cap)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, (GLsizei)cap, NULL, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static int has_extension(const char *name)
{
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++)
		if (strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), name) == 0)
			return 1;
	return 0;
}

static void check_core(void)
{
	static const EGLint versions[][2] = {{3, 3}, {4, 5}, {4, 6}};
	EGLContext core = EGL_NO_CONTEXT;
	int major = 0, minor = 0;
	for (size_t i = 0; i < 3 && core == EGL_NO_CONTEXT; i++) {
		const EGLint list[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, versions[i][0], EGL_CONTEXT_MINOR_VERSION_KHR,
		                       versions[i][1], EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
		                       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
		core = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, list);
		fprintf(stderr, "jsrf: egl: core %d.%d %s (0x%x)\n", versions[i][0], versions[i][1],
		        core == EGL_NO_CONTEXT ? "refused" : "created", eglGetError());
		major = versions[i][0];
		minor = versions[i][1];
	}
	if (core == EGL_NO_CONTEXT || !eglMakeCurrent(g_display, g_surface, g_surface, core)) {
		result("3 core context", 0, "(no core context 3.3, 4.5 or 4.6)");
		return;
	}
	GLint mask = 0;
	glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
	result("3 core context", (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0, "(asked %d.%d; %s; GLSL %s; profile mask 0x%x)",
	       major, minor, (const char *)glGetString(GL_VERSION),
	       (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), mask);
	int clamp = has_extension("GL_EXT_depth_clamp") || has_extension("GL_ARB_depth_clamp");
	result("3 depth clamp", clamp, "(EXT %d ARB %d)", has_extension("GL_EXT_depth_clamp"),
	       has_extension("GL_ARB_depth_clamp"));
	GLint formats = 0;
	glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
	result("3 program binaries", 1, "(extension %d; %d format(s); optional for JSRF)",
	       has_extension("GL_ARB_get_program_binary"), formats);

	/* a triangle through #version 330 core shaders and a VAO/VBO into an FBO, read back */
	static const char *vs = "#version 330 core\n"
	                        "layout(location = 0) in vec2 v0;\n"
	                        "void main() { gl_Position = vec4(v0, 0.0, 1.0); }\n";
	static const char *fs = "#version 330 core\n"
	                        "out vec4 color;\n"
	                        "uniform vec4 tint;\n"
	                        "void main() { color = tint; }\n";
	char log[512] = "";
	GLuint v = compile(GL_VERTEX_SHADER, vs, log, sizeof log);
	GLuint f = v ? compile(GL_FRAGMENT_SHADER, fs, log, sizeof log) : 0;
	GLuint program = 0;
	if (v && f) {
		program = glCreateProgram();
		glAttachShader(program, v);
		glAttachShader(program, f);
		glLinkProgram(program);
		GLint linked = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (!linked) {
			glGetProgramInfoLog(program, sizeof log, NULL, log);
			program = 0;
		}
	}
	result("3 GLSL 330 core", program != 0, "(%s)", program ? "compiled and linked" : log);
	if (!program) {
		eglMakeCurrent(g_display, g_surface, g_surface, g_screen_context);
		return;
	}
	GLuint fbo, color, vao, vbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenTextures(1, &color);
	glBindTexture(GL_TEXTURE_2D, color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	static const float triangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f}; /* covers the whole target */
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, 640, 480);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(program);
	glUniform4f(glGetUniformLocation(program, "tint"), 0.2f, 0.8f, 0.4f, 1.0f);
	glEnable(GL_DEPTH_CLAMP);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	unsigned char rgba[4] = {0};
	glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	GLenum error = glGetError();
	int right = abs(rgba[0] - 51) < 8 && abs(rgba[1] - 204) < 8 && abs(rgba[2] - 102) < 8;
	result("3 draw and read back", complete == GL_FRAMEBUFFER_COMPLETE && right && error == GL_NO_ERROR,
	       "(framebuffer 0x%x; pixel %d,%d,%d; glGetError 0x%x)", complete, rgba[0], rgba[1], rgba[2], error);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	eglMakeCurrent(g_display, g_surface, g_surface, g_screen_context);
}

/* ---- 4: a signal to another thread ---- */

static volatile sig_atomic_t g_handled;
static pthread_t g_handled_on;
static volatile int g_worker_stop, g_worker_steps;

static void on_suspend_signal(int sig)
{
	(void)sig;
	g_handled_on = pthread_self();
	g_handled++;
}

static void *worker(void *arg)
{
	(void)arg;
	while (!g_worker_stop) {
		g_worker_steps++;
		struct timespec wait = {0, 1000000};
		nanosleep(&wait, NULL);
	}
	return NULL;
}

static void check_signal(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_suspend_signal;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	int installed = sigaction(SIGUSR2, &sa, NULL);
	pthread_t t;
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		result("4 signal to a thread", 0, "(no thread)");
		return;
	}
	struct timespec settle = {0, 50000000};
	nanosleep(&settle, NULL);
	int killed = pthread_kill(t, SIGUSR2);
	for (int i = 0; i < 100 && !g_handled; i++)
		nanosleep(&settle, NULL);
	int before = g_worker_steps;
	nanosleep(&settle, NULL);
	int carried_on = g_worker_steps > before;
	g_worker_stop = 1;
	pthread_join(t, NULL);
	result("4 signal to a thread", installed == 0 && killed == 0 && g_handled == 1 && pthread_equal(g_handled_on, t) &&
	                                   carried_on,
	       "(sigaction %d; pthread_kill %d; handled %d time(s), on the worker %d; worker carried on %d)", installed,
	       killed, (int)g_handled, g_handled ? pthread_equal(g_handled_on, t) != 0 : 0, carried_on);
}

int main(void)
{
	fprintf(stderr, "jsrf: phase 0, what JSRF Unleashed's runtime needs from the PS5\n");
	int screen = open_display();
	if (screen)
		a2ps5_screen_lines("JSRF PHASE 0", (const char *const[]){"RUNNING THE CHECKS"}, (const int[]){1}, 1);
	check_memory();
	check_signal();
	if (screen)
		check_core();
	else
		result("3 core context", 0, "(no EGL display)");
	fprintf(stderr, "jsrf: phase 0 done: %d check(s) failed\n", g_failures);
	fflush(stderr);
	for (;;) { /* the summary stays until the title is closed (the PS button) */
		if (screen)
			a2ps5_screen_lines(g_failures ? "JSRF PHASE 0: SOME CHECKS FAILED" : "JSRF PHASE 0: ALL PASSED",
			                   g_lines, g_pass, g_count);
		struct timespec wait = {0, 250000000};
		nanosleep(&wait, NULL);
	}
}
