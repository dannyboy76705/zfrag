/* SPDX-License-Identifier: MIT */
/*
 * zfrag split - cut an existing archive into numbered parts, without recompressing anything.
 *
 *   zfrag split --size SIZE [-d DIR] [--force] [-q] ARCHIVE
 *
 * The parts are named like the ones `pack --split` writes (ARCHIVE.001, ARCHIVE.002, ...) and end
 * between frames, so joining them with cat gives back exactly the original archive, and every
 * command reads them as one archive. The original is left alone. -d writes the parts into another
 * directory (".", "./" and "dir/" all work); without it they go beside the archive.
 *
 * ARCHIVE may itself be a split archive (re-splitting it to another size): its parts are read as
 * one. Writing over the archive's own parts is refused.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive.h"
#include "outset.h"
#include "util.h"
#include "zfrag_format.h"

static void usage(void)
{
	fprintf(stderr,
"usage: zfrag split --size SIZE [-d DIR] [--force] [-q] ARCHIVE\n"
"  --size SIZE   the most a part may hold: 4300M, 4G, ... (required). Parts end between frames, so\n"
"                a part can be up to one frame short of SIZE\n"
"  -d DIR        write the parts into DIR (default: beside ARCHIVE). \".\" and \"./\" mean the\n"
"                current directory\n"
"  --force       replace parts of the same name that already exist\n"
"  -q            no summary\n"
"The parts are named ARCHIVE.001, ARCHIVE.002, ... ; cat ARCHIVE.0* gives back the original.\n"
"ARCHIVE is left in place. Every zfrag command reads the parts as one archive: name the archive\n"
"as before (ARCHIVE) or any one part.\n");
}

static unsigned digits(size_t n)
{
	unsigned d = 1;

	while (n >= 10) { n /= 10; d++; }
	return d;
}

int zfrag_split(int argc, char **argv)
{
	const char *arch = NULL, *dir = NULL;
	uint64_t size = 0;
	int force = 0, quiet = 0, rc = 1;
	zf_archive a, b;
	outset *os = NULL;
	uint64_t *starts = NULL;
	size_t nstarts = 0, k;
	char err[512], outbase[4096], dirbuf[4096];
	const char *name;
	uint8_t *buf = NULL;
	int have_a = 0, have_b = 0, committed = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (parse_size(argv[++i], &size)) { fprintf(stderr, "bad --size '%s' (a size such as 4300M or 4G)\n", argv[i]); return 2; }
		}
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--force")) force = 1;
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
		else if (!arch) arch = argv[i];
		else { usage(); return 2; }
	}
	if (!arch || !size) { usage(); return 2; }

	if (zf_open(&a, arch)) { fprintf(stderr, "zfrag split: %s\n", a.err); return 1; }
	have_a = 1;
	{
		const char *sl = strrchr(a.base, '/');
		size_t zl = strlen(ZF_SUFFIX), nl;

		name = sl ? sl + 1 : a.base;
		nl = strlen(name);
		if (nl < zl || strcmp(name + nl - zl, ZF_SUFFIX)) {
			fprintf(stderr, "zfrag split: the archive's name must end in %s so that its parts can be found again (rename it first)\n", ZF_SUFFIX);
			goto out;
		}
		if (dir) {
			snprintf(dirbuf, sizeof dirbuf, "%s", dir);
			for (size_t l = strlen(dirbuf); l > 1 && dirbuf[l - 1] == '/'; ) dirbuf[--l] = '\0';
			if (!*dirbuf) snprintf(dirbuf, sizeof dirbuf, ".");
			snprintf(outbase, sizeof outbase, "%s/%s", dirbuf, name);
		} else {
			snprintf(outbase, sizeof outbase, "%s", a.base);
		}
	}
	if (dir) {
		struct stat sb;

		if (stat(dirbuf, &sb) || !S_ISDIR(sb.st_mode)) { fprintf(stderr, "zfrag split: %s: %s\n", dirbuf, errno ? strerror(errno) : "not a directory"); goto out; }
	}

	/* where each part starts: at a frame boundary, as late as the size allows. The index and trailer
	 * are a unit too: they go in the last part if they fit, else in a part of their own. */
	starts = malloc((a.nframes + 2) * sizeof *starts);
	if (!starts) { fprintf(stderr, "zfrag split: out of memory\n"); goto out; }
	starts[nstarts++] = 0;
	{
		uint64_t start = 0;

		for (uint64_t i = 0; i < a.nframes; i++) {
			uint64_t fs = get_le64(a.foff + 8 * i), fe = get_le64(a.foff + 8 * (i + 1));

			if (fe - start > size) {
				if (fs == start) {
					fprintf(stderr, "zfrag split: frame %llu alone is %llu bytes: --size must be larger than that\n",
						(unsigned long long)i, (unsigned long long)(fe - fs));
					goto out;
				}
				start = fs;
				starts[nstarts++] = start;
				if (fe - start > size) {
					fprintf(stderr, "zfrag split: frame %llu alone is %llu bytes: --size must be larger than that\n",
						(unsigned long long)i, (unsigned long long)(fe - fs));
					goto out;
				}
			}
		}
		if (a.filesize - start > size) {
			uint64_t is = get_le64(a.foff + 8 * a.nframes);

			if (is == start || a.filesize - is > size) {
				fprintf(stderr, "zfrag split: the index and trailer take %llu bytes: --size must be larger than that\n",
					(unsigned long long)(a.filesize - is));
				goto out;
			}
			starts[nstarts++] = is;
		}
	}

	/* never write over the archive's own parts */
	{
		char **ex = NULL;
		size_t nex = 0;

		if (zf_find_series_files(outbase, &ex, &nex)) { fprintf(stderr, "zfrag split: cannot look for existing parts\n"); goto out; }
		for (size_t e = 0; e < nex; e++) {
			struct stat sx;

			if (stat(ex[e], &sx)) continue;
			for (size_t p = 0; p < a.nparts; p++) {
				struct stat sp;

				if (fstat(a.parts[p].fd, &sp) == 0 && sp.st_dev == sx.st_dev && sp.st_ino == sx.st_ino) {
					fprintf(stderr, "zfrag split: %s is one of the archive's own parts and would be overwritten: write to another directory with -d\n", ex[e]);
					zf_free_paths(ex, nex);
					goto out;
				}
			}
		}
		zf_free_paths(ex, nex);
	}

	os = outset_new(outbase, 1, 0, nstarts > 999 ? digits(nstarts) : ZF_PART_WIDTH, force, 1, err, sizeof err);
	if (!os) { fprintf(stderr, "zfrag split: %s\n", err); goto out; }
	outset_install_signals(os);
	buf = malloc(4u << 20);
	if (!buf) { fprintf(stderr, "zfrag split: out of memory\n"); goto out; }
	for (k = 0; k < nstarts; k++) {
		uint64_t from = starts[k], to = k + 1 < nstarts ? starts[k + 1] : a.filesize;

		if (k && outset_start_part(os, from, err, sizeof err)) { fprintf(stderr, "zfrag split: %s\n", err); goto out; }
		for (uint64_t off = from; off < to; ) {
			size_t take = to - off < (4u << 20) ? (size_t)(to - off) : (4u << 20);

			if (zf_pread(&a, buf, take, off)) { fprintf(stderr, "zfrag split: cannot read the archive at offset %llu\n", (unsigned long long)off); goto out; }
			if (outset_write(os, buf, take, off, err, sizeof err)) { fprintf(stderr, "zfrag split: %s\n", err); goto out; }
			off += take;
		}
	}
	if (outset_commit(os, err, sizeof err)) { fprintf(stderr, "zfrag split: %s\n", err); goto out; }
	committed = 1;

	/* the parts, opened as an archive, must be the original: the same size, the same index */
	{
		char first[4200];

		zf_part_name(first, sizeof first, outbase, ZF_PART_FIRST, nstarts > 999 ? digits(nstarts) : ZF_PART_WIDTH);
		if (zf_open(&b, first)) { fprintf(stderr, "zfrag split: the parts do not open as an archive: %s\n", b.err); goto out; }
		have_b = 1;
		if (b.filesize != a.filesize || b.nparts != nstarts || b.nframes != a.nframes || b.nfiles != a.nfiles) {
			fprintf(stderr, "zfrag split: the parts do not add up to the original archive (internal error)\n");
			goto out;
		}
	}
	if (!quiet) {
		char sz[32], big[32], last[32];
		size_t bi = 0;

		for (k = 1; k < nstarts; k++) if (outset_part_bytes(os, k) > outset_part_bytes(os, bi)) bi = k;
		human(sz, sizeof sz, a.filesize);
		human(big, sizeof big, outset_part_bytes(os, bi));
		human(last, sizeof last, outset_part_bytes(os, nstarts - 1));
		printf("%s (%s) split into %zu part%s: %s .. %s, the largest %s, the last %s\n", name, sz, nstarts, nstarts == 1 ? "" : "s",
		       outset_part_path(os, 0), outset_part_path(os, nstarts - 1), big, last);
		printf("the original archive was not removed; cat %s.0* gives it back\n", outbase);
	}
	rc = 0;
out:
	if (os) { if (committed) outset_free(os); else outset_abort(os); }
	if (have_b) zf_close(&b);
	if (have_a) zf_close(&a);
	free(starts);
	free(buf);
	return rc;
}
