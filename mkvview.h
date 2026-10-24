/* SPDX-License-Identifier: MIT */
/* mkvview.h - an mjpegstore set inside an archive, seen as one .mkv file per clip: the "virtual" half of
 * `zfrag mount` for a set. No FUSE in here.
 *
 * The set is what `mjpegmount` mounts from a disk: specs.txt and one folder per clip, each holding the
 * clip's JPEG pictures (in bucket folders b00000, b00001, ...). It is looked for at the top of the archive
 * or in a top-level folder. Each clip appears as <clip>.mkv, the stream `mjpegstore -s` writes (but for the
 * writing-app name in the header, which says mjpegmount), readable at any offset. The Matroska layout comes from mjpegmount.c, which is compiled into zfrag as a
 * library (-DMJPEGMOUNT_AS_LIBRARY, see mjpegmount.h) with the archive as the place the files live, so the
 * two programs cannot disagree. A picture is read through the archive's decoded-frame cache.
 *
 * Progressive sets only, as in mjpegmount. One set per process. */
#ifndef ZFRAG_MKVVIEW_H
#define ZFRAG_MKVVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "tree.h"

/* Looks for a set. Returns 1 and opens it (messages about skipped clips go to stderr; describe gets a short
 * summary and base where it was found: "" or "/folder"), 0 if there is none, -1 if there is one that cannot be
 * used (the reason is on stderr). */
int mv_open(zt_tree *t, char *describe, size_t dn, char *base, size_t bn);
void mv_close(void);

/* The same functions as mjpegmount.h, under shorter names. */
size_t mv_nclips(void);
const char *mv_clip_name(size_t i);
void *mv_find(const char *path);                  /* "/<clip>.mkv" */
int mv_ready(void *clip);                         /* 0, or -EIO */
uint64_t mv_size(void *clip);
int64_t mv_mtime(void *clip);
ssize_t mv_read(void *clip, void *buf, size_t size, uint64_t off);

#endif
