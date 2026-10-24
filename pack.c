/* SPDX-License-Identifier: MIT */
/*
 * zfrag pack - write a zfrag archive: exactly the files named in a list, as a tar
 * stream cut into independent zstd frames, with an index at the end.
 *
 *   zfrag pack -T natural.txt -o set.tar.zfrag [-C dir] [-0] [--level 19]
 *              [--frame 32M] [-j 4] [--no-pad] [--force] [-q]
 *
 * The list is the whole interface: a file is in the archive if and only if it is
 * named, and the stream holds them in the order written: ordering the list (natural
 * order, similarity order, identical files first) is the job of the program that makes it,
 * such as sortof. Only regular files are stored; anything else in the list is skipped with
 * a message and the exit status is 1. Ownership is stored as 0/0 so an archive does not depend on who
 * made it.
 *
 * The output is written under temporary names and renamed only when it is complete, so
 * a failed or interrupted run leaves nothing behind. With --split SIZE it is written as parts
 * (NAME.001, NAME.002, ...) that end between frames; see outset.h. Frames are compressed in
 * parallel but written in order, so the archive is identical for any -j.
 * --strip-components N drops the first N components of every stored name (like tar), so an
 * archive of dir/game1/... and dir/game2/... can hold game1/... and game2/...; mounted at
 * "dir" the files are back at their old paths, whatever the directory is called later.
 * The format is described in zfrag_format.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>
#include <zstd.h>

#include "archive.h"
#include "assets.h"
#include "outset.h"
#include "tar.h"
#include "util.h"
#include "zfrag_format.h"

/* ---------------------------------------------------------------- */
/* The tar stream, produced on demand                                */
/* ---------------------------------------------------------------- */

typedef struct {
	uint64_t data_off, size;
	int64_t mtime;
	uint32_t mode, hdr_len, name_off, name_len;
} frec;

typedef struct {
	char **paths;
	size_t n, next;
	const char *dir;
	uint8_t pend[TAR_MAX_HDR];
	size_t pend_len, pend_pos;
	int fd;
	uint64_t remaining;
	size_t padding;
	uint64_t gen_pos;               /* bytes of the stream queued so far */
	int ended;
	size_t end_left;
	frec *recs;
	size_t nrecs, caprecs;
	char *names;
	size_t names_len, names_cap;
	uint64_t skipped, data_read, files_started;
	int quiet;
	const char *cur;                /* the list path being read, for messages */
	unsigned strip;                 /* --strip-components */
	char **alias;                   /* NULL, or per list entry the name to store instead of the path (assets) */
	uint64_t dupnames;              /* stored names that appear more than once */
	size_t dupex;                   /* record index of the first such name, for the message */
	/* progress */
	int tty;
	double t0, last;
} tstream;

static void say(tstream *ts, const char *fmt, const char *a)
{
	if (ts->tty) fprintf(stderr, "\r%100s\r", "");
	fprintf(stderr, fmt, a);
}

static void progress(tstream *ts, uint64_t total)
{
	double t = now();

	if (!ts->tty || t - ts->last < 0.2) return;
	ts->last = t;
	{
		const int width = 30;
		double frac = total ? (double)ts->files_started / (double)total : 1.0, el = t - ts->t0;
		double rate = el > 0 ? (double)ts->data_read / el / 1e6 : 0, eta = -1;
		int filled;
		char etas[32] = "";

		if (frac > 1) frac = 1;
		if (ts->files_started > 0 && el > 0) eta = el * (double)(total - ts->files_started) / (double)ts->files_started;
		if (eta >= 0) {
			long e = (long)(eta + 0.5);
			if (e >= 3600) snprintf(etas, sizeof etas, " eta %ldh%02ldm", e / 3600, (e % 3600) / 60);
			else snprintf(etas, sizeof etas, " eta %ld:%02ld", e / 60, e % 60);
		}
		filled = (int)(frac * width);
		fprintf(stderr, "\rpacking    [");
		for (int i = 0; i < width; i++) fputc(i < filled ? '#' : '-', stderr);
		fprintf(stderr, "] %5.1f%% (%llu/%zu) %.0f MB/s%s   ", frac * 100.0,
			(unsigned long long)ts->files_started, (size_t)total, rate, etas);
		fflush(stderr);
	}
}

static int add_rec(tstream *ts, const frec *r, const char *name, size_t nlen)
{
	if (ts->nrecs == ts->caprecs) {
		size_t nc = ts->caprecs ? ts->caprecs * 2 : 4096;
		frec *np = realloc(ts->recs, nc * sizeof *np);
		if (!np) return -1;
		ts->recs = np;
		ts->caprecs = nc;
	}
	if (ts->names_len + nlen > ts->names_cap) {
		size_t nc = ts->names_cap ? ts->names_cap * 2 : 1 << 20;
		char *nn;
		while (nc < ts->names_len + nlen) nc *= 2;
		nn = realloc(ts->names, nc);
		if (!nn) return -1;
		ts->names = nn;
		ts->names_cap = nc;
	}
	if (ts->names_len + nlen > 0xFFFFFFF0u) return -1;
	memcpy(ts->names + ts->names_len, name, nlen);
	ts->recs[ts->nrecs] = *r;
	ts->recs[ts->nrecs].name_off = (uint32_t)ts->names_len;
	ts->recs[ts->nrecs].name_len = (uint32_t)nlen;
	ts->nrecs++;
	ts->names_len += nlen;
	return 0;
}

/* open the next usable list entry and queue its header; 1 = ok, 0 = none left, -1 = error */
static int next_file(tstream *ts)
{
	while (ts->next < ts->n) {
		size_t idx = ts->next++;
		const char *p = ts->paths[idx], *name;
		const char *al = ts->alias ? ts->alias[idx] : NULL;
		char full[PATH_MAX];
		struct stat sb;
		size_t nlen, hl, padded;
		frec r;
		int fd;

		if (al) { name = al; nlen = strlen(al); }               /* an asset: stored as given, not stripped */
		else if (tar_norm_name(p, &name, &nlen)) { say(ts, "skipping %s: empty or unsafe name (.. component)\n", p); ts->skipped++; continue; }
		if (!al && ts->strip && tar_strip_name(&name, &nlen, ts->strip)) {
			say(ts, "skipping %s: nothing is left of the name after --strip-components\n", p);
			ts->skipped++;
			continue;
		}
		if (ts->dir) snprintf(full, sizeof full, "%s/%s", ts->dir, p);
		else snprintf(full, sizeof full, "%s", p);
		if (lstat(full, &sb) || !S_ISREG(sb.st_mode)) {
			say(ts, "skipping %s: not a regular file\n", p);
			ts->skipped++;
			continue;
		}
		fd = open(full, O_RDONLY);
		if (fd < 0) { say(ts, "skipping %s: cannot open\n", p); ts->skipped++; continue; }
		if (fstat(fd, &sb) || !S_ISREG(sb.st_mode)) { close(fd); ts->skipped++; continue; }
		hl = tar_header(ts->pend, sizeof ts->pend, name, nlen, (uint64_t)sb.st_size, (int64_t)sb.st_mtime, sb.st_mode & 0777);
		if (!hl) { close(fd); say(ts, "skipping %s: name too long for a tar header\n", p); ts->skipped++; continue; }
		padded = (size_t)(((uint64_t)sb.st_size + ZF_TAR_BLOCK - 1) / ZF_TAR_BLOCK * ZF_TAR_BLOCK - (uint64_t)sb.st_size);
		r.data_off = ts->gen_pos + hl;
		r.size = (uint64_t)sb.st_size;
		r.mtime = (int64_t)sb.st_mtime;
		r.mode = sb.st_mode & 0777;
		r.hdr_len = (uint32_t)hl;
		if (add_rec(ts, &r, name, nlen)) { close(fd); return -1; }
		ts->pend_len = hl;
		ts->pend_pos = 0;
		ts->gen_pos += hl + (uint64_t)sb.st_size + padded;
		ts->cur = p;
		ts->fd = fd;
		ts->remaining = (uint64_t)sb.st_size;
		ts->padding = padded;
		ts->files_started++;
		return 1;
	}
	return 0;
}

/* fill dst with the next bytes of the stream; returns how many (< want only at the end) */
static ssize_t ts_read(tstream *ts, uint8_t *dst, size_t want, uint64_t total_files)
{
	size_t got = 0;

	while (got < want) {
		size_t take;

		if (ts->pend_pos < ts->pend_len) {
			take = ts->pend_len - ts->pend_pos;
			if (take > want - got) take = want - got;
			memcpy(dst + got, ts->pend + ts->pend_pos, take);
			ts->pend_pos += take; got += take;
		} else if (ts->fd >= 0 && ts->remaining) {
			take = ts->remaining < want - got ? (size_t)ts->remaining : want - got;
			ssize_t r = read(ts->fd, dst + got, take);
			if (r < 0 && errno == EINTR) continue;
			if (r <= 0) {                           /* the file shrank: pad with zeros, as tar does */
				say(ts, "warning: %s became shorter while it was being read; padded with zeros\n", ts->cur);
				memset(dst + got, 0, take);
				r = (ssize_t)take;
			}
			got += (size_t)r; ts->remaining -= (uint64_t)r; ts->data_read += (uint64_t)r;
		} else if (ts->padding) {
			take = ts->padding < want - got ? ts->padding : want - got;
			memset(dst + got, 0, take);
			got += take; ts->padding -= take;
		} else if (ts->fd >= 0) {
			close(ts->fd);
			ts->fd = -1;
		} else if (ts->next < ts->n) {
			int r = next_file(ts);
			progress(ts, total_files);
			if (r < 0) return -1;
		} else if (!ts->ended) {
			ts->ended = 1;
			ts->end_left = 2 * ZF_TAR_BLOCK;        /* the end-of-archive marker */
			ts->gen_pos += ts->end_left;
		} else if (ts->end_left) {
			take = ts->end_left < want - got ? ts->end_left : want - got;
			memset(dst + got, 0, take);
			got += take; ts->end_left -= take;
		} else {
			break;
		}
	}
	return (ssize_t)got;
}

/* ---------------------------------------------------------------- */
/* Parallel compression, in-order output                             */
/* ---------------------------------------------------------------- */

enum { S_EMPTY, S_FILLING, S_FILLED, S_BUSY };

typedef struct { uint8_t *raw; size_t len; uint64_t seq; int state; } slot;

typedef struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	slot *slots;
	int nslots, level, wlog, err, done;
	outset *os;
	size_t frame;
	uint64_t next_write, cur_off;
	uint64_t *foff;
	size_t nfoff, capfoff;
	char msg[400];
} pctx;

static void set_err(pctx *c, const char *m)
{
	if (!c->err) { c->err = 1; snprintf(c->msg, sizeof c->msg, "%s", m); }
	pthread_cond_broadcast(&c->cv);
}

static void *worker(void *arg)
{
	pctx *c = arg;
	ZSTD_CCtx *cc = ZSTD_createCCtx();
	size_t cap = ZSTD_compressBound(c->frame);
	uint8_t *out = malloc(cap);

	if (!cc || !out) {
		pthread_mutex_lock(&c->mu); set_err(c, "out of memory"); pthread_mutex_unlock(&c->mu);
		goto done;
	}
	ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, c->level);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_windowLog, c->wlog);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, 0);
	for (;;) {
		slot *s = NULL;
		size_t clen;
		uint64_t off;

		pthread_mutex_lock(&c->mu);
		for (;;) {
			if (c->err) break;
			/* always the lowest frame number first, or workers could all be holding
			 * later frames while waiting for one nobody has picked up */
			for (int i = 0; i < c->nslots; i++)
				if (c->slots[i].state == S_FILLED && (!s || c->slots[i].seq < s->seq)) s = &c->slots[i];
			if (s) { s->state = S_BUSY; break; }
			if (c->done) break;
			pthread_cond_wait(&c->cv, &c->mu);
		}
		pthread_mutex_unlock(&c->mu);
		if (!s) break;

		clen = ZSTD_compress2(cc, out, cap, s->raw, s->len);
		if (ZSTD_isError(clen)) {
			pthread_mutex_lock(&c->mu); set_err(c, ZSTD_getErrorName(clen)); pthread_mutex_unlock(&c->mu);
			break;
		}
		pthread_mutex_lock(&c->mu);
		while (!c->err && c->next_write != s->seq) pthread_cond_wait(&c->cv, &c->mu);
		if (c->err) { pthread_mutex_unlock(&c->mu); break; }
		off = c->cur_off;
		pthread_mutex_unlock(&c->mu);

		{
			char werr[300];

			if (outset_write(c->os, out, clen, off, werr, sizeof werr)) {
				pthread_mutex_lock(&c->mu); set_err(c, werr); pthread_mutex_unlock(&c->mu);
				break;
			}
		}
		pthread_mutex_lock(&c->mu);
		if (c->nfoff == c->capfoff) {
			size_t nc = c->capfoff ? c->capfoff * 2 : 1024;
			uint64_t *nf = realloc(c->foff, nc * sizeof *nf);
			if (!nf) { set_err(c, "out of memory"); pthread_mutex_unlock(&c->mu); break; }
			c->foff = nf;
			c->capfoff = nc;
		}
		c->foff[c->nfoff++] = off;
		c->cur_off += clen;
		c->next_write++;
		s->state = S_EMPTY;
		pthread_cond_broadcast(&c->cv);
		pthread_mutex_unlock(&c->mu);
	}
done:
	free(out);
	if (cc) ZSTD_freeCCtx(cc);
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Index                                                             */
/* ---------------------------------------------------------------- */

static tstream *g_sort_ts;

static int cmp_rec(const void *a, const void *b)
{
	const frec *x = &g_sort_ts->recs[*(const size_t *)a], *y = &g_sort_ts->recs[*(const size_t *)b];
	size_t n = x->name_len < y->name_len ? x->name_len : y->name_len;
	int c = memcmp(g_sort_ts->names + x->name_off, g_sort_ts->names + y->name_off, n);

	if (c) return c;
	return x->name_len < y->name_len ? -1 : x->name_len > y->name_len;
}

/* builds the index payload; returns a malloc'd buffer and its length */
static uint8_t *build_index(tstream *ts, const pctx *c, size_t frame, size_t *len)
{
	size_t nfr = c->nfoff, nf = ts->nrecs, i;
	size_t total = ZF_IDX_HDR + 8 * (nfr + 1) + (size_t)ZF_REC_SIZE * nf + ts->names_len;
	uint8_t *b = calloc(1, total ? total : 1), *p;
	size_t *ord = malloc((nf ? nf : 1) * sizeof *ord);

	if (!b || !ord) { free(b); free(ord); return NULL; }
	memcpy(b, ZF_INDEX_MAGIC, 8);
	put_le32(b + 8, ZF_INDEX_VERSION);
	put_le32(b + 12, (uint32_t)c->level);
	put_le64(b + 16, frame);
	put_le64(b + 24, ts->gen_pos);
	put_le64(b + 32, nfr);
	put_le64(b + 40, nf);
	put_le64(b + 48, ts->names_len);
	p = b + ZF_IDX_HDR;
	for (i = 0; i < nfr; i++) put_le64(p + 8 * i, c->foff[i]);
	put_le64(p + 8 * nfr, c->cur_off);              /* just past the last frame: where the index starts */
	p += 8 * (nfr + 1);
	for (i = 0; i < nf; i++) ord[i] = i;
	g_sort_ts = ts;
	qsort(ord, nf, sizeof *ord, cmp_rec);
	for (i = 1; i < nf; i++)                        /* the same name stored twice: extraction keeps only one */
		if (cmp_rec(&ord[i - 1], &ord[i]) == 0) {
			if (!ts->dupnames) ts->dupex = ord[i];
			ts->dupnames++;
		}
	for (i = 0; i < nf; i++) {
		const frec *r = &ts->recs[ord[i]];
		uint8_t *q = p + (size_t)ZF_REC_SIZE * i;

		put_le64(q, r->data_off);
		put_le64(q + 8, r->size);
		put_le64(q + 16, (uint64_t)r->mtime);
		put_le32(q + 24, r->mode);
		put_le32(q + 28, r->hdr_len);
		put_le32(q + 32, r->name_off);
		put_le32(q + 36, r->name_len);
	}
	p += (size_t)ZF_REC_SIZE * nf;
	memcpy(p, ts->names, ts->names_len);
	free(ord);
	*len = total;
	return b;
}

/* ---------------------------------------------------------------- */
/* Command                                                           */
/* ---------------------------------------------------------------- */

/* what the name would be if it ended in .tar.zfrag: ".zst", ".tar.zst", ".tar" and
 * ".zfrag" endings are replaced, anything else is kept and the suffix added */
static void suggest_name(const char *name, char *out, size_t cap)
{
	static const char *const strip[] = { ".tar.zst", ".zst", ".tar.zfrag", ".zfrag", ".tar" };
	size_t len = strlen(name);

	for (size_t i = 0; i < sizeof strip / sizeof *strip; i++) {
		size_t sl = strlen(strip[i]);
		if (len > sl && !strcmp(name + len - sl, strip[i])) { len -= sl; break; }
	}
	snprintf(out, cap, "%.*s%s", (int)len, name, ZF_SUFFIX);
}

static int has_suffix(const char *s, const char *suf)
{
	size_t l = strlen(s), n = strlen(suf);

	return l > n && !strcmp(s + l - n, suf);
}


static void usage(void)
{
	fprintf(stderr,
"usage: zfrag pack -T LIST -o ARCHIVE [options]\n"
"  -T LIST       the files to pack, one path per line (required). Exactly these\n"
"                files go in; nothing else does\n"
"  -o ARCHIVE    the archive to write (required). The name must end in .tar.zfrag: it\n"
"                is readable by zstd but is not an ordinary tar.zst. Written under a\n"
"                temporary name and renamed when complete, so a failed run leaves\n"
"                nothing behind\n"
"  --split SIZE  write the archive as parts of at most SIZE (K, M, G, T: 4300M, 4G) named\n"
"                ARCHIVE.001, ARCHIVE.002, ... instead of one file, for discs or small\n"
"                drives. Parts end between frames; every command reads them as one archive\n"
"                (give the normal name, ARCHIVE, or any part), and joining them with cat\n"
"                gives the same bytes a single file would hold. `zfrag split` does the\n"
"                same to an archive that already exists\n"
"  -C DIR        paths in LIST are relative to DIR (stored names are as listed)\n"
"  --strip-components N\n"
"                remove the first N path components from every stored name, like tar. The files are\n"
"                still read from the paths in LIST, but the archive holds what is inside a\n"
"                directory: with LIST paths dir/game1/... and N=1 it stores game1/..., and a mount\n"
"                at dir puts every file back at dir/game1/..., however the directory is named\n"
"                later. Two paths that become the same name are reported\n"
"  --assets DIR  also store the files directly in DIR (audio, subtitles, cover art, ...) as assets/NAME, after\n"
"                everything in LIST, whatever DIR is called on disk. A mount shows assets/ in native/. Relative to -C\n"
"  -0            LIST is NUL-separated\n"
"  --level N     zstd level (default 19)\n"
"  --frame SIZE  uncompressed bytes per frame, 64K..32M (default 32M): the unit a\n"
"                reader decodes to read any part of a file\n"
"  -j N          frames compressed at a time (default 4); the archive is the same\n"
"                for any N\n"
"  --no-pad      do not pad the archive to a multiple of 4096 bytes. By default it is padded, because a\n"
"                loop device drops a partial last sector, which would cut the trailer off and make the\n"
"                archive unfit for a loop device (`zfrag pad` pads one that is not)\n"
"  --force       overwrite ARCHIVE if it exists\n"
"  -q            no progress and no summary\n"
"Only regular files are stored; anything else in the list is skipped with a message\n"
"(and the exit status is 1). Files are packed in the order of the list; to order it\n"
"(natural, similarity, duplicates first) make the list with the sortof-* tools.\n");
}

int zfrag_pack(int argc, char **argv)
{
	const char *list = NULL, *outpath = NULL, *dir = NULL;
	int nul = 0, level = 19, jobs = 4, no_pad = 0, force = 0, quiet = 0, i, rc = 1;
	unsigned strip = 0;
	uint64_t frame = ZF_DEFAULT_FRAME;
	size_t listlen = 0, n = 0, nfiles_listed;
	char *buf = NULL, **paths = NULL, oerr[512];
	uint64_t split = 0;
	const char *assets = NULL;
	char *apool = NULL, **alias = NULL;
	size_t nadded = 0;
	uint64_t abytes = 0;
	outset *os = NULL;
	tstream ts;
	pctx c;
	pthread_t *th = NULL;
	int started = 0, wlog = 10, renamed = 0;
	double t0;

	memset(&ts, 0, sizeof ts);
	memset(&c, 0, sizeof c);
	ts.fd = -1;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-T") && i + 1 < argc) list = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
			if (parse_size(argv[++i], &frame) || frame < ZF_MIN_FRAME_RAW || frame > ZF_MAX_FRAME_RAW) {
				fprintf(stderr, "bad --frame '%s' (64K..32M)\n", argv[i]);
				return 2;
			}
		}
		else if (!strcmp(argv[i], "-j") && i + 1 < argc) jobs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--split") && i + 1 < argc) {
			if (parse_size(argv[++i], &split)) {
				fprintf(stderr, "bad --split '%s' (a size such as 4300M or 4G)\n", argv[i]);
				return 2;
			}
		}
		else if (!strcmp(argv[i], "--strip-components") && i + 1 < argc) {
			char *e;
			long v = strtol(argv[++i], &e, 10);
			if (e == argv[i] || *e || v < 0 || v > 64) { fprintf(stderr, "bad --strip-components '%s' (0..64)\n", argv[i]); return 2; }
			strip = (unsigned)v;
		}
		else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
		else if (!strcmp(argv[i], "-0")) nul = 1;
		else if (!strcmp(argv[i], "--no-pad")) no_pad = 1;
		else if (!strcmp(argv[i], "--force")) force = 1;
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else { usage(); return 2; }
	}
	if (!list || !outpath || level < 1 || level > 22 || jobs < 1 || jobs > 64) { usage(); return 2; }
	if (!has_suffix(outpath, ZF_SUFFIX)) {
		char sug[PATH_MAX];

		suggest_name(outpath, sug, sizeof sug);
		fprintf(stderr, "zfrag pack: the archive name must end in %s (not .tar.zst: a zfrag archive is read by zstd "
				"but is not an ordinary tar.zst). Did you mean %s ?\n", ZF_SUFFIX, sug);
		return 2;
	}
	if (split && split < frame + frame / 64 + 4096) {
		fprintf(stderr, "--split %llu is too small for --frame %lluK: a part must hold at least one whole frame (use more than %llu)\n",
			(unsigned long long)split, (unsigned long long)(frame >> 10), (unsigned long long)(frame + frame / 64 + 4096));
		return 2;
	}
	if (outset_precheck(outpath, split != 0, force, 0, oerr, sizeof oerr)) { fprintf(stderr, "%s\n", oerr); return 1; }
	while (wlog < ZF_MAX_WINDOW_LOG && ((uint64_t)1 << wlog) < frame) wlog++;

	buf = slurp(list, &listlen);
	if (!buf) { fprintf(stderr, "%s: %s\n", list, strerror(errno)); return 1; }
	n = split_list(buf, listlen, nul ? '\0' : '\n', &paths);
	if (n == (size_t)-1) { perror("alloc"); n = 0; goto out; }
	if (n == 0) { fprintf(stderr, "the list is empty; nothing to pack\n"); goto out; }
	if (assets) {                                   /* the assets go after everything the list names */
		size_t m = assets_add(&paths, &alias, n, dir, assets, &apool, &nadded, &abytes);

		if (m == (size_t)-1) goto out;
		n = m;
		if (nadded && !quiet) {
			char hb[32];

			human(hb, sizeof hb, abytes);
			fprintf(stderr, "zfrag pack: %zu asset file%s (%s) added as %s/\n", nadded, nadded == 1 ? "" : "s", hb, ASSETS_NAME);
		}
	}
	nfiles_listed = n;

	ts.tty = isatty(2) && !quiet;
	os = outset_new(outpath, split != 0, split, ZF_PART_WIDTH, force, 0, oerr, sizeof oerr);
	if (!os) { fprintf(stderr, "%s\n", oerr); goto out; }
	outset_install_signals(os);

	ts.paths = paths; ts.alias = alias; ts.n = n; ts.dir = dir; ts.quiet = quiet; ts.strip = strip;
	ts.t0 = ts.last = now();
	t0 = now();
	pthread_mutex_init(&c.mu, NULL);
	pthread_cond_init(&c.cv, NULL);
	c.nslots = jobs + 2;
	c.slots = calloc((size_t)c.nslots, sizeof *c.slots);
	c.level = level; c.wlog = wlog; c.os = os; c.frame = (size_t)frame;
	if (!c.slots) { perror("alloc"); goto out; }
	for (i = 0; i < c.nslots; i++) {
		c.slots[i].raw = malloc((size_t)frame);
		if (!c.slots[i].raw) { perror("alloc"); goto out; }
	}
	th = calloc((size_t)jobs, sizeof *th);
	if (!th) { perror("alloc"); goto out; }
	for (i = 0; i < jobs; i++) {
		if (pthread_create(&th[i], NULL, worker, &c)) { fprintf(stderr, "pthread_create failed\n"); pthread_mutex_lock(&c.mu); set_err(&c, "cannot start a worker"); pthread_mutex_unlock(&c.mu); break; }
		started++;
	}

	/* the producer: cut the stream into frames and hand them out */
	for (uint64_t seq = 0; ; seq++) {
		slot *s = NULL;
		ssize_t got;

		pthread_mutex_lock(&c.mu);
		for (;;) {
			if (c.err) break;
			for (i = 0; i < c.nslots; i++)
				if (c.slots[i].state == S_EMPTY) { s = &c.slots[i]; break; }
			if (s) { s->state = S_FILLING; break; }
			pthread_cond_wait(&c.cv, &c.mu);
		}
		pthread_mutex_unlock(&c.mu);
		if (!s) break;
		got = ts_read(&ts, s->raw, (size_t)frame, nfiles_listed);
		pthread_mutex_lock(&c.mu);
		if (got < 0) { set_err(&c, "out of memory building the index"); s->state = S_EMPTY; pthread_mutex_unlock(&c.mu); break; }
		if (got == 0) { s->state = S_EMPTY; pthread_mutex_unlock(&c.mu); break; }
		s->len = (size_t)got;
		s->seq = seq;
		s->state = S_FILLED;
		pthread_cond_broadcast(&c.cv);
		pthread_mutex_unlock(&c.mu);
		if ((size_t)got < frame) break;
	}
	pthread_mutex_lock(&c.mu);
	c.done = 1;
	pthread_cond_broadcast(&c.cv);
	pthread_mutex_unlock(&c.mu);
	for (i = 0; i < started; i++) pthread_join(th[i], NULL);
	if (ts.tty) fprintf(stderr, "\r%100s\r", "");
	if (c.err) { fprintf(stderr, "pack failed: %s\n", c.msg); goto out; }
	if (ts.nrecs == 0) { fprintf(stderr, "none of the %zu listed entries could be packed\n", nfiles_listed); goto out; }

	{
		size_t ilen = 0;
		uint8_t *idx = build_index(&ts, &c, (size_t)frame, &ilen), *unit;
		uint64_t ioff = c.cur_off;
		size_t ulen, padtotal = 0, tpos;

		if (!idx) { fprintf(stderr, "out of memory building the index\n"); goto out; }
		if (ilen > 0xFFFFFFF0u) { fprintf(stderr, "the index is too large\n"); free(idx); goto out; }
		/* The index frame, the padding and the trailer are one unit: they stay together, in the last part.
		 * The padding makes the whole archive a multiple of ZF_ALIGN, so that a loop device does not drop
		 * the trailer with a partial last sector: a skippable frame of at least its 8-byte header. */
		if (!no_pad && (ioff + 8 + ilen + ZF_TRAILER_SIZE) % ZF_ALIGN) {
			uint64_t r = ZF_ALIGN - (ioff + 8 + ilen + ZF_TRAILER_SIZE) % ZF_ALIGN;

			if (r < 8) r += ZF_ALIGN;
			padtotal = (size_t)r;
		}
		tpos = 8 + ilen + padtotal;
		ulen = tpos + ZF_TRAILER_SIZE;
		unit = calloc(1, ulen);
		if (!unit) { fprintf(stderr, "out of memory building the index\n"); free(idx); goto out; }
		put_le32(unit, ZF_SKIP_INDEX);
		put_le32(unit + 4, (uint32_t)ilen);
		memcpy(unit + 8, idx, ilen);
		if (padtotal) {
			put_le32(unit + 8 + ilen, ZF_SKIP_PAD);
			put_le32(unit + 8 + ilen + 4, (uint32_t)(padtotal - 8));      /* the payload is the zeros calloc left */
		}
		put_le32(unit + tpos, ZF_SKIP_TRAILER);
		put_le32(unit + tpos + 4, ZF_TRAILER_PAYLOAD);
		memcpy(unit + tpos + 8, ZF_TRAILER_MAGIC, 8);
		put_le64(unit + tpos + 16, ioff);
		put_le64(unit + tpos + 24, ilen);
		put_le64(unit + tpos + 32, XXH64(idx, ilen, 0));
		free(idx);
		if (outset_write(os, unit, ulen, ioff, oerr, sizeof oerr)) { fprintf(stderr, "%s\n", oerr); free(unit); goto out; }
		free(unit);
		if (outset_commit(os, oerr, sizeof oerr)) { fprintf(stderr, "%s\n", oerr); goto out; }
		renamed = 1;

		if (!quiet) {
			char a[32], b[32], d[32];
			uint64_t total_out = ioff + ulen;
			double secs = now() - t0;
			char fl[16];

			human(a, sizeof a, ts.gen_pos);
			human(b, sizeof b, total_out);
			human(d, sizeof d, ilen);
			if (frame >= (1u << 20)) snprintf(fl, sizeof fl, "%lluM", (unsigned long long)(frame >> 20));
			else snprintf(fl, sizeof fl, "%lluK", (unsigned long long)(frame >> 10));
			printf("%s: %zu files, %s -> %s (%.2f%%) in %zu frames of %s, index %s, level %d, %.1f s\n",
			       outpath, ts.nrecs, a, b, 100.0 * (double)total_out / (double)ts.gen_pos,
			       c.nfoff, fl, d, level, secs);
			if (outset_nparts(os) > 1 || split) {
				char pa[32], pb[32];
				size_t np = outset_nparts(os), big = 0;

				for (size_t k = 1; k < np; k++) if (outset_part_bytes(os, k) > outset_part_bytes(os, big)) big = k;
				human(pa, sizeof pa, outset_part_bytes(os, big));
				human(pb, sizeof pb, outset_part_bytes(os, np - 1));
				printf("parts: %zu (%s .. %s), the largest %s, the last %s\n", np, outset_part_path(os, 0),
				       outset_part_path(os, np - 1), pa, pb);
			}
			if (ts.skipped)
				printf("skipped %llu entries (see the messages above)\n", (unsigned long long)ts.skipped);
		}
		if (ts.dupnames) {
			const frec *r = &ts.recs[ts.dupex];

			fprintf(stderr, "warning: %llu stored name%s appear%s more than once (first: %.*s). A normal extraction keeps only\n"
					"         one of each. The list names a path twice, or --strip-components made two paths equal.\n",
				(unsigned long long)ts.dupnames, ts.dupnames == 1 ? "" : "s", ts.dupnames == 1 ? "s" : "",
				(int)r->name_len, ts.names + r->name_off);
		}
		rc = (ts.skipped || ts.dupnames) ? 1 : 0;
	}

out:
	if (os) { if (renamed) outset_free(os); else outset_abort(os); }
	if (ts.fd >= 0) close(ts.fd);
	if (c.slots) { for (i = 0; i < c.nslots; i++) free(c.slots[i].raw); free(c.slots); }
	free(c.foff);
	free(th);
	free(ts.recs);
	free(ts.names);
	free(paths);
	free(buf);
	free(apool);
	free(alias);
	return rc;
}
