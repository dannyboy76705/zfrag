/* SPDX-License-Identifier: MIT */
/*
 * zfrag packtest - measure compression ratio and decode speed per frame size.
 *
 *   zfrag packtest -T list.txt [-C dir] [-0] [--level 19] [--frames 1M,4M,16M]
 *                  [-j N] [--limit SIZE]
 *                  [--sample [--window X] [--every X]] [-q]
 *
 * Give it the list you would pack, in the order you would pack it: like the packer,
 * it takes the list as written and does not reorder it (order the list with sortof).
 *
 * Every file goes into the stream, as a real compression run would see it, and
 * the stream is cut into frames of the given target size; nothing is left out.
 *
 * Each frame is compressed independently with single-threaded zstd, exactly as
 * the archiver will do it, then decompressed and compared byte for byte. Totals
 * are summed over all frames, so the result does not depend on -j; -j only runs
 * several frames at once.
 *
 * zstd's window is set to the frame size (frames are capped at 32 MiB), otherwise the
 * level's default window would limit matches inside large frames.
 *
 * --sample feeds a fraction of the data to the compressor, to estimate the result
 * for a tenth of the work: in each directory of the list (a component clip's y,
 * u and v), the first WINDOW of every EVERY (default 2 s of every 20 s, taken
 * from the frame rate in specs.txt) is taken and the rest skipped. The windows
 * are joined into one stream and cut into frames as usual, so a large frame
 * simply holds several windows.
 *
 * Memory: each worker holds three buffers of one frame plus the compressor
 * state, which at level 19 is large (hundreds of MiB for 16M frames).
 * Frames are cut from the byte stream, so a file may straddle two frames;
 * that has no measurable effect on the ratio.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "zfrag_format.h"
#include "util.h"

/* ---------------------------------------------------------------- */
/* Shared reader: hands out the list's bytes in order                */
/* ---------------------------------------------------------------- */
typedef struct {
	char **paths;
	size_t nfiles, next;
	const char *dir;
	int err;
	uint8_t *map;
	size_t maplen, mapoff;
	uint64_t in_bytes, skipped, limit;
	int finished;
	pthread_mutex_t mu;
} reader;

static void rd_start(reader *r)
{
	r->next = 0;
	r->map = NULL;
	r->err = 0;
	r->in_bytes = r->skipped = 0;
	r->finished = 0;
}

/* map the next usable file; 1 = ok, 0 = list exhausted, -1 = error */
static int rd_open(reader *r)
{
	while (r->next < r->nfiles) {
		char full[PATH_MAX];
		const char *p = r->paths[r->next++];
		struct stat sb;
		int fd;
		void *m;

		if (r->dir) snprintf(full, sizeof full, "%s/%s", r->dir, p);
		else snprintf(full, sizeof full, "%s", p);

		fd = open(full, O_RDONLY);
		if (fd < 0 || fstat(fd, &sb) || !S_ISREG(sb.st_mode)) {
			if (fd >= 0) close(fd);
			r->skipped++;
			continue;
		}
		if (sb.st_size == 0) {
			close(fd);
			continue;
		}
		m = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (m == MAP_FAILED) {
			r->skipped++;
			continue;
		}
		r->map = m;
		r->maplen = (size_t)sb.st_size;
		r->mapoff = 0;
		return 1;
	}
	return 0;
}

/* caller holds r->mu. Returns bytes copied; 0 means no more data. */
static size_t rd_fill(reader *r, uint8_t *buf, size_t target)
{
	size_t got = 0;

	if (r->limit) {
		if (r->in_bytes >= r->limit)
			return 0;
		if (r->limit - r->in_bytes < target)
			target = (size_t)(r->limit - r->in_bytes);
	}
	while (got < target) {
		size_t n;

		if (!r->map) {
			int o = rd_open(r);
			if (o < 0) r->err = 1;
			if (o <= 0) break;
		}
		n = r->maplen - r->mapoff;
		if (n > target - got)
			n = target - got;
		memcpy(buf + got, r->map + r->mapoff, n);
		got += n;
		r->mapoff += n;
		if (r->mapoff == r->maplen) {
			munmap(r->map, r->maplen);
			r->map = NULL;
		}
	}
	__atomic_store_n(&r->in_bytes, r->in_bytes + got, __ATOMIC_RELAXED);
	return got;
}

/* ---------------------------------------------------------------- */
/* Workers                                                           */
/* ---------------------------------------------------------------- */
typedef struct {
	reader *r;
	size_t frame;
	int level, wlog, err;
	uint64_t raw, comp, frames;
	double enc_s, dec_s;
} job;

static void *worker(void *arg)
{
	job *j = arg;
	ZSTD_CCtx *cc = ZSTD_createCCtx();
	size_t cap = ZSTD_compressBound(j->frame);
	uint8_t *in = malloc(j->frame), *out = malloc(cap), *chk = malloc(j->frame);

	if (!cc || !in || !out || !chk) {
		fprintf(stderr, "out of memory (frame %zu)\n", j->frame);
		j->err = 1;
		goto done;
	}
	ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, j->level);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_windowLog, j->wlog);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, 0);

	for (;;) {
		size_t n, c, d;
		double t;

		pthread_mutex_lock(&j->r->mu);
		n = rd_fill(j->r, in, j->frame);
		pthread_mutex_unlock(&j->r->mu);
		if (!n)
			break;

		t = now();
		c = ZSTD_compress2(cc, out, cap, in, n);
		j->enc_s += now() - t;
		if (ZSTD_isError(c)) {
			fprintf(stderr, "compress: %s\n", ZSTD_getErrorName(c));
			j->err = 1;
			break;
		}

		t = now();
		d = ZSTD_decompress(chk, j->frame, out, c);
		j->dec_s += now() - t;
		if (ZSTD_isError(d) || d != n || memcmp(chk, in, n)) {
			fprintf(stderr, "roundtrip FAILED on a frame\n");
			j->err = 1;
			break;
		}
		j->raw += n;
		j->comp += c;
		j->frames++;
	}
done:
	free(in);
	free(out);
	free(chk);
	if (cc)
		ZSTD_freeCCtx(cc);
	__atomic_add_fetch(&j->r->finished, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */
static int parse_bytes(const char *s, uint64_t *out)
{
	char *e;
	unsigned long long v = strtoull(s, &e, 10);

	if (e == s)
		return -1;
	switch (*e) {
	case 'K': case 'k': v <<= 10; e++; break;
	case 'M': case 'm': v <<= 20; e++; break;
	case 'G': case 'g': v <<= 30; e++; break;
	}
	if (*e || !v)
		return -1;
	*out = v;
	return 0;
}

static void size_label(char *buf, size_t n, uint64_t v)
{
	if (v >= (1u << 20) && !(v & ((1u << 20) - 1)))
		snprintf(buf, n, "%lluM", (unsigned long long)(v >> 20));
	else if (v >= (1u << 10) && !(v & ((1u << 10) - 1)))
		snprintf(buf, n, "%lluK", (unsigned long long)(v >> 10));
	else
		snprintf(buf, n, "%llu", (unsigned long long)v);
}

/* ---------------------------------------------------------------- */
/* Sampling: N seconds out of every M, per directory                 */
/* ---------------------------------------------------------------- */

typedef struct {
	size_t n;                       /* files in the sample */
	char **path;
	size_t nwin;                    /* windows taken */
} sample_t;

static void sample_free(sample_t *sm)
{
	free(sm->path);
	memset(sm, 0, sizeof *sm);
}

static int same_dir(const char *a, const char *b)
{
	const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
	size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;

	return la == lb && !memcmp(a, b, la);
}

/* The list is cut into runs of consecutive files from one directory (for a
 * component clip: the y, then the u, then the v planes); in each run, the first
 * wf files of every ef files are taken. The
 * sample is just those files, in order, and it is fed to the compressor as one
 * stream: frames are cut at the usual size and may span windows. */
static int build_sample(char **paths, size_t nfiles, size_t wf, size_t ef, sample_t *sm)
{
	size_t a, k;

	memset(sm, 0, sizeof *sm);
	sm->path = malloc((nfiles + 1) * sizeof *sm->path);
	if (!sm->path) return -1;

	for (a = 0; a < nfiles; ) {
		size_t b = a + 1, L, s0;

		while (b < nfiles && same_dir(paths[b], paths[a])) b++;
		L = b - a;
		for (s0 = 0; s0 < L; s0 += ef) {
			size_t e = s0 + wf < L ? s0 + wf : L;

			sm->nwin++;
			for (k = s0; k < e; k++) sm->path[sm->n++] = paths[a + k];
		}
		a = b;
	}
	return 0;
}

/* framerate=24000:1001 (or N/D, or N) from a specs.txt */
static int fps_from_specs(const char *file, double *fps)
{
	FILE *f = fopen(file, "r");
	char line[256];

	if (!f) return -1;
	while (fgets(line, sizeof line, f)) {
		char *p = line, *e;
		double n, d = 1;

		while (*p == ' ' || *p == '\t') p++;
		if (strncmp(p, "framerate", 9)) continue;
		p += 9;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '=') continue;
		p++;
		n = strtod(p, &e);
		if (e == p || n <= 0) break;
		if (*e == ':' || *e == '/') {
			d = strtod(e + 1, &e);
			if (d <= 0) break;
		}
		*fps = n / d;
		fclose(f);
		return 0;
	}
	fclose(f);
	return -1;
}

/* "2" or "2s" = seconds, "48f" = files */
static int parse_span(const char *s, double *v, int *files)
{
	char *e;
	double x = strtod(s, &e);

	if (e == s || x <= 0) return -1;
	*files = 0;
	if (*e == 'f' || *e == 'F') { *files = 1; e++; }
	else if (*e == 's' || *e == 'S') e++;
	if (*e) return -1;
	*v = x;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
"usage: zfrag packtest -T LIST [options]\n"
"  -T LIST         file list, one path per line (required)\n"
"  -0              LIST is NUL-separated\n"
"  -C DIR          prefix every path in LIST with DIR/\n"
"  --level N       zstd level (default 19; slow to encode, decode speed is\n"
"                  mostly unaffected by level)\n"
"  --frames A,B,.. frame sizes to test (default 1M,4M,16M; 64K..32M)\n"
"  -j N            compress N frames at a time (default 1). Totals do not\n"
"                  depend on N. Memory is about 3x frame size per job plus\n"
"                  the compressor state.\n"
"  --limit SIZE    stop after SIZE bytes of input per frame size (e.g. 2G),\n"
"                  for a quick sample of the start of the list\n"
"  --sample        feed a sample to the compressor instead of everything: the first\n"
"                  WINDOW of every EVERY of each directory (default 2 s of every 20 s,\n"
"                  10%%), joined into one stream and cut into frames as usual, so the\n"
"                  numbers estimate how the episode compresses at each frame size for\n"
"                  a tenth of the work\n"
"  --window X      sample window: seconds (2 or 2s) or files (48f)\n"
"  --every X       sample period, same units (default 20)\n"
"  --specs FILE    specs.txt giving the frame rate (default: a specs.txt line in the\n"
"                  list); --fps N gives it directly\n"
"  -q              no progress output\n");
}

/* ---------------------------------------------------------------- */
/* Entry point: argv[0] is "packtest"                                */
/* ---------------------------------------------------------------- */
int zfrag_packtest(int argc, char **argv)
{
	const char *list = NULL, *dir = NULL, *frames = "1M,4M,16M";
	int nul = 0, quiet = 0, level = 19, jobs = 1;
	uint64_t limit = 0, fsz[16];
	size_t nfs = 0, listlen = 0, fi;
	char *buf = NULL, *fcopy = NULL, *tok, *save = NULL;
	reader r;
	int i, rc = 1, tty;
	int sample = 0, win_files = 0, every_files = 0;
	double win_v = 2, every_v = 20, fps_opt = 0, fps = 0;
	const char *specs_file = NULL;
	char **orig_paths = NULL;
	size_t nlisted = 0;
	sample_t sm;
	int have_sm = 0;

	memset(&r, 0, sizeof r);
	memset(&sm, 0, sizeof sm);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-T") && i + 1 < argc) list = argv[++i];
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = argv[++i];
		else if (!strcmp(argv[i], "-j") && i + 1 < argc) jobs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
			if (parse_bytes(argv[++i], &limit)) { usage(); return 2; }
		}
		else if (!strcmp(argv[i], "--sample")) sample = 1;
		else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
			if (parse_span(argv[++i], &win_v, &win_files)) { fprintf(stderr, "bad --window '%s'\n", argv[i]); return 2; }
			sample = 1;
		}
		else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
			if (parse_span(argv[++i], &every_v, &every_files)) { fprintf(stderr, "bad --every '%s'\n", argv[i]); return 2; }
			sample = 1;
		}
		else if (!strcmp(argv[i], "--specs") && i + 1 < argc) specs_file = argv[++i];
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc) { fps_opt = atof(argv[++i]); if (fps_opt <= 0) { usage(); return 2; } }
		else if (!strcmp(argv[i], "-0")) nul = 1;
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else { usage(); return 2; }
	}
	if (!list || level < 1 || level > 22 || jobs < 1 || jobs > 64) {
		usage();
		return 2;
	}

	fcopy = strdup(frames);
	if (!fcopy) { perror("strdup"); return 1; }
	for (tok = strtok_r(fcopy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		uint64_t v;
		if (nfs == 16 || parse_bytes(tok, &v) || v < ZF_MIN_FRAME_RAW || v > ZF_MAX_FRAME_RAW) {
			fprintf(stderr, "bad --frames entry '%s' (64K..32M)\n", tok);
			goto out;
		}
		fsz[nfs++] = v;
	}
	if (!nfs) { usage(); goto out; }

	buf = slurp(list, &listlen);
	if (!buf) { fprintf(stderr, "%s: %s\n", list, strerror(errno)); goto out; }
	r.nfiles = split_list(buf, listlen, nul ? '\0' : '\n', &r.paths);
	if (r.nfiles == (size_t)-1) { perror("alloc"); r.nfiles = 0; goto out; }
	r.dir = dir;
	tty = isatty(2) && !quiet;
	if (sample) {
		size_t wf, ef;

		fps = fps_opt;
		if (fps <= 0 && (!win_files || !every_files)) {
			char sp[PATH_MAX];

			if (specs_file) {
				snprintf(sp, sizeof sp, "%s", specs_file);
			} else {
				sp[0] = '\0';
				for (size_t k = 0; k < r.nfiles && !sp[0]; k++) {
					const char *sl = strrchr(r.paths[k], '/'), *base = sl ? sl + 1 : r.paths[k];
					if (!strcmp(base, "specs.txt")) {
						if (dir) snprintf(sp, sizeof sp, "%s/%s", dir, r.paths[k]);
						else snprintf(sp, sizeof sp, "%s", r.paths[k]);
					}
				}
			}
			if (!sp[0]) { fprintf(stderr, "no specs.txt in the list to take the frame rate from: give --specs FILE or --fps N (or use file counts, e.g. --window 48f --every 480f)\n"); goto out; }
			if (fps_from_specs(sp, &fps)) { fprintf(stderr, "%s: no usable framerate= line\n", sp); goto out; }
		}
		wf = win_files ? (size_t)(win_v + 0.5) : (size_t)(win_v * fps + 0.5);
		ef = every_files ? (size_t)(every_v + 0.5) : (size_t)(every_v * fps + 0.5);
		if (wf < 1) wf = 1;
		if (ef < wf) { fprintf(stderr, "the window (%zu files) is longer than the period (%zu files)\n", wf, ef); goto out; }
		nlisted = r.nfiles;
		orig_paths = r.paths;
		have_sm = 1;
		if (build_sample(r.paths, r.nfiles, wf, ef, &sm)) { perror("alloc"); goto out; }
		r.paths = sm.path;
		r.nfiles = sm.n;
		printf("sample: %zu of every %zu files per directory", wf, ef);
		if (fps > 0) printf(" (%.4g s of every %.4g s at %.3f fps)", (double)wf / fps, (double)ef / fps, fps);
		printf(", %zu windows, %zu of %zu listed files\n", sm.nwin, sm.n, nlisted);
	}
	r.limit = limit;
	pthread_mutex_init(&r.mu, NULL);

	printf("packtest: zstd level %d, window = frame size, checksum on, "
	       "%zu files listed, every file kept in the stream, in the order listed, %d job%s\n",
	       level, r.nfiles, jobs, jobs == 1 ? "" : "s");
	printf("%7s %8s %12s %12s %9s %9s %9s %8s\n", "frame", "frames",
	       "input", "compressed", "% of orig", "enc MB/s", "dec MB/s", "wall");
	fflush(stdout);

	for (fi = 0; fi < nfs; fi++) {
		job *jb = calloc((size_t)jobs, sizeof *jb);
		pthread_t *th = calloc((size_t)jobs, sizeof *th);
		uint64_t raw = 0, comp = 0, nframes = 0, orig;
		double enc = 0, dec = 0, t0, wall;
		int wlog = 10, bad = 0, started = 0;
		char lbl[24], a[32], b[32];

		rd_start(&r);
		if (!jb || !th) {
			perror("alloc");
			free(jb);
			free(th);
			goto out;
		}
		while (wlog < ZF_MAX_WINDOW_LOG && ((uint64_t)1 << wlog) < fsz[fi])
			wlog++;

		size_label(lbl, sizeof lbl, fsz[fi]);
		t0 = now();
		for (i = 0; i < jobs; i++) {
			jb[i].r = &r;
			jb[i].frame = (size_t)fsz[fi];
			jb[i].level = level;
			jb[i].wlog = wlog;
			if (pthread_create(&th[i], NULL, worker, &jb[i])) {
				fprintf(stderr, "pthread_create failed\n");
				jb[i].err = 1;
				break;
			}
			started++;
		}
		while (__atomic_load_n(&r.finished, __ATOMIC_SEQ_CST) < started) {
			usleep(250000);
			if (tty) {
				human(a, sizeof a, __atomic_load_n(&r.in_bytes, __ATOMIC_RELAXED));
				fprintf(stderr, "\r  %s frames: %s read", lbl, a);
			}
		}
		for (i = 0; i < started; i++)
			pthread_join(th[i], NULL);
		wall = now() - t0;
		if (tty)
			fprintf(stderr, "\r%60s\r", "");

		for (i = 0; i < jobs; i++) {
			raw += jb[i].raw;
			comp += jb[i].comp;
			nframes += jb[i].frames;
			enc += jb[i].enc_s;
			dec += jb[i].dec_s;
			bad |= jb[i].err;
		}
		bad |= r.err || started < jobs;
		orig = r.in_bytes;

		human(a, sizeof a, raw);
		human(b, sizeof b, comp);
		printf("%7s %8llu %12s %12s %8.2f%% %9.1f %9.1f %7.1fs%s\n", lbl,
		       (unsigned long long)nframes, a, b,
		       orig ? 100.0 * (double)comp / (double)orig : 0.0,
		       enc > 0 ? raw / enc / 1e6 : 0.0,
		       dec > 0 ? raw / dec / 1e6 : 0.0, wall,
		       bad ? "  ERROR" : "");
		if (r.skipped)
			printf("        (%llu unreadable files skipped)\n",
			       (unsigned long long)r.skipped);
		fflush(stdout);
		free(jb);
		free(th);
		if (bad)
			goto out;
	}
	printf("\n'%% of orig' = compressed / input.\n"
	       "enc/dec MB/s are per-thread rates over uncompressed bytes. dec MB/s is\n"
	       "what playback sees (one frame at a time); compare it with the clip's\n"
	       "raw data rate (width * height * 1.5 * fps for 4:2:0 8-bit).\n");
	rc = 0;

out:
	if (have_sm) { sample_free(&sm); free(orig_paths); }
	else free(r.paths);
	free(buf);
	free(fcopy);
	return rc;
}
