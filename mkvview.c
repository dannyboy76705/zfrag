/* SPDX-License-Identifier: MIT */
/* mkvview.c - see mkvview.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mjpegmount.h"
#include "mkvview.h"

static zt_tree *g_t;

static const zt_node *node_of(const char *path)
{
	return zt_lookup(g_t, path[0] ? path : "/");               /* the set at the top of the archive is "" */
}

static int ar_list(void *ctx, const char *dir, int (*cb)(void *, const char *, int, uint64_t), void *arg)
{
	const zt_node *d = node_of(dir);

	(void)ctx;
	if (!d) return -ENOENT;
	if (!d->is_dir) return -ENOTDIR;
	for (uint32_t i = 0; i < d->nkids; i++) {
		const zt_node *k = d->kids[i];

		if (cb(arg, k->name, k->is_dir, k->is_dir ? 0 : k->size)) break;
	}
	return 0;
}

static int ar_stat(void *ctx, const char *path, int *kind, uint64_t *size, int64_t *mtime)
{
	const zt_node *n = node_of(path);

	(void)ctx;
	if (!n) return -ENOENT;
	*kind = n->is_dir ? 2 : 1;
	*size = n->is_dir ? 0 : n->size;
	*mtime = n->is_dir ? g_t->archive_mtime : n->mtime;
	return 0;
}

static int ar_pread(void *ctx, const char *path, void *buf, size_t n, uint64_t off)
{
	const zt_node *f = node_of(path);
	size_t got = 0;

	(void)ctx;
	if (!f) return -ENOENT;
	if (f->is_dir) return -EISDIR;
	if (off > f->size || n > f->size - off) return -EIO;      /* shorter than when listed */
	while (got < n) {
		ssize_t r = zt_read(g_t, f, off + got, (char *)buf + got, n - got);

		if (r < 0) return (int)r;
		if (r == 0) return -EIO;
		got += (size_t)r;
	}
	return 0;
}

static const mj_source g_archive = { ar_list, ar_stat, ar_pread, NULL };

int mv_open(zt_tree *t, char *describe, size_t dn, char *base, size_t bn)
{
	char dir[PATH_MAX];

	g_t = t;
	dir[0] = 0;
	if (!mjl_probe(&g_archive, "")) {
		int found = 0;

		for (uint32_t i = 0; i < t->top->nkids && !found; i++) {
			if (!t->top->kids[i]->is_dir) continue;
			snprintf(dir, sizeof dir, "/%s", t->top->kids[i]->name);
			found = mjl_probe(&g_archive, dir);
		}
		if (!found) return 0;
	}
	snprintf(base, bn, "%s", dir);
	if (mjl_open(&g_archive, dir, describe, dn)) return -1;
	return 1;
}

void mv_close(void) { mjl_close(); }
size_t mv_nclips(void) { return mjl_nclips(); }
const char *mv_clip_name(size_t i) { return mjl_clip_name(i); }
void *mv_find(const char *path) { return mjl_find(path); }
int mv_ready(void *clip) { return mjl_ready(clip); }
uint64_t mv_size(void *clip) { return mjl_size(clip); }
int64_t mv_mtime(void *clip) { return mjl_mtime(clip); }
ssize_t mv_read(void *clip, void *buf, size_t size, uint64_t off) { return mjl_read(clip, buf, size, off); }
