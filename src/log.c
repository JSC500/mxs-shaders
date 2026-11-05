#include "common.h"
#include <share.h>
#include <stdarg.h>

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

void log_init(void)
{
	wchar_t path[MAX_PATH], sub[MAX_PATH];

	/* Beside the game's own data. MX Simulator makes that folder, but a fresh install may not have yet. */
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH - 64))
		GetTempPathW(MAX_PATH - 64, g_dir);
	wcscat_s(g_dir, MAX_PATH, L"\\MX Simulator");
	CreateDirectoryW(g_dir, NULL);
	wcscat_s(g_dir, MAX_PATH, L"\\mxs-shaders");
	CreateDirectoryW(g_dir, NULL);

	swprintf_s(sub, MAX_PATH, L"%s\\shaders", g_dir);
	CreateDirectoryW(sub, NULL);
	swprintf_s(sub, MAX_PATH, L"%s\\frames", g_dir);
	CreateDirectoryW(sub, NULL);
	swprintf_s(sub, MAX_PATH, L"%s\\riders", g_dir);
	CreateDirectoryW(sub, NULL);
	swprintf_s(sub, MAX_PATH, L"%s\\shaderpacks", g_dir);
	CreateDirectoryW(sub, NULL);

	swprintf_s(path, MAX_PATH, L"%s\\mxs-shaders.log", g_dir);
	g_log = _wfsopen(path, L"w", _SH_DENYNO);
}

void log_msg(const char *fmt, ...)
{
	va_list ap;

	if (!g_log)
		return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

void log_path(wchar_t *path, const wchar_t *name)
{
	swprintf_s(path, MAX_PATH, L"%s\\%s", g_dir, name);
}

FILE *log_open(const wchar_t *subdir, const char *name)
{
	wchar_t path[MAX_PATH];

	swprintf_s(path, MAX_PATH, L"%s\\%s\\%S", g_dir, subdir, name);
	return _wfsopen(path, L"wb", _SH_DENYNO);
}
