#pragma once

/* The in-game settings panel, styled after the game's own menus. */

enum { UI_SLIDER, UI_CHOICE, UI_TOGGLE };
enum { UI_SHOW_NUMBER, UI_SHOW_CLOCK };	/* a clock slider's 0 is "Track", then quarter hours from 00:00 */
enum { UI_TAB_LOOK, UI_TAB_WEATHER, UI_TAB_DISPLAY, UI_TABS };
#define UI_TAB_TOP UI_TABS	/* not on a page: under the shader pack, always showing */

typedef struct {
	const char *label;
	const wchar_t *key;	/* in the ini */
	int kind;
	int *value;
	int min, max, step;	/* sliders */
	const int *choices;
	int choice_count;
	BOOL restart;		/* only takes effect on the next start */
	const char *const *names;	/* choices shown by name, else as numbers */
	int tab;		/* which page of the panel it's on */
	int show;		/* how a slider's value is written */
} ui_item;

/* The GL state to leave behind, none of it goes through the hooks. */
typedef struct {
	GLuint program, vao, array_buffer;
	GLenum active_unit, blend_src, blend_dst;
	GLint viewport[4];
	int key;		/* the virtual key that opens the panel */
	int enabled;		/* 0 while the game draws with its own look */
	BOOL debug;		/* offers reloading the shaders */
} ui_state;

void ui_init(PROC (WINAPI *get_proc)(LPCSTR), const ui_item *items, int count);
void ui_context_lost(void);
BOOL ui_is_open(void);
BOOL ui_is_reading_key(void);	/* the player is choosing a new panel key */
void ui_toggle(HWND game);
void ui_draw(HWND game, int w, int h, const wchar_t *pack, const ui_state *s);

/* Implemented by hooks.c. */
void ui_setting_changed(const ui_item *item);
void ui_pack_changed(const wchar_t *name);
void ui_key_changed(int key);
void ui_enabled_changed(int enabled);
void ui_reload_requested(void);
