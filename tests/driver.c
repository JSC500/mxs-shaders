/*
 Usage: driver <id file> [screenshot.png]
 Plays a user against fake_mxbin through XTest, so XQueryKeymap and XQueryPointer see real
 X input: press F8, click 3 times (should be held back), press F8, click twice (should reach the
 game), move focus away and press F8 (should be ignored) and click once (should reach the game),
 then resize the window and quit.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

Bool XTestFakeKeyEvent(Display *, unsigned, Bool, unsigned long);
Bool XTestFakeButtonEvent(Display *, unsigned, Bool, unsigned long);
Bool XTestFakeMotionEvent(Display *, int, int, int, unsigned long);

/* Long enough for a few frames of Mesa's software renderer with MSAA and the panel on one core
 (about 20 fps in the test container). The real game runs at 120 fps or more. */
#define HOLD 400000

static Display *d;

static void key(KeySym s)
{
	KeyCode c = XKeysymToKeycode(d, s);

	XTestFakeKeyEvent(d, c, True, 0);
	XFlush(d);
	usleep(HOLD); /* held over several frames, like a real press */
	XTestFakeKeyEvent(d, c, False, 0);
	XFlush(d);
	usleep(HOLD * 2);
}

static void click(int x, int y)
{
	XTestFakeMotionEvent(d, 0, x, y, 0);
	XTestFakeButtonEvent(d, 1, True, 0);
	XFlush(d);
	usleep(HOLD);
	XTestFakeButtonEvent(d, 1, False, 0);
	XFlush(d);
	usleep(HOLD);
}

int main(int argc, char **argv)
{
	unsigned long top = 0, child = 0;
	FILE *f;
	Window other, frame;
	XEvent e;
	int i;

	for (i = 0; i < 100 && !(f = fopen(argv[1], "r")); i++)
		usleep(50000);
	if (!f || fscanf(f, "%lu %lu", &top, &child) != 2)
		return 2;
	fclose(f);
	d = XOpenDisplay(NULL);
	usleep(100000);
	/* Do what GNOME's window manager does: put the game's top-level window in a frame and focus
	 the top-level window itself, not the frame and not the drawing child. */
	frame = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 640, 400, 0, 0, 0);
	XMapWindow(d, frame);
	XReparentWindow(d, top, frame, 0, 0);
	XAddToSaveSet(d, top); /* as a window manager does, so the game's window survives this connection */
	XSync(d, False);
	usleep(100000);
	XSetInputFocus(d, top, RevertToParent, CurrentTime);
	XSync(d, False);
	/* Wait until the game has drawn 30 frames: compiling shaders on a software renderer is slow. */
	{
		char ready[1100];

		snprintf(ready, sizeof ready, "%s.ready", argv[1]);
		for (i = 0; i < 600 && access(ready, F_OK); i++)
			usleep(50000);
		usleep(300000);
	}
	key(XK_F8);
	usleep(500000);
	if (argc > 2) { /* a screenshot of the open panel */
		char cmd[1200];

		snprintf(cmd, sizeof cmd, "import -window root '%s' 2>/dev/null", argv[2]);
		if (system(cmd))
			fprintf(stderr, "driver: screenshot failed\n");
	}
	for (i = 0; i < 3; i++)
		click(100 + 40 * i, 120);
	key(XK_F8);
	for (i = 0; i < 2; i++)
		click(200, 200 + 30 * i);
	/* Focus on another window: F8 must be ignored there. */
	other = XCreateSimpleWindow(d, DefaultRootWindow(d), 900, 700, 50, 50, 0, 0, 0);
	XMapWindow(d, other);
	XSync(d, False);
	usleep(200000);
	XSetInputFocus(d, other, RevertToPointerRoot, CurrentTime);
	XSync(d, False);
	usleep(400000);
	key(XK_F8);
	click(300, 300);
	XSetInputFocus(d, top, RevertToParent, CurrentTime);
	XSync(d, False);
	usleep(400000);
	/* Open the panel, then switch away: it must close by itself, as on Windows. */
	key(XK_F8);
	XSetInputFocus(d, other, RevertToPointerRoot, CurrentTime);
	XSync(d, False);
	usleep(1500000); /* focus is checked every 15 frames, about 0.75 s on the software renderer */
	XSetInputFocus(d, top, RevertToParent, CurrentTime);
	XSync(d, False);
	usleep(400000);
	/* Resize the top-level window, as the window manager does around a fullscreen toggle. */
	XResizeWindow(d, frame, 800, 500);
	XResizeWindow(d, top, 800, 500);
	XSync(d, False);
	usleep(800000);
	memset(&e, 0, sizeof e);
	e.type = ClientMessage;
	e.xclient.window = child;
	e.xclient.format = 32;
	XSendEvent(d, child, False, 0, &e);
	XFlush(d);
	XCloseDisplay(d);
	return 0;
}
