/* JSRF for PS5 - a plain status screen for the disc import, before the game's renderer runs (from a2-racer-2-ps5's
 * ps5/a2_ps5_screen.c): upper case 5x7 pixel font, drawn as triangles in a compatibility context. */
#include <EGL/egl.h>

#include <GL/gl.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 5x7 glyphs, one byte per row, bit 4 the leftmost pixel. */
static const struct {
	char c;
	unsigned char rows[7];
} kFont[] = {
	{'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
	{'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
	{'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
	{'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
	{'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
	{'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
	{'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
	{'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
	{'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
	{'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
	{'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},
	{'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
	{'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 12, 4, 8}},       {':', {0, 12, 12, 0, 12, 12, 0}},
	{'-', {0, 0, 0, 31, 0, 0, 0}},       {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
	{'/', {1, 1, 2, 4, 8, 16, 16}},      {'%', {24, 25, 2, 4, 8, 19, 3}},     {'!', {4, 4, 4, 4, 4, 0, 4}},
	{'?', {14, 17, 1, 2, 4, 0, 4}},      {'\'', {4, 4, 8, 0, 0, 0, 0}},       {'_', {0, 0, 0, 0, 0, 0, 31}},
	{'<', {2, 4, 8, 16, 8, 4, 2}},       {'>', {8, 4, 2, 1, 2, 4, 8}},
};

static const unsigned char *glyph(char c)
{
	c = (char)toupper((unsigned char)c);
	for (size_t i = 0; i < sizeof kFont / sizeof kFont[0]; i++)
		if (kFont[i].c == c)
			return kFont[i].rows;
	return NULL; /* a space, or a character the font does not have */
}

static void rect(float x, float y, float w, float h)
{
	glVertex2f(x, y);
	glVertex2f(x + w, y);
	glVertex2f(x + w, y + h);
	glVertex2f(x, y);
	glVertex2f(x + w, y + h);
	glVertex2f(x, y + h);
}

/* One line of text centred at y; px is the size of a font pixel. */
static void text(const char *s, float y, float px, int w)
{
	float x = (w - (float)strlen(s) * 6.0f * px) / 2.0f;
	glBegin(GL_TRIANGLES);
	for (; *s; s++, x += 6.0f * px) {
		const unsigned char *g = glyph(*s);
		for (int row = 0; g && row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (g[row] & (16 >> col))
					rect(x + col * px, y + row * px, px, px);
	}
	glEnd();
}

/* A title and one line per check, green when it passed and red when not. */
void jsrf_ps5_screen_lines(const char *title, const char *const *lines, const int *pass, int count)
{
	EGLDisplay display = eglGetCurrentDisplay();
	EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
	EGLint w = 0, h = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);
	glViewport(0, 0, w, h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glClearColor(0.06f, 0.08f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, w, h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	float px = h / 270.0f; /* 4 at 1080p */
	glColor3f(1.0f, 0.78f, 0.2f);
	text(title, h * 0.08f, px * 1.5f, w);
	for (int i = 0; i < count; i++) {
		if (pass[i])
			glColor3f(0.4f, 0.9f, 0.45f);
		else
			glColor3f(1.0f, 0.35f, 0.3f);
		text(lines[i], h * 0.18f + i * px * 11.0f, px, w);
	}
	eglSwapBuffers(display, surface);
}

/* The start menu (jsrf_ps5_menu.c, decision log D35): rows of a label and its value, the chosen row in yellow with
   arrows round its value, and one line underneath (the countdown or the buttons). */
void jsrf_ps5_screen_menu(const char *title, const char *subtitle, const char *const *labels,
                          const char *const *values, int count, int selected, const char *footer)
{
	EGLDisplay display = eglGetCurrentDisplay();
	EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
	EGLint w = 0, h = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);
	glViewport(0, 0, w, h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glClearColor(0.06f, 0.08f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, w, h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	float px = h / 270.0f;
	glColor3f(1.0f, 0.78f, 0.2f);
	text(title, h * 0.08f, px * 1.5f, w);
	if (subtitle) {
		glColor3f(0.55f, 0.6f, 0.7f);
		text(subtitle, h * 0.08f + px * 14.0f, px, w);
	}
	char row[96];
	for (int i = 0; i < count; i++) {
		if (values[i])
			snprintf(row, sizeof row, i == selected ? "%-12s < %s >" : "%-12s   %s  ", labels[i], values[i]);
		else
			snprintf(row, sizeof row, i == selected ? "> %s <" : "%s", labels[i]);
		if (i == selected)
			glColor3f(1.0f, 0.85f, 0.25f);
		else
			glColor3f(0.85f, 0.88f, 0.92f);
		text(row, h * 0.32f + i * px * 14.0f, px * 1.2f, w);
	}
	if (footer) {
		glColor3f(0.4f, 0.9f, 0.45f);
		text(footer, h * 0.32f + count * px * 14.0f + px * 12.0f, px, w);
	}
	eglSwapBuffers(display, surface);
}
