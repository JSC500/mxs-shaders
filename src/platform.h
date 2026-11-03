/*
 platform.h: every OS service the mod needs, behind one small interface.

 The Linux build's services: platform_linux.c (files, ini, time, GL lookup), input_x11.c
 (window, keys, pointer, cursor) and log_linux.c. The shared code reaches them through the
 Win32 stand-ins in win_compat.h. All strings here are UTF-8, and paths use '/'.
*/
#ifndef MXS_PLATFORM_H
#define MXS_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* --- logging: the same mxs-shaders.log as log_msg (log_linux.c) --- */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- paths and files --- */
/* $XDG_DATA_HOME/mxs-shaders, else ~/.local/share/mxs-shaders. Created on first use. */
const char *plat_data_dir(void);
/* Writes data_dir + "/" + rel into out. Returns 0 if it didn't fit. */
int plat_data_path(char *out, size_t size, const char *rel);
int plat_mkdirs(const char *path);
int plat_is_file(const char *path);
int plat_is_dir(const char *path);
/* Calls cb for every entry except "." and "..". Returns the number of entries, -1 on error. */
typedef void (*plat_dir_cb)(const char *name, int is_dir, void *user);
int plat_list_dir(const char *path, plat_dir_cb cb, void *user);

/* --- ini files, with the semantics of GetPrivateProfile* / WritePrivateProfileString ---
 Section and key names are case-insensitive. Values are trimmed and one pair of matching
 quotes is removed. A UTF-8 or UTF-16LE file written on Windows reads the same.
*/
int ini_get_int(const char *file, const char *section, const char *key, int def);
/* Copies the value or def into out and returns its length. */
size_t ini_get_str(const char *file, const char *section, const char *key, const char *def, char *out,
		   size_t size);
/* value NULL removes the key, key NULL removes the section. Returns 1 on success. */
int ini_set_str(const char *file, const char *section, const char *key, const char *value);
int ini_set_int(const char *file, const char *section, const char *key, int value);

/* --- time (replaces QueryPerformanceCounter / QueryPerformanceFrequency) --- */
int64_t plat_ticks(void);     /* monotonic nanoseconds */
int64_t plat_tick_freq(void); /* 1e9 */
double plat_seconds(void);    /* since the library loaded */

/* --- GL function lookup (replaces wglGetProcAddress + GetProcAddress on opengl32) --- */
void *plat_gl_proc(const char *name);

/* --- window and input (input_x11.c) ---
 Key codes are Windows virtual-key codes everywhere (ui_key=119 is F8), so ini files carry over.
*/
/* Called once per frame from the swap hook, before anything else reads input. */
void plat_frame_begin(void *display, unsigned long drawable);
void plat_client_size(int *w, int *h);
unsigned long plat_window(void); /* the X window the game draws in */
int plat_window_focused(void);
int plat_key_down(int vk);
/* The first key that went down since the last call, as a virtual-key code, or 0. */
int plat_key_pressed_any(void);
const char *plat_key_name(int vk, char *buf, size_t size);
/* Pointer position in the drawable, clamped to the client rect. Returns 1 if it is inside. */
int plat_pointer(int *x, int *y, int *left_down);
/* While on, button and motion events for the game's windows are dropped before the game reads them. */
void plat_block_pointer(int on);
unsigned plat_pointer_events_dropped(void);
/* Whether Escape was pressed while the panel was open (held back from the game) since the last call. */
int plat_escape_pressed(void);
/* Like ShowCursor: a count that hides the cursor over the game's window while it is below 0. */
int plat_show_cursor(int show);
/* Size changes seen by the event or resize hooks, so the size is re-read without a round trip every frame. */
void plat_note_resize(unsigned long window);

#endif
