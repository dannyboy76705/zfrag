/* SPDX-License-Identifier: MIT */
/* tar.h - writing and reading the ustar/pax headers zfrag puts in its stream.
 * Only regular files are stored. Ownership is written as 0/0 with no names, so
 * an archive does not depend on who made it. */
#ifndef ZFRAG_TAR_H
#define ZFRAG_TAR_H

#include <stddef.h>
#include <stdint.h>

#define TAR_MAX_HDR (16 * 1024)         /* room for pax + ustar header of a PATH_MAX path */
#define TAR_NAME_MAX 8192

/* Writes the header block(s) for a regular file: a pax extended header first if
 * the path or size does not fit ustar, then the ustar header. Returns the number
 * of bytes written (a multiple of 512), or 0 if out is too small. */
size_t tar_header(uint8_t *out, size_t cap, const char *path, size_t plen,
		  uint64_t size, int64_t mtime, unsigned mode);

typedef struct {
	char name[TAR_NAME_MAX];
	uint64_t size;
	size_t hdr_len;                 /* bytes of header blocks, pax included */
	int type;                       /* typeflag of the real header */
} tar_info;

/* Parses what tar_header wrote (or any ustar/pax header) from buf[0..len).
 * Returns 0 on success, -1 on a malformed header. */
int tar_parse(const uint8_t *buf, size_t len, tar_info *ti);

/* The name stored for a list path: leading "/" and "./" dropped. Returns -1 for an
 * empty name or one with a ".." component (never stored: it could escape on extraction). */
int tar_norm_name(const char *p, const char **out, size_t *len);

/* Drops the first n path components of a name already cleaned by tar_norm_name ("dir/game1/x"
 * with n=1 becomes "game1/x"), like tar's --strip-components. Returns -1 if nothing is left. */
int tar_strip_name(const char **name, size_t *len, unsigned n);

#endif
