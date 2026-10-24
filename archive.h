/* SPDX-License-Identifier: MIT */
/* archive.h - reading a zfrag archive: open, find a file, read a range of the
 * tar stream. Everything read from the file is checked before it is trusted. */
#ifndef ZFRAG_ARCHIVE_H
#define ZFRAG_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>
#include <zstd.h>

#include "zfrag_format.h"

static inline void put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void put_le64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint32_t get_le32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 3; i >= 0; i--) v = v << 8 | p[i];
	return v;
}
static inline uint64_t get_le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
	return v;
}

typedef struct {
	uint64_t data_off, size;
	int64_t mtime;
	uint32_t mode, hdr_len;
	const char *name;               /* not NUL-terminated */
	uint32_t name_len;
} zf_rec;

/* An archive is one file, or several files read as one: the parts of a split archive (made
 * with `split`: set.tar.zfrag.00, .01, ... or .aa, .ab, ...), joined in order. */
typedef struct { int fd; uint64_t start, size; } zf_part;

typedef struct {
	zf_part *parts;
	size_t nparts;
	int64_t mtime;                  /* of the newest part */
	char base[1024];                /* the archive's name: the file, or the parts' common name without .NNN */
	char series[320];               /* "" for a single file, else a description of the parts */
	uint32_t level;                 /* zstd level recorded by pack; 0 if not recorded */
	uint64_t filesize, frame_raw, stream_len, nframes, nfiles, names_len, index_off;
	uint64_t gap;                   /* bytes between the end of the index frame and the trailer: padding, or 0 */
	uint8_t *idx;                   /* the whole index payload */
	const uint8_t *foff, *files, *names;
	uint8_t *cbuf, *rbuf;
	size_t ccap;
	int64_t cached;                 /* frame held in rbuf, or -1 */
	ZSTD_DCtx *dctx;
	char err[1024];
} zf_archive;

/* How parts are named: BASE.001, BASE.002, ... (ZF_PART_WIDTH digits, counting from ZF_PART_FIRST,
 * so part 001 is disc 1). One function names them for `pack --split` and `zfrag split`; the reader
 * also accepts what `split -d` (.00, .01 or .000) and plain `split` (.aa, .ab) write. */
#define ZF_PART_WIDTH 3
#define ZF_PART_FIRST 1
void zf_part_name(char *buf, size_t cap, const char *base, unsigned n, unsigned width);

/* Lists the existing files named BASE.<suffix> of any series (digits or lowercase letters), for
 * refusing to write over them, or removing them with --force. Returns 0 (n may be 0) or -1. */
int zf_find_series_files(const char *base, char ***paths, size_t *n);
void zf_free_paths(char **paths, size_t n);

/* Reads bytes [off, off+n) of the whole archive, whichever parts they are in. Thread-safe. */
int zf_pread(const zf_archive *a, void *buf, size_t n, uint64_t off);

/* Opens an archive. `path` is a file, or a split archive: its normal name (set.tar.zfrag, which does
 * not exist as a file when it is split) or any one of its parts (set.tar.zfrag.01); the other parts
 * are found next to it. The stem must end in .tar.zfrag, the name `zfrag pack` requires.
 * Returns 0, or -1 with the reason in a->err. */
int zf_open(zf_archive *a, const char *path);
void zf_close(zf_archive *a);
void zf_rec_get(const zf_archive *a, uint64_t i, zf_rec *r);
int64_t zf_find(const zf_archive *a, const char *path, size_t len);   /* record index, or -1 */
uint64_t zf_frame_len(const zf_archive *a, uint64_t i);               /* raw bytes in frame i */
int zf_frame(zf_archive *a, uint64_t i, const uint8_t **p, size_t *n);
int zf_read(zf_archive *a, uint64_t off, uint64_t n, uint8_t *dst);   /* a range of the tar stream */

/* Thread-safe decoding, for readers that decode several frames at once (the mount):
 * each thread (or cache slot) owns a zf_decoder, and zf_decode_frame touches only the
 * archive's read-only index, pread()s the compressed bytes and uses only the decoder.
 * `out` needs zf_frame_len(a, i) bytes. Returns 0, or -1 with the reason in err. */
typedef struct { ZSTD_DCtx *dctx; } zf_decoder;
int zf_decoder_init(zf_decoder *d);
void zf_decoder_free(zf_decoder *d);
int zf_decode_frame(const zf_archive *a, uint64_t i, zf_decoder *d, uint8_t *out, char *err, size_t errlen);

#endif
