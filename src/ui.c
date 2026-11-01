/*
 The in-game settings panel, in the look of the game's menus. It is drawn straight
 onto the finished frame, so none of the post-processing touches it.

 The proxy can't take input away from the game, so while the panel is open an
 invisible window lies over the game. It catches the mouse without taking focus,
 and the game never sees those clicks.
*/
#include "common.h"
#include "post.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 4
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_LINK_STATUS 0x8B82

#define UNIT_FONT 17		/* above the post-processing units */
#define FONT_COLUMNS 16
#define FONT_ROWS 6
#define FONT_FIRST 32
#define FONT_LAST 126
#define MAX_QUADS 4096
#define MAX_PACKS 32
#define VERTEX_FLOATS 8

typedef struct {
	float r, g, b, a;
} color;

static const color TEXT_LIGHT = { 0.9f, 0.9f, 0.9f, 1.0f }, TEXT_DARK = { 0.0f, 0.0f, 0.0f, 1.0f };
static const color SHADOW = { 0.0f, 0.0f, 0.0f, 0.7f }, ASTERISK = { 0.95f, 0.12f, 0.12f, 1.0f };
static const color LABEL_BOX = { 0.15f, 0.19f, 0.25f, 0.6f }, LABEL_EDGE = { 0.1f, 0.12f, 0.16f, 0.75f };
static const color BUTTON_BOX = { 0.42f, 0.44f, 0.47f, 0.36f }, BUTTON_EDGE = { 0.55f, 0.56f, 0.58f, 0.3f };
static const color SELECTED = { 0.9f, 0.91f, 0.93f, 0.85f }, SELECTED_EDGE = { 0.97f, 0.97f, 0.98f, 0.95f };
static const color BAR = { 0.92f, 0.92f, 0.92f, 0.8f };
static const color DIM = { 0.0f, 0.0f, 0.0f, 0.3f };

static void (WINAPI *gl_Enable)(GLenum), (WINAPI *gl_Disable)(GLenum);
static GLboolean (WINAPI *gl_IsEnabled)(GLenum);
static void (WINAPI *gl_BlendFunc)(GLenum, GLenum);
static void (WINAPI *gl_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (WINAPI *gl_BindTexture)(GLenum, GLuint);
static void (WINAPI *gl_GenTextures)(GLsizei, GLuint *);
static void (WINAPI *gl_DeleteTextures)(GLsizei, const GLuint *);
static void (WINAPI *gl_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (WINAPI *gl_TexParameteri)(GLenum, GLenum, GLint);
static void (WINAPI *gl_DrawArrays)(GLenum, GLint, GLsizei);
static void (WINAPI *gl_ActiveTexture)(GLenum);
static GLuint (WINAPI *gl_CreateShader)(GLenum);
static void (WINAPI *gl_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (WINAPI *gl_CompileShader)(GLuint);
static GLuint (WINAPI *gl_CreateProgram)(void);
static void (WINAPI *gl_AttachShader)(GLuint, GLuint);
static void (WINAPI *gl_LinkProgram)(GLuint);
static void (WINAPI *gl_DeleteShader)(GLuint);
static void (WINAPI *gl_GetProgramiv)(GLuint, GLenum, GLint *);
static void (WINAPI *gl_UseProgram)(GLuint);
static GLint (WINAPI *gl_GetUniformLocation)(GLuint, const GLchar *);
static void (WINAPI *gl_Uniform1i)(GLint, GLint);
static void (WINAPI *gl_Uniform2f)(GLint, float, float);
static void (WINAPI *gl_GenVertexArrays)(GLsizei, GLuint *);
static void (WINAPI *gl_BindVertexArray)(GLuint);
static void (WINAPI *gl_GenBuffers)(GLsizei, GLuint *);
static void (WINAPI *gl_BindBuffer)(GLenum, GLuint);
static void (WINAPI *gl_BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
static void (WINAPI *gl_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (WINAPI *gl_EnableVertexAttribArray)(GLuint);
static void (WINAPI *gl_BindAttribLocation)(GLuint, GLuint, const GLchar *);
static void (WINAPI *gl_BindFramebuffer)(GLenum, GLuint);

static BOOL g_ready, g_open;
static const ui_item *g_items;
static int g_item_count;
static GLuint g_prog, g_vao, g_vbo, g_font;
static GLint g_loc_screen;
static int g_font_px, g_cell, g_advance[FONT_LAST - FONT_FIRST + 1];

static float g_vertices[MAX_QUADS * 6 * VERTEX_FLOATS];
static int g_vertex_count;

static HWND g_input_window;
static HCURSOR g_game_cursor;
static BOOL g_mouse_was_down, g_reading_key, g_key_was_down[256];
static int g_dragging = -1;


static wchar_t g_packs[MAX_PACKS][64];
static int g_pack_count;

static const char *const g_vs =
	"#version 150\n"
	"in vec2 a_pos, a_uv;\n"
	"in vec4 a_color;\n"
	"out vec2 v_uv;\n"
	"out vec4 v_color;\n"
	"uniform vec2 u_screen;\n"
	"void main()\n"
	"{\n"
	"	v_uv = a_uv;\n"
	"	v_color = a_color;\n"
	"	gl_Position = vec4(a_pos / u_screen * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
	"}\n";

/* Text has coordinates in the font, shapes use a negative one for full coverage. */
static const char *const g_fs =
	"#version 150\n"
	"in vec2 v_uv;\n"
	"in vec4 v_color;\n"
	"out vec4 f_color;\n"
	"uniform sampler2D u_font;\n"
	"void main()\n"
	"{\n"
	"	float a = v_uv.x < 0.0 ? 1.0 : texture(u_font, v_uv).r;\n"
	"	f_color = vec4(v_color.rgb, v_color.a * a);\n"
	"}\n";

#define LOAD(var, name) *(PROC *)&var = get_proc(name)
#define LOAD_GL11(var, name) *(FARPROC *)&var = GetProcAddress(g_real_gl, name)

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = gl_CreateShader(type);

	gl_ShaderSource(s, 1, &src, NULL);
	gl_CompileShader(s);
	return s;
}

void ui_init(PROC (WINAPI *get_proc)(LPCSTR), const ui_item *items, int count)
{
	GLuint vs, fs;
	GLint ok = 0;

	g_items = items;
	g_item_count = count;

	LOAD_GL11(gl_Enable, "glEnable");
	LOAD_GL11(gl_Disable, "glDisable");
	LOAD_GL11(gl_IsEnabled, "glIsEnabled");
	LOAD_GL11(gl_BlendFunc, "glBlendFunc");
	LOAD_GL11(gl_Viewport, "glViewport");
	LOAD_GL11(gl_BindTexture, "glBindTexture");
	LOAD_GL11(gl_GenTextures, "glGenTextures");
	LOAD_GL11(gl_DeleteTextures, "glDeleteTextures");
	LOAD_GL11(gl_TexImage2D, "glTexImage2D");
	LOAD_GL11(gl_TexParameteri, "glTexParameteri");
	LOAD_GL11(gl_DrawArrays, "glDrawArrays");

	LOAD(gl_ActiveTexture, "glActiveTexture");
	LOAD(gl_CreateShader, "glCreateShader");
	LOAD(gl_ShaderSource, "glShaderSource");
	LOAD(gl_CompileShader, "glCompileShader");
	LOAD(gl_CreateProgram, "glCreateProgram");
	LOAD(gl_AttachShader, "glAttachShader");
	LOAD(gl_LinkProgram, "glLinkProgram");
	LOAD(gl_DeleteShader, "glDeleteShader");
	LOAD(gl_GetProgramiv, "glGetProgramiv");
	LOAD(gl_UseProgram, "glUseProgram");
	LOAD(gl_GetUniformLocation, "glGetUniformLocation");
	LOAD(gl_Uniform1i, "glUniform1i");
	LOAD(gl_Uniform2f, "glUniform2f");
	LOAD(gl_GenVertexArrays, "glGenVertexArrays");
	LOAD(gl_BindVertexArray, "glBindVertexArray");
	LOAD(gl_GenBuffers, "glGenBuffers");
	LOAD(gl_BindBuffer, "glBindBuffer");
	LOAD(gl_BufferData, "glBufferData");
	LOAD(gl_VertexAttribPointer, "glVertexAttribPointer");
	LOAD(gl_EnableVertexAttribArray, "glEnableVertexAttribArray");
	LOAD(gl_BindAttribLocation, "glBindAttribLocation");
	LOAD(gl_BindFramebuffer, "glBindFramebuffer");

	vs = compile(GL_VERTEX_SHADER, g_vs);
	fs = compile(GL_FRAGMENT_SHADER, g_fs);
	g_prog = gl_CreateProgram();
	gl_AttachShader(g_prog, vs);
	gl_AttachShader(g_prog, fs);
	gl_BindAttribLocation(g_prog, 0, "a_pos");
	gl_BindAttribLocation(g_prog, 1, "a_uv");
	gl_BindAttribLocation(g_prog, 2, "a_color");
	gl_LinkProgram(g_prog);
	gl_DeleteShader(vs);
	gl_DeleteShader(fs);
	gl_GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		log_msg("ui: shaders failed to link, no settings panel");
		return;
	}
	gl_UseProgram(g_prog);
	gl_Uniform1i(gl_GetUniformLocation(g_prog, "u_font"), UNIT_FONT);
	g_loc_screen = gl_GetUniformLocation(g_prog, "u_screen");
	gl_UseProgram(0);

	gl_GenVertexArrays(1, &g_vao);
	gl_GenBuffers(1, &g_vbo);
	g_font = 0;
	g_font_px = 0;
	g_ready = TRUE;
}

void ui_context_lost(void)
{
	g_ready = FALSE;
}

BOOL ui_is_open(void)
{
	return g_open;
}

BOOL ui_is_reading_key(void)
{
	return g_reading_key;
}

/* --- the font --- */

/* Verdana Bold Italic is close to the game's menu font and comes with Windows. */
static void font_build(int px)
{
	BITMAPINFO bmi = { 0 };
	unsigned char *pixels = NULL;
	int cell = px * 2, w = FONT_COLUMNS * cell, h = FONT_ROWS * cell, c, i;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap;
	HFONT font;

	bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
	font = CreateFontW(-px, 0, 0, 0, FW_BOLD, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Verdana");
	if (!bitmap || !font || !pixels) {
		log_msg("ui: font failed");
		DeleteDC(dc);
		return;
	}
	SelectObject(dc, bitmap);
	SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	for (c = FONT_FIRST; c <= FONT_LAST; c++) {
		char ch = (char)c;
		SIZE size;

		i = c - FONT_FIRST;
		GetTextExtentPoint32A(dc, &ch, 1, &size);
		g_advance[i] = size.cx;
		TextOutA(dc, i % FONT_COLUMNS * cell + px / 4, i / FONT_COLUMNS * cell, &ch, 1);
	}

	if (!g_font)
		gl_GenTextures(1, &g_font);
	gl_ActiveTexture(GL_TEXTURE0 + UNIT_FONT);
	gl_BindTexture(GL_TEXTURE_2D, g_font);
	gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	DeleteObject(font);
	DeleteObject(bitmap);
	DeleteDC(dc);
	g_font_px = px;
	g_cell = cell;
}

/* --- drawing --- */

static void vertex(float x, float y, float u, float v, color c)
{
	float *f = &g_vertices[g_vertex_count++ * VERTEX_FLOATS];

	f[0] = x;
	f[1] = y;
	f[2] = u;
	f[3] = v;
	f[4] = c.r;
	f[5] = c.g;
	f[6] = c.b;
	f[7] = c.a;
}

static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, color c)
{
	if (g_vertex_count + 6 > MAX_QUADS * 6)
		return;
	vertex(x0, y0, u0, v0, c);
	vertex(x1, y0, u1, v0, c);
	vertex(x1, y1, u1, v1, c);
	vertex(x0, y0, u0, v0, c);
	vertex(x1, y1, u1, v1, c);
	vertex(x0, y1, u0, v1, c);
}

static void fill(float x, float y, float w, float h, color c)
{
	quad_uv(x, y, x + w, y + h, -1.0f, 0.0f, -1.0f, 0.0f, c);
}

static void box(float x, float y, float w, float h, color back, color edge)
{
	const float t = max(2.0f, (float)(int)(h * 0.1f));

	fill(x, y, w, h, back);
	fill(x, y, w, t, edge);
	fill(x, y + h - t, w, t, edge);
	fill(x, y + t, t, h - 2 * t, edge);
	fill(x + w - t, y + t, t, h - 2 * t, edge);
}

static void label_box(float x, float y, float w, float h)
{
	box(x, y, w, h, LABEL_BOX, LABEL_EDGE);
}

/* Buttons and values, white under the mouse like the game's menus. */
static void button_box(float x, float y, float w, float h, BOOL selected)
{
	box(x, y, w, h, selected ? SELECTED : BUTTON_BOX, selected ? SELECTED_EDGE : BUTTON_EDGE);
}

/* The menus space their letters out. */
static float spacing(void)
{
	return g_font_px * 0.13f;
}

static float text_width(const char *s)
{
	float w = 0.0f;

	for (; *s; s++)
		if (*s >= FONT_FIRST && *s <= FONT_LAST)
			w += g_advance[*s - FONT_FIRST] + spacing();
	return w;
}

static void glyphs(float x, float y, const char *s, color c)
{
	const float tw = 1.0f / FONT_COLUMNS, th = 1.0f / FONT_ROWS;

	for (; *s; s++) {
		int i;
		float u, v;

		if (*s < FONT_FIRST || *s > FONT_LAST)
			continue;
		i = *s - FONT_FIRST;
		u = (i % FONT_COLUMNS) * tw;
		v = (i / FONT_COLUMNS) * th;
		quad_uv(x - g_font_px * 0.25f, y, x - g_font_px * 0.25f + g_cell, y + g_cell, u, v, u + tw, v + th, c);
		x += g_advance[i] + spacing();
	}
}

static void text(float x, float y, float h, const char *s, color c)
{
	float top = y + (h - g_font_px * 1.25f) * 0.5f;

	if (c.r > 0.5f)
		glyphs(x + 1.5f, top + 1.5f, s, SHADOW);
	glyphs(x, top, s, c);
}

/* --- the input window --- */

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_SETCURSOR:
		SetCursor(g_game_cursor);
		return TRUE;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void input_window_open(HWND game)
{
	static BOOL registered;
	POINT origin = { 0, 0 };
	RECT r;

	if (!registered) {
		WNDCLASSW wc = { 0 };

		wc.lpfnWndProc = input_proc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = L"MXSShadersInput";
		RegisterClassW(&wc);
		registered = TRUE;
	}
	g_game_cursor = (HCURSOR)GetClassLongPtrW(game, GCLP_HCURSOR);
	GetClientRect(game, &r);
	ClientToScreen(game, &origin);
	g_input_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MXSShadersInput", L"", WS_POPUP,
					 origin.x, origin.y, r.right, r.bottom, game, NULL, GetModuleHandleW(NULL), NULL);
	if (!g_input_window) {
		log_msg("ui: no input window (%lu), clicks will reach the game", GetLastError());
		return;
	}
	/* The lowest alpha that still receives the mouse, invisible but solid. */
	SetLayeredWindowAttributes(g_input_window, 0, 1, LWA_ALPHA);
	ShowWindow(g_input_window, SW_SHOWNOACTIVATE);
}

static void input_window_close(void)
{
	if (g_input_window)
		DestroyWindow(g_input_window);
	g_input_window = NULL;
}

static void packs_scan(void)
{
	wchar_t pattern[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	wcscpy_s(g_packs[0], 64, L"default");
	g_pack_count = 1;
	log_path(pattern, L"shaderpacks\\*");
	h = FindFirstFileW(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
		    !wcscmp(fd.cFileName, L"default") || wcslen(fd.cFileName) >= 64 || g_pack_count == MAX_PACKS)
			continue;
		wcscpy_s(g_packs[g_pack_count++], 64, fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

void ui_toggle(HWND game)
{
	if (!g_ready)
		return;
	g_open = !g_open;
	g_dragging = -1;
	g_reading_key = FALSE;
	g_mouse_was_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	if (g_open) {
		packs_scan();
		input_window_open(game);
	} else {
		input_window_close();
	}
}

/* --- the panel --- */

static BOOL inside(float px, float py, float x, float y, float w, float h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

static int choice_index(const ui_item *it)
{
	int i;

	for (i = 0; i < it->choice_count; i++)
		if (it->choices[i] == *it->value)
			return i;
	return 0;
}


static void slider_set(const ui_item *it, float t)
{
	int v = it->min + (int)((it->max - it->min) * t / it->step + 0.5f) * it->step;

	*it->value = min(max(v, it->min), it->max);
}

static void pack_row(float x, float y, float label_w, float value_w, float row, float mx, float my, BOOL click, const wchar_t *pack)
{
	char name[64];
	BOOL hover = inside(mx, my, x, y, label_w + value_w, row);
	int i, current = 0;

	for (i = 0; i < g_pack_count; i++)
		if (!_wcsicmp(g_packs[i], pack))
			current = i;
	WideCharToMultiByte(CP_ACP, 0, g_packs[current], -1, name, sizeof name, NULL, NULL);

	label_box(x, y, label_w, row);
	text(x + row * 0.2f, y, row, "Shader Pack", TEXT_LIGHT);
	button_box(x + label_w, y, value_w, row, hover);
	text(x + label_w + row * 0.2f, y, row, name, hover ? TEXT_DARK : TEXT_LIGHT);

	if (hover && click && g_pack_count > 1)
		ui_pack_changed(g_packs[(current + 1) % g_pack_count]);
}

static void item_row(int index, float x, float y, float label_w, float value_w, float row, float mx, float my, BOOL down, BOOL click)
{
	const ui_item *it = &g_items[index];
	BOOL hover = inside(mx, my, x, y, label_w + value_w, row);
	BOOL active = g_dragging == index || (hover && g_dragging < 0);
	char value[32];

	label_box(x, y, label_w, row);
	text(x + row * 0.2f, y, row, it->label, TEXT_LIGHT);
	if (it->restart)
		text(x + row * 0.2f + text_width(it->label), y, row, "*", ASTERISK);

	if (it->kind == UI_SLIDER) {
		float number_w = row * 2.4f, bar_x = x + label_w + number_w, bar_w = value_w - number_w;
		float t = (float)(*it->value - it->min) / (it->max - it->min);
		if (hover && click)
			g_dragging = index;
		if (g_dragging == index) {
			if (down)
				slider_set(it, (mx - bar_x) / bar_w);
			else {
				g_dragging = -1;
				ui_setting_changed(it);
			}
		}

		label_box(x + label_w, y, number_w, row);
		sprintf_s(value, sizeof value, "%d", *it->value);
		text(x + label_w + number_w - text_width(value) - row * 0.2f, y, row, value, TEXT_LIGHT);

		button_box(bar_x, y, bar_w, row, active);
		fill(bar_x + row * 0.12f, y + row * 0.16f, (bar_w - row * 0.24f) * min(max(t, 0.0f), 1.0f), row * 0.68f, active ? TEXT_DARK : BAR);
		return;
	}

	if (it->kind == UI_TOGGLE)
		strcpy_s(value, sizeof value, *it->value ? "Yes" : "No");
	else
		sprintf_s(value, sizeof value, "%d", *it->value);
	button_box(x + label_w, y, value_w, row, active);
	text(x + label_w + row * 0.2f, y, row, value, active ? TEXT_DARK : TEXT_LIGHT);

	if (hover && click) {
		if (it->kind == UI_TOGGLE)
			*it->value = !*it->value;
		else
			*it->value = it->choices[(choice_index(it) + 1) % it->choice_count];
		ui_setting_changed(it);
	}
}

/* The first key to go down since the last call, not counting the mouse buttons. */
static int key_pressed(void)
{
	int vk, pressed = 0;

	for (vk = VK_BACK; vk < 256; vk++) {
		BOOL down = (GetAsyncKeyState(vk) & 0x8000) != 0;

		if (down && !g_key_was_down[vk] && !pressed)
			pressed = vk;
		g_key_was_down[vk] = down;
	}
	return pressed;
}

/* Names come from the keyboard layout. The font skips what it doesn't have, such as umlauts. */
static void key_name(int vk, char *name, int size)
{
	UINT code = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC_EX);
	LONG lp = (code & 0xff) << 16 | ((code & 0xff00) ? 1 << 24 : 0);

	if (!GetKeyNameTextA(lp, name, size))
		sprintf_s(name, size, "Key %d", vk);
}

static void panel(HWND game, int w, int h, const wchar_t *pack, int key, BOOL debug)
{
	float row = (float)(h / 26), gap = (float)(int)(row * 0.22f);
	float panel_w = min(w * 0.92f, h * 1.5f), x = (w - panel_w) * 0.5f, y = h * 0.05f;
	float label_w = (float)(int)(panel_w * 0.36f), value_w = panel_w - label_w, mx, my, key_x, key_w;
	BOOL down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, click = down && !g_mouse_was_down, close_hover, reload_hover = FALSE, key_hover;
	char key_label[48] = "Press a Key";
	POINT p;
	int i;

	GetCursorPos(&p);
	ScreenToClient(game, &p);
	mx = (float)p.x;
	my = (float)p.y;
	g_mouse_was_down = down;

	/* Escape or a click cancels. The click stays here, so it doesn't change a setting too. */
	if (g_reading_key) {
		int pressed = key_pressed();

		if (click) {
			g_reading_key = FALSE;
			click = FALSE;
		} else if (pressed) {
			g_reading_key = FALSE;
			if (pressed != VK_ESCAPE)
				ui_key_changed(pressed);
		}
	}

	fill(0.0f, 0.0f, (float)w, (float)h, DIM);

	label_box(x, y, label_w, row * 1.1f);
	text(x + row * 0.2f, y, row * 1.1f, "MXS Shaders", TEXT_LIGHT);
	y += row * 1.1f + gap * 3.0f;

	close_hover = inside(mx, my, x, y, label_w, row);
	button_box(x, y, label_w, row, close_hover);
	text(x + row * 0.2f, y, row, "Back to Game", close_hover ? TEXT_DARK : TEXT_LIGHT);
	if (debug) {
		reload_hover = inside(mx, my, x + label_w + gap, y, label_w, row);
		button_box(x + label_w + gap, y, label_w, row, reload_hover);
		text(x + label_w + gap + row * 0.2f, y, row, "Reload Current Shader", reload_hover ? TEXT_DARK : TEXT_LIGHT);
	}

	/* The rows already fill the screen, so this one shares the top line. */
	key_x = x + (label_w + gap) * 2.0f;
	key_w = x + panel_w - key_x;
	key_hover = inside(mx, my, key_x, y, key_w, row);
	if (!g_reading_key) {
		char name[32];

		key_name(key, name, sizeof name);
		sprintf_s(key_label, sizeof key_label, "Panel Key: %s", name);
	}
	button_box(key_x, y, key_w, row, key_hover);
	text(key_x + row * 0.2f, y, row, key_label, key_hover ? TEXT_DARK : TEXT_LIGHT);
	y += row + gap * 3.0f;

	pack_row(x, y, label_w, value_w, row, mx, my, click, pack);
	y += row + gap;
	for (i = 0; i < g_item_count; i++) {
		item_row(i, x, y, label_w, value_w, row, mx, my, down, click);
		y += row + gap;
	}

	y += gap * 2.0f;
	button_box(x, y, panel_w, row, FALSE);
	text(x + row * 0.2f, y, row, "*", ASTERISK);
	text(x + row * 0.2f + text_width("*"), y, row, "Needs a restart", TEXT_LIGHT);



	if (reload_hover && click)
		ui_reload_requested();
	if (key_hover && click) {
		key_pressed();	/* keys already held don't count */
		g_reading_key = TRUE;
	}
	if (close_hover && click)
		ui_toggle(game);
}

void ui_draw(HWND game, int w, int h, const wchar_t *pack, const ui_state *s)
{
	GLboolean blend, depth, cull, scissor, coverage;
	int px = h / 26 * 3 / 5;

	if (!g_ready || !g_open || w <= 0 || h <= 0)
		return;

	blend = gl_IsEnabled(GL_BLEND);
	depth = gl_IsEnabled(GL_DEPTH_TEST);
	cull = gl_IsEnabled(GL_CULL_FACE);
	scissor = gl_IsEnabled(GL_SCISSOR_TEST);
	coverage = gl_IsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
	gl_Disable(GL_DEPTH_TEST);
	gl_Disable(GL_CULL_FACE);
	gl_Disable(GL_SCISSOR_TEST);
	gl_Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	gl_Enable(GL_BLEND);
	gl_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	if (px != g_font_px)
		font_build(px);

	g_vertex_count = 0;
	panel(game, w, h, pack, s->key, s->debug);

	gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl_Viewport(0, 0, w, h);
	gl_UseProgram(g_prog);
	gl_Uniform2f(g_loc_screen, (float)w, (float)h);
	gl_ActiveTexture(GL_TEXTURE0 + UNIT_FONT);
	gl_BindTexture(GL_TEXTURE_2D, g_font);
	gl_BindVertexArray(g_vao);
	gl_BindBuffer(GL_ARRAY_BUFFER, g_vbo);
	gl_BufferData(GL_ARRAY_BUFFER, g_vertex_count * VERTEX_FLOATS * sizeof(float), g_vertices, GL_STREAM_DRAW);
	gl_EnableVertexAttribArray(0);
	gl_EnableVertexAttribArray(1);
	gl_EnableVertexAttribArray(2);
	gl_VertexAttribPointer(0, 2, GL_FLOAT, 0, VERTEX_FLOATS * sizeof(float), (const void *)0);
	gl_VertexAttribPointer(1, 2, GL_FLOAT, 0, VERTEX_FLOATS * sizeof(float), (const void *)(2 * sizeof(float)));
	gl_VertexAttribPointer(2, 4, GL_FLOAT, 0, VERTEX_FLOATS * sizeof(float), (const void *)(4 * sizeof(float)));
	gl_DrawArrays(GL_TRIANGLES, 0, g_vertex_count);

	gl_BindBuffer(GL_ARRAY_BUFFER, s->array_buffer);
	gl_BindVertexArray(s->vao);
	gl_UseProgram(s->program);
	gl_ActiveTexture(s->active_unit);
	gl_Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	gl_BlendFunc(s->blend_src, s->blend_dst);
	if (!blend)
		gl_Disable(GL_BLEND);
	if (depth)
		gl_Enable(GL_DEPTH_TEST);
	if (cull)
		gl_Enable(GL_CULL_FACE);
	if (scissor)
		gl_Enable(GL_SCISSOR_TEST);
	if (coverage)
		gl_Enable(GL_SAMPLE_ALPHA_TO_COVERAGE);
}
