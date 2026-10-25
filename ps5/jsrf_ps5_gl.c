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

static int gl_open(void)
{
	if (g_context != EGL_NO_CONTEXT)
		return 0;
	jsrf_ps5_status_end();
	if (egl_open() != 0)
		return -1;
	const EGLint context_attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
	};
	g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, context_attributes);
	if (g_context == EGL_NO_CONTEXT || !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
		fprintf(stderr, "jsrf: egl: no 3.3 core context (0x%x)\n", eglGetError());
		g_context = EGL_NO_CONTEXT;
		return -1;
	}
	eglSwapInterval(g_display, 1);
	fprintf(stderr, "jsrf: gl: %s on %s, %dx%d\n", (const char *)glGetString(GL_VERSION),
	        (const char *)glGetString(GL_RENDERER), g_width, g_height);
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
	return ok ? 0 : -1;
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
	(void)window;
	eglSwapBuffers(g_display, g_surface);
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
