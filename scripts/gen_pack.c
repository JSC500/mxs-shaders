/*
 Bakes a shader pack into a C file, like gen_pack.ps1 does for the Windows build:
	gen_pack <pack folder> <out.c>
 Files are sorted by name without case, as PowerShell's Sort-Object does.
*/
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int by_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
	char *names[512], path[4096];
	int n = 0, i, c;
	struct dirent *e;
	DIR *d;
	FILE *out, *in;

	if (argc != 3 || !(d = opendir(argv[1]))) {
		fprintf(stderr, "usage: gen_pack <pack folder> <out.c>\n");
		return 1;
	}
	while ((e = readdir(d)) && n < 512) {
		struct stat st;

		snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
		if (e->d_name[0] != '.' && !stat(path, &st) && S_ISREG(st.st_mode))
			names[n++] = strdup(e->d_name);
	}
	closedir(d);
	if (!n) {
		fprintf(stderr, "gen_pack: no files in %s\n", argv[1]);
		return 1;
	}
	qsort(names, (size_t)n, sizeof names[0], by_name);
	if (!(out = fopen(argv[2], "w")))
		return 1;
	fprintf(out, "#include \"common.h\"\n");
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
		if (!(in = fopen(path, "rb")))
			return 1;
		fprintf(out, "static const unsigned char file_%d[] = {", i);
		while ((c = fgetc(in)) != EOF)
			fprintf(out, "%d,", c);
		fprintf(out, "0};\n");
		fclose(in);
	}
	fprintf(out, "const embedded_file embedded_pack[] = {\n");
	for (i = 0; i < n; i++)
		fprintf(out, "\t{ \"%s\", (const char *)file_%d },\n", names[i], i);
	fprintf(out, "\t{ 0, 0 }\n};\n");
	return fclose(out) != 0;
}
