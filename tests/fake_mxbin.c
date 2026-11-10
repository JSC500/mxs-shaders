/*
 Usage: mxbin <id file> [folder of shader_NNNN_{vs,fs}_<role>.txt files from a probe run]

 A stand-in for the native MX Simulator binary, built as "mxbin", for testing the preload library
 without the game. It copies what the probe log shows the game doing: a legacy glXChooseVisual
 visual (double buffer, RGBA 5/6/5, depth 16), a top-level window plus a child it draws in (keys and
 buttons only, no motion), glXCreateContext + glXMakeCurrent on the child, an XPending/XNextEvent
 loop, two XQueryPointer calls a frame, a resize of the child on ConfigureNotify, and glXSwapBuffers.
 It prints what it received, so the test can check what the library let through.

 Given the probe's shader dump, it also compiles and links the game's own shaders in pairs, through
 functions fetched with glXGetProcAddress like the game, so the library swaps in the pack's shaders
 and the test can see whether Mesa compiles and links them. The game's shaders aren't in the repo.
*/
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
/* No GL headers on the build machine, so the few declarations needed are here. */
typedef unsigned char GLubyte;
typedef struct __GLXcontextRec *GLXContext;
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_DEPTH_SIZE 12
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 4
XVisualInfo *glXChooseVisual(Display *, int, int *);
GLXContext glXCreateContext(Display *, XVisualInfo *, GLXContext, Bool);
Bool glXMakeCurrent(Display *, XID, GLXContext);
void glXDestroyContext(Display *, GLXContext);
void glXSwapBuffers(Display *, XID);
void (*glXGetProcAddress(const GLubyte *))(void);
void glClearColor(float, float, float, float);
void glClear(unsigned);
void glDrawArrays(unsigned, int, int);

/* EGL, declared here for the same reason as the GL calls. MXS_TEST_EGL=1 makes the stand-in open
 its display the way the game does when the X server has no GLX: EGL, eglGetProcAddress and
 eglSwapBuffers instead of the GLX calls. */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int EGLint;
typedef unsigned EGLBoolean;
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_OPENGL_API 0x30A2
EGLDisplay eglGetDisplay(void *);
EGLBoolean eglBindAPI(unsigned);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, unsigned long, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
void *eglGetProcAddress(const char *);

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_LINK_STATUS 0x8B82

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *b;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	n = (long)fread(b, 1, (size_t)n, f);
	b[n] = 0;
	fclose(f);
	return b;
}

#define GPA(T, n) ((T)glXGetProcAddress((const GLubyte *)n))

/* Compiles each vertex shader with the fragment shader after it, as the game's ids pair up. */
static void game_shaders(const char *dir)
{
	unsigned (*create_shader)(unsigned) = GPA(unsigned (*)(unsigned), "glCreateShader");
	void (*shader_source)(unsigned, int, const char *const *, const int *) =
		GPA(void (*)(unsigned, int, const char *const *, const int *), "glShaderSource");
	void (*compile)(unsigned) = GPA(void (*)(unsigned), "glCompileShader");
	unsigned (*create_program)(void) = GPA(unsigned (*)(void), "glCreateProgram");
	void (*attach)(unsigned, unsigned) = GPA(void (*)(unsigned, unsigned), "glAttachShader");
	void (*link)(unsigned) = GPA(void (*)(unsigned), "glLinkProgram");
	void (*get_programiv)(unsigned, unsigned, int *) = GPA(void (*)(unsigned, unsigned, int *), "glGetProgramiv");
	char *names[64], path[1024];
	int n = 0, i, linked = 0, failed = 0;
	struct dirent *e;
	DIR *d = opendir(dir);

	if (!d) {
		printf("no shader folder %s\n", dir);
		return;
	}
	while ((e = readdir(d)) && n < 64) {
		if (!strncmp(e->d_name, "shader_", 7))
			names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof names[0], by_name);
	for (i = 0; i + 1 < n; i++) {
		unsigned vs, fs, prog;
		char *vsrc, *fsrc;
		int ok = 0;

		if (!strstr(names[i], "_vs_") || !strstr(names[i + 1], "_fs_"))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		vsrc = slurp(path);
		snprintf(path, sizeof path, "%s/%s", dir, names[i + 1]);
		fsrc = slurp(path);
		vs = create_shader(GL_VERTEX_SHADER);
		shader_source(vs, 1, (const char *const *)&vsrc, NULL);
		compile(vs);
		fs = create_shader(GL_FRAGMENT_SHADER);
		shader_source(fs, 1, (const char *const *)&fsrc, NULL);
		compile(fs);
		prog = create_program();
		attach(prog, vs);
		attach(prog, fs);
		link(prog);
		get_programiv(prog, GL_LINK_STATUS, &ok);
		printf("program %u from %s + %s: %s\n", prog, names[i], names[i + 1], ok ? "linked" : "LINK FAILED");
		ok ? linked++ : failed++;
		free(vsrc);
		free(fsrc);
		i++;
	}
	printf("game shaders: %d programs linked, %d failed\n", linked, failed);
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

int main(int argc, char **argv)
{
	int attribs[] = {GLX_DOUBLEBUFFER, GLX_RGBA, GLX_RED_SIZE, 5, GLX_GREEN_SIZE, 6, GLX_BLUE_SIZE, 5,
			 GLX_DEPTH_SIZE, 16, None};
	const char *idfile = argc > 1 ? argv[1] : "/tmp/fake_mxbin.id";
	int egl_mode = getenv("MXS_TEST_EGL") != NULL;
	const char *gpa = egl_mode ? "eglGetProcAddress" : "glXGetProcAddress";
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes a;
	Window top, child;
	GLXContext ctx = NULL;	/* only made without MXS_TEST_EGL, and only destroyed in that case */
	EGLDisplay edpy = NULL;
	EGLSurface esurf = NULL;
	EGLContext ectx = NULL;
	EGLConfig ecfg = NULL;
	unsigned presses = 0, releases = 0, frames = 0, poll_clicks = 0, was_down = 0, escapes = 0;
	int quit = 0;
	FILE *f;
	void (*use_program)(unsigned);
	void *p_draw;

	if (!d)
		return 2;
	if (egl_mode) {
		EGLint major = 0, minor = 0, count = 0, visual = 0;
		EGLint eattrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE,
				   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
		XVisualInfo want;
		int found = 0;

		edpy = eglGetDisplay(NULL);
		if (!edpy || !eglInitialize(edpy, &major, &minor))
			return 5;
		/* Without this EGL hands out an OpenGL ES context, as the game's eglBindAPI call avoids. */
		if (!eglBindAPI(EGL_OPENGL_API))
			return 8;
		if (!eglChooseConfig(edpy, eattrs, &ecfg, 1, &count) || count < 1)
			return 6;
		eglGetConfigAttrib(edpy, ecfg, EGL_NATIVE_VISUAL_ID, &visual);
		want.visualid = (VisualID)visual;
		vi = XGetVisualInfo(d, VisualIDMask, &want, &found);
		if (!vi || !found)
			return 7;
	} else {
		vi = glXChooseVisual(d, DefaultScreen(d), attribs);
		if (!vi)
			return 3;
	}
	memset(&a, 0, sizeof a);
	a.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask |
		       PropertyChangeMask;
	top = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 640, 400, 0, vi->depth, InputOutput, vi->visual,
			    CWColormap | CWEventMask, &a);
	a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ExposureMask;
	child = XCreateWindow(d, top, 0, 0, 640, 400, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask,
			      &a);
	XMapWindow(d, child);
	XStoreName(d, top, "MX Simulator");
	XMapWindow(d, top);
	f = fopen(idfile, "w");
	fprintf(f, "%lu %lu\n", (unsigned long)top, (unsigned long)child);
	fclose(f);
	/* In the real game the window manager frames the window before the context is made current
	 (probe: ReparentNotify at 0.084 s, glXMakeCurrent at 0.219 s). Give the test driver time to do the same. */
	XFlush(d);
	usleep(800000);
	if (egl_mode) {
		esurf = eglCreateWindowSurface(edpy, ecfg, (unsigned long)child, NULL);
		ectx = eglCreateContext(edpy, ecfg, NULL, NULL);
		if (!esurf || !ectx || !eglMakeCurrent(edpy, esurf, esurf, ectx))
			return 4;
		use_program = (void (*)(unsigned))eglGetProcAddress("glUseProgram");
		p_draw = (void *)eglGetProcAddress("glDrawArrays");
	} else {
		ctx = glXCreateContext(d, vi, NULL, True);
		if (!ctx || !glXMakeCurrent(d, child, ctx))
			return 4;
		use_program = (void (*)(unsigned))glXGetProcAddress((const GLubyte *)"glUseProgram");
		p_draw = (void *)glXGetProcAddress((const GLubyte *)"glDrawArrays");
	}
	printf("%s(glUseProgram) %s, %s(glDrawArrays) %s\n", gpa, use_program ? "found" : "missing", gpa,
	       p_draw == (void *)glDrawArrays ? "is the hook" : "BYPASSES THE HOOK");
	if (argc > 2)
		game_shaders(argv[2]);
	fflush(stdout);

	while (!quit && frames < 3000) {
		Window r, c;
		int rx, ry, wx, wy;
		unsigned m;

		while (XPending(d)) {
			XEvent e;

			XNextEvent(d, &e);
			if (e.type == KeyPress && e.xkey.keycode == XKeysymToKeycode(d, XK_Escape))
				escapes++;
			else if (e.type == ButtonPress)
				presses++;
			else if (e.type == ButtonRelease)
				releases++;
			else if (e.type == ConfigureNotify && e.xconfigure.window == top)
				XResizeWindow(d, child, (unsigned)e.xconfigure.width, (unsigned)e.xconfigure.height);
			else if (e.type == ClientMessage)
				quit = 1;
		}
		/* The game may read clicks from the button state rather than events: count those too. */
		XQueryPointer(d, child, &r, &c, &rx, &ry, &wx, &wy, &m);
		if ((m & Button1Mask) && !was_down)
			poll_clicks++;
		was_down = (m & Button1Mask) != 0;
		XQueryPointer(d, child, &r, &c, &rx, &ry, &wx, &wy, &m);
		glClearColor(0.2f, 0.3f, 0.4f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if (egl_mode)
			eglSwapBuffers(edpy, esurf);
		else
			glXSwapBuffers(d, child);
		frames++;
		if (frames == 30) { /* tells the driver the game is drawing, so its input won't fall into startup */
			char ready[1100];

			snprintf(ready, sizeof ready, "%s.ready", idfile);
			fclose(fopen(ready, "w"));
		}
		usleep(8000);
	}
	printf("game received %u button presses, %u releases, saw %u clicks by polling, ran %u frames\n", presses,
	       releases, poll_clicks, frames);
	printf("game received %u escape presses\n", escapes);
	if (egl_mode) {
		eglMakeCurrent(edpy, NULL, NULL, NULL);
	} else {
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, ctx);
	}
	XCloseDisplay(d);
	return 0;
}
