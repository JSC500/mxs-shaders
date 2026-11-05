/*
 win_compat_linux.c: the Win32 stand-ins declared in win_compat.h, on top of platform.h.
*/
#define _GNU_SOURCE
#include "win_compat.h"
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wctype.h>

/* --- UTF-8 --- */

size_t compat_to_utf8(char *out, size_t size, const wchar_t *in)
{
	size_t n = 0;

	if (!size)
		return 0;
	for (; in && *in; in++) {
		unsigned c = (unsigned)*in;
		char b[4];
		size_t k, len;

		if (c < 0x80) {
			b[0] = (char)c;
			len = 1;
		} else if (c < 0x800) {
			b[0] = (char)(0xc0 | c >> 6);
			b[1] = (char)(0x80 | (c & 0x3f));
			len = 2;
		} else if (c < 0x10000) {
			b[0] = (char)(0xe0 | c >> 12);
			b[1] = (char)(0x80 | (c >> 6 & 0x3f));
			b[2] = (char)(0x80 | (c & 0x3f));
			len = 3;
		} else {
			b[0] = (char)(0xf0 | (c >> 18 & 0x07));
			b[1] = (char)(0x80 | (c >> 12 & 0x3f));
			b[2] = (char)(0x80 | (c >> 6 & 0x3f));
			b[3] = (char)(0x80 | (c & 0x3f));
			len = 4;
		}
		if (n + len >= size)
			break;
		for (k = 0; k < len; k++)
			out[n++] = b[k];
	}
	out[n] = 0;
	return n;
}

size_t compat_from_utf8(wchar_t *out, size_t size, const char *in)
{
	const unsigned char *p = (const unsigned char *)in;
	size_t n = 0;

	if (!size)
		return 0;
	while (p && *p && n + 1 < size) {
		unsigned c = *p++;
		int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

		if (more)
			c &= 0x3f >> more;
		while (more-- && (*p & 0xc0) == 0x80)
			c = c << 6 | (*p++ & 0x3f);
		out[n++] = (wchar_t)c;
	}
	out[n] = 0;
	return n;
}

int WideCharToMultiByte(UINT code_page, DWORD flags, const wchar_t *in, int in_count, char *out, int out_size,
			const char *default_char, BOOL *used_default)
{
	(void)code_page;
	(void)flags;
	(void)in_count;
	(void)default_char;
	if (used_default)
		*used_default = FALSE;
	if (!out || out_size <= 0)
		return 0;
	return (int)compat_to_utf8(out, (size_t)out_size, in) + 1;
}

void compat_path(char *out, size_t size, const wchar_t *in)
{
	char *p;

	compat_to_utf8(out, size, in);
	for (p = out; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
}

/* --- strings --- */

/* MSVC reads %s and %c in a wide format as wide, and %S and %C as narrow. */
int swprintf_s(wchar_t *buf, size_t size, const wchar_t *fmt, ...)
{
	wchar_t conv[512];
	const wchar_t *f = fmt;
	size_t o = 0;
	va_list ap;
	int r;

	while (*f && o + 4 < sizeof conv / sizeof conv[0]) {
		int length = 0; /* 0 none, 'h' or 'l' */

		if (*f != L'%') {
			conv[o++] = *f++;
			continue;
		}
		conv[o++] = *f++;
		if (*f == L'%') {
			conv[o++] = *f++;
			continue;
		}
		while (*f && wcschr(L"-+ #0123456789.*", *f) && o + 4 < sizeof conv / sizeof conv[0])
			conv[o++] = *f++;
		while (*f && wcschr(L"hlLzjtq", *f) && o + 4 < sizeof conv / sizeof conv[0]) {
			length = *f == L'h' || *f == L'l' ? *f : length;
			conv[o++] = *f++;
		}
		if (*f == L's' || *f == L'c') {
			if (length == L'h')
				o--; /* %hs is narrow on both */
			else if (!length)
				conv[o++] = L'l';
			conv[o++] = *f++;
		} else if (*f == L'S' || *f == L'C') {
			if (length == L'l' || length == L'h')
				o--;
			if (length == L'l')
				conv[o++] = L'l';
			conv[o++] = *f == L'S' ? L's' : L'c';
			f++;
		} else if (*f) {
			conv[o++] = *f++;
		}
	}
	conv[o] = 0;
	va_start(ap, fmt);
	r = vswprintf(buf, size, conv, ap);
	va_end(ap);
	if (r < 0 && size)
		buf[size - 1] = 0;
	return r;
}

int strcpy_s(char *dst, size_t size, const char *src)
{
	if (!size)
		return EINVAL;
	snprintf(dst, size, "%s", src);
	return 0;
}

int wcscpy_s(wchar_t *dst, size_t size, const wchar_t *src)
{
	size_t n = wcslen(src);

	if (!size)
		return EINVAL;
	n = MIN(n, size - 1);
	wmemcpy(dst, src, n);
	dst[n] = 0;
	return 0;
}

int wcscat_s(wchar_t *dst, size_t size, const wchar_t *src)
{
	size_t used = wcsnlen(dst, size);

	if (used >= size)
		return EINVAL;
	return wcscpy_s(dst + used, size - used, src);
}

int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
	for (;; a++, b++) {
		wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);

		if (x != y || !x)
			return (int)x - (int)y;
	}
}

/* --- files --- */

FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
	char p[MAX_PATH * 2], m[16];

	compat_path(p, sizeof p, path);
	compat_to_utf8(m, sizeof m, mode);
	return fopen(p, m);
}

int _wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode)
{
	*f = _wfopen(path, mode);
	return *f ? 0 : errno ? errno : ENOENT;
}

DWORD GetFileAttributesW(const wchar_t *path)
{
	char p[MAX_PATH * 2];
	struct stat st;

	compat_path(p, sizeof p, path);
	if (stat(p, &st))
		return INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

typedef struct {
	DIR *dir;
	char base[MAX_PATH * 2];
} find_handle;

static BOOL find_fill(find_handle *h, WIN32_FIND_DATAW *data)
{
	struct dirent *e = readdir(h->dir);
	char full[MAX_PATH * 3];

	if (!e)
		return FALSE;
	snprintf(full, sizeof full, "%s/%s", h->base, e->d_name);
	data->dwFileAttributes = plat_is_dir(full) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	compat_from_utf8(data->cFileName, MAX_PATH, e->d_name);
	return TRUE;
}

/* Only "folder\*" patterns, which is all the shared code uses. */
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *data)
{
	find_handle *h = calloc(1, sizeof *h);
	char *slash;

	if (!h)
		return INVALID_HANDLE_VALUE;
	compat_path(h->base, sizeof h->base, pattern);
	slash = strrchr(h->base, '/');
	if (slash && slash[1] == '*' && !slash[2])
		*slash = 0;
	h->dir = opendir(h->base);
	if (!h->dir || !find_fill(h, data)) {
		if (h->dir)
			closedir(h->dir);
		free(h);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *data)
{
	return find_fill(h, data);
}

BOOL FindClose(HANDLE h)
{
	find_handle *f = h;

	closedir(f->dir);
	free(f);
	return TRUE;
}

DWORD GetLastError(void)
{
	return (DWORD)errno;
}

/* --- ini --- */

UINT GetPrivateProfileIntW(const wchar_t *section, const wchar_t *key, int def, const wchar_t *path)
{
	char p[MAX_PATH * 2], s[256], k[256];

	compat_path(p, sizeof p, path);
	compat_to_utf8(s, sizeof s, section);
	compat_to_utf8(k, sizeof k, key);
	return (UINT)ini_get_int(p, s, k, def);
}

DWORD GetPrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *def, wchar_t *out,
			       DWORD size, const wchar_t *path)
{
	char p[MAX_PATH * 2], s[256], k[256], d[1024], v[2048];

	compat_path(p, sizeof p, path);
	compat_to_utf8(s, sizeof s, section);
	compat_to_utf8(k, sizeof k, key);
	compat_to_utf8(d, sizeof d, def ? def : L"");
	ini_get_str(p, s, k, d, v, sizeof v);
	return (DWORD)compat_from_utf8(out, size, v);
}

BOOL WritePrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *value,
				const wchar_t *path)
{
	char p[MAX_PATH * 2], s[256], k[256], v[2048];

	compat_path(p, sizeof p, path);
	compat_to_utf8(s, sizeof s, section);
	if (key)
		compat_to_utf8(k, sizeof k, key);
	if (value)
		compat_to_utf8(v, sizeof v, value);
	return ini_set_str(p, s, key ? k : NULL, value ? v : NULL);
}

/* --- time --- */

BOOL QueryPerformanceCounter(LARGE_INTEGER *t)
{
	t->QuadPart = plat_ticks();
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
	f->QuadPart = plat_tick_freq();
	return TRUE;
}

/* --- window and input --- */

SHORT GetAsyncKeyState(int vk)
{
	return plat_key_down(vk) ? (SHORT)-32768 : 0; /* the high bit, 0x8000 */
}

HWND GetForegroundWindow(void)
{
	return plat_window_focused() ? (HWND)plat_window() : 0;
}

BOOL GetClientRect(HWND w, RECT *r)
{
	int cw, ch;

	(void)w;
	plat_client_size(&cw, &ch);
	r->left = r->top = 0;
	r->right = cw;
	r->bottom = ch;
	return TRUE;
}

BOOL GetCursorPos(POINT *p)
{
	int x, y, left;

	plat_pointer(&x, &y, &left);
	p->x = x;
	p->y = y;
	return TRUE;
}

BOOL ScreenToClient(HWND w, POINT *p)
{
	(void)w;
	(void)p;
	return TRUE;
}

int ShowCursor(BOOL show)
{
	return plat_show_cursor(show);
}

UINT MapVirtualKeyA(UINT code, UINT type)
{
	(void)type;
	return code & 0xff;
}

int GetKeyNameTextA(LONG lparam, char *name, int size)
{
	plat_key_name((int)(lparam >> 16 & 0xff), name, (size_t)size);
	return (int)strlen(name);
}

/* --- GL --- */

FARPROC GetProcAddress(HMODULE module, const char *name)
{
	(void)module;
	return (FARPROC)plat_gl_proc(name);
}
