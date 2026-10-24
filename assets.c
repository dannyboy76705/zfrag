/* SPDX-License-Identifier: MIT */
/* assets.c - see assets.h. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "assets.h"

typedef struct { char *name; uint64_t size; } item_t;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* digit runs compare by value ("f2" before "f10") */
static int natural_cmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			const char *ea, *eb;
			size_t la, lb;

			while (*a == '0' && isdigit((unsigned char)a[1])) a++;
			while (*b == '0' && isdigit((unsigned char)b[1])) b++;
			ea = a; eb = b;
			while (isdigit((unsigned char)*ea)) ea++;
			while (isdigit((unsigned char)*eb)) eb++;
			la = (size_t)(ea - a); lb = (size_t)(eb - b);
			if (la != lb) return la < lb ? -1 : 1;
			{ int c = strncmp(a, b, la); if (c) return c < 0 ? -1 : 1; }
			a = ea; b = eb;
		} else {
			if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
			a++; b++;
		}
	}
	return *a ? 1 : *b ? -1 : 0;
}
static int cmp_item(const void *x, const void *y) { return natural_cmp(((const item_t *)x)->name, ((const item_t *)y)->name); }

static int in_sorted(char **v, size_t n, const char *p)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(v[mid], p);

		if (!c) return 1;
		if (c < 0) lo = mid + 1; else hi = mid;
	}
	return 0;
}

size_t assets_add(char ***pathsp, char ***aliasp, size_t n, const char *dir, const char *folder,
		  char **pool, size_t *nadded, uint64_t *bytes)
{
	char **paths = *pathsp, **alias, **have = NULL, real[PATH_MAX], fl[PATH_MAX];
	item_t *v = NULL;
	size_t nv = 0, cap = 0, i, total = 0, off;
	DIR *d;
	struct dirent *e;
	struct stat st;
	size_t fln;

	*pool = NULL; *aliasp = NULL; *nadded = 0; *bytes = 0;
	snprintf(fl, sizeof fl, "%s", folder);
	fln = strlen(fl);
	while (fln > 1 && fl[fln - 1] == '/') fl[--fln] = 0;
	if (dir && dir[0] && fl[0] != '/') snprintf(real, sizeof real, "%s/%s", dir, fl);
	else snprintf(real, sizeof real, "%s", fl);
	if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "zfrag pack: --assets %s: %s\n", folder, errno && stat(real, &st) != 0 ? strerror(errno) : "not a folder");
		return (size_t)-1;
	}
	d = opendir(real);
	if (!d) { fprintf(stderr, "zfrag pack: --assets %s: %s\n", folder, strerror(errno)); return (size_t)-1; }
	have = malloc((n + 1) * sizeof *have);
	if (!have) { closedir(d); fprintf(stderr, "zfrag pack: out of memory\n"); return (size_t)-1; }
	memcpy(have, paths, n * sizeof *have);
	qsort(have, n, sizeof *have, cmp_str);
	while ((e = readdir(d))) {
		char f[PATH_MAX + 300], listed[PATH_MAX + 300];

		if (e->d_name[0] == '.') continue;
		snprintf(f, sizeof f, "%s/%s", real, e->d_name);
		if (lstat(f, &st) != 0 || !S_ISREG(st.st_mode)) continue;          /* a link is not a regular file: pack skips those */
		snprintf(listed, sizeof listed, "%s/%s", fl, e->d_name);
		if (in_sorted(have, n, listed)) continue;                       /* the list already has this very file */
		if (nv == cap) {
			size_t nc = cap ? cap * 2 : 32;
			item_t *nvv = realloc(v, nc * sizeof *v);

			if (!nvv) goto oom;
			v = nvv; cap = nc;
		}
		v[nv].name = strdup(e->d_name);
		v[nv].size = (uint64_t)st.st_size;
		if (!v[nv].name) goto oom;
		nv++;
	}
	closedir(d);
	free(have);
	if (!nv) {
		fprintf(stderr, "zfrag pack: warning: --assets %s holds no files to add\n", folder);
		free(v);
		return n;
	}
	qsort(v, nv, sizeof *v, cmp_item);
	for (i = 0; i < nv; i++) total += strlen(fl) + 1 + strlen(v[i].name) + 1 + sizeof ASSETS_NAME + strlen(v[i].name) + 1;
	{
		char *p = malloc(total), **np = realloc(paths, (n + nv) * sizeof *np), **al = calloc(n + nv, sizeof *al);

		if (!p || !np || !al) {
			free(p); free(al);
			if (np) *pathsp = np;
			for (i = 0; i < nv; i++) free(v[i].name);
			free(v);
			fprintf(stderr, "zfrag pack: out of memory while adding assets\n");
			return (size_t)-1;
		}
		paths = np;
		for (off = 0, i = 0; i < nv; i++) {
			int l1 = snprintf(p + off, total - off, "%s/%s", fl, v[i].name);

			paths[n + i] = p + off;
			off += (size_t)l1 + 1;
			l1 = snprintf(p + off, total - off, "%s/%s", ASSETS_NAME, v[i].name);
			al[n + i] = p + off;
			off += (size_t)l1 + 1;
			*bytes += v[i].size;
			free(v[i].name);
		}
		*pool = p;
		alias = al;
	}
	free(v);
	*pathsp = paths;
	*aliasp = alias;
	*nadded = nv;
	return n + nv;
oom:
	closedir(d);
	free(have);
	for (i = 0; i < nv; i++) free(v[i].name);
	free(v);
	fprintf(stderr, "zfrag pack: out of memory while looking at --assets\n");
	return (size_t)-1;
}
