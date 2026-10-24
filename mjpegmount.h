/*
 * mjpegmount.h - mjpegmount.c used as a library (compile it with -DMJPEGMOUNT_AS_LIBRARY).
 *
 * The same code that `mjpegmount` runs, without FUSE and without the disk: the place the set lives in is a
 * mj_source, so a program can serve a set from anywhere (zfrag mount serves one from inside an archive).
 * One set at a time per process. MIT License, Copyright (c) 2026 Daniel Lee Witzel.
 */
#ifndef MJPEGMOUNT_H
#define MJPEGMOUNT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Must match the definition in mjpegmount.c. Paths are "dir/name" under the set directory given to mjl_open. */
#ifndef MJ_SOURCE_DEFINED
#define MJ_SOURCE_DEFINED
typedef struct mj_source {
    /* calls cb(arg, name, is_dir, size) for each regular file and directory in dir; a nonzero return from cb
     * stops it. Returns 0, or -errno. */
    int (*list)(void *ctx, const char *dir, int (*cb)(void *arg, const char *name, int is_dir, uint64_t size), void *arg);
    /* kind: 1 = file, 2 = directory. 0, or -errno. */
    int (*stat)(void *ctx, const char *path, int *kind, uint64_t *size, int64_t *mtime);
    /* exactly n bytes at off, or -errno (-EIO if the file is shorter) */
    int (*pread)(void *ctx, const char *path, void *buf, size_t n, uint64_t off);
    void *ctx;
} mj_source;
#endif

/* 1 if setdir looks like a set (a specs.txt, and subdirectories that hold pictures), else 0. Prints nothing. */
int mjl_probe(const mj_source *src, const char *setdir);
/* Reads specs.txt and finds the clips. 0, or -1 (the reason is on stderr). describe gets a short summary. */
int mjl_open(const mj_source *src, const char *setdir, char *describe, size_t dn);
void mjl_close(void);

size_t mjl_nclips(void);
const char *mjl_clip_name(size_t i);              /* "<clip>.mkv" */
void *mjl_find(const char *path);                 /* "/<clip>.mkv", else NULL */
int mjl_ready(void *clip);                        /* indexes the clip the first time: 0, or -EIO */
uint64_t mjl_size(void *clip);                    /* size of the .mkv; valid after mjl_ready */
int64_t mjl_mtime(void *clip);
ssize_t mjl_read(void *clip, void *buf, size_t size, uint64_t off);   /* bytes read (0 at the end), or -errno; thread-safe */

#endif
