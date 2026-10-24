/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tree.h"

#define MAX_DEPTH 256

static zt_node *node_new(zt_tree *t, const char *name, size_t len, int is_dir)
{
	zt_node *n = calloc(1, sizeof *n);

	if (!n) return NULL;
	n->name = malloc(len + 1);
	if (!n->name) { free(n); return NULL; }
	memcpy(n->name, name, len);
	n->name[len] = '\0';
	n->is_dir = (unsigned char)is_dir;
	n->seq = (uint32_t)t->seq++;
	return n;
}

static void node_free(zt_node *n)
{
	if (!n) return;
	for (uint32_t i = 0; i < n->nkids; i++) node_free(n->kids[i]);
	free(n->kids);
	free(n->name);
	free(n);
}

static int add_kid(zt_node *d, zt_node *k)
{
	if (d->nkids == d->capkids) {
		uint32_t nc = d->capkids ? d->capkids * 2 : 4;
		zt_node **nk = realloc(d->kids, nc * sizeof *nk);

		if (!nk) return -1;
		d->kids = nk;
		d->capkids = nc;
	}
	d->kids[d->nkids++] = k;
	return 0;
}

static int cmp_kid(const void *a, const void *b)
{
	const zt_node *x = *(zt_node *const *)a, *y = *(zt_node *const *)b;
	int c = strcmp(x->name, y->name);

	if (c) return c;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static uint64_t count_nodes(const zt_node *n);

/* Sort every directory's entries by name, and where two entries have the same name keep the
 * first one made (the lower seq). Returns how many entries (counting whole subtrees) were dropped. */
static uint64_t finish_dir(zt_node *d)
{
	uint64_t dropped = 0;
	uint32_t w = 0;

	if (d->nkids > 1) qsort(d->kids, d->nkids, sizeof *d->kids, cmp_kid);
	for (uint32_t i = 0; i < d->nkids; i++) {
		zt_node *k = d->kids[i];

		if (w > 0 && !strcmp(d->kids[w - 1]->name, k->name)) {
			dropped += count_nodes(k);
			node_free(k);
			continue;
		}
		d->kids[w++] = k;
	}
	d->nkids = w;
	for (uint32_t i = 0; i < d->nkids; i++)
		if (d->kids[i]->is_dir) dropped += finish_dir(d->kids[i]);
	return dropped;
}

/* files in a subtree (what "hidden" counts: archive entries, not directories) */
static uint64_t count_nodes(const zt_node *n)
{
	uint64_t c = 0;

	if (!n->is_dir) return 1;
	for (uint32_t i = 0; i < n->nkids; i++) c += count_nodes(n->kids[i]);
	return c;
}

static void count_under(const zt_node *n, uint64_t *files, uint64_t *dirs)
{
	if (!n->is_dir) { (*files)++; return; }
	(*dirs)++;
	for (uint32_t i = 0; i < n->nkids; i++) count_under(n->kids[i], files, dirs);
}

/* Builds the tree from the index. The index is sorted by name, so everything below one
 * directory is one contiguous run: keep the directory chain of the previous name, reuse what
 * the next name shares with it, and make nodes for the rest. */
static int build(zt_tree *t, char *err, size_t errlen)
{
	zf_archive *a = &t->ar;
	zt_node *stack[MAX_DEPTH];
	size_t depth = 0;

	t->top = node_new(t, "", 0, 1);
	if (!t->top) { snprintf(err, errlen, "out of memory"); return -1; }
	t->top->mode = 0555;
	t->top->mtime = t->archive_mtime;
	for (uint64_t i = 0; i < a->nfiles; i++) {
		zf_rec r;
		const char *comp[MAX_DEPTH];
		size_t clen[MAX_DEPTH], nc = 0, j, d;
		const char *p, *end;
		zt_node *parent, *f;
		int bad = 0;

		zf_rec_get(a, i, &r);
		p = r.name;
		end = r.name + r.name_len;
		while (p <= end) {
			const char *e = memchr(p, '/', (size_t)(end - p));
			size_t l = e ? (size_t)(e - p) : (size_t)(end - p);

			if (nc == MAX_DEPTH || l == 0 || memchr(p, '\0', l) || (l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.')) { bad = 1; break; }
			comp[nc] = p; clen[nc] = l; nc++;
			if (!e) break;
			p = e + 1;
		}
		if (bad || nc == 0) { t->hidden++; continue; }
		/* the directory chain: nc-1 components, the last one is the file's name */
		for (j = 0; j < depth && j + 1 < nc; j++)
			if (strlen(stack[j]->name) != clen[j] || memcmp(stack[j]->name, comp[j], clen[j])) break;
		depth = j;
		for (d = j; d + 1 < nc; d++) {
			zt_node *dn = node_new(t, comp[d], clen[d], 1);

			parent = d ? stack[d - 1] : t->top;
			if (!dn || add_kid(parent, dn)) { node_free(dn); snprintf(err, errlen, "out of memory"); return -1; }
			dn->mode = 0555;
			dn->mtime = t->archive_mtime;
			stack[d] = dn;
		}
		depth = nc - 1;
		parent = depth ? stack[depth - 1] : t->top;
		f = node_new(t, comp[nc - 1], clen[nc - 1], 0);
		if (!f || add_kid(parent, f)) { node_free(f); snprintf(err, errlen, "out of memory"); return -1; }
		f->data_off = r.data_off;
		f->size = r.size;
		f->mtime = r.mtime;
		f->mode = r.mode & 0777;
	}
	t->hidden += finish_dir(t->top);
	return 0;
}

static const zt_node *child(const zt_node *d, const char *name, size_t len)
{
	uint32_t lo = 0, hi = d->nkids;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const char *kn = d->kids[mid]->name;
		int c = strncmp(kn, name, len);

		if (c == 0) c = kn[len] ? 1 : 0;        /* kn longer than name sorts after it */
		if (c == 0) return d->kids[mid];
		if (c < 0) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

const zt_node *zt_lookup(const zt_tree *t, const char *path)
{
	const zt_node *n = t->top;

	while (*path == '/') path++;
	while (*path) {
		const char *e = strchr(path, '/');
		size_t l = e ? (size_t)(e - path) : strlen(path);

		if (!n->is_dir) return NULL;
		n = child(n, path, l);
		if (!n) return NULL;
		path += l;
		while (*path == '/') path++;
	}
	return n;
}

int zt_open(zt_tree *t, const char *archive, size_t cache_mib, unsigned threads, char *err, size_t errlen)
{
	size_t nslots;

	memset(t, 0, sizeof *t);
	if (zf_open(&t->ar, archive)) { snprintf(err, errlen, "%s", t->ar.err); zf_close(&t->ar); return -1; }
	t->archive_mtime = t->ar.mtime;
	if (build(t, err, errlen)) { zt_close(t); return -1; }
	count_under(t->top, &t->nfiles, &t->ndirs);
	if (threads < 1) threads = 1;
	if (cache_mib == ZT_CACHE_AUTO) nslots = threads > ZT_AUTO_MIN_SLOTS ? threads : ZT_AUTO_MIN_SLOTS;
	else nslots = (size_t)(((uint64_t)cache_mib << 20) / t->ar.frame_raw);
	t->fc = fc_new(&t->ar, nslots, threads);
	if (!t->fc) { snprintf(err, errlen, "out of memory"); zt_close(t); return -1; }
	return 0;
}

void zt_close(zt_tree *t)
{
	fc_free(t->fc);
	node_free(t->top);
	zf_close(&t->ar);
	memset(t, 0, sizeof *t);
}

ssize_t zt_read(zt_tree *t, const zt_node *n, uint64_t off, void *buf, size_t len)
{
	char err[160];

	if (n->is_dir) return -EISDIR;
	if (off >= n->size) return 0;
	if (len > n->size - off) len = (size_t)(n->size - off);
	if (fc_read(t->fc, n->data_off + off, len, buf, err, sizeof err)) return -EIO;
	return (ssize_t)len;
}
