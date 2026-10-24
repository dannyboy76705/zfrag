/* SPDX-License-Identifier: MIT */
/* cdcview.h - a cdcstore repository inside an archive, seen as the files it holds: the model behind the
 * "virtual" half of `zfrag mount`. No FUSE in here.
 *
 * `cdcstore-list -k REPO` lists a repository's chunk files for a sorter, and the archive holds them (and
 * cdcstore.idx) under their repository paths: chunks/bNNNNN/cNNNNNNNNN. This reads cdcstore.idx out of the
 * archive and serves the repository's files from those chunk files, through the archive's decoded-frame
 * cache, so a read decodes only the frames its chunks are in. The repository is looked for at the top of
 * the archive or in a top-level folder (the first one found is used).
 *
 * The index layout is the one in cdcstore-common.h (version 1); it is read here without cdcstore's code.
 * The index's own checksum is not checked here (the archive's frames carry their own); `cdcstore-verify`
 * checks a repository's contents. */
#ifndef ZFRAG_CDCVIEW_H
#define ZFRAG_CDCVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "tree.h"

typedef struct cv_repo cv_repo;

/* Looks for cdcstore.idx. Returns 1 and sets *out (free it with cv_close) if there is a repository,
 * 0 if there is none, -1 with the reason in err if there is one that cannot be used. */
int cv_open(cv_repo **out, zt_tree *t, char *err, size_t errlen);
void cv_close(cv_repo *c);

const char *cv_base(const cv_repo *c);               /* "" or "/folder": where the repository is in the archive */
uint64_t cv_nfiles(const cv_repo *c);
uint64_t cv_nchunks(const cv_repo *c);

typedef struct {
	int is_dir, is_link;
	uint64_t size;
	int64_t mtime;
	uint32_t mode;                  /* permission bits as stored */
	const char *target;             /* links */
	long index;                     /* the entry (-2 for the top directory) */
} cv_info;

/* "/" is the top directory; "/a/b" a path in the repository. Returns 0, or -ENOENT. */
int cv_stat(const cv_repo *c, const char *path, cv_info *out);

/* Calls fn(arg, name) for each entry of a directory. Returns 0, -ENOENT or -ENOTDIR. */
int cv_readdir(const cv_repo *c, const char *path, int (*fn)(void *arg, const char *name, const cv_info *st), void *arg);

/* Reads from a file entry (cv_stat's index). Returns the bytes read (0 at or past the end), or -errno. Thread-safe. */
ssize_t cv_read(cv_repo *c, zt_tree *t, long index, uint64_t off, void *buf, size_t len);

#endif
