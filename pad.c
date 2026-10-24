/* SPDX-License-Identifier: MIT */
/*
 * pad.c - zfrag pad: make an existing archive's size a multiple of ZF_ALIGN, by appending.
 *
 * A loop device holds whole 512-byte sectors and drops a partial last one, which would drop the
 * archive's trailer, so an archive of any other size cannot sit whole on a loop device. pack pads new
 * archives; this does it for ones that were made without (by an older zfrag, or with --no-pad).
 *
 * Nothing is rewritten. A padding frame and a copy of the trailer are appended, in one write, to
 * the archive's last part; the old trailer stays where it was, inside the file, as a skippable
 * frame that readers skip (see zfrag_format.h). If the write fails the file is cut back to its old
 * length, so an interrupted run cannot damage the archive.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "archive.h"

#define USAGE \
	"usage: zfrag pad [-q] ARCHIVE\n" \
	"  Appends a padding frame and a copy of the trailer to the archive (to its last part, if it is in\n" \
	"  parts) so that its size is a multiple of 4096 bytes. A loop device drops a partial last sector,\n" \
	"  so an archive of any other size cannot sit whole on a loop device. pack pads new archives already.\n" \
	"  Nothing is recompressed or rewritten: bytes are only appended, and the old trailer stays in the\n" \
	"  file as a skippable frame that every reader skips.\n"

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int zfrag_pad(int argc, char **argv)
{
	const char *arch = NULL;
	int quiet = 0, rc = 1, fd = -1, i;
	zf_archive a;
	uint8_t *buf = NULL;
	uint64_t padlen, before, last_size;
	char link[64];
	size_t len;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { fputs(USAGE, stdout); return 0; }
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (!arch) arch = argv[i];
		else { arch = NULL; break; }
	}
	if (!arch) { fputs(USAGE, stderr); return 2; }
	if (zf_open(&a, arch)) { fprintf(stderr, "zfrag pad: %s\n", a.err); return 1; }
	before = a.filesize;
	if (before % ZF_ALIGN == 0) {
		if (!quiet) printf("%s: %llu bytes, already a multiple of %u: nothing to do\n", arch, (unsigned long long)before, ZF_ALIGN);
		zf_close(&a);
		return 0;
	}

	/* the new frame is 8 + padlen bytes and the trailer 40: the file then ends on a multiple of ZF_ALIGN */
	padlen = (ZF_ALIGN - (before + 8 + ZF_TRAILER_SIZE) % ZF_ALIGN) % ZF_ALIGN;
	len = (size_t)(8 + padlen + ZF_TRAILER_SIZE);
	buf = calloc(1, len);
	if (!buf) { fprintf(stderr, "zfrag pad: out of memory\n"); goto out; }
	put32(buf, ZF_SKIP_PAD);
	put32(buf + 4, (uint32_t)padlen);
	if (zf_pread(&a, buf + 8 + padlen, ZF_TRAILER_SIZE, before - ZF_TRAILER_SIZE)) { fprintf(stderr, "zfrag pad: cannot read the trailer\n"); goto out; }

	/* reopen the last part for writing through the descriptor the reader already holds: it is the same
	 * file even if the name has been moved since */
	snprintf(link, sizeof link, "/proc/self/fd/%d", a.parts[a.nparts - 1].fd);
	fd = open(link, O_RDWR);
	if (fd < 0) { fprintf(stderr, "zfrag pad: cannot open the archive for writing: %s\n", strerror(errno)); goto out; }
	last_size = (uint64_t)lseek(fd, 0, SEEK_END);
	if (last_size != a.parts[a.nparts - 1].size) { fprintf(stderr, "zfrag pad: the archive changed while it was being read\n"); goto out; }
	if (write(fd, buf, len) != (ssize_t)len || fsync(fd)) {
		fprintf(stderr, "zfrag pad: writing failed: %s; the archive is left as it was\n", errno ? strerror(errno) : "short write");
		if (ftruncate(fd, (off_t)last_size)) fprintf(stderr, "zfrag pad: cannot cut the file back: %s\n", strerror(errno));
		goto out;
	}
	close(fd);
	fd = -1;
	zf_close(&a);

	/* check the result the way any reader would see it */
	if (zf_open(&a, arch)) { fprintf(stderr, "zfrag pad: the padded archive does not open: %s\n", a.err); goto out; }
	if (a.filesize % ZF_ALIGN != 0) { fprintf(stderr, "zfrag pad: the padded archive is not a multiple of %u bytes\n", ZF_ALIGN); goto out; }
	if (!quiet) printf("%s: padded from %llu to %llu bytes (+%llu)\n", arch, (unsigned long long)before, (unsigned long long)a.filesize,
			   (unsigned long long)(a.filesize - before));
	rc = 0;
out:
	if (fd >= 0) close(fd);
	free(buf);
	zf_close(&a);
	return rc;
}
