/* SPDX-License-Identifier: MIT */
/* outset.h - where `pack` and `split` write: one archive file, or numbered parts of one.
 *
 * Everything is written under temporary names (NAME.tmp, NAME.001.tmp, ...) and renamed into place
 * only by outset_commit, so a failed or interrupted run leaves no half-written archive: outset_abort
 * and the signal handler remove every temporary file.
 *
 * Parts are named by zf_part_name (BASE.001, BASE.002, ...). A "unit" (a compressed frame, or the
 * index with its trailer) is never split across two parts: if it would not fit in what is left of
 * the current part, a new part is started first. The offsets given are positions in the whole
 * archive (the parts joined), so the writer needs no knowledge of the parts. */
#ifndef ZFRAG_OUTSET_H
#define ZFRAG_OUTSET_H

#include <stddef.h>
#include <stdint.h>

typedef struct outset outset;

/* base: the archive's name. multi: write BASE.001, BASE.002, ... instead of one file BASE.
 * part_size: with multi, the most a part may hold (0: parts start only when outset_start_part
 * says so). width: digits in the part numbers. force: replace what already exists (a file BASE and
 * any series of parts named BASE.*), else refuse. keep_base: a file named BASE is expected (the
 * archive being split) and is neither a conflict nor ever removed. The first file is created at once, so an
 * unwritable place fails early. Returns NULL with the reason in err. */
outset *outset_new(const char *base, int multi, uint64_t part_size, unsigned width, int force, int keep_base,
		   char *err, size_t errlen);

/* Would outset_new refuse because something is in the way? Checks only, deletes nothing: for
 * failing early, before a long scan. Returns 0, or -1 with the reason. */
int outset_precheck(const char *base, int multi, int force, int keep_base, char *err, size_t errlen);

/* Writes one unit that belongs at offset `off` of the whole archive. Returns 0 or -1. */
int outset_write(outset *o, const void *buf, size_t len, uint64_t off, char *err, size_t errlen);

/* Begins a new part at offset `off` (a no-op if the current part is still empty). */
int outset_start_part(outset *o, uint64_t off, char *err, size_t errlen);

/* Flushes everything and renames the temporary files into place. Returns 0 or -1. */
int outset_commit(outset *o, char *err, size_t errlen);

/* Removes the temporary files and frees the set. Safe after a failed commit. */
void outset_abort(outset *o);
void outset_free(outset *o);

size_t outset_nparts(const outset *o);
const char *outset_part_path(const outset *o, size_t i);       /* the final name */
uint64_t outset_part_bytes(const outset *o, size_t i);

/* From now on SIGINT, SIGTERM and SIGHUP remove the temporary files and exit. */
void outset_install_signals(outset *o);

#endif
