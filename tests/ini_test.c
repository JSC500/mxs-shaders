/* Unit test for the ini reader and writer in platform_linux.c. */
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void write_file(const char *p, const void *d, size_t n)
{
	FILE *f = fopen(p, "wb");
	fwrite(d, 1, n, f);
	fclose(f);
}

static char *read_file(const char *p)
{
	static char buf[4096];
	FILE *f = fopen(p, "rb");
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

int main(int argc, char **argv)
{
	const char *f = argc > 1 ? argv[1] : "/tmp/mxs-ini-test.ini";
	char s[64];

	unlink(f);
	/* Missing file: defaults. */
	CHECK(ini_get_int(f, "mxs-shaders", "ui_key", 119) == 119);
	CHECK(ini_get_str(f, "mxs-shaders", "pack", "default", s, sizeof s) == 7 && !strcmp(s, "default"));
	/* Create, add, replace. */
	CHECK(ini_set_int(f, "mxs-shaders", "fog", 10));
	CHECK(ini_set_str(f, "mxs-shaders", "pack", "psx"));
	CHECK(ini_set_int(f, "pack psx", "fog", 40));
	CHECK(ini_set_int(f, "mxs-shaders", "fog", 25));
	CHECK(!strcmp(read_file(f), "[mxs-shaders]\nfog=25\npack=psx\n\n[pack psx]\nfog=40\n"));
	CHECK(ini_get_int(f, "mxs-shaders", "fog", 0) == 25);
	CHECK(ini_get_int(f, "pack psx", "fog", 0) == 40);
	/* Case-insensitive names, kept spelling, negatives, non-numbers. */
	CHECK(ini_get_int(f, "MXS-Shaders", "FOG", 0) == 25);
	CHECK(ini_set_int(f, "MXS-SHADERS", "Fog", -7));
	CHECK(strstr(read_file(f), "fog=-7\n") != NULL);
	CHECK(ini_get_int(f, "mxs-shaders", "fog", 0) == -7);
	CHECK(ini_set_str(f, "mxs-shaders", "weird", "abc"));
	CHECK(ini_get_int(f, "mxs-shaders", "weird", 5) == 0);
	CHECK(ini_set_str(f, "mxs-shaders", "empty", ""));
	CHECK(ini_get_int(f, "mxs-shaders", "empty", 5) == 0);
	/* Delete a key, then a section. */
	CHECK(ini_set_str(f, "mxs-shaders", "weird", NULL));
	CHECK(ini_get_int(f, "mxs-shaders", "weird", 5) == 5);
	CHECK(ini_set_str(f, "pack psx", NULL, NULL));
	CHECK(ini_get_int(f, "pack psx", "fog", 3) == 3);
	CHECK(strstr(read_file(f), "psx]") == NULL);

	/* A file written on Windows: CRLF, comments, spaces, quotes, an empty line inside the section. */
	{
		const char *w = "; comment\r\n[mxs-shaders]\r\n  shadow_size = 4096 \r\npack=\"my pack\"\r\n\r\n[settings]\r\nx=1\r\n";
		write_file(f, w, strlen(w));
	}
	CHECK(ini_get_int(f, "mxs-shaders", "shadow_size", 0) == 4096);
	CHECK(ini_get_str(f, "mxs-shaders", "pack", "", s, sizeof s) && !strcmp(s, "my pack"));
	CHECK(ini_set_int(f, "mxs-shaders", "msaa", 4));
	CHECK(ini_get_int(f, "mxs-shaders", "msaa", 0) == 4);
	CHECK(ini_get_int(f, "settings", "x", 0) == 1);
	CHECK(strstr(read_file(f), "; comment") == read_file(f));
	/* The new key lands in its section, before [settings]. */
	CHECK(strstr(read_file(f), "msaa=4") < strstr(read_file(f), "[settings]"));

	/* UTF-16LE with a BOM, as WritePrivateProfileStringW keeps when the file started that way. */
	{
		const char *a = "[mxs-shaders]\r\nui_key=120\r\n";
		unsigned char w[128];
		size_t i, n = 2;

		w[0] = 0xff, w[1] = 0xfe;
		for (i = 0; a[i]; i++)
			w[n++] = (unsigned char)a[i], w[n++] = 0;
		write_file(f, w, n);
		CHECK(ini_get_int(f, "mxs-shaders", "ui_key", 119) == 120);
	}
	/* UTF-8 BOM. */
	write_file(f, "\xef\xbb\xbf[mxs-shaders]\nui_key=121\n", 28);
	CHECK(ini_get_int(f, "mxs-shaders", "ui_key", 119) == 121);

	unlink(f);
	printf("ini test: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
