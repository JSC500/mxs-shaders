#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* log.c: everything goes to %LOCALAPPDATA%\mxs-shaders, the game folder is usually read-only. */
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

/* proxy.c */
extern HMODULE g_real_gl;

/* hooks.c */
void hooks_init(void);
