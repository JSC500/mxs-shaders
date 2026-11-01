/*
 Shader packs live in %LOCALAPPDATA%\mxs-shaders\shaderpacks\<name>.
 A stage file is named after its role, e.g. terrain.fsh. The default pack is built
 into the DLL and any pack only holds what it changes: a file it doesn't have comes
 from the default pack, and one neither has means the game's own shader is used.
*/
#include "common.h"
#include <stdlib.h>
#include <string.h>

static wchar_t g_pack_dir[MAX_PATH];

static char *read_file(const wchar_t *path)
{
	FILE *f = _wfopen(path, L"rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n + 1);
	if (buf) {
		n = (long)fread(buf, 1, n, f);
		buf[n] = 0;
	}
	fclose(f);
	return buf;
}

static char *read_pack_file(const char *name)
{
	wchar_t path[MAX_PATH];
	char *text;
	int i;

	swprintf_s(path, MAX_PATH, L"%s\\%S", g_pack_dir, name);
	text = read_file(path);
	if (text)
		return text;
	for (i = 0; embedded_pack[i].name; i++)
		if (!strcmp(embedded_pack[i].name, name))
			return _strdup(embedded_pack[i].text);
	return NULL;
}

void pack_init(const wchar_t *name)
{
	wchar_t base[MAX_PATH];

	g_pack_dir[0] = 0;
	if (!name[0])
		return;
	log_path(base, L"shaderpacks");
	swprintf_s(g_pack_dir, MAX_PATH, L"%s\\%s", base, name);
	log_msg("shader pack: %S", g_pack_dir);
	if (wcscmp(name, L"default") && GetFileAttributesW(g_pack_dir) == INVALID_FILE_ATTRIBUTES)
		log_msg("shader pack folder not found, using the default pack");
}

/* Expands one level of #include "file" lines, which is all packs need. */
char *pack_load(const char *file)
{
	static const char directive[] = "#include \"";
	char *src, *out, *inc, *line, *next;
	size_t cap, used = 0;

	if (!g_pack_dir[0])
		return NULL;
	src = read_pack_file(file);
	if (!src)
		return NULL;

	cap = strlen(src) + 1;
	out = malloc(cap);
	for (line = src; *line; line = next) {
		size_t len;

		next = strchr(line, '\n');
		next = next ? next + 1 : line + strlen(line);
		len = next - line;

		inc = NULL;
		if (!strncmp(line, directive, sizeof directive - 1)) {
			char name[MAX_PATH];
			const char *end = strchr(line + sizeof directive - 1, '"');

			if (end && end < next && (size_t)(end - line) < sizeof name) {
				size_t n = end - line - (sizeof directive - 1);

				memcpy(name, line + sizeof directive - 1, n);
				name[n] = 0;
				inc = read_pack_file(name);
				if (!inc)
					log_msg("pack: %s includes missing file %s", file, name);
			}
		}

		cap += (inc ? strlen(inc) : len) + 1;
		out = realloc(out, cap);
		if (inc) {
			len = strlen(inc);
			memcpy(out + used, inc, len);
			used += len;
			out[used++] = '\n';
			free(inc);
		} else {
			memcpy(out + used, line, len);
			used += len;
		}
	}
	out[used] = 0;
	free(src);
	return out;
}
