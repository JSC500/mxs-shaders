#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "win_compat.h"	/* the Linux build: the same names on top of platform.h */
#endif
#include <stdio.h>

/* log.c: everything goes to %LOCALAPPDATA%\mxs-shaders, the game folder is usually read-only.
 log_linux.c: the same in $XDG_DATA_HOME/mxs-shaders, normally ~/.local/share/mxs-shaders. */
void log_init(void);
void log_msg(const char *fmt, ...);
FILE *log_open(const wchar_t *subdir, const char *name);
void log_path(wchar_t *path, const wchar_t *name);

/* pack.c */
void pack_init(const wchar_t *name);
char *pack_load(const char *file);

/* The default pack, generated from shaderpacks\default at build time. */
typedef struct {
	const char *name, *text;
} embedded_file;
extern const embedded_file embedded_pack[];

/* proxy.c (log_linux.c on Linux, where it is only a placeholder) */
extern HMODULE g_real_gl;

/* hooks.c */
void hooks_init(void);
