/*
 platform_linux.c: paths, files, ini, time and GL lookup for the Linux build.
*/
#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- time --- */

static int64_t g_t0;

int64_t plat_ticks(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int64_t plat_tick_freq(void)
{
	return 1000000000;
}

double plat_seconds(void)
{
	if (!g_t0)
		g_t0 = plat_ticks();
	return (plat_ticks() - g_t0) / 1e9;
}

/* --- paths --- */

static char g_data[1024];

int plat_mkdirs(const char *path)
{
	char tmp[1024], *p;

	if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
		return 0;
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) && errno != EEXIST)
				return 0;
			*p = '/';
		}
	}
	return !mkdir(tmp, 0755) || errno == EEXIST;
}

const char *plat_data_dir(void)
{
	const char *xdg, *home;
	struct passwd *pw;

	if (g_data[0])
		return g_data;
	xdg = getenv("XDG_DATA_HOME");
	home = getenv("HOME");
	if ((!home || !*home) && (pw = getpwuid(getuid())))
		home = pw->pw_dir;
	/* The XDG spec says a relative XDG_DATA_HOME is to be ignored. */
	if (xdg && xdg[0] == '/')
		snprintf(g_data, sizeof g_data, "%s/mxs-shaders", xdg);
	else if (home && *home)
		snprintf(g_data, sizeof g_data, "%s/.local/share/mxs-shaders", home);
	else
		snprintf(g_data, sizeof g_data, "/tmp/mxs-shaders-%u", (unsigned)getuid());
	plat_mkdirs(g_data);
	return g_data;
}

int plat_data_path(char *out, size_t size, const char *rel)
{
	return snprintf(out, size, "%s/%s", plat_data_dir(), rel) < (int)size;
}

int plat_is_file(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISREG(st.st_mode);
}

int plat_is_dir(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_list_dir(const char *path, plat_dir_cb cb, void *user)
{
	DIR *d = opendir(path);
	struct dirent *e;
	char full[1280];
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(full, sizeof full, "%s/%s", path, e->d_name);
		cb(e->d_name, plat_is_dir(full), user);
		n++;
	}
	closedir(d);
	return n;
}

/* --- GL lookup --- */

void *plat_gl_proc(const char *name)
{
	static void *(*gpa)(const unsigned char *);
	void *p;

	/* RTLD_NEXT skips this library, so the mod gets the real function, never its own hook. */
	if (!gpa)
		gpa = (void *(*)(const unsigned char *))dlsym(RTLD_NEXT, "glXGetProcAddressARB");
	p = dlsym(RTLD_NEXT, name);
	if (!p && gpa)
		p = gpa((const unsigned char *)name);
	return p;
}

/* --- ini ---
 The file is read whole on every call, like the Windows functions do for a file that isn't cached.
 The mod reads settings at start and writes one value when a setting changes, so this is cheap.
*/

static void utf8_put(char **o, unsigned c)
{
	if (c < 0x80) {
		*(*o)++ = (char)c;
	} else if (c < 0x800) {
		*(*o)++ = (char)(0xc0 | c >> 6);
		*(*o)++ = (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		*(*o)++ = (char)(0xe0 | c >> 12);
		*(*o)++ = (char)(0x80 | (c >> 6 & 0x3f));
		*(*o)++ = (char)(0x80 | (c & 0x3f));
	} else {
		*(*o)++ = (char)(0xf0 | c >> 18);
		*(*o)++ = (char)(0x80 | (c >> 12 & 0x3f));
		*(*o)++ = (char)(0x80 | (c >> 6 & 0x3f));
		*(*o)++ = (char)(0x80 | (c & 0x3f));
	}
}

/* Returns the file as UTF-8 text, or NULL when it doesn't exist. */
static char *ini_load(const char *file)
{
	FILE *f = fopen(file, "rb");
	long n;
	unsigned char *raw;
	char *txt, *o;
	long i;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || !(raw = malloc((size_t)n + 1))) {
		fclose(f);
		return NULL;
	}
	n = (long)fread(raw, 1, (size_t)n, f);
	fclose(f);
	raw[n] = 0;
	if (n >= 2 && raw[0] == 0xff && raw[1] == 0xfe) {
		/* UTF-16LE, as WritePrivateProfileStringW writes when the file already had this BOM. */
		txt = o = malloc((size_t)n * 2 + 1);
		for (i = 2; i + 1 < n; i += 2) {
			unsigned c = raw[i] | raw[i + 1] << 8;

			if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
				unsigned lo = raw[i + 2] | raw[i + 3] << 8;

				if (lo >= 0xdc00 && lo < 0xe000) {
					c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
					i += 2;
				}
			}
			utf8_put(&o, c);
		}
		*o = 0;
		free(raw);
		return txt;
	}
	if (n >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf)
		memmove(raw, raw + 3, (size_t)n - 2);
	return (char *)raw;
}

struct span {
	const char *p;
	size_t n;
};

static struct span trim(const char *p, size_t n)
{
	struct span s;

	while (n && (*p == ' ' || *p == '\t' || *p == '\r'))
		p++, n--;
	while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
		n--;
	s.p = p;
	s.n = n;
	return s;
}

static int span_eq(struct span s, const char *name)
{
	return strlen(name) == s.n && !strncasecmp(s.p, name, s.n);
}

/* A parsed line: kind 0 other, 1 section header, 2 key=value. */
struct line {
	const char *start;
	size_t len; /* without the newline */
	int kind;
	struct span name, value;
};

static void parse_line(struct line *l)
{
	struct span t = trim(l->start, l->len);
	const char *eq;

	l->kind = 0;
	if (!t.n || t.p[0] == ';' || t.p[0] == '#')
		return;
	if (t.p[0] == '[') {
		const char *end = memchr(t.p, ']', t.n);

		if (end) {
			l->kind = 1;
			l->name = trim(t.p + 1, (size_t)(end - t.p - 1));
		}
		return;
	}
	eq = memchr(t.p, '=', t.n);
	if (!eq)
		return;
	l->kind = 2;
	l->name = trim(t.p, (size_t)(eq - t.p));
	l->value = trim(eq + 1, t.n - (size_t)(eq - t.p) - 1);
	if (l->value.n >= 2 && (l->value.p[0] == '"' || l->value.p[0] == '\'') &&
	    l->value.p[l->value.n - 1] == l->value.p[0]) {
		l->value.p++;
		l->value.n -= 2;
	}
}

/* Splits text into lines. The caller frees the array. */
static struct line *split_lines(const char *txt, int *count)
{
	int cap = 64, n = 0;
	struct line *v = malloc(sizeof *v * (size_t)cap);
	const char *p = txt;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		if (n == cap)
			v = realloc(v, sizeof *v * (size_t)(cap *= 2));
		v[n].start = p;
		v[n].len = len;
		parse_line(&v[n]);
		n++;
		p += len + (nl ? 1 : 0);
	}
	*count = n;
	return v;
}

/* Finds the key's line, or -1. */
static int find_key(struct line *v, int n, const char *section, const char *key)
{
	int i, in = 0;

	for (i = 0; i < n; i++) {
		if (v[i].kind == 1)
			in = span_eq(v[i].name, section);
		else if (in && v[i].kind == 2 && span_eq(v[i].name, key))
			return i;
	}
	return -1;
}

size_t ini_get_str(const char *file, const char *section, const char *key, const char *def, char *out,
		   size_t size)
{
	char *txt = ini_load(file);
	struct line *v;
	int n, i;
	const char *src = def ? def : "";
	size_t len = strlen(src);

	if (txt) {
		v = split_lines(txt, &n);
		i = find_key(v, n, section, key);
		if (i >= 0) {
			src = v[i].value.p;
			len = v[i].value.n;
		}
		if (size) {
			len = MIN(len, size - 1);
			memcpy(out, src, len);
			out[len] = 0;
		}
		free(v);
		free(txt);
		return size ? len : 0;
	}
	if (size) {
		len = MIN(len, size - 1);
		memcpy(out, src, len);
		out[len] = 0;
	}
	return size ? len : 0;
}

int ini_get_int(const char *file, const char *section, const char *key, int def)
{
	char buf[64], probe[2];

	/* A missing key gives def, a present one is parsed like atoi (text that isn't a number gives 0). */
	if (ini_get_str(file, section, key, "\x01", probe, sizeof probe) == 1 && probe[0] == 1)
		return def;
	ini_get_str(file, section, key, "", buf, sizeof buf);
	return (int)strtol(buf, NULL, 10);
}

static int write_all(const char *file, const char *data, size_t n)
{
	char tmp[1200];
	FILE *f;
	int ok;

	snprintf(tmp, sizeof tmp, "%s.tmp%d", file, (int)getpid());
	f = fopen(tmp, "wb");
	if (!f)
		return 0;
	ok = fwrite(data, 1, n, f) == n;
	ok &= !fclose(f);
	if (ok)
		ok = !rename(tmp, file);
	if (!ok)
		unlink(tmp);
	return ok;
}

struct buf {
	char *p;
	size_t n, cap;
};

static void put(struct buf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void puts_(struct buf *b, const char *s)
{
	put(b, s, strlen(s));
}

static void put_kv(struct buf *b, const char *key, const char *value)
{
	puts_(b, key);
	puts_(b, "=");
	puts_(b, value);
	puts_(b, "\n");
}

int ini_set_str(const char *file, const char *section, const char *key, const char *value)
{
	char *txt = ini_load(file);
	struct line *v = NULL;
	struct buf b = {0};
	int n = 0, i, sec = -1, last = -1, hit = -1, in = 0, ok;

	if (txt)
		v = split_lines(txt, &n);
	/* Find the section, its last key line and the key itself. */
	for (i = 0; i < n; i++) {
		if (v[i].kind == 1) {
			in = span_eq(v[i].name, section);
			if (in && sec < 0) {
				sec = i;
				last = i;
			}
			continue;
		}
		if (in && i > sec && sec >= 0) {
			if (v[i].kind == 2) {
				last = i;
				if (key && hit < 0 && span_eq(v[i].name, key))
					hit = i;
			} else if (trim(v[i].start, v[i].len).n) {
				last = i;
			}
		}
	}
	in = 0;
	for (i = 0; i < n; i++) {
		if (v[i].kind == 1)
			in = span_eq(v[i].name, section);
		if (!key && in)
			continue; /* removing the whole section */
		if (i == hit) {
			if (value) {
				put(&b, v[i].name.p, v[i].name.n); /* keep the key's spelling */
				puts_(&b, "=");
				puts_(&b, value);
				puts_(&b, "\n");
			}
			continue;
		}
		put(&b, v[i].start, v[i].len);
		puts_(&b, "\n");
		if (i == last && hit < 0 && key && value)
			put_kv(&b, key, value);
	}
	if (sec < 0 && key && value) {
		if (b.n && b.p[b.n - 1] == '\n' && b.n > 1 && b.p[b.n - 2] != '\n')
			puts_(&b, "\n");
		puts_(&b, "[");
		puts_(&b, section);
		puts_(&b, "]\n");
		put_kv(&b, key, value);
	}
	ok = write_all(file, b.p ? b.p : "", b.n);
	free(b.p);
	free(v);
	free(txt);
	return ok;
}

int ini_set_int(const char *file, const char *section, const char *key, int value)
{
	char buf[24];

	snprintf(buf, sizeof buf, "%d", value);
	return ini_set_str(file, section, key, buf);
}
