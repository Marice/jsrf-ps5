/* JSRF for PS5 - the screen: one EGL OpenGL 3.3 core context (decision log D2).
 *
 * The renderer (src/nv2a_gl/nv2a_gl.c) opens its window and context through SDL. SDL's video in a title is the
 * offscreen driver, which has no GL, so SDL's GL half is replaced here at link time (--wrap, see CMakeLists.txt):
 * SDL_GL_CreateContext hands out this context, SDL_GL_MakeCurrent makes it current on the calling thread,
 * SDL_GL_SwapWindow swaps it. Windows stay SDL's, without SDL_WINDOW_OPENGL. From a2-racer-2-ps5's ps5/a2_ps5_gl.c,
 * with the core profile phase 0 measured (4.6 Core for a 3.3 request). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;
static int g_width, g_height;

static EGLConfig g_config;
static EGLContext g_status_context = EGL_NO_CONTEXT;

/* The display and the one window surface, for the status screen and then the game. */
static int egl_open(void)
{
	if (g_surface != EGL_NO_SURFACE)
		return 0;
	g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	/* 4K output (decision log D31): the SDK's runtime display modes (docs/display-modes.md) take 1080p, 1440p or
	   2160p, set before eglInitialize; the PS5 scales the picture to the TV's own mode. Looked up, so an SDK without
	   the call keeps 1080p. */
	{
		typedef EGLBoolean (*set_mode_fn)(EGLDisplay, EGLint, EGLint);
		set_mode_fn set_mode = (set_mode_fn)eglGetProcAddress("eglSetDisplayModePS5");
		EGLBoolean ok = set_mode && g_display != EGL_NO_DISPLAY ? set_mode(g_display, 3840, 2160) : EGL_FALSE;
		fprintf(stderr, "jsrf: egl: display mode 3840x2160 %s\n",
		        set_mode ? (ok ? "set" : "refused") : "not offered by this SDK (1080p)");
	}
	if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "jsrf: egl: no display (0x%x)\n", eglGetError());
		return -1;
	}
	/* What nv2a_gl.c asks SDL for: depth 24, stencil 8, double buffered. */
	const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
	};
	EGLint count = 0;
	if (!eglChooseConfig(g_display, attributes, &g_config, 1, &count) || count < 1) {
		fprintf(stderr, "jsrf: egl: no config (0x%x)\n", eglGetError());
		return -1;
	}
	g_surface = eglCreateWindowSurface(g_display, g_config, 0, NULL);
	if (g_surface == EGL_NO_SURFACE) {
		fprintf(stderr, "jsrf: egl: no surface (0x%x)\n", eglGetError());
		return -1;
	}
	eglQuerySurface(g_display, g_surface, EGL_WIDTH, &g_width);
	eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &g_height);
	return 0;
}

/* A compatibility context for jsrf_ps5_screen.c's fixed-function status screen, until the game starts. */
int jsrf_ps5_status_begin(void)
{
	if (egl_open() != 0)
		return -1;
	if (g_status_context == EGL_NO_CONTEXT) {
		const EGLint compat[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
		                         EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE};
		g_status_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, compat);
	}
	if (g_status_context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(g_display, g_surface, g_surface, g_status_context)) {
		fprintf(stderr, "jsrf: egl: no context for the status screen (0x%x)\n", eglGetError());
		return -1;
	}
	return 0;
}

void jsrf_ps5_status_end(void)
{
	if (g_status_context == EGL_NO_CONTEXT)
		return;
	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(g_display, g_status_context);
	g_status_context = EGL_NO_CONTEXT;
}

/* Tries one context on the current surface; logs each step (decision log D6). */
static EGLContext try_context(const char *what, const EGLint *attributes)
{
	EGLContext c = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, attributes);
	EGLint created = eglGetError();
	EGLBoolean current = c != EGL_NO_CONTEXT && eglMakeCurrent(g_display, g_surface, g_surface, c);
	EGLint made = eglGetError();
	fprintf(stderr, "jsrf: egl: %s on thread %p: create %s (0x%x), make current %s (0x%x)\n", what,
	        (void *)pthread_self(), c != EGL_NO_CONTEXT ? "ok" : "refused", created, current ? "ok" : "refused",
	        made);
	fflush(stderr);
	if (c != EGL_NO_CONTEXT && !current) {
		eglDestroyContext(g_display, c);
		c = EGL_NO_CONTEXT;
	}
	return c;
}

int32_t sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *physical,
                                           size_t *size);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);

static void log_memory(const char *when)
{
	int64_t physical = 0;
	size_t largest = 0, flexible = 0;
	sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0, &physical, &largest);
	sceKernelAvailableFlexibleMemorySize(&flexible);
	fprintf(stderr, "jsrf: memory %s: direct %lld MiB in all, largest free block %zu MiB; flexible %zu MiB free\n",
	        when, (long long)(sceKernelGetDirectMemorySize() >> 20), largest >> 20, flexible >> 20);
	fflush(stderr);
}

static int gl_create(void);

/* Called on the main thread before the engine starts: the renderer's context is made while the driver can still
   have what it needs. With the engine running, eglCreateContext refused every context (0x3009, decision log D6);
   the renderer's thread then only makes this one current. */
int jsrf_ps5_gl_prepare(void)
{
	log_memory("before the renderer's context");
	int r = gl_create();
	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); /* free for the renderer's thread */
	return r;
}

static int gl_open(void)
{
	if (g_context != EGL_NO_CONTEXT) {
		if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
			fprintf(stderr, "jsrf: egl: the prepared context on thread %p: make current refused (0x%x)\n",
			        (void *)pthread_self(), eglGetError());
			return -1;
		}
		fprintf(stderr, "jsrf: gl: the prepared context, current on thread %p\n", (void *)pthread_self());
		fflush(stderr);
		return 0;
	}
	log_memory("when the renderer asked for its context");
	return gl_create();
}

static int gl_create(void)
{
	jsrf_ps5_status_end();
	if (egl_open() != 0)
		return -1;
	static const EGLint core[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
	};
	static const EGLint compat[] = {
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE,
	};
	g_context = try_context("3.3 core", core);
	if (g_context == EGL_NO_CONTEXT) {
		/* the surface made again, on this thread */
		eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(g_display, g_surface);
		g_surface = eglCreateWindowSurface(g_display, g_config, 0, NULL);
		fprintf(stderr, "jsrf: egl: the window surface made again: %s (0x%x)\n",
		        g_surface != EGL_NO_SURFACE ? "ok" : "refused", eglGetError());
		g_context = try_context("3.3 core, new surface", core);
	}
	if (g_context == EGL_NO_CONTEXT)
		g_context = try_context("compatibility", compat); /* 4.6 compat also takes #version 330 core */
	if (g_context == EGL_NO_CONTEXT) {
		fprintf(stderr, "jsrf: egl: no context for the renderer\n");
		return -1;
	}
	eglSwapInterval(g_display, 1);
	fprintf(stderr, "jsrf: gl: %s on %s, %dx%d\n", (const char *)glGetString(GL_VERSION),
	        (const char *)glGetString(GL_RENDERER), g_width, g_height);
	fflush(stderr);
	return 0;
}

SDL_Window *__real_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	/* The offscreen driver has no GL; the picture goes through the EGL surface instead. */
	return __real_SDL_CreateWindow(title, x, y, w, h, flags & ~(Uint32)SDL_WINDOW_OPENGL);
}

SDL_GLContext __wrap_SDL_GL_CreateContext(SDL_Window *window)
{
	(void)window;
	if (gl_open() != 0) {
		SDL_SetError("no EGL 3.3 core context");
		return NULL;
	}
	return (SDL_GLContext)g_context;
}

void __wrap_SDL_GL_DeleteContext(SDL_GLContext context)
{
	(void)context; /* one context for the whole run */
}

int __wrap_SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
	(void)window;
	if (g_context == EGL_NO_CONTEXT)
		return -1;
	EGLBoolean ok = context ? eglMakeCurrent(g_display, g_surface, g_surface, g_context)
	                        : eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (!ok) {
		fprintf(stderr, "jsrf: egl: SDL_GL_MakeCurrent(%s) on thread %p refused (0x%x)\n",
		        context ? "the context" : "none", (void *)pthread_self(), eglGetError());
		fflush(stderr);
	}
	return ok ? 0 : -1;
}

int32_t sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *physical,
                                           size_t *size);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);

/* Where a present's time goes (decision log D8): the swap and SDL's event pump, timed here, a line every two
   seconds. */
#include <time.h>
static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double g_swap_ms, g_poll_ms, g_since;
static unsigned g_swaps, g_polls;

static void time_report(void)
{
	double now = now_ms();
	if (g_since == 0.0)
		g_since = now;
	if (now - g_since < 2000.0)
		return;
	extern unsigned long g_vblank_count; /* src/kernel/kernel_bridge.c: display interrupts delivered */
	static unsigned long last_vblanks;
	fprintf(stderr, "jsrf: present: %u swaps, %.2f ms each; %u SDL_PollEvent calls, %.3f ms each (%.0f ms of %.0f); "
	        "%.1f vblanks/s\n",
	        g_swaps, g_swaps ? g_swap_ms / g_swaps : 0.0, g_polls, g_polls ? g_poll_ms / g_polls : 0.0,
	        g_swap_ms + g_poll_ms, now - g_since, (g_vblank_count - last_vblanks) * 1000.0 / (now - g_since));
	last_vblanks = g_vblank_count;
	/* memory, so a shortage before a crash shows (decision log D16) */
	{
		void ps5_heap_numbers(size_t *size, size_t *live, size_t *peak, size_t *blocks, size_t *failures);
		size_t hs, hl, hp, hb, hf, flexible = 0, largest = 0;
		int64_t physical = 0;
		ps5_heap_numbers(&hs, &hl, &hp, &hb, &hf);
		sceKernelAvailableFlexibleMemorySize(&flexible);
		sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0, &physical, &largest);
		fprintf(stderr, "jsrf: memory: heap %zu of %zu MiB live (peak %zu, %zu blocks, %zu refused); flexible %zu MiB "
		        "free; direct largest free %zu MiB\n", hl >> 20, hs >> 20, hp >> 20, hb, hf, flexible >> 20,
		        largest >> 20);
	}
	g_swap_ms = g_poll_ms = 0.0;
	g_swaps = g_polls = 0;
	g_since = now;
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
	(void)window;
	double t0 = now_ms();
	eglSwapBuffers(g_display, g_surface);
	g_swap_ms += now_ms() - t0;
	g_swaps++;
	time_report();
}

int __real_SDL_PollEvent(SDL_Event *event);
int __wrap_SDL_PollEvent(SDL_Event *event)
{
	double t0 = now_ms();
	int r = __real_SDL_PollEvent(event);
	g_poll_ms += now_ms() - t0;
	g_polls++;
	return r;
}

void __wrap_SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
	(void)window;
	if (w)
		*w = g_width;
	if (h)
		*h = g_height;
}

int __wrap_SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
	(void)attr;
	(void)value; /* the context's attributes are fixed above */
	return 0;
}

int __wrap_SDL_GL_SetSwapInterval(int interval)
{
	return eglSwapInterval(g_display, interval) ? 0 : -1;
}
