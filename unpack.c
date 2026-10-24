/* SPDX-License-Identifier: MIT */
/*
 * The read side of a zfrag archive:
 *
 *   zfrag cat     ARCHIVE                      the tar stream, on stdout
 *   zfrag ls      ARCHIVE                      the files in it (size, name)
 *   zfrag extract [-C DIR] ARCHIVE [PATH...]   some or all files, decoding only the
 *                                              frames they live in
 *   zfrag verify  ARCHIVE [-T LIST] [-C DIR]   check the archive; with a list, also
 *                                              compare it with the files on disk
 *
 * None of them needs root. Plain `zstd -dc ARCHIVE | tar x` also works, with no zfrag.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive.h"
#include "tar.h"
#include "util.h"

/* ---------------------------------------------------------------- */

#define USAGE_CAT     "usage: zfrag cat ARCHIVE   (writes the tar stream to stdout)\n"
#define USAGE_LS      "usage: zfrag ls ARCHIVE\n" \
                      "  lists every file in the archive with its size\n"
#define USAGE_EXTRACT "usage: zfrag extract [-C DIR] [-q] ARCHIVE [PATH...]\n" \
                      "  PATH is a file name in the archive, or a directory prefix ending in /;\n" \
                      "  with no PATH everything is extracted. Only the frames those files are in are decoded.\n"
#define USAGE_VERIFY  "usage: zfrag verify [-T LIST [-C DIR] [--strip-components N]] [-q] ARCHIVE\n" \
                      "  checks the index and that every frame decodes with the right size and checksum;\n" \
                      "  with -T LIST it also compares each listed file on disk (paths relative to -C DIR)\n" \
                      "  with the archive's copy\n"

/* "zfrag COMMAND --help" (or -h): the usage on stdout, exit status 0 */
static int wants_help(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return 1;
	return 0;
}

int zfrag_cat(int argc, char **argv)
{
	if (wants_help(argc, argv)) { fputs(USAGE_CAT, stdout); return 0; }
	zf_archive a;
	uint64_t i;
	int rc = 1;

	if (argc != 2) { fputs(USAGE_CAT, stderr); return 2; }
	if (isatty(STDOUT_FILENO)) { fprintf(stderr, "zfrag cat: stdout is a terminal; pipe it (| tar -t) or redirect it\n"); return 2; }
	if (zf_open(&a, argv[1])) { fprintf(stderr, "zfrag: %s\n", a.err); return 1; }
	for (i = 0; i < a.nframes; i++) {
		const uint8_t *p;
		size_t n;

		if (zf_frame(&a, i, &p, &n)) { fprintf(stderr, "zfrag: %s\n", a.err); goto out; }
		if (fwrite(p, 1, n, stdout) != n) { fprintf(stderr, "zfrag cat: write error\n"); goto out; }
	}
	if (fflush(stdout)) { fprintf(stderr, "zfrag cat: write error\n"); goto out; }
	rc = 0;
out:
	zf_close(&a);
	return rc;
}

int zfrag_ls(int argc, char **argv)
{
	if (wants_help(argc, argv)) { fputs(USAGE_LS, stdout); return 0; }
	zf_archive a;
	uint64_t i;

	if (argc != 2) { fputs(USAGE_LS, stderr); return 2; }
	if (zf_open(&a, argv[1])) { fprintf(stderr, "zfrag: %s\n", a.err); return 1; }
	for (i = 0; i < a.nfiles; i++) {
		zf_rec r;

		zf_rec_get(&a, i, &r);
		printf("%12llu %.*s\n", (unsigned long long)r.size, (int)r.name_len, r.name);
	}
	zf_close(&a);
	return 0;
}

/* ---------------------------------------------------------------- */

static int safe_name(const char *n, size_t len)
{
	size_t k = 0;

	if (!len || n[0] == '/') return 0;
	while (k < len) {
		size_t e = k;
		while (e < len && n[e] != '/') e++;
		if (e == k) return 0;                                   /* empty component */
		if (e - k == 2 && n[k] == '.' && n[k + 1] == '.') return 0;
		if (e - k == 1 && n[k] == '.') return 0;
		if (memchr(n + k, '\0', e - k)) return 0;
		k = e + 1;
	}
	return 1;
}

static int mkdirs_for(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(path, 0777) && errno != EEXIST) { int e = errno; *p = '/'; errno = e; return -1; }
		*p = '/';
	}
	return 0;
}

static int extract_one(zf_archive *a, const zf_rec *r, const char *dir)
{
	char path[PATH_MAX];
	uint64_t off = r->data_off, left = r->size;
	struct timespec ts[2];
	int fd;

	if (!safe_name(r->name, r->name_len)) { fprintf(stderr, "zfrag: refusing unsafe name '%.*s'\n", (int)r->name_len, r->name); return -1; }
	if (snprintf(path, sizeof path, "%s/%.*s", dir, (int)r->name_len, r->name) >= (int)sizeof path) {
		fprintf(stderr, "zfrag: path too long: %.*s\n", (int)r->name_len, r->name);
		return -1;
	}
	if (mkdirs_for(path)) { fprintf(stderr, "zfrag: %s: %s\n", path, strerror(errno)); return -1; }
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0) { fprintf(stderr, "zfrag: %s: %s\n", path, strerror(errno)); return -1; }
	while (left) {
		const uint8_t *p;
		size_t fl, take;
		uint64_t fi = off / a->frame_raw, in = off % a->frame_raw;

		if (zf_frame(a, fi, &p, &fl)) { fprintf(stderr, "zfrag: %s\n", a->err); close(fd); unlink(path); return -1; }
		take = fl - (size_t)in < left ? fl - (size_t)in : (size_t)left;
		for (size_t done = 0; done < take; ) {
			ssize_t w = write(fd, p + in + done, take - done);
			if (w < 0 && errno == EINTR) continue;
			if (w <= 0) { fprintf(stderr, "zfrag: %s: %s\n", path, strerror(errno)); close(fd); unlink(path); return -1; }
			done += (size_t)w;
		}
		off += take; left -= take;
	}
	fchmod(fd, r->mode & 0777);
	ts[0].tv_sec = ts[1].tv_sec = (time_t)r->mtime;
	ts[0].tv_nsec = ts[1].tv_nsec = 0;
	futimens(fd, ts);
	return close(fd);
}

static zf_archive *g_cmp_a;

static int cmp_stream(const void *x, const void *y)
{
	zf_rec a, b;

	zf_rec_get(g_cmp_a, *(const uint64_t *)x, &a);
	zf_rec_get(g_cmp_a, *(const uint64_t *)y, &b);
	return a.data_off < b.data_off ? -1 : a.data_off > b.data_off;
}

int zfrag_extract(int argc, char **argv)
{
	const char *dir = ".", *arch = NULL;
	int quiet = 0, rc = 1, i, bad = 0;
	zf_archive a;
	uint64_t *sel = NULL, nsel = 0, k;
	char **want = NULL;
	int nwant = 0;

	if (wants_help(argc, argv)) { fputs(USAGE_EXTRACT, stdout); return 0; }
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (!arch) arch = argv[i];
		else { want = realloc(want, (size_t)(nwant + 1) * sizeof *want); want[nwant++] = argv[i]; }
	}
	if (!arch) {
		fputs(USAGE_EXTRACT, stderr);
		free(want);
		return 2;
	}
	if (zf_open(&a, arch)) { fprintf(stderr, "zfrag: %s\n", a.err); free(want); return 1; }
	sel = malloc((a.nfiles ? a.nfiles : 1) * sizeof *sel);
	if (!sel) { perror("alloc"); goto out; }
	if (!nwant) {
		for (k = 0; k < a.nfiles; k++) sel[nsel++] = k;
	} else {
		for (i = 0; i < nwant; i++) {
			size_t l = strlen(want[i]);
			int found = 0;

			if (l && want[i][l - 1] == '/') {
				for (k = 0; k < a.nfiles; k++) {
					zf_rec r;
					zf_rec_get(&a, k, &r);
					if (r.name_len >= l && !memcmp(r.name, want[i], l)) { sel[nsel++] = k; found = 1; }
				}
			} else {
				int64_t f = zf_find(&a, want[i], l);
				if (f >= 0) { sel[nsel++] = (uint64_t)f; found = 1; }
			}
			if (!found) { fprintf(stderr, "zfrag: %s: not in the archive\n", want[i]); bad = 1; }
		}
	}
	g_cmp_a = &a;
	qsort(sel, nsel, sizeof *sel, cmp_stream);              /* stream order: each frame is decoded about once */
	for (k = 0; k < nsel; k++) {
		zf_rec r;

		zf_rec_get(&a, sel[k], &r);
		if (extract_one(&a, &r, dir)) bad = 1;
	}
	if (!quiet) fprintf(stderr, "extracted %llu file%s into %s\n", (unsigned long long)nsel, nsel == 1 ? "" : "s", dir);
	rc = bad ? 1 : 0;
out:
	free(sel);
	free(want);
	zf_close(&a);
	return rc;
}

/* ---------------------------------------------------------------- */

/* read up to n bytes (fewer only at end of file or on an error) */
static size_t read_n(int fd, uint8_t *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t x = read(fd, buf + got, n - got);

		if (x < 0 && errno == EINTR) continue;
		if (x <= 0) break;
		got += (size_t)x;
	}
	return got;
}

int zfrag_verify(int argc, char **argv)
{
	const char *arch = NULL, *list = NULL, *dir = NULL;
	int quiet = 0, rc = 1, i, bad = 0;
	zf_archive a;
	uint64_t *ord = NULL, k, ok_frames = 0;
	static uint8_t hbuf[TAR_MAX_HDR + ZF_TAR_BLOCK], zeros[2 * ZF_TAR_BLOCK];
	char *lbuf = NULL, **paths = NULL;
	size_t llen, np = 0;
	unsigned strip = 0;

	if (wants_help(argc, argv)) { fputs(USAGE_VERIFY, stdout); return 0; }
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-T") && i + 1 < argc) list = argv[++i];
		else if (!strcmp(argv[i], "--strip-components") && i + 1 < argc) {
			char *e;
			long v = strtol(argv[++i], &e, 10);
			if (e == argv[i] || *e || v < 0 || v > 64) { fprintf(stderr, "bad --strip-components '%s' (0..64)\n", argv[i]); return 2; }
			strip = (unsigned)v;
		}
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (!arch) arch = argv[i];
		else { arch = NULL; break; }
	}
	if (!arch) { fputs(USAGE_VERIFY, stderr); return 2; }
	if (zf_open(&a, arch)) { fprintf(stderr, "zfrag: %s\n", a.err); return 1; }

	/* 1. every frame decodes, with the right size and checksum */
	for (k = 0; k < a.nframes; k++) {
		const uint8_t *p;
		size_t n;

		if (zf_frame(&a, k, &p, &n)) { fprintf(stderr, "FAIL: %s\n", a.err); bad = 1; break; }
		ok_frames++;
	}

	/* 2. the tar stream ends properly, and every index record matches its header */
	if (!bad) {
		if (a.stream_len < 2 * ZF_TAR_BLOCK || a.stream_len % ZF_TAR_BLOCK ||
		    zf_read(&a, a.stream_len - 2 * ZF_TAR_BLOCK, 2 * ZF_TAR_BLOCK, hbuf) || memcmp(hbuf, zeros, sizeof zeros)) {
			fprintf(stderr, "FAIL: the tar stream does not end with the end-of-archive marker\n");
			bad = 1;
		}
		ord = malloc((a.nfiles ? a.nfiles : 1) * sizeof *ord);
		if (!ord) { perror("alloc"); goto out; }
		for (k = 0; k < a.nfiles; k++) ord[k] = k;
		g_cmp_a = &a;
		qsort(ord, a.nfiles, sizeof *ord, cmp_stream);
		for (k = 0; k < a.nfiles && !bad; k++) {
			zf_rec r;
			tar_info ti;
			size_t want_len;

			zf_rec_get(&a, ord[k], &r);
			want_len = r.hdr_len < sizeof hbuf ? r.hdr_len : sizeof hbuf;
			if (r.hdr_len % ZF_TAR_BLOCK || r.hdr_len > TAR_MAX_HDR || zf_read(&a, r.data_off - r.hdr_len, want_len, hbuf) ||
			    tar_parse(hbuf, want_len, &ti) || ti.hdr_len != r.hdr_len || ti.size != r.size ||
			    strlen(ti.name) != r.name_len || memcmp(ti.name, r.name, r.name_len)) {
				fprintf(stderr, "FAIL: the index and the tar header disagree for '%.*s'\n", (int)r.name_len, r.name);
				bad = 1;
			}
		}
	}

	/* 3. optionally: compare with the files on disk */
	if (list && !bad) {
		uint64_t same = 0, missing = 0, differ = 0;
		static uint8_t fb[1 << 20], ab[1 << 20];

		lbuf = slurp(list, &llen);
		if (!lbuf) { fprintf(stderr, "%s: %s\n", list, strerror(errno)); goto out; }
		np = split_list(lbuf, llen, '\n', &paths);
		if (np == (size_t)-1) { perror("alloc"); np = 0; goto out; }
		for (k = 0; k < np; k++) {
			const char *name;
			size_t nl;
			char full[PATH_MAX];
			int64_t f;
			zf_rec r;
			int fd, differs = 0;
			uint64_t pos = 0;
			struct stat sb;

			if (tar_norm_name(paths[k], &name, &nl)) continue;
			if (strip && tar_strip_name(&name, &nl, strip)) continue;       /* pack skipped it too */
			if (dir) snprintf(full, sizeof full, "%s/%s", dir, paths[k]);
			else snprintf(full, sizeof full, "%s", paths[k]);
			if (lstat(full, &sb) || !S_ISREG(sb.st_mode)) continue;        /* pack skipped it too */
			f = zf_find(&a, name, nl);
			if (f < 0) { if (!quiet) fprintf(stderr, "MISSING from the archive: %s\n", paths[k]); missing++; continue; }
			zf_rec_get(&a, (uint64_t)f, &r);
			fd = open(full, O_RDONLY);
			if (fd < 0) { fprintf(stderr, "cannot read %s\n", full); bad = 1; continue; }
			if ((uint64_t)sb.st_size != r.size) differs = 1;
			while (!differs && pos < r.size) {
				size_t take = r.size - pos < sizeof fb ? (size_t)(r.size - pos) : sizeof fb;
				size_t got = read_n(fd, fb, take);

				if (got != take || zf_read(&a, r.data_off + pos, take, ab) || memcmp(fb, ab, take)) differs = 1;
				pos += take;
			}
			close(fd);
			if (differs) { if (!quiet) fprintf(stderr, "DIFFERS: %s\n", paths[k]); differ++; } else same++;
		}
		if (!quiet) printf("compared with the list: %llu identical, %llu differ, %llu missing\n",
				   (unsigned long long)same, (unsigned long long)differ, (unsigned long long)missing);
		if (differ || missing) bad = 1;
	}

	if (!quiet && !bad) {
		char b[32];

		human(b, sizeof b, a.stream_len);
		char pt[48] = "";

		if (a.nparts > 1) snprintf(pt, sizeof pt, " in %zu parts", a.nparts);
		char lv[24] = "";

		if (a.level) snprintf(lv, sizeof lv, ", level %u", a.level);
		printf("%s: ok: %llu frames, %llu files, %s of tar stream%s%s\n", arch, (unsigned long long)ok_frames,
		       (unsigned long long)a.nfiles, b, lv, pt);
	}
	rc = bad ? 1 : 0;
out:
	free(ord);
	free(paths);
	free(lbuf);
	zf_close(&a);
	return rc;
}
