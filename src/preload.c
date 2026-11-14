/*
 preload.c: the Linux counterpart of proxy.c, the export stubs and the wgl hooks.

 The library is loaded with LD_PRELOAD, so the dynamic linker binds the game's imports of the
 functions defined here to these definitions, and each one forwards to the next definition
 (libGL, libX11) found with dlsym(RTLD_NEXT). Nothing in the game's binary or memory changes,
 which keeps hard goal 1.

 It only acts inside a process whose executable is called mxbin (MXS_SHADERS_FORCE=1 overrides
 that for tests). Anywhere else every hook is a plain pass-through, so the library is harmless
 if LD_PRELOAD reaches bash or a program the game starts.
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "common.h"
#include "linux_internal.h"
#include "platform.h"
#include "post.h"

#define EXPORT __attribute__((visibility("default")))

typedef void *GLXContext;
typedef XID GLXDrawable;

/* static_hooks.txt without the wgl functions: X(name, parameters, arguments). All return void. */
#define MXS_STATIC_GL(X) \
	X(glClear, (GLbitfield mask), (mask)) \
	X(glDrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count)) \
	X(glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices), (mode, count, type, indices)) \
	X(glViewport, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h)) \
	X(glEnable, (GLenum cap), (cap)) \
	X(glDisable, (GLenum cap), (cap)) \
	X(glBlendFunc, (GLenum s, GLenum d), (s, d)) \
	X(glDepthMask, (GLboolean flag), (flag)) \
	X(glBindTexture, (GLenum target, GLuint tex), (target, tex)) \
	X(glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, \
			 GLenum type, const void *data), \
	  (target, level, internal, w, h, border, format, type, data)) \
	X(glTexParameteri, (GLenum target, GLenum pname, GLint value), (target, pname, value)) \
	X(glTexParameterf, (GLenum target, GLenum pname, float value), (target, pname, value)) \
	X(glTexParameterfv, (GLenum target, GLenum pname, const float *value), (target, pname, value)) \
	X(glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels), \
	  (x, y, w, h, format, type, pixels)) \
	X(glFinish, (void), ())

/* hooks.c */
#define X(name, params, args) void hook_##name params;
MXS_STATIC_GL(X)
#undef X
void hooks_init(void);
void hooks_glx_context_created(HGLRC rc);
void hooks_glx_context_destroyed(HGLRC rc);
void hooks_glx_made_current(HDC dc, HGLRC rc);
void hooks_glx_swap(HDC dc);
void *hooks_glx_proc(const char *name, void *p);
void hooks_egl_context_created(HGLRC rc);
void hooks_egl_context_destroyed(HGLRC rc);
void hooks_egl_made_current(HDC dc, HGLRC rc);
void hooks_egl_swap(HDC dc);
void *hooks_egl_proc(const char *name, void *p);

static int g_active;

/* Resolves the next definition of a function the first time it's needed. A race between two
 threads stores the same pointer, so it's harmless. */
#define NEXT(name) \
	({ \
		if (!next_##name) \
			next_##name = (__typeof__(next_##name))dlsym(RTLD_NEXT, #name); \
		next_##name; \
	})

/* --- real functions --- */

#define X(name, params, args) static void (*next_##name) params;
MXS_STATIC_GL(X)
#undef X

static GLXContext (*next_glXCreateContext)(Display *, XVisualInfo *, GLXContext, Bool);
static void (*next_glXDestroyContext)(Display *, GLXContext);
static Bool (*next_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
static void (*next_glXSwapBuffers)(Display *, GLXDrawable);
static void *(*next_glXGetProcAddress)(const unsigned char *);
static void *(*next_glXGetProcAddressARB)(const unsigned char *);
static GLXContext (*next_glXGetCurrentContext)(void);
static int (*next_XPending)(Display *);
static int (*next_XNextEvent)(Display *, XEvent *);
static int (*next_XIfEvent)(Display *, XEvent *, Bool (*)(Display *, XEvent *, XPointer), XPointer);
static int (*next_XResizeWindow)(Display *, Window, unsigned, unsigned);
static Window (*next_XCreateWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, int, unsigned,
				    Visual *, unsigned long, XSetWindowAttributes *);
static int (*next_XChangeWindowAttributes)(Display *, Window, unsigned long, XSetWindowAttributes *);
static Bool (*next_XQueryPointer)(Display *, Window, Window *, Window *, int *, int *, int *, int *, unsigned *);

/* EGL, the game's other way of opening its display (when the X server has no GLX). */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef unsigned EGLBoolean;
typedef int EGLint;

static void *(*next_eglGetProcAddress)(const char *);
static EGLContext (*next_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*next_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface (*next_eglCreateWindowSurface)(EGLDisplay, EGLConfig, unsigned long, const EGLint *);
static EGLBoolean (*next_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*next_eglSwapBuffers)(EGLDisplay, EGLSurface);

static void resolve_all(void)
{
#define X(name, params, args) NEXT(name);
	MXS_STATIC_GL(X)
#undef X
	NEXT(glXCreateContext);
	NEXT(glXDestroyContext);
	NEXT(glXMakeCurrent);
	NEXT(glXSwapBuffers);
	NEXT(glXGetProcAddress);
	NEXT(glXGetProcAddressARB);
	NEXT(glXGetCurrentContext);
	NEXT(XPending);
	NEXT(XNextEvent);
	NEXT(XIfEvent);
	NEXT(XResizeWindow);
	NEXT(XQueryPointer);
	NEXT(XCreateWindow);
	NEXT(XChangeWindowAttributes);
	NEXT(eglGetProcAddress);
	NEXT(eglCreateContext);
	NEXT(eglDestroyContext);
	NEXT(eglCreateWindowSurface);
	NEXT(eglMakeCurrent);
	NEXT(eglSwapBuffers);
}

/* --- start-up (replaces DllMain) --- */

static int exe_is_game(char *exe, size_t size)
{
	ssize_t n = readlink("/proc/self/exe", exe, size - 1);
	const char *base, *force = getenv("MXS_SHADERS_FORCE");

	if (n <= 0)
		return 0;
	exe[n] = 0;
	base = strrchr(exe, '/');
	base = base ? base + 1 : exe;
	return !strcmp(base, "mxbin") || (force && !strcmp(force, "1"));
}

__attribute__((constructor)) static void mxs_init(void)
{
	char exe[1024];
	const char *e;

	if (!exe_is_game(exe, sizeof exe))
		return;
	resolve_all();
	log_init();
	plat_log("MXS Shaders (Linux) attached, pid %d, exe %s", (int)getpid(), exe);
	e = getenv("XDG_SESSION_TYPE");
	plat_log("session %s, DISPLAY=%s, data folder %s", e ? e : "(unset)",
		 getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)", plat_data_dir());
	if (!next_XNextEvent || (!next_glXSwapBuffers && !next_eglSwapBuffers))
		plat_log("failed: the game's libGL, libEGL or libX11 functions weren't found, staying inactive");
	else
		g_active = 1;
	if (g_active)
		hooks_init();
}

/* --- static gl* hooks --- */

#define X(name, params, args) \
	EXPORT void name params \
	{ \
		if (g_active) \
			hook_##name args; \
		else \
			NEXT(name) args; \
	}
MXS_STATIC_GL(X)
#undef X

static const struct {
	const char *name;
	void *fn;
} g_static[] = {
#define X(name, params, args) {#name, (void *)name},
	MXS_STATIC_GL(X)
#undef X
};

/* --- GLX --- */

static GLXContext g_main_ctx;
static Display *g_main_dpy;
static GLXDrawable g_main_drawable;
static int g_ready;

/* The EGL path's window and the X connection the input layer reads it through (see the EGL
 section at the end of this file). */
static Display *g_egl_dpy;
static Window g_egl_win;

EXPORT GLXContext glXCreateContext(Display *d, XVisualInfo *vis, GLXContext share, Bool direct)
{
	GLXContext c = NEXT(glXCreateContext)(d, vis, share, direct);

	if (g_active)
		hooks_glx_context_created(c);
	return c;
}

EXPORT void glXDestroyContext(Display *d, GLXContext c)
{
	if (g_active && c)
		hooks_glx_context_destroyed(c);
	if (g_active && c && c == g_main_ctx) {
		g_main_ctx = NULL;
		g_main_drawable = 0;
		g_ready = 0;
	}
	NEXT(glXDestroyContext)(d, c);
}

EXPORT Bool glXMakeCurrent(Display *d, GLXDrawable w, GLXContext c)
{
	Bool ok = NEXT(glXMakeCurrent)(d, w, c);

	if (!g_active || !ok || !c || !w)
		return ok;
	/* The main context is the first one made current on a window, as on Windows. */
	if (!g_main_ctx)
		g_main_ctx = c;
	if (c == g_main_ctx) {
		g_main_dpy = d;
		g_main_drawable = w;
		g_ready = 1;
		/* The input layer needs the window before context_ready reads its size. */
		plat_frame_begin(d, w);
	}
	hooks_glx_made_current((HDC)w, c);
	return ok;
}

EXPORT void glXSwapBuffers(Display *d, GLXDrawable w)
{
	if (g_active && g_ready && d == g_main_dpy && w == g_main_drawable &&
	    NEXT(glXGetCurrentContext)() == g_main_ctx) {
		plat_frame_begin(d, w);
		hooks_glx_swap((HDC)w);
	}
	NEXT(glXSwapBuffers)(d, w);
}

static void *proc_for(const char *name, void *real, void *(*wrap)(const char *, void *))
{
	size_t i;
	void *wrapped;

	if (!g_active || !name)
		return real;
	/* A function hooked statically must not leak out unhooked through this path. */
	for (i = 0; i < sizeof g_static / sizeof g_static[0]; i++) {
		if (!strcmp(name, g_static[i].name))
			return real ? g_static[i].fn : NULL;
	}
	wrapped = real ? wrap(name, real) : NULL;
	return wrapped ? wrapped : real;
}

EXPORT void *glXGetProcAddress(const unsigned char *name)
{
	return proc_for((const char *)name, NEXT(glXGetProcAddress)(name), hooks_glx_proc);
}

EXPORT void *glXGetProcAddressARB(const unsigned char *name)
{
	return proc_for((const char *)name, NEXT(glXGetProcAddressARB)(name), hooks_glx_proc);
}

/* --- X events: drop pointer events while the panel is open, notice resizes --- */

static int game_display(Display *d)
{
	if (!g_active || !d)
		return 0;
	/* Under EGL no hook ever sees the game's own Display (eglCreateWindowSurface gives only the
	 window), and the library only acts inside the game process, so every X connection is the
	 game's. Without this the panel's input filtering silently does nothing on that path. */
	return g_egl_win ? 1 : d == g_main_dpy;
}

EXPORT int XPending(Display *d)
{
	if (game_display(d) && input_filtering())
		input_purge(d);
	return NEXT(XPending)(d);
}

EXPORT int XNextEvent(Display *d, XEvent *e)
{
	int r;

	if (!game_display(d))
		return NEXT(XNextEvent)(d, e);
	if (input_filtering())
		input_purge(d);
	for (;;) {
		r = NEXT(XNextEvent)(d, e);
		if (e->type == ConfigureNotify)
			plat_note_resize(e->xconfigure.window);
		/* One that arrived after the purge. The game only calls this after XPending, so another
		 event is normally queued. If none is, waiting for one is what XNextEvent does anyway. */
		if (input_filtering() && input_drop_event(e))
			continue;
		return r;
	}
}

EXPORT int XIfEvent(Display *d, XEvent *e, Bool (*pred)(Display *, XEvent *, XPointer), XPointer arg)
{
	/* Imported by the game but not called in the probe run. Purge first so a predicate that
	 waits for a click can't see one while the panel is open. */
	if (game_display(d) && input_filtering())
		input_purge(d);
	return NEXT(XIfEvent)(d, e, pred, arg);
}

EXPORT int XResizeWindow(Display *d, Window w, unsigned width, unsigned height)
{
	int r = NEXT(XResizeWindow)(d, w, width, height);

	if (game_display(d))
		plat_note_resize(w);
	return r;
}

Bool real_XQueryPointer(Display *d, Window w, Window *root, Window *child, int *rx, int *ry, int *wx, int *wy,
			unsigned *mask)
{
	return NEXT(XQueryPointer)(d, w, root, child, rx, ry, wx, wy, mask);
}

/* The game reads the pointer and buttons with this twice a frame (probe), so blocking click events
 alone isn't enough: while the panel is open it sees no buttons down and the pointer held still. */
EXPORT Bool XQueryPointer(Display *d, Window w, Window *root, Window *child, int *rx, int *ry, int *wx, int *wy,
			  unsigned *mask)
{
	Bool r = NEXT(XQueryPointer)(d, w, root, child, rx, ry, wx, wy, mask);

	if (r && game_display(d) && input_blocking())
		input_filter_query_pointer(child, rx, ry, wx, wy, mask);
	return r;
}

/* --- cursors: record what the game gives its windows, so a hidden cursor can be put back --- */

EXPORT Window XCreateWindow(Display *d, Window parent, int x, int y, unsigned w, unsigned h, unsigned border,
			    int depth, unsigned cls, Visual *vis, unsigned long mask, XSetWindowAttributes *a)
{
	Window r = NEXT(XCreateWindow)(d, parent, x, y, w, h, border, depth, cls, vis, mask, a);

	if (g_active && r && a && (mask & CWCursor))
		input_note_cursor(r, a->cursor);
	return r;
}

EXPORT int XChangeWindowAttributes(Display *d, Window w, unsigned long mask, XSetWindowAttributes *a)
{
	int r = NEXT(XChangeWindowAttributes)(d, w, mask, a);

	if (g_active && a && (mask & CWCursor))
		input_note_cursor(w, a->cursor);
	return r;
}

/* --- EGL: the other way the game opens its display ---
 When the X server has no GLX extension the game falls back to EGL ("No GLX extension on X
 server", then "Opening display via EGL at ..."). It then fetches every function past GL 1.1
 with eglGetProcAddress and presents with eglSwapBuffers, so without these two hooks none of the
 dynamic hooks is ever reached: the game still renders, but no shadows, no post and no panel.

 eglCreateWindowSurface is where the game's X window shows up (g_egl_win, declared with the GLX
 state above). The input layer needs a Display connection of its own, because no EGL call hands
 one over. */

static Display *egl_display(void)
{
	if (!g_egl_dpy)
		g_egl_dpy = XOpenDisplay(NULL);
	return g_egl_dpy;
}

EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, unsigned long win, const EGLint *attrs)
{
	EGLSurface s;

	if (!next_eglCreateWindowSurface)
		return NULL;
	s = NEXT(eglCreateWindowSurface)(d, c, win, attrs);
	if (g_active && s) {
		g_egl_win = (Window)win;
		egl_display();
	}
	return s;
}

EXPORT EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs)
{
	EGLContext ctx;

	if (!next_eglCreateContext)
		return NULL;
	ctx = NEXT(eglCreateContext)(d, c, share, attrs);
	if (g_active && ctx)
		hooks_egl_context_created((HGLRC)ctx);
	return ctx;
}

EXPORT EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext ctx)
{
	if (!next_eglDestroyContext)
		return 0;
	if (g_active && ctx)
		hooks_egl_context_destroyed((HGLRC)ctx);
	return NEXT(eglDestroyContext)(d, ctx);
}

EXPORT void *eglGetProcAddress(const char *name)
{
	if (!next_eglGetProcAddress)
		return NULL;
	return proc_for(name, NEXT(eglGetProcAddress)(name), hooks_egl_proc);
}

EXPORT EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read_surface, EGLContext ctx)
{
	EGLBoolean ok;

	if (!next_eglMakeCurrent)
		return 0;
	ok = NEXT(eglMakeCurrent)(d, draw, read_surface, ctx);
	/* A releasing call (no context, no surface) changes nothing. */
	if (g_active && ok && ctx && draw && g_egl_win && egl_display()) {
		plat_frame_begin(g_egl_dpy, g_egl_win);
		hooks_egl_made_current((HDC)g_egl_win, ctx);
	}
	return ok;
}

EXPORT EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
	if (!next_eglSwapBuffers)
		return 0;
	if (g_active && s && g_egl_win && g_egl_dpy) {
		plat_frame_begin(g_egl_dpy, g_egl_win);
		hooks_egl_swap((HDC)g_egl_win);
	}
	return NEXT(eglSwapBuffers)(d, s);
}
