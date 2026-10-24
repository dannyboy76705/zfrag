/* SPDX-License-Identifier: MIT */
/* util.h - small helpers shared by the zfrag subcommands. */
#ifndef ZFRAG_UTIL_H
#define ZFRAG_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline void human(char *buf, size_t n, uint64_t b)
{
	if (b >= (1ULL << 30))
		snprintf(buf, n, "%.2f GiB", (double)b / (1ULL << 30));
	else if (b >= (1ULL << 20))
		snprintf(buf, n, "%.2f MiB", (double)b / (1ULL << 20));
	else if (b >= (1ULL << 10))
		snprintf(buf, n, "%.2f KiB", (double)b / (1ULL << 10));
	else
		snprintf(buf, n, "%llu B", (unsigned long long)b);
}

static inline char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	size_t cap = 0, n = 0, r;

	if (!f)
		return NULL;
	for (;;) {
		if (n + 65536 + 1 > cap) {
			char *nb;
			cap = cap ? cap * 2 : 1 << 20;
			nb = realloc(buf, cap);
			if (!nb) { free(buf); fclose(f); return NULL; }
			buf = nb;
		}
		r = fread(buf + n, 1, cap - n - 1, f);
		if (r == 0)
			break;
		n += r;
	}
	fclose(f);
	buf[n] = '\0';
	*len = n;
	return buf;
}

static inline double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


/* Split a slurped list in place into NUL-terminated paths. Returns the number
 * of paths (malloc'd array in *out), or (size_t)-1 on out of memory. */
static inline size_t split_list(char *buf, size_t len, char sep, char ***out)
{
	char **paths = NULL;
	size_t n = 0, cap = 0;
	char *p = buf, *end = buf + len;

	while (p < end) {
		char *e = memchr(p, sep, (size_t)(end - p));
		if (!e)
			e = end;
		*e = '\0';
		if (e > p) {
			if (n == cap) {
				char **np;
				cap = cap ? cap * 2 : 4096;
				np = realloc(paths, cap * sizeof *paths);
				if (!np) {
					free(paths);
					return (size_t)-1;
				}
				paths = np;
			}
			paths[n++] = p;
		}
		p = e + 1;
	}
	*out = paths;
	return n;
}


/* "4300M", "4G", "64K", "1T" or a plain byte count: 0, or -1 if it is not one */
static inline int parse_size(const char *s, uint64_t *out)
{
	char *e;
	unsigned long long v = strtoull(s, &e, 10);

	if (e == s) return -1;
	if (*e == 'K' || *e == 'k') { v <<= 10; e++; }
	else if (*e == 'M' || *e == 'm') { v <<= 20; e++; }
	else if (*e == 'G' || *e == 'g') { v <<= 30; e++; }
	else if (*e == 'T' || *e == 't') { v <<= 40; e++; }
	if (*e || !v) return -1;
	*out = v;
	return 0;
}

#endif /* ZFRAG_UTIL_H */
