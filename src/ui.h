#pragma once

/* The in-game settings panel, styled after the game's own menus. */

enum { UI_SLIDER, UI_CHOICE, UI_TOGGLE };

typedef struct {
	const char *label;
	const wchar_t *key;	/* in the ini */
	int kind;
	int *value;
	int min, max, step;	/* sliders */
	const int *choices;
	int choice_count;
	BOOL restart;		/* only takes effect on the next start */
} ui_item;

/* The GL state to leave behind, none of it goes through the hooks. */
typedef struct {
	GLuint program, vao, array_buffer;
	GLenum active_unit, blend_src, blend_dst;
	GLint viewport[4];
	BOOL debug;		/* offers reloading the shaders */
} ui_state;

void ui_init(PROC (WINAPI *get_proc)(LPCSTR), const ui_item *items, int count);
void ui_context_lost(void);
BOOL ui_is_open(void);
void ui_toggle(HWND game);
void ui_draw(HWND game, int w, int h, const wchar_t *pack, const ui_state *s);

/* Implemented by hooks.c. */
void ui_setting_changed(const ui_item *item);
void ui_pack_changed(const wchar_t *name);
void ui_reload_requested(void);
