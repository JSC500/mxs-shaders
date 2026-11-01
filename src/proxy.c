/*
 Loads the real opengl32.dll and fills the table used by the generated stubs.
 Every export we don't hook jumps straight through that table.
*/
#include "common.h"

extern void *real_table[];
extern const char *const real_names[];
extern const int real_count;

HMODULE g_real_gl;

static BOOL load_real(void)
{
	wchar_t path[MAX_PATH];
	int i, missing = 0;

	GetSystemDirectoryW(path, MAX_PATH);
	wcscat_s(path, MAX_PATH, L"\\opengl32.dll");

	g_real_gl = LoadLibraryW(path);
	if (!g_real_gl) {
		log_msg("FATAL: can't load %S (error %lu)", path, GetLastError());
		return FALSE;
	}

	for (i = 0; i < real_count; i++) {
		real_table[i] = (void *)GetProcAddress(g_real_gl, real_names[i]);
		if (!real_table[i])
			missing++;
	}
	log_msg("real opengl32 loaded from %S: %d exports, %d missing", path, real_count, missing);
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;

	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		log_init();
		log_msg("MXS Shaders proxy attached");
		if (!load_real())
			return FALSE;
		hooks_init();
	}
	return TRUE;
}
