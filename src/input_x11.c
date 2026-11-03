/*
 input_x11.c: window size, focus, keys and pointer for the F8 panel, read by polling.

 The probe showed the game never grabs or warps the pointer and reads it with XQueryPointer
 itself, so polling with XQueryKeymap and XQueryPointer on the game's Display works in menus
 and races. Everything here runs on the game's thread, inside its glXSwapBuffers call, so
 using the game's Display connection is safe.

 Key codes stay Windows virtual-key codes (ui_key=119 is F8) so one ini works on both systems.
*/
#define _GNU_SOURCE
#include "linux_internal.h"
#include "platform.h"

#include <string.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

struct vkmap {
	unsigned char vk;
	KeySym sym[2];
};

/* Virtual-key code to keysyms. The punctuation rows follow the US layout, as VK_OEM_* do. */
static const struct vkmap g_keys[] = {
	{0x08, {XK_BackSpace}}, {0x09, {XK_Tab}}, {0x0d, {XK_Return, XK_KP_Enter}},
	{0x10, {XK_Shift_L, XK_Shift_R}}, {0x11, {XK_Control_L, XK_Control_R}}, {0x12, {XK_Alt_L, XK_Alt_R}},
	{0x13, {XK_Pause}}, {0x14, {XK_Caps_Lock}}, {0x1b, {XK_Escape}}, {0x20, {XK_space}},
	{0x21, {XK_Prior}}, {0x22, {XK_Next}}, {0x23, {XK_End}}, {0x24, {XK_Home}},
	{0x25, {XK_Left}}, {0x26, {XK_Up}}, {0x27, {XK_Right}}, {0x28, {XK_Down}},
	{0x2c, {XK_Print}}, {0x2d, {XK_Insert}}, {0x2e, {XK_Delete}},
	{0x5b, {XK_Super_L}}, {0x5c, {XK_Super_R}}, {0x5d, {XK_Menu}},
	{0x6a, {XK_KP_Multiply}}, {0x6b, {XK_KP_Add}}, {0x6d, {XK_KP_Subtract}}, {0x6e, {XK_KP_Decimal}},
	{0x6f, {XK_KP_Divide}}, {0x90, {XK_Num_Lock}}, {0x91, {XK_Scroll_Lock}},
	{0xba, {XK_semicolon}}, {0xbb, {XK_equal}}, {0xbc, {XK_comma}}, {0xbd, {XK_minus}},
	{0xbe, {XK_period}}, {0xbf, {XK_slash}}, {0xc0, {XK_grave}}, {0xdb, {XK_bracketleft}},
	{0xdc, {XK_backslash}}, {0xdd, {XK_bracketright}}, {0xde, {XK_apostrophe}},
};

static int vk_syms(int vk, KeySym out[2])
{
	size_t i;

	out[0] = out[1] = NoSymbol;
	if (vk >= 0x70 && vk <= 0x87) { /* F1 to F24 */
		out[0] = XK_F1 + (KeySym)(vk - 0x70);
		return 1;
	}
	if (vk >= 0x30 && vk <= 0x39) {
		out[0] = XK_0 + (KeySym)(vk - 0x30);
		return 1;
	}
	if (vk >= 0x41 && vk <= 0x5a) {
		out[0] = XK_a + (KeySym)(vk - 0x41);
		return 1;
	}
	if (vk >= 0x60 && vk <= 0x69) {
		out[0] = XK_KP_0 + (KeySym)(vk - 0x60);
		return 1;
	}
	for (i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
		if (g_keys[i].vk == vk) {
			out[0] = g_keys[i].sym[0];
			out[1] = g_keys[i].sym[1];
			return 1;
		}
	}
	return 0;
}

/* --- per-frame state --- */

static Display *g_dpy;
static Window g_win;
static Window g_chain[8]; /* the drawable and the windows above it, up to the window manager's frame */
static int g_chain_n;
static int g_w, g_h, g_size_dirty = 1, g_size_age;
static char g_keymap[32], g_prev_keymap[32];
static int g_keymap_valid;
static KeyCode g_vk_code[256][2];
static int g_vk_ready;
static int g_ptr_x, g_ptr_y, g_ptr_in, g_ptr_left, g_ptr_frame = -1, g_frame;
static int g_focus = 1, g_focus_age;

static void build_vk_codes(void)
{
	KeySym s[2];
	int vk, j;

	for (vk = 0; vk < 256; vk++) {
		g_vk_code[vk][0] = g_vk_code[vk][1] = 0;
		if (!vk_syms(vk, s))
			continue;
		for (j = 0; j < 2; j++) {
			if (s[j] != NoSymbol)
				g_vk_code[vk][j] = XKeysymToKeycode(g_dpy, s[j]);
		}
	}
	g_vk_ready = 1;
}

/* The game draws into a child of its top-level window, and the window manager puts that in a
 frame of its own (GNOME does, probe: ReparentNotify). Focus can sit on any of them, usually the
 top-level, so all of them count as the game. */
static void build_chain(Display *d, Window w)
{
	Window root, parent, *kids = NULL;
	unsigned n;

	g_chain_n = 0;
	while (w && g_chain_n < (int)(sizeof g_chain / sizeof g_chain[0])) {
		g_chain[g_chain_n++] = w;
		if (!XQueryTree(d, w, &root, &parent, &kids, &n))
			break;
		if (kids)
			XFree(kids);
		if (!parent || parent == root)
			break;
		w = parent;
	}
}

static int in_chain(Window w)
{
	int i;

	for (i = 0; i < g_chain_n; i++) {
		if (g_chain[i] == w)
			return 1;
	}
	return 0;
}

static int focus_is_game(Display *d, Window focus)
{
	if (focus == PointerRoot)
		return 1;
	if (focus == None)
		return 0;
	if (in_chain(focus))
		return 1;
	/* The window may have been framed or reframed since the chain was built. */
	build_chain(d, g_win);
	return in_chain(focus);
}

static const char *chain_text(void)
{
	static char buf[160];
	size_t off = 0;
	int i;

	buf[0] = 0;
	for (i = 0; i < g_chain_n && off < sizeof buf - 20; i++)
		off += (size_t)snprintf(buf + off, sizeof buf - off, " 0x%lx", (unsigned long)g_chain[i]);
	return buf;
}

void plat_note_resize(unsigned long window)
{
	(void)window;
	g_size_dirty = 1;
}

void plat_frame_begin(void *display, unsigned long drawable)
{
	Display *d = display;
	Window focus;
	int revert;

	g_frame++;
	if (d != g_dpy || drawable != g_win) {
		g_dpy = d;
		g_win = drawable;
		build_chain(d, drawable);
		g_vk_ready = 0;
		g_size_dirty = 1;
		g_keymap_valid = 0;
	}
	if (!g_vk_ready)
		build_vk_codes();
	/* Size: re-read when a hook saw a resize, and every 60 frames in case one is missed. */
	if (g_size_dirty || ++g_size_age >= 60) {
		Window root;
		int x, y;
		unsigned w, h, bw, depth;

		if (XGetGeometry(d, drawable, &root, &x, &y, &w, &h, &bw, &depth)) {
			g_w = (int)w;
			g_h = (int)h;
		}
		g_size_dirty = 0;
		g_size_age = 0;
	}
	/* Focus: every 15 frames is quick enough to notice alt-tab and saves a round trip per frame. */
	if (++g_focus_age >= 15 || g_frame < 3) {
		int was = g_focus;

		XGetInputFocus(d, &focus, &revert);
		g_focus = focus_is_game(d, focus);
		g_focus_age = 0;
		if (g_focus != was || g_frame < 3)
			plat_log("focus %s (focus window 0x%lx, game windows%s)", g_focus ? "on the game" : "elsewhere",
				 (unsigned long)focus, chain_text());
	}
	memcpy(g_prev_keymap, g_keymap, sizeof g_keymap);
	XQueryKeymap(d, g_keymap);
	if (!g_keymap_valid) {
		memcpy(g_prev_keymap, g_keymap, sizeof g_keymap);
		g_keymap_valid = 1;
	}
}

void plat_client_size(int *w, int *h)
{
	*w = g_w;
	*h = g_h;
}

unsigned long plat_window(void)
{
	return g_win;
}

int plat_window_focused(void)
{
	return g_focus;
}

static int code_down(const char *map, KeyCode c)
{
	return c && (map[c >> 3] >> (c & 7) & 1);
}

int plat_key_down(int vk)
{
	if (!g_dpy || vk <= 0 || vk > 255)
		return 0;
	if (vk == 1 || vk == 2) { /* VK_LBUTTON, VK_RBUTTON */
		int x, y, left;

		plat_pointer(&x, &y, &left);
		return vk == 1 ? left : 0;
	}
	return code_down(g_keymap, g_vk_code[vk][0]) || code_down(g_keymap, g_vk_code[vk][1]);
}

int plat_key_pressed_any(void)
{
	int vk, j;

	if (!g_dpy)
		return 0;
	for (vk = 8; vk < 256; vk++) {
		for (j = 0; j < 2; j++) {
			KeyCode c = g_vk_code[vk][j];

			if (code_down(g_keymap, c) && !code_down(g_prev_keymap, c))
				return vk;
		}
	}
	return 0;
}

const char *plat_key_name(int vk, char *buf, size_t size)
{
	KeySym s[2];
	const char *n;

	if (vk_syms(vk, s) && s[0] != NoSymbol && (n = XKeysymToString(s[0]))) {
		/* "Shift_L" reads better as "Shift" for the generic keys. */
		size_t len = strlen(n);

		if (s[1] != NoSymbol && len > 2 && n[len - 2] == '_')
			len -= 2;
		snprintf(buf, size, "%.*s", (int)len, n);
		if (buf[0] >= 'a' && buf[0] <= 'z' && !buf[1])
			buf[0] = (char)(buf[0] - 32);
	} else {
		snprintf(buf, size, "Key %d", vk);
	}
	return buf;
}

int plat_pointer(int *x, int *y, int *left_down)
{
	if (g_ptr_frame != g_frame && g_dpy) {
		Window root, child;
		int rx, ry, wx, wy;
		unsigned mask;

		g_ptr_frame = g_frame;
		g_ptr_in = 0;
		if (real_XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &wx, &wy, &mask)) {
			g_ptr_in = wx >= 0 && wy >= 0 && wx < g_w && wy < g_h;
			/* Clamp: during fullscreen toggles the probe saw positions far outside the window. */
			g_ptr_x = wx < 0 ? 0 : wx >= g_w ? MAX(g_w - 1, 0) : wx;
			g_ptr_y = wy < 0 ? 0 : wy >= g_h ? MAX(g_h - 1, 0) : wy;
			g_ptr_left = !!(mask & Button1Mask);
		} else {
			g_ptr_left = 0; /* pointer on another screen */
		}
	}
	*x = g_ptr_x;
	*y = g_ptr_y;
	*left_down = g_ptr_left;
	return g_ptr_in;
}

/* --- dropping pointer events while the panel is open (called from the event hooks) --- */

static int g_block;
static unsigned g_dropped; /* since the panel last opened */

#define BUTTON_MASKS (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)

/* What the game's XQueryPointer calls report while the panel is open: the pointer where it was when
 the panel opened, with no buttons down. The Windows panel gets the same effect from an input window
 over the game, which then gets no mouse input at all. Modifier keys stay live, because the keyboard
 still reaches the game on Windows too. */
static int g_snap_valid, g_snap_rx, g_snap_ry;
static Window g_snap_child;

void plat_block_pointer(int on)
{
	Window root, child;
	int rx, ry, wx, wy;
	unsigned mask;

	if (on && !g_block && g_dpy) {
		g_snap_valid = real_XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &wx, &wy, &mask);
		g_snap_rx = rx;
		g_snap_ry = ry;
		g_snap_child = child;
		g_dropped = 0;
		plat_log("panel open: the game's mouse input is held back");
	} else if (!on && g_block) {
		plat_log("panel closed: %u pointer events were held back from the game", g_dropped);
	}
	g_block = on;
}

void input_filter_query_pointer(Window *child, int *rx, int *ry, int *wx, int *wy, unsigned *mask)
{
	if (g_snap_valid) {
		/* Window-relative positions move with the root ones, whichever window was asked about. */
		*wx -= *rx - g_snap_rx;
		*wy -= *ry - g_snap_ry;
		*rx = g_snap_rx;
		*ry = g_snap_ry;
		*child = g_snap_child;
	}
	*mask &= ~(unsigned)BUTTON_MASKS;
}

unsigned plat_pointer_events_dropped(void)
{
	return g_dropped;
}

static Bool is_pointer_event(Display *d, XEvent *e, XPointer arg)
{
	(void)d;
	(void)arg;
	return e->type == ButtonPress || e->type == ButtonRelease || e->type == MotionNotify;
}

int input_blocking(void)
{
	return g_block;
}

int input_is_pointer_event(const XEvent *e)
{
	return e->type == ButtonPress || e->type == ButtonRelease || e->type == MotionNotify;
}

void input_count_dropped(void)
{
	g_dropped++;
}

/* Removes queued pointer events. XCheckIfEvent never blocks. */
void input_purge(Display *d)
{
	XEvent e;

	while (XCheckIfEvent(d, &e, is_pointer_event, NULL))
		g_dropped++;
}

/* --- hiding the cursor (ShowCursor) ---
 The probe showed the game giving its drawing window a cursor when it creates it, and no
 XDefineCursor import. So the library records the cursor each game window was given (the
 XCreateWindow and XChangeWindowAttributes hooks), shows a blank one of its own while hidden,
 and puts the recorded one back afterwards. X has no call to read a window's cursor back.
*/

#define MAX_CURSORS 16

static struct {
	Window w;
	Cursor c;
} g_cursors[MAX_CURSORS];
static int g_cursor_count; /* ShowCursor's display count, hidden below 0 */
static int g_cursor_hidden;
static Cursor g_blank;

static Cursor recorded_cursor(Window w)
{
	int i;

	for (i = 0; i < MAX_CURSORS; i++) {
		if (g_cursors[i].w == w)
			return g_cursors[i].c;
	}
	return None;
}

static void cursor_apply(void)
{
	int hide = g_cursor_count < 0;
	Cursor c;

	if (!g_dpy || !g_win || hide == g_cursor_hidden)
		return;
	if (hide) {
		if (!g_blank) {
			static const char zero[8];
			Pixmap p = XCreateBitmapFromData(g_dpy, g_win, zero, 8, 8);
			XColor black;

			memset(&black, 0, sizeof black);
			g_blank = XCreatePixmapCursor(g_dpy, p, p, &black, &black, 0, 0);
			XFreePixmap(g_dpy, p);
		}
		XDefineCursor(g_dpy, g_win, g_blank);
	} else if ((c = recorded_cursor(g_win)) != None) {
		XDefineCursor(g_dpy, g_win, c);
	} else {
		XUndefineCursor(g_dpy, g_win);
	}
	g_cursor_hidden = hide;
	plat_log("cursor %s over the game window", hide ? "hidden" : "shown again");
}

int plat_show_cursor(int show)
{
	g_cursor_count += show ? 1 : -1;
	cursor_apply();
	return g_cursor_count;
}

void input_note_cursor(unsigned long window, unsigned long cursor)
{
	int i, free_slot = -1;

	for (i = 0; i < MAX_CURSORS; i++) {
		if (g_cursors[i].w == window) {
			g_cursors[i].c = cursor;
			free_slot = -2;
			break;
		}
		if (!g_cursors[i].w && free_slot == -1)
			free_slot = i;
	}
	if (free_slot >= 0) {
		g_cursors[free_slot].w = window;
		g_cursors[free_slot].c = cursor;
	}
	/* The game changed it while hidden: hide it again over the new one. */
	if (g_cursor_hidden && window == g_win && g_dpy && g_blank)
		XDefineCursor(g_dpy, g_win, g_blank);
}
