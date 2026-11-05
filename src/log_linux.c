/*
 log_linux.c: log.c for the Linux build. Everything goes to $XDG_DATA_HOME/mxs-shaders,
 normally ~/.local/share/mxs-shaders, with the same layout as %LOCALAPPDATA%\mxs-shaders.
*/
#define _GNU_SOURCE
#include "common.h"
#include "platform.h"

#include <stdarg.h>

HMODULE g_real_gl; /* only a placeholder: GetProcAddress ignores it on Linux */

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

void log_init(void)
{
	static const char *const subdirs[] = { "shaders", "frames", "riders", "shaderpacks" };
	char path[MAX_PATH];
	size_t i;

	if (g_log)
		return;
	plat_seconds();
	compat_from_utf8(g_dir, MAX_PATH, plat_data_dir());
	for (i = 0; i < sizeof subdirs / sizeof subdirs[0]; i++) {
		plat_data_path(path, sizeof path, subdirs[i]);
		plat_mkdirs(path);
	}
	if (plat_data_path(path, sizeof path, "mxs-shaders.log"))
		g_log = fopen(path, "w");
}

static void log_v(const char *fmt, va_list ap)
{
	if (!g_log)
		return;
	vfprintf(g_log, fmt, ap);
	fputc('\n', g_log);
	fflush(g_log);
}

void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_v(fmt, ap);
	va_end(ap);
}

void plat_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_v(fmt, ap);
	va_end(ap);
}

/* The shared code builds paths with '\', which the file functions in win_compat_linux.c turn into '/'. */
void log_path(wchar_t *path, const wchar_t *name)
{
	swprintf_s(path, MAX_PATH, L"%s/%s", g_dir, name);
}

FILE *log_open(const wchar_t *subdir, const char *name)
{
	wchar_t path[MAX_PATH];

	swprintf_s(path, MAX_PATH, L"%s/%s/%S", g_dir, subdir, name);
	return _wfopen(path, L"wb");
}
