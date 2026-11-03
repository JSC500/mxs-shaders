/* Shared between preload.c and input_x11.c only. */
#ifndef MXS_LINUX_INTERNAL_H
#define MXS_LINUX_INTERNAL_H

#include <X11/Xlib.h>

int input_blocking(void);
/* Whether any events are being held back from the game now (panel open, or Escape still down). */
int input_filtering(void);
/* Whether the game must not see this event. */
int input_drop_event(const XEvent *e);
void input_purge(Display *d);
/* The cursor a game window was given, recorded so it can be put back after hiding. */
void input_note_cursor(unsigned long window, unsigned long cursor);
/* Rewrites a result of the game's XQueryPointer while the panel is open. */
void input_filter_query_pointer(Window *child, int *rx, int *ry, int *wx, int *wy, unsigned *mask);
/* The real XQueryPointer, for the library's own use (its exported one is the game's filtered view). */
Bool real_XQueryPointer(Display *d, Window w, Window *root, Window *child, int *rx, int *ry, int *wx, int *wy,
			unsigned *mask);

#endif
