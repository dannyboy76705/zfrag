/* SPDX-License-Identifier: MIT */
/* cdcview.c - see cdcview.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cdcview.h"

/* the cdcstore index, version 1 (cdcstore-common.h): little endian, packed */
#pragma pack(push, 1)
typedef struct { uint64_t h0, h1; uint32_t len, id; } cv_chunk;
typedef struct { uint64_t off; uint32_t chunk; } cv_ref;
typedef struct {
	uint64_t path_off, aux_off;
	uint64_t size;
	int64_t  mtime;
	uint64_t ref_start, fh0, fh1;
	uint32_t mode, nref, parent, end;
	uint8_t  type, pad[7];
} cv_entry;
typedef struct {
	char magic[8];
	uint32_t version, avg, whole, bucket, next_id, pad;
	uint64_t nch, nen, nrefs, nstr;
	uint64_t off_ch, off_en, off_refs, off_str;
	uint64_t total, checksum;
} cv_hdr;
#pragma pack(pop)
enum { CV_DEL = 0, CV_FILE = 1, CV_DIR = 2, CV_LINK = 3 };

struct cv_repo {
	char base[PATH_MAX];            /* "" or "/folder" */
	uint32_t bucket;
	cv_chunk *ch;   uint64_t nch;
	cv_entry *en;   uint64_t nen;
	cv_ref *refs;   uint64_t nrefs;
	char *str;      uint64_t nstr;
	const zt_node **cnode;          /* per chunk: its file in the archive, found the first time it is read */
	uint64_t nfiles;
};

static int key_of(unsigned char c) { return c == 0 ? 0 : c == '/' ? 1 : c + 2; }
static int path_cmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = key_of((unsigned char)*a), y = key_of((unsigned char)*b);
		if (x != y) return x < y ? -1 : 1;
		if (!*a) return 0;
	}
}

static const char *ent_path(const cv_repo *c, const cv_entry *e) { return c->str + e->path_off; }

void cv_close(cv_repo *c)
{
	if (!c) return;
	free(c->ch); free(c->en); free(c->refs); free(c->str); free(c->cnode);
	free(c);
}

const char *cv_base(const cv_repo *c) { return c->base; }
uint64_t cv_nfiles(const cv_repo *c) { return c->nfiles; }
uint64_t cv_nchunks(const cv_repo *c) { return c->nch; }

static const zt_node *find_idx(zt_tree *t, char *base, size_t baselen)
{
	const zt_node *n = zt_lookup(t, "/cdcstore.idx");

	base[0] = 0;
	if (n && !n->is_dir) return n;
	for (uint32_t i = 0; i < t->top->nkids; i++) {
		const zt_node *d = t->top->kids[i];
		char p[PATH_MAX];

		if (!d->is_dir) continue;
		snprintf(p, sizeof p, "/%s/cdcstore.idx", d->name);
		n = zt_lookup(t, p);
		if (n && !n->is_dir) { snprintf(base, baselen, "/%s", d->name); return n; }
	}
	return NULL;
}

int cv_open(cv_repo **out, zt_tree *t, char *err, size_t errlen)
{
	char base[PATH_MAX];
	const zt_node *in = find_idx(t, base, sizeof base);
	cv_repo *c;
	uint8_t *buf;
	cv_hdr h;
	uint64_t len;

	*out = NULL;
	if (!in) return 0;
	len = in->size;
	if (len < sizeof h || len > (1ull << 32)) { snprintf(err, errlen, "%s/cdcstore.idx has a wrong size (%llu)", base, (unsigned long long)len); return -1; }
	buf = malloc(len);
	if (!buf) { snprintf(err, errlen, "out of memory reading cdcstore.idx"); return -1; }
	for (uint64_t got = 0; got < len;) {
		ssize_t r = zt_read(t, in, got, buf + got, (size_t)(len - got > (1u << 24) ? (1u << 24) : len - got));
		if (r <= 0) { snprintf(err, errlen, "cannot read %s/cdcstore.idx from the archive", base); free(buf); return -1; }
		got += (uint64_t)r;
	}
	memcpy(&h, buf, sizeof h);
	if (memcmp(h.magic, "CDCSTOR1", 8) || h.version != 1) {
		snprintf(err, errlen, "%s/cdcstore.idx is not a cdcstore index (or a newer version)", base);
		free(buf);
		return -1;
	}
	if (h.bucket == 0 || h.nch > len || h.nen > len || h.nrefs > len || h.nstr > len ||
	    h.off_ch > len || h.off_en > len || h.off_refs > len || h.off_str > len ||
	    h.off_ch + h.nch * sizeof(cv_chunk) > len || h.off_en + h.nen * sizeof(cv_entry) > len ||
	    h.off_refs + h.nrefs * sizeof(cv_ref) > len || h.off_str + h.nstr > len) {
		snprintf(err, errlen, "%s/cdcstore.idx is damaged (sizes do not add up)", base);
		free(buf);
		return -1;
	}
	c = calloc(1, sizeof *c);
	if (!c) { free(buf); snprintf(err, errlen, "out of memory"); return -1; }
	snprintf(c->base, sizeof c->base, "%s", base);
	c->bucket = h.bucket; c->nch = h.nch; c->nen = h.nen; c->nrefs = h.nrefs; c->nstr = h.nstr;
	c->ch = malloc((h.nch + 1) * sizeof *c->ch);
	c->en = malloc((h.nen + 1) * sizeof *c->en);
	c->refs = malloc((h.nrefs + 1) * sizeof *c->refs);
	c->str = malloc(h.nstr + 1);
	c->cnode = calloc(h.nch + 1, sizeof *c->cnode);
	if (!c->ch || !c->en || !c->refs || !c->str || !c->cnode) { snprintf(err, errlen, "out of memory"); cv_close(c); free(buf); return -1; }
	memcpy(c->ch, buf + h.off_ch, h.nch * sizeof *c->ch);
	memcpy(c->en, buf + h.off_en, h.nen * sizeof *c->en);
	memcpy(c->refs, buf + h.off_refs, h.nrefs * sizeof *c->refs);
	memcpy(c->str, buf + h.off_str, h.nstr);
	c->str[h.nstr] = 0;
	free(buf);
	/* every offset the entries hold must stay inside the tables, so a damaged index cannot send a read astray */
	for (uint64_t i = 0; i < c->nen; i++) {
		const cv_entry *e = &c->en[i];

		if (e->path_off >= c->nstr || (e->type == CV_LINK && e->aux_off >= c->nstr) ||
		    (e->type == CV_FILE && (e->ref_start > c->nrefs || e->nref > c->nrefs - e->ref_start)) ||
		    (e->type == CV_DIR && (e->end > c->nen)) || e->type > CV_LINK) {
			snprintf(err, errlen, "%s/cdcstore.idx is damaged (entry %llu)", base, (unsigned long long)i);
			cv_close(c);
			return -1;
		}
		if (e->type == CV_FILE) c->nfiles++;
	}
	for (uint64_t i = 0; i < c->nrefs; i++)
		if (c->refs[i].chunk >= c->nch) { snprintf(err, errlen, "%s/cdcstore.idx is damaged (chunk number)", base); cv_close(c); return -1; }
	*out = c;
	return 1;
}

static long find_entry(const cv_repo *c, const char *path)
{
	uint64_t lo = 0, hi = c->nen;

	while (lo < hi) {
		uint64_t mid = (lo + hi) / 2;
		int r = path_cmp(ent_path(c, &c->en[mid]), path);

		if (r == 0) return c->en[mid].type == CV_DEL ? -1 : (long)mid;
		if (r < 0) lo = mid + 1; else hi = mid;
	}
	return -1;
}

static void info_of(const cv_repo *c, long i, cv_info *out)
{
	const cv_entry *e = &c->en[i];

	memset(out, 0, sizeof *out);
	out->index = i;
	out->is_dir = e->type == CV_DIR;
	out->is_link = e->type == CV_LINK;
	out->size = out->is_link ? strlen(c->str + e->aux_off) : e->size;
	out->mtime = e->mtime;
	out->mode = e->mode & 07777;
	out->target = out->is_link ? c->str + e->aux_off : NULL;
}

int cv_stat(const cv_repo *c, const char *path, cv_info *out)
{
	long i;

	while (*path == '/') path++;
	if (!*path) {
		memset(out, 0, sizeof *out);
		out->is_dir = 1; out->mode = 0555; out->index = -2;
		return 0;
	}
	i = find_entry(c, path);
	if (i < 0) return -ENOENT;
	info_of(c, i, out);
	return 0;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

int cv_readdir(const cv_repo *c, const char *path, int (*fn)(void *, const char *, const cv_info *), void *arg)
{
	uint64_t i, stop;
	cv_info in;

	while (*path == '/') path++;
	if (!*path) { i = 0; stop = c->nen; }
	else {
		long d = find_entry(c, path);

		if (d < 0) return -ENOENT;
		if (c->en[d].type != CV_DIR) return -ENOTDIR;
		i = (uint64_t)d + 1; stop = c->en[d].end;
	}
	while (i < stop && i < c->nen) {
		const cv_entry *e = &c->en[i];

		info_of(c, (long)i, &in);
		if (fn(arg, base_name(ent_path(c, e)), &in)) break;
		if (e->end <= i) break;                                 /* a damaged index must not loop */
		i = e->end;
	}
	return 0;
}

static const zt_node *chunk_node(cv_repo *c, zt_tree *t, uint32_t row)
{
	const zt_node *n = __atomic_load_n(&c->cnode[row], __ATOMIC_ACQUIRE);

	if (!n) {
		char p[PATH_MAX + 64];
		uint32_t id = c->ch[row].id;

		snprintf(p, sizeof p, "%s/chunks/b%05u/c%09u", c->base, id / c->bucket, id);
		n = zt_lookup(t, p);
		if (n && (n->is_dir || n->size != c->ch[row].len)) n = NULL;
		if (n) __atomic_store_n(&c->cnode[row], n, __ATOMIC_RELEASE);
	}
	return n;
}

ssize_t cv_read(cv_repo *c, zt_tree *t, long index, uint64_t off, void *buf, size_t len)
{
	const cv_entry *e;
	const cv_ref *rf;
	uint64_t lo, hi;
	size_t done = 0;

	if (index < 0 || (uint64_t)index >= c->nen) return -EIO;
	e = &c->en[index];
	if (e->type != CV_FILE) return -EINVAL;
	if (off >= e->size) return 0;
	if (len > e->size - off) len = (size_t)(e->size - off);
	rf = c->refs + e->ref_start;
	lo = 0; hi = e->nref;
	while (hi - lo > 1) { uint64_t mid = (lo + hi) / 2; if (rf[mid].off <= off) lo = mid; else hi = mid; }
	for (uint64_t k = lo; k < e->nref && done < len; k++) {
		uint32_t row = rf[k].chunk;
		uint64_t cstart = rf[k].off, clen = c->ch[row].len, from = off + done - cstart;
		size_t n;
		const zt_node *cn;
		ssize_t r;

		if (from >= clen) return -EIO;
		n = (size_t)(clen - from);
		if (n > len - done) n = len - done;
		cn = chunk_node(c, t, row);
		if (!cn) return -EIO;                           /* the chunk file is not in the archive */
		r = zt_read(t, cn, from, (uint8_t *)buf + done, n);
		if (r < 0) return r;
		if ((size_t)r != n) return -EIO;
		done += n;
	}
	return (ssize_t)done;
}
