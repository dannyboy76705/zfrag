/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>

#include "archive.h"

static int fail(zf_archive *a, const char *msg)
{
	snprintf(a->err, sizeof a->err, "%s", msg);
	return -1;
}

/* as fail, with a pointer to the usual causes when the archive is in several parts */
static int fail_parts(zf_archive *a, const char *msg)
{
	if (a->nparts > 1) {
		snprintf(a->err, sizeof a->err, "%s (the archive is in %zu parts: one may be missing, damaged or from another archive)", msg, a->nparts);
		return -1;
	}
	return fail(a, msg);
}

static int pread_all(int fd, void *buf, size_t n, uint64_t off)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = pread(fd, p, n, (off_t)off);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; off += (uint64_t)r; n -= (size_t)r;
	}
	return 0;
}

/* ---- reading across the parts of a split archive ---------------------- */

static int pread_part(const zf_archive *a, void *buf, size_t n, uint64_t off)
{
	uint8_t *p = buf;
	size_t k = 0;

	if (off > a->filesize || n > a->filesize - off) return -1;
	while (n) {
		uint64_t in;
		size_t take;

		while (k + 1 < a->nparts && off >= a->parts[k].start + a->parts[k].size) k++;
		in = off - a->parts[k].start;
		take = a->parts[k].size - in < n ? (size_t)(a->parts[k].size - in) : n;
		if (!take || pread_all(a->parts[k].fd, p, take, in)) return -1;
		p += take; off += take; n -= take;
	}
	return 0;
}

void zf_close(zf_archive *a)
{
	for (size_t i = 0; i < a->nparts; i++)
		if (a->parts[i].fd >= 0) close(a->parts[i].fd);
	free(a->parts);
	if (a->dctx) ZSTD_freeDCtx(a->dctx);
	free(a->idx); free(a->cbuf); free(a->rbuf);
	memset(a, 0, sizeof *a);
}

/* ---- finding the parts ------------------------------------------------- */

#define MAX_PARTS 10000

/* "STEM.SUFFIX", the suffix all digits or all lowercase letters (what `split` writes), the stem
 * ending in .tar.zfrag. Returns 1 and the pieces, or 0. */
static int series_name(const char *base, size_t *stemlen, char *cls, size_t *width)
{
	size_t len = strlen(base), sl, zl = strlen(ZF_SUFFIX), w;
	const char *dot = strrchr(base, '.');
	int digits = 1, lower = 1;

	if (!dot || dot == base) return 0;
	w = len - (size_t)(dot + 1 - base);
	if (w == 0 || w > 8) return 0;
	for (size_t i = 0; i < w; i++) {
		char ch = dot[1 + i];
		if (ch < '0' || ch > '9') digits = 0;
		if (ch < 'a' || ch > 'z') lower = 0;
	}
	if (!digits && !lower) return 0;
	sl = (size_t)(dot - base);
	if (sl < zl || strncmp(base + sl - zl, ZF_SUFFIX, zl)) return 0;
	*stemlen = sl;
	*cls = digits ? 'd' : 'a';
	*width = w;
	return 1;
}

/* the entries of dir named STEM.<suffix> of one kind; cls 0 = any kind (then groups are counted) */
typedef struct { char cls; size_t width; } sgroup;

static int scan_dir(const char *dir, const char *stem, size_t stemlen, char cls, size_t width,
		    char ***out, size_t *nout, sgroup *groups, size_t *ngroups)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char **v = NULL;
	size_t n = 0, cap = 0;

	if (ngroups) *ngroups = 0;
	if (!d) return -1;
	while ((e = readdir(d))) {
		char c2 = 0;
		size_t w2, sl2;

		if (!series_name(e->d_name, &sl2, &c2, &w2) || sl2 != stemlen || strncmp(e->d_name, stem, stemlen)) continue;
		if (groups && ngroups) {
			size_t g;
			for (g = 0; g < *ngroups; g++) if (groups[g].cls == c2 && groups[g].width == w2) break;
			if (g == *ngroups && *ngroups < 8) { groups[g].cls = c2; groups[g].width = w2; (*ngroups)++; }
		}
		if (cls && (c2 != cls || w2 != width)) continue;
		if (!cls) continue;
		if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); if (!v) { closedir(d); return -1; } }
		v[n++] = strdup(e->d_name + stemlen + 1);
	}
	closedir(d);
	if (out) { *out = v; *nout = n; } else { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }
	return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int next_suffix(char *s, size_t w, char cls)
{
	for (size_t i = w; i-- > 0; ) {
		if (cls == 'd') { if (s[i] < '9') { s[i]++; return 0; } s[i] = '0'; }
		else { if (s[i] < 'z') { s[i]++; return 0; } s[i] = 'a'; }
	}
	return -1;
}

static void free_names(char **v, size_t n)
{
	for (size_t i = 0; i < n; i++) free(v[i]);
	free(v);
}

/* Works out which files make up the archive `path` names. A plain file is itself. Otherwise the
 * parts of a split archive, in order; every kind of mistake gets its own message. */
static int resolve_parts(const char *path, char ***names, size_t *n, char *outbase, size_t outbaselen, char *err, size_t errlen)
{
	struct stat sb;
	const char *slash = strrchr(path, '/'), *base = slash ? slash + 1 : path;
	char dir[4096], stem[1024], **sfx = NULL, cls = 0;
	size_t dirlen = slash ? (size_t)(slash - path) : 0, stemlen = 0, width = 0, ns = 0;
	sgroup groups[8];
	size_t ng = 0;
	int exists = stat(path, &sb) == 0;

	if (exists && !S_ISREG(sb.st_mode)) { snprintf(err, errlen, "%.150s: not a regular file", path); return -1; }
	if (!exists && errno != ENOENT) { snprintf(err, errlen, "%.300s: %s", path, strerror(errno)); return -1; }
	if (exists && !series_name(base, &stemlen, &cls, &width)) {                 /* an ordinary single file */
		*names = malloc(sizeof **names);
		if (!*names || !((*names)[0] = strdup(path))) { snprintf(err, errlen, "out of memory"); return -1; }
		*n = 1;
		snprintf(outbase, outbaselen, "%s", path);
		return 0;
	}
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(dirlen ? dirlen : 1) : 1, slash ? path : ".");
	if (!exists) {
		/* the name of a split archive that is not a file itself: SET.tar.zfrag, with SET.tar.zfrag.00 ... beside it */
		size_t zl = strlen(ZF_SUFFIX), bl = strlen(base);

		if (bl < zl || strcmp(base + bl - zl, ZF_SUFFIX)) { snprintf(err, errlen, "%.300s: %s", path, strerror(ENOENT)); return -1; }
		stemlen = bl;
		if (stemlen >= sizeof stem) { snprintf(err, errlen, "%.150s: name too long", path); return -1; }
		memcpy(stem, base, stemlen); stem[stemlen] = '\0';
		if (scan_dir(dir, stem, stemlen, 0, 0, NULL, NULL, groups, &ng)) { snprintf(err, errlen, "%.300s: %s", dir, strerror(errno)); return -1; }
		if (ng == 0) { snprintf(err, errlen, "%.150s: no such file, and no parts named %.150s.00, %.150s.01, ... (or .aa, .ab, ...) found beside it", path, base, base); return -1; }
		if (ng > 1) { snprintf(err, errlen, "%.150s: more than one series of parts is present (%.150s.%.150s... and %.150s.%.150s...): name one part to say which",
				       path, base, groups[0].cls == 'd' ? "00" : "aa", base, groups[1].cls == 'd' ? "00" : "aa"); return -1; }
		cls = groups[0].cls; width = groups[0].width;
	} else {
		memcpy(stem, base, stemlen); stem[stemlen] = '\0';
	}
	if (scan_dir(dir, stem, stemlen, cls, width, &sfx, &ns, NULL, NULL)) { snprintf(err, errlen, "%.300s: %s", dir, strerror(errno)); return -1; }
	if (ns == 0 || ns > MAX_PARTS) { snprintf(err, errlen, "%.150s: no parts found", path); free_names(sfx, ns); return -1; }
	qsort(sfx, ns, sizeof *sfx, cmp_str);
	{
		char expect[16], one[16];

		memset(expect, cls == 'd' ? '0' : 'a', width); expect[width] = '\0';
		memcpy(one, expect, width + 1);
		if (cls == 'd') one[width - 1] = '1';
		if (strcmp(sfx[0], expect) && !(cls == 'd' && !strcmp(sfx[0], one))) {
			snprintf(err, errlen, "the first part of %.150s is missing: the lowest part found is %.150s.%.150s (expected %.150s.%.150s)", stem, stem, sfx[0], stem, expect);
			free_names(sfx, ns);
			return -1;
		}
		memcpy(expect, sfx[0], width + 1);
		for (size_t i = 0; i < ns; i++) {
			if (strcmp(sfx[i], expect)) {
				snprintf(err, errlen, "part %.150s.%.150s of %.150s is missing (the next one found is %.150s.%.150s)", stem, expect, stem, stem, sfx[i]);
				free_names(sfx, ns);
				return -1;
			}
			if (next_suffix(expect, width, cls)) break;
		}
	}
	*names = calloc(ns, sizeof **names);
	if (!*names) { snprintf(err, errlen, "out of memory"); free_names(sfx, ns); return -1; }
	for (size_t i = 0; i < ns; i++) {
		size_t l = strlen(dir) + 1 + stemlen + 1 + width + 1;
		(*names)[i] = malloc(l);
		if (!(*names)[i]) { snprintf(err, errlen, "out of memory"); free_names(*names, i); free_names(sfx, ns); return -1; }
		if (slash) snprintf((*names)[i], l, "%s/%s.%s", dir, stem, sfx[i]);
		else snprintf((*names)[i], l, "%s.%s", stem, sfx[i]);
	}
	*n = ns;
	if (slash) snprintf(outbase, outbaselen, "%.600s/%.400s", dir, stem);
	else snprintf(outbase, outbaselen, "%s", stem);
	free_names(sfx, ns);
	return 0;
}

int zf_pread(const zf_archive *a, void *buf, size_t n, uint64_t off)
{
	return pread_part(a, buf, n, off);
}

void zf_part_name(char *buf, size_t cap, const char *base, unsigned n, unsigned width)
{
	snprintf(buf, cap, "%s.%0*u", base, (int)width, n);
}

void zf_free_paths(char **paths, size_t n)
{
	free_names(paths, n);
}

int zf_find_series_files(const char *base, char ***paths, size_t *n)
{
	const char *slash = strrchr(base, '/'), *name = slash ? slash + 1 : base;
	char dir[4096], **v = NULL;
	size_t dl = slash ? (size_t)(slash - base) : 0, nv = 0, cap = 0, ng = 0;
	sgroup groups[8];

	*paths = NULL;
	*n = 0;
	snprintf(dir, sizeof dir, "%.*s", slash ? (int)(dl ? dl : 1) : 1, slash ? base : ".");
	if (scan_dir(dir, name, strlen(name), 0, 0, NULL, NULL, groups, &ng)) return errno == ENOENT ? 0 : -1;
	for (size_t g = 0; g < ng; g++) {
		char **sfx = NULL;
		size_t ns = 0;

		if (scan_dir(dir, name, strlen(name), groups[g].cls, groups[g].width, &sfx, &ns, NULL, NULL)) return -1;
		for (size_t i = 0; i < ns; i++) {
			size_t l = strlen(base) + 1 + strlen(sfx[i]) + 1;

			if (nv == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); if (!v) return -1; }
			v[nv] = malloc(l);
			if (!v[nv]) return -1;
			snprintf(v[nv++], l, "%s.%s", base, sfx[i]);
		}
		free_names(sfx, ns);
	}
	*paths = v;
	*n = nv;
	return 0;
}

/* opens every part and lays them end to end */
static int open_parts(zf_archive *a, char **names, size_t n)
{
	uint64_t total = 0;

	a->parts = calloc(n, sizeof *a->parts);
	if (!a->parts) { snprintf(a->err, sizeof a->err, "out of memory"); return -1; }
	for (size_t i = 0; i < n; i++) {
		struct stat sb;

		a->parts[i].fd = open(names[i], O_RDONLY);
		a->nparts = i + 1;
		if (a->parts[i].fd < 0) {
			snprintf(a->err, sizeof a->err, n > 1 ? "cannot open part %s: %s (is its disc mounted?)" : "%s: %s", names[i], strerror(errno));
			goto fail;
		}
		if (fstat(a->parts[i].fd, &sb) || !S_ISREG(sb.st_mode)) { snprintf(a->err, sizeof a->err, "%s: not a regular file", names[i]); goto fail; }
		a->parts[i].start = total;
		a->parts[i].size = (uint64_t)sb.st_size;
		total += (uint64_t)sb.st_size;
		if ((int64_t)sb.st_mtime > a->mtime) a->mtime = (int64_t)sb.st_mtime;
	}
	a->filesize = total;
	if (n > 1) {
		const char *f = strrchr(names[0], '/'), *l = strrchr(names[n - 1], '/');

		snprintf(a->series, sizeof a->series, "%zu parts, %s .. %s", n, f ? f + 1 : names[0], l ? l + 1 : names[n - 1]);
	}
	return 0;
fail:
	for (size_t i = 0; i < a->nparts; i++) if (a->parts[i].fd >= 0) close(a->parts[i].fd);
	free(a->parts);
	a->parts = NULL;
	a->nparts = 0;
	return -1;
}

int zf_open(zf_archive *a, const char *path)
{
	uint8_t tr[ZF_TRAILER_SIZE], ih[8];
	uint64_t ioff, ilen, i, want, gap;
	char **names = NULL;
	size_t nnames = 0;

	memset(a, 0, sizeof *a);
	a->cached = -1;
	if (resolve_parts(path, &names, &nnames, a->base, sizeof a->base, a->err, sizeof a->err)) return -1;
	if (open_parts(a, names, nnames)) { free_names(names, nnames); return -1; }
	free_names(names, nnames);
	if (a->filesize < ZF_TRAILER_SIZE) return fail_parts(a, "not a zfrag archive (the file is too small)");

	/* the trailer: the last frame of the file */
	if (pread_part(a, tr, sizeof tr, a->filesize - ZF_TRAILER_SIZE)) return fail(a, "cannot read the trailer");
	if (get_le32(tr) != ZF_SKIP_TRAILER || get_le32(tr + 4) != ZF_TRAILER_PAYLOAD ||
	    memcmp(tr + 8, ZF_TRAILER_MAGIC, 8))
		return fail_parts(a, "not a zfrag archive: it has no zfrag trailer. A plain .tar.zst cannot be mounted or read "
				     "this way (read it with zstd -dc | tar x); a zfrag archive that was cut short looks the same");
	ioff = get_le64(tr + 16);
	ilen = get_le64(tr + 24);
	want = get_le64(tr + 32);
	if (ioff > a->filesize || ilen > a->filesize || ioff + 8 + ilen + ZF_TRAILER_SIZE > a->filesize)
		return fail_parts(a, "the trailer does not match the file size");
	/* between the index and the trailer there may be padding: whole skippable frames and nothing else */
	gap = a->filesize - ZF_TRAILER_SIZE - (ioff + 8 + ilen);
	if (gap) {
		uint8_t *gb = gap <= ZF_ALIGN * 4 ? malloc((size_t)gap) : NULL;
		uint64_t pos = 0;

		if (!gb) return fail_parts(a, "the trailer does not match the file size");
		if (pread_part(a, gb, (size_t)gap, ioff + 8 + ilen)) { free(gb); return fail(a, "cannot read the padding"); }
		while (gap - pos >= 8 && (get_le32(gb + pos) & 0xFFFFFFF0u) == 0x184D2A50u && get_le32(gb + pos + 4) <= gap - pos - 8)
			pos += 8 + get_le32(gb + pos + 4);
		free(gb);
		if (pos != gap) return fail_parts(a, "the trailer does not match the file size (what lies between the index and the trailer is not padding)");
	}
	a->gap = gap;

	/* the index */
	if (pread_part(a, ih, 8, ioff) || get_le32(ih) != ZF_SKIP_INDEX || get_le32(ih + 4) != ilen)
		return fail(a, "bad index frame header");
	if (ilen < ZF_IDX_HDR || ilen > ((uint64_t)1 << 32)) return fail(a, "bad index size");
	a->idx = malloc((size_t)ilen);
	if (!a->idx) return fail(a, "out of memory");
	if (pread_part(a, a->idx, (size_t)ilen, ioff + 8)) return fail(a, "cannot read the index");
	if (XXH64(a->idx, (size_t)ilen, 0) != want) return fail(a, "the index is corrupt (checksum mismatch)");
	if (memcmp(a->idx, ZF_INDEX_MAGIC, 8) || get_le32(a->idx + 8) != ZF_INDEX_VERSION)
		return fail(a, "unknown index version");
	a->level = get_le32(a->idx + 12);
	a->frame_raw = get_le64(a->idx + 16);
	a->stream_len = get_le64(a->idx + 24);
	a->nframes = get_le64(a->idx + 32);
	a->nfiles = get_le64(a->idx + 40);
	a->names_len = get_le64(a->idx + 48);
	a->index_off = ioff;
	if (a->frame_raw < ZF_MIN_FRAME_RAW || a->frame_raw > ZF_MAX_FRAME_RAW)
		return fail(a, "frame size outside the format's limits");
	if (a->nframes > ilen / 8 || a->nfiles > ilen / ZF_REC_SIZE || a->names_len > ilen)
		return fail(a, "index counts are larger than the index");
	if (ZF_IDX_HDR + 8 * (a->nframes + 1) + (uint64_t)ZF_REC_SIZE * a->nfiles + a->names_len != ilen)
		return fail(a, "index sections do not add up");
	if ((a->stream_len + a->frame_raw - 1) / a->frame_raw != a->nframes)
		return fail(a, "frame count does not match the stream length");
	a->foff = a->idx + ZF_IDX_HDR;
	a->files = a->foff + 8 * (a->nframes + 1);
	a->names = a->files + (size_t)ZF_REC_SIZE * a->nfiles;

	/* frame offsets must rise and stay before the index */
	for (i = 0; i < a->nframes; i++)
		if (get_le64(a->foff + 8 * i) >= get_le64(a->foff + 8 * (i + 1))) return fail(a, "frame table is not increasing");
	if (get_le64(a->foff + 8 * a->nframes) != ioff) return fail(a, "the last frame does not end at the index");
	/* every file record must lie inside the stream and the names pool */
	for (i = 0; i < a->nfiles; i++) {
		const uint8_t *r = a->files + (size_t)ZF_REC_SIZE * i;
		uint64_t d = get_le64(r), s = get_le64(r + 8);
		uint32_t hl = get_le32(r + 28), no = get_le32(r + 32), nl = get_le32(r + 36);

		if (d > a->stream_len || s > a->stream_len - d || hl > d || (uint64_t)no + nl > a->names_len || nl == 0)
			return fail(a, "a file record points outside the archive");
	}
	a->dctx = ZSTD_createDCtx();
	a->rbuf = malloc((size_t)a->frame_raw);
	if (!a->dctx || !a->rbuf) return fail(a, "out of memory");
	return 0;
}

void zf_rec_get(const zf_archive *a, uint64_t i, zf_rec *r)
{
	const uint8_t *p = a->files + (size_t)ZF_REC_SIZE * i;

	r->data_off = get_le64(p);
	r->size = get_le64(p + 8);
	r->mtime = (int64_t)get_le64(p + 16);
	r->mode = get_le32(p + 24);
	r->hdr_len = get_le32(p + 28);
	r->name = (const char *)a->names + get_le32(p + 32);
	r->name_len = get_le32(p + 36);
}

static int cmp_name(const zf_rec *r, const char *path, size_t len)
{
	size_t n = r->name_len < len ? r->name_len : len;
	int c = memcmp(r->name, path, n);

	if (c) return c;
	return r->name_len < len ? -1 : r->name_len > len;
}

int64_t zf_find(const zf_archive *a, const char *path, size_t len)
{
	uint64_t lo = 0, hi = a->nfiles;

	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		zf_rec r;
		int c;

		zf_rec_get(a, mid, &r);
		c = cmp_name(&r, path, len);
		if (c == 0) return (int64_t)mid;
		if (c < 0) lo = mid + 1; else hi = mid;
	}
	return -1;
}

uint64_t zf_frame_len(const zf_archive *a, uint64_t i)
{
	uint64_t start = i * a->frame_raw;

	return a->stream_len - start < a->frame_raw ? a->stream_len - start : a->frame_raw;
}

int zf_frame(zf_archive *a, uint64_t i, const uint8_t **p, size_t *n)
{
	uint64_t s, e, want;
	size_t got;

	if (i >= a->nframes) return fail(a, "frame number out of range");
	if (a->cached == (int64_t)i) { *p = a->rbuf; *n = (size_t)zf_frame_len(a, i); return 0; }
	s = get_le64(a->foff + 8 * i);
	e = get_le64(a->foff + 8 * (i + 1));
	if (e - s > ((uint64_t)1 << 31)) return fail(a, "frame too large");
	if (a->ccap < e - s) {
		uint8_t *nb = realloc(a->cbuf, (size_t)(e - s));
		if (!nb) return fail(a, "out of memory");
		a->cbuf = nb;
		a->ccap = (size_t)(e - s);
	}
	if (pread_part(a, a->cbuf, (size_t)(e - s), s)) return fail(a, "cannot read a frame");
	want = zf_frame_len(a, i);
	got = ZSTD_decompressDCtx(a->dctx, a->rbuf, (size_t)a->frame_raw, a->cbuf, (size_t)(e - s));
	if (ZSTD_isError(got)) {
		snprintf(a->err, sizeof a->err, "frame %llu: %s", (unsigned long long)i, ZSTD_getErrorName(got));
		a->cached = -1;
		return -1;
	}
	if (got != want) { a->cached = -1; return fail(a, "a frame decoded to the wrong size"); }
	a->cached = (int64_t)i;
	*p = a->rbuf;
	*n = got;
	return 0;
}

int zf_read(zf_archive *a, uint64_t off, uint64_t n, uint8_t *dst)
{
	if (off > a->stream_len || n > a->stream_len - off) return fail(a, "read past the end of the stream");
	while (n) {
		const uint8_t *p;
		size_t fl, take;
		uint64_t fi = off / a->frame_raw, in = off % a->frame_raw;

		if (zf_frame(a, fi, &p, &fl)) return -1;
		take = fl - (size_t)in < n ? fl - (size_t)in : (size_t)n;
		memcpy(dst, p + in, take);
		dst += take; off += take; n -= take;
	}
	return 0;
}

/* ---- thread-safe decoding ---------------------------------------------- */

int zf_decoder_init(zf_decoder *d)
{
	d->dctx = ZSTD_createDCtx();
	return d->dctx ? 0 : -1;
}

void zf_decoder_free(zf_decoder *d)
{
	if (d->dctx) ZSTD_freeDCtx(d->dctx);
	d->dctx = NULL;
}

int zf_decode_frame(const zf_archive *a, uint64_t i, zf_decoder *d, uint8_t *out, char *err, size_t errlen)
{
	uint64_t s, e;
	uint8_t *cbuf;
	size_t got;

	if (i >= a->nframes) { snprintf(err, errlen, "frame number out of range"); return -1; }
	s = get_le64(a->foff + 8 * i);
	e = get_le64(a->foff + 8 * (i + 1));
	if (e - s > ((uint64_t)1 << 31)) { snprintf(err, errlen, "frame too large"); return -1; }
	cbuf = malloc((size_t)(e - s));
	if (!cbuf) { snprintf(err, errlen, "out of memory"); return -1; }
	if (pread_part(a, cbuf, (size_t)(e - s), s)) {
		free(cbuf);
		snprintf(err, errlen, "cannot read frame %llu", (unsigned long long)i);
		return -1;
	}
	got = ZSTD_decompressDCtx(d->dctx, out, (size_t)a->frame_raw, cbuf, (size_t)(e - s));
	free(cbuf);
	if (ZSTD_isError(got)) {
		snprintf(err, errlen, "frame %llu: %s", (unsigned long long)i, ZSTD_getErrorName(got));
		return -1;
	}
	if (got != zf_frame_len(a, i)) { snprintf(err, errlen, "frame %llu decoded to the wrong size", (unsigned long long)i); return -1; }
	return 0;
}
