/* SPDX-License-Identifier: MIT */
/* tree.h - an archive seen as a read-only directory tree: the model behind `zfrag mount`.
 *
 * The archive stores only files, by path. The tree adds the directories those paths imply,
 * gives every directory its entries in name order (so a lookup is a binary search, even in a
 * directory of 35,000 files), and reads file data through a cache of decoded frames. No FUSE
 * in here: the mount code is a thin layer over this, and this is what the tests drive.
 *
 * The tree shows the archive's names as they are stored. An archive packed with
 * `pack --strip-components N` holds what is inside a directory, so mounting it at that
 * directory's name puts every file back at the path the list used.
 *
 * Names that cannot be shown (the same path twice, a path that is both a file and a directory,
 * a ".." component) are left out and counted in `hidden`. */
#ifndef ZFRAG_TREE_H
#define ZFRAG_TREE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "archive.h"
#include "fcache.h"

typedef struct zt_node {
	char *name;                     /* one path component, NUL-terminated ("" for the top) */
	struct zt_node **kids;          /* directories: entries sorted by name */
	uint32_t nkids, capkids;
	uint64_t data_off, size;        /* files: where the data starts in the tar stream, and its length */
	int64_t mtime;
	uint32_t mode;                  /* permission bits as stored (directories: 0555) */
	uint32_t seq;                   /* creation order, to keep the first of two equal names */
	unsigned char is_dir;
} zt_node;

typedef struct {
	zf_archive ar;
	fcache *fc;
	zt_node *top;                   /* the root directory */
	uint64_t nfiles, ndirs;
	uint64_t hidden;                /* archive entries left out (see above) */
	int64_t archive_mtime;
	uint64_t seq;
} zt_tree;

/* The cache size to ask for when the caller has no number: one decoded frame per thread (so the memory
 * used for decoding follows the thread count, which follows the CPUs), but not fewer than
 * ZT_AUTO_MIN_SLOTS: playing one component clip keeps three frames in use at once. */
#define ZT_CACHE_AUTO       ((size_t)-1)
#define ZT_AUTO_MIN_SLOTS   4

/* cache_mib: how much decoded data to keep (0 = the minimum, two frames; ZT_CACHE_AUTO as above).
 * threads: how many frames may be decoded at once (at least 1). Returns 0, or -1 with a message in
 * err. A file that is not a zfrag archive gives "not a zfrag archive: ...". */
int zt_open(zt_tree *t, const char *archive, size_t cache_mib, unsigned threads, char *err, size_t errlen);
void zt_close(zt_tree *t);

/* "/" is the top directory; "/a/b" a path below it. NULL if there is no such entry. */
const zt_node *zt_lookup(const zt_tree *t, const char *path);

/* Reads from a file node. Returns the bytes read (0 at or past the end), or -errno. Thread-safe. */
ssize_t zt_read(zt_tree *t, const zt_node *n, uint64_t off, void *buf, size_t len);

#endif
