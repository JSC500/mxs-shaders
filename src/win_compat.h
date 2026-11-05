/*
 win_compat.h: the part of the Win32 API that hooks.c, post.c, pack.c and ui.c use,
 for the Linux build. common.h includes it instead of windows.h when _WIN32 isn't defined.

 Each function behaves the way the shared code relies on, on top of platform.h, so the
 shared files stay as they are and the Windows build doesn't change at all. A few things
 that only make sense on Windows (the wgl hooks, borderless_apply, the GDI font and the
 input window) are behind #ifdef _WIN32 in those files instead.

 Things to know:
 - Paths stay wchar_t like on Windows. Every function here that takes a path converts it to
   UTF-8 and turns '\' into '/', so L"shaderpacks\\psx" works as it is.
 - swprintf_s follows MSVC's rules, where %s in a wide format is a wide string and %S a narrow
   one. glibc's swprintf reads them the other way round.
 - HWND and HDC are both the X window the game draws in.
 - GetCursorPos already gives window coordinates, and ScreenToClient leaves them alone.
*/
#ifndef MXS_WIN_COMPAT_H
#define MXS_WIN_COMPAT_H

#ifdef _WIN32
#error "win_compat.h is for the Linux build only"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

typedef int BOOL;
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef intptr_t LONG_PTR;
typedef short SHORT;
typedef unsigned short WORD;
typedef unsigned long HWND; /* an X Window */
typedef unsigned long HDC;  /* the X drawable the game swaps */
typedef void *HGLRC;
typedef void *HMODULE;
typedef void *HANDLE;
typedef void (*PROC)(void);
typedef PROC FARPROC;
typedef const char *LPCSTR;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK
#define MAX_PATH 1024

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

typedef struct {
	LONG left, top, right, bottom;
} RECT;

typedef struct {
	LONG x, y;
} POINT;

typedef union {
	struct {
		DWORD LowPart;
		LONG HighPart;
	} u;
	long long QuadPart;
} LARGE_INTEGER;

/* For save_frame's .bmp files. */
typedef struct __attribute__((packed)) {
	WORD bfType;
	DWORD bfSize;
	WORD bfReserved1, bfReserved2;
	DWORD bfOffBits;
} BITMAPFILEHEADER;

typedef struct {
	DWORD biSize;
	LONG biWidth, biHeight;
	WORD biPlanes, biBitCount;
	DWORD biCompression, biSizeImage;
	LONG biXPelsPerMeter, biYPelsPerMeter;
	DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

#define VK_LBUTTON 0x01
#define VK_BACK 0x08
#define VK_ESCAPE 0x1B
#define VK_F8 0x77
#define VK_F9 0x78
#define MAPVK_VK_TO_VSC_EX 4

#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

typedef struct {
	DWORD dwFileAttributes;
	wchar_t cFileName[MAX_PATH];
} WIN32_FIND_DATAW;

/* --- strings --- */
int swprintf_s(wchar_t *buf, size_t size, const wchar_t *fmt, ...);
#define sprintf_s snprintf
int strcpy_s(char *dst, size_t size, const char *src);
int wcscpy_s(wchar_t *dst, size_t size, const wchar_t *src);
int wcscat_s(wchar_t *dst, size_t size, const wchar_t *src);
#define _strdup strdup
#define _stricmp strcasecmp
int _wcsicmp(const wchar_t *a, const wchar_t *b);

/* UTF-8 and wchar_t, done here rather than through the C locale, which the game leaves at "C". */
size_t compat_to_utf8(char *out, size_t size, const wchar_t *in);
size_t compat_from_utf8(wchar_t *out, size_t size, const char *in);
/* Converts to UTF-8 whatever the code page (the panel's font only draws ASCII anyway). Only the
 NUL-terminated form (in_count -1) is supported, which is how the shared code calls it. */
#define CP_ACP 0
#define CP_UTF8 65001
int WideCharToMultiByte(UINT code_page, DWORD flags, const wchar_t *in, int in_count, char *out, int out_size,
			const char *default_char, BOOL *used_default);
/* A wchar_t path as a UTF-8 Linux path, with '\' turned into '/'. */
void compat_path(char *out, size_t size, const wchar_t *in);

/* --- files and ini --- */
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
int _wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode);
DWORD GetFileAttributesW(const wchar_t *path);
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *data);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *data);
BOOL FindClose(HANDLE h);
UINT GetPrivateProfileIntW(const wchar_t *section, const wchar_t *key, int def, const wchar_t *path);
DWORD GetPrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *def, wchar_t *out,
			       DWORD size, const wchar_t *path);
BOOL WritePrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *value,
				const wchar_t *path);
DWORD GetLastError(void);

/* --- time --- */
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);

/* --- window and input, from input_x11.c's per-frame state --- */
SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow(void);
BOOL GetClientRect(HWND w, RECT *r);
#define WindowFromDC(dc) ((HWND)(dc))
BOOL GetCursorPos(POINT *p);
BOOL ScreenToClient(HWND w, POINT *p);
int ShowCursor(BOOL show);
/* key_name in ui.c asks for a scan code and then its name. Here the "scan code" is the
 virtual key itself, which GetKeyNameTextA turns back into the X key's name. */
UINT MapVirtualKeyA(UINT code, UINT type);
int GetKeyNameTextA(LONG lparam, char *name, int size);

/* --- GL lookup: g_real_gl is a placeholder, GetProcAddress finds the real GL function --- */
FARPROC GetProcAddress(HMODULE module, const char *name);

#endif
