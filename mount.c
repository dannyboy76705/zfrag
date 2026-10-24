/* SPDX-License-Identifier: MIT */
/*
 * zfrag mount - an archive as a read-only directory tree (libfuse3).
 *
 *   zfrag mount [-f] [-d] [-s] [-o OPTIONS] ARCHIVE MOUNTPOINT
 *   fusermount3 -u MOUNTPOINT
 *
 * Options after -o: cache=MIB (decoded frames kept in memory, default 256), threads=N, and any libfuse
 * option (allow_other, ...), which is passed through. The mount is always read-only.
 *
 * If the archive holds a cdcstore repository (cdcstore.idx and its chunks/ folder, at the top or in a
 * top-level folder), the mount has two halves, served by this one process and one frame cache:
 *   MOUNTPOINT/native/    the archive's own files (the index and the chunk files)
 *   MOUNTPOINT/virtual/   the files the repository holds, with their paths, modes, times and links
 * An mjpegstore set (specs.txt and a folder of pictures per clip) is found the same way, and virtual/ then holds
 * one <clip>.mkv per clip (mkvview.h). Otherwise the archive's files are at the top of the mount, as they are
 * stored. See cdcview.h.
 *
 * The files appear at the paths they are stored under. An archive packed with
 * `zfrag pack --strip-components N` holds what is inside a directory, so mounting it at that
 * directory's name puts every file back at the path the list used.
 *
 * All the logic is in tree.c (tree, lookups, cached reads) and is tested without FUSE; this file
 * only translates libfuse3 calls into those functions. A file that is not a zfrag archive,
 * a plain .tar.zst for instance, is refused with "not a zfrag archive".
 */
#define _GNU_SOURCE
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <fuse.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "threads.h"
#include "tree.h"
#include "cdcview.h"
#include "mkvview.h"

/* libfuse 3.14 honours -o max_threads=N (checked: with N=3 the daemon peaks at 3 workers plus the main
 * thread); older versions are left to their own default, and the decoder pool and the cache are bounded
 * by the thread count either way. */
#if defined(FUSE_MAKE_VERSION) && defined(FUSE_VERSION)
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 14)
#define ZM_HAVE_MAX_THREADS 1
#endif
#endif

static zt_tree g_tree;
static uid_t g_uid;
static gid_t g_gid;
static cv_repo *g_cv;                   /* a cdcstore repository found in the archive: native/ and virtual/ */
static int g_mkv;                       /* or an mjpegstore set: virtual/ holds one .mkv per clip */

/* ---------------------------------------------------------------- */
/* libfuse3 operations                                               */
/* ---------------------------------------------------------------- */

static void fill_stat(const zt_node *n, struct stat *st)
{
	memset(st, 0, sizeof *st);
	if (n->is_dir) {
		uint32_t sub = 0;

		for (uint32_t i = 0; i < n->nkids; i++) sub += n->kids[i]->is_dir;
		st->st_mode = S_IFDIR | (n->mode ? n->mode : 0555);
		st->st_nlink = 2 + sub;                 /* the convention: tools like find rely on it */
	} else {
		st->st_mode = S_IFREG | (n->mode & 0777);
		st->st_nlink = 1;
		st->st_size = (off_t)n->size;
		st->st_blocks = (blkcnt_t)((n->size + 511) / 512);
	}
	st->st_blksize = 4096;
	st->st_uid = g_uid;                             /* the user who mounted it, so they can read everything */
	st->st_gid = g_gid;
	st->st_mtime = (time_t)n->mtime;
	st->st_atime = st->st_ctime = (time_t)n->mtime;
}

/* ---- the views: with a cdcstore repository inside, "/" holds native and virtual ---- */

enum { V_NATIVE, V_CDC, V_ROOT, V_NONE };

static int sub_of(const char *path, const char *name, size_t n, const char **sub)
{
	if (strncmp(path, name, n) || (path[n] && path[n] != '/')) return 0;
	*sub = path[n] ? path + n : "/";
	return 1;
}

static int route(const char *path, const char **sub)
{
	*sub = path;
	if (!g_cv && !g_mkv) return V_NATIVE;
	if (!strcmp(path, "/")) return V_ROOT;
	if (sub_of(path, "/native", 7, sub)) return V_NATIVE;
	if (sub_of(path, "/virtual", 8, sub)) return V_CDC;
	return V_NONE;
}

static void fill_dir_stat(struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFDIR | 0555;
	st->st_nlink = 2 + ((g_cv || g_mkv) ? 2 : 0);
	st->st_blksize = 4096;
	st->st_uid = g_uid;
	st->st_gid = g_gid;
	st->st_mtime = st->st_atime = st->st_ctime = (time_t)g_tree.archive_mtime;
}

static void fill_stat_cv(const cv_info *c, struct stat *st)
{
	memset(st, 0, sizeof *st);
	if (c->is_dir) { st->st_mode = S_IFDIR | ((c->mode & ~(mode_t)0222) | 0500); st->st_nlink = 2; }
	else if (c->is_link) { st->st_mode = S_IFLNK | 0777; st->st_nlink = 1; st->st_size = (off_t)c->size; }
	else {
		st->st_mode = S_IFREG | (c->mode & 0777 & ~(mode_t)0222);
		st->st_nlink = 1;
		st->st_size = (off_t)c->size;
		st->st_blocks = (blkcnt_t)((c->size + 511) / 512);
	}
	st->st_blksize = 4096;
	st->st_uid = g_uid;
	st->st_gid = g_gid;
	st->st_mtime = st->st_atime = st->st_ctime = (time_t)c->mtime;
}

/* ---- an mjpegstore set: virtual/ is a folder of <clip>.mkv files ---- */

static int mkv_getattr(const char *sub, struct stat *st)
{
	void *c;

	if (!strcmp(sub, "/")) { fill_dir_stat(st); return 0; }
	c = mv_find(sub);
	if (!c) return -ENOENT;
	if (mv_ready(c)) return -EIO;
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFREG | 0444;
	st->st_nlink = 1;
	st->st_size = (off_t)mv_size(c);
	st->st_blocks = (blkcnt_t)((mv_size(c) + 511) / 512);
	st->st_blksize = 4096;
	st->st_uid = g_uid;
	st->st_gid = g_gid;
	st->st_mtime = st->st_atime = st->st_ctime = (time_t)mv_mtime(c);
	return 0;
}

static int zm_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	const char *sub;
	const zt_node *n;
	cv_info ci;
	int v = route(path, &sub), r;

	(void)fi;
	if (v == V_ROOT) { fill_dir_stat(st); return 0; }
	if (v == V_CDC && g_mkv) return mkv_getattr(sub, st);
	if (v == V_CDC) {
		r = cv_stat(g_cv, sub, &ci);
		if (r) return r;
		fill_stat_cv(&ci, st);
		return 0;
	}
	if (v == V_NONE) return -ENOENT;
	n = zt_lookup(&g_tree, sub);
	if (!n) return -ENOENT;
	fill_stat(n, st);
	return 0;
}

static int cv_fill(void *arg, const char *name, const cv_info *in)
{
	struct { void *buf; fuse_fill_dir_t filler; } *a = arg;

	(void)in;
	return a->filler(a->buf, name, NULL, 0, 0);
}

static int zm_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
		      struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	const char *sub;
	const zt_node *d;
	int v = route(path, &sub);

	(void)off; (void)fi; (void)flags;
	if (v == V_NONE) return -ENOENT;
	if (v == V_ROOT) {
		if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) return 0;
		if (filler(buf, "native", NULL, 0, 0)) return 0;
		filler(buf, "virtual", NULL, 0, 0);
		return 0;
	}
	if (v == V_CDC && g_mkv) {
		if (strcmp(sub, "/") != 0) return mv_find(sub) ? -ENOTDIR : -ENOENT;
		if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) return 0;
		for (size_t i = 0; i < mv_nclips(); i++)
			if (filler(buf, mv_clip_name(i), NULL, 0, 0)) break;
		return 0;
	}
	if (v == V_CDC) {
		struct { void *buf; fuse_fill_dir_t filler; } a = { buf, filler };

		cv_info ci;
		int r = cv_stat(g_cv, sub, &ci);

		if (r) return r;
		if (!ci.is_dir) return -ENOTDIR;
		if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) return 0;
		return cv_readdir(g_cv, sub, cv_fill, &a);
	}
	d = zt_lookup(&g_tree, sub);
	if (!d) return -ENOENT;
	if (!d->is_dir) return -ENOTDIR;
	if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) return 0;
	for (uint32_t i = 0; i < d->nkids; i++)
		if (filler(buf, d->kids[i]->name, NULL, 0, 0)) break;
	return 0;
}

static int zm_readlink(const char *path, char *buf, size_t size)
{
	const char *sub;
	cv_info ci;
	int v = route(path, &sub), r;

	if (v == V_NONE) return -ENOENT;
	if (v != V_CDC || g_mkv) return -EINVAL;        /* nothing in the archive itself, or in a set, is a link */
	r = cv_stat(g_cv, sub, &ci);
	if (r) return r;
	if (!ci.is_link) return -EINVAL;
	snprintf(buf, size, "%s", ci.target);
	return 0;
}

/* a handle is the archive node's address (even), or for the virtual half the entry number: (index + 1) << 1 | 1 */
#define CV_FH(index) ((((uint64_t)(index) + 1) << 1) | 1)

static int zm_open(const char *path, struct fuse_file_info *fi)
{
	const char *sub;
	const zt_node *n;
	cv_info ci;
	int v = route(path, &sub), r;

	if (v == V_NONE) return -ENOENT;
	if (v == V_ROOT) return -EISDIR;
	if (v == V_CDC && g_mkv) {
		void *c;

		if (!strcmp(sub, "/")) return -EISDIR;
		c = mv_find(sub);
		if (!c) return -ENOENT;
		if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
		if (mv_ready(c)) return -EIO;
		fi->fh = (uint64_t)(uintptr_t)c;
		fi->keep_cache = 1;
		return 0;
	}
	if (v == V_CDC) {
		r = cv_stat(g_cv, sub, &ci);
		if (r) return r;
		if (ci.is_dir) return -EISDIR;
		if (ci.is_link) return -EINVAL;
		if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
		fi->fh = CV_FH(ci.index);
		fi->keep_cache = 1;
		return 0;
	}
	n = zt_lookup(&g_tree, sub);
	if (!n) return -ENOENT;
	if (n->is_dir) return -EISDIR;
	if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;       /* read-only */
	fi->fh = (uint64_t)(uintptr_t)n;                /* nodes live as long as the mount: no lookup per read */
	fi->keep_cache = 1;                             /* the archive never changes */
	return 0;
}

static int zm_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	const char *sub;
	const zt_node *n;
	cv_info ci;
	ssize_t r;
	int v = route(path, &sub);

	if (off < 0) return -EINVAL;
	if (v == V_CDC && g_mkv) {
		void *c = fi && fi->fh ? (void *)(uintptr_t)fi->fh : mv_find(sub);

		if (!c) return -ENOENT;
		return (int)mv_read(c, buf, size, (uint64_t)off);
	}
	if (v == V_CDC) {
		long index;

		if (fi && fi->fh) index = (long)(fi->fh >> 1) - 1;
		else {
			r = cv_stat(g_cv, sub, &ci);
			if (r) return (int)r;
			if (ci.is_dir) return -EISDIR;
			index = ci.index;
		}
		r = cv_read(g_cv, &g_tree, index, (uint64_t)off, buf, size);
		return (int)r;
	}
	if (v != V_NATIVE) return v == V_ROOT ? -EISDIR : -ENOENT;
	n = fi && fi->fh ? (const zt_node *)(uintptr_t)fi->fh : zt_lookup(&g_tree, sub);

	if (!n) return -ENOENT;
	r = zt_read(&g_tree, n, (uint64_t)off, buf, size);
	return (int)r;
}

static void *zm_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->entry_timeout = 3600.0;                    /* nothing ever changes, so let the kernel remember */
	cfg->attr_timeout = 3600.0;
	cfg->negative_timeout = 3600.0;
	cfg->kernel_cache = 1;
	return NULL;
}

static const struct fuse_operations zm_ops = {
	.getattr = zm_getattr,
	.readdir = zm_readdir,
	.readlink = zm_readlink,
	.open    = zm_open,
	.read    = zm_read,
	.init    = zm_init,
};

/* ---------------------------------------------------------------- */
/* Command line                                                      */
/* ---------------------------------------------------------------- */

#define MAX_FLAGS 8

typedef struct {
	const char *archive, *mnt;
	size_t cache_mib;               /* ZT_CACHE_AUTO unless cache=MIB was given */
	unsigned threads;               /* threads=N, else the default rule (resolved by finish_opts) */
	int user_max_threads;           /* the user passed their own max_threads: libfuse gets that, not ours */
	char fuse_o[1024];              /* the -o items libfuse should see, comma separated */
	const char *flags[MAX_FLAGS];   /* -f, -d, -s */
	int nflags;
	int help;
} mount_opts;

static void usage(FILE *f)
{
	fprintf(f,
"usage: zfrag mount [-f] [-d] [-s] [-o OPTIONS] ARCHIVE MOUNTPOINT\n"
"  Mounts the archive read-only as a directory tree; unmount with: fusermount3 -u MOUNTPOINT\n"
"  If the archive holds a cdcstore repository (cdcstore.idx and chunks/), the mount has two halves, from the\n"
"  same process: MOUNTPOINT/native (the archive's files) and MOUNTPOINT/virtual (the files the repository holds)\n"
"  An mjpegstore set (specs.txt and a folder of JPEG pictures per clip) gets the same two halves, with virtual/\n"
"  holding one <clip>.mkv per clip: the stream mjpegstore -s writes, readable at any offset\n"
"  -f  stay in the foreground     -d  debug (implies -f)     -s  single-threaded\n"
"  -o  comma-separated options:\n"
"        threads=N    frames decoded at once, 1 to %d (default: 2 per CPU, at most %d: %u here).\n"
"                     Each decoding thread works on one frame and needs one frame-sized buffer, and\n"
"                     nothing else sets the memory cap, so decoding memory follows the CPUs\n"
"        cache=MIB    decoded data kept in memory (default: one frame per thread, at least %d\n"
"                     frames: %u MiB at the largest frame size here). Reading a file front to back\n"
"                     decodes each frame once; a larger cache helps random access and several readers\n"
"        anything else is passed to libfuse (allow_other, ...)\n"
"  Only archives made by zfrag pack can be mounted (not a plain .tar.zst).\n"
"  A split archive (ARCHIVE.tar.zfrag.001, .002, ... beside each other, from pack --split or\n"
"  zfrag split) mounts as one: give the normal name or any part. Every part must be present.\n"
"  An archive packed with --strip-components N holds the contents of a directory: mount it at that\n"
"  directory's name and every file is back at its old path.\n", ZF_THREADS_LIMIT, ZF_THREADS_DEFAULT_CAP, zf_default_threads(),
	ZT_AUTO_MIN_SLOTS,
	(unsigned)(((zf_default_threads() > ZT_AUTO_MIN_SLOTS ? zf_default_threads() : ZT_AUTO_MIN_SLOTS) * (unsigned long)ZF_MAX_FRAME_RAW) >> 20));
}

/* adds one -o item: ours are consumed, the rest go to libfuse. Returns -1 with err set. */
static int add_option(mount_opts *o, const char *item, char *err, size_t errlen)
{
	if (!*item) return 0;
	if (!strncmp(item, "cache=", 6)) {
		char *e;
		unsigned long v = strtoul(item + 6, &e, 10);

		if (e == item + 6 || *e || v > 1048576) { snprintf(err, errlen, "bad cache size '%s' (MiB, e.g. cache=512)", item + 6); return -1; }
		o->cache_mib = v;
		return 0;
	}
	if (!strncmp(item, "threads=", 8)) {
		char *e;
		unsigned long v = strtoul(item + 8, &e, 10);

		if (e == item + 8 || *e || v < 1 || v > ZF_THREADS_LIMIT) {
			snprintf(err, errlen, "bad thread count '%s' (1 to %d, e.g. threads=4)", item + 8, ZF_THREADS_LIMIT);
			return -1;
		}
		o->threads = (unsigned)v;
		return 0;
	}
	if (!strncmp(item, "max_threads=", 12)) o->user_max_threads = 1;      /* theirs wins; it still goes to libfuse */
	if (!strcmp(item, "ro")) return 0;                              /* always */
	if (!strcmp(item, "rw")) { snprintf(err, errlen, "zfrag mounts are read-only"); return -1; }
	if (strlen(o->fuse_o) + strlen(item) + 2 > sizeof o->fuse_o) { snprintf(err, errlen, "too many options"); return -1; }
	if (o->fuse_o[0]) strcat(o->fuse_o, ",");
	strcat(o->fuse_o, item);
	return 0;
}

static int parse_mount_args(int argc, char **argv, mount_opts *o, char *err, size_t errlen)
{
	memset(o, 0, sizeof *o);
	o->cache_mib = ZT_CACHE_AUTO;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) { o->help = 1; return 0; }
		if (!strcmp(a, "-f") || !strcmp(a, "-d") || !strcmp(a, "-s")) {
			if (o->nflags < MAX_FLAGS) o->flags[o->nflags++] = a;
			continue;
		}
		if (!strncmp(a, "-o", 2)) {
			char list[1024], *tok, *save = NULL;
			const char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);

			if (!v) { snprintf(err, errlen, "-o needs an argument"); return -1; }
			snprintf(list, sizeof list, "%s", v);
			for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
				if (add_option(o, tok, err, errlen)) return -1;
			continue;
		}
		if (a[0] == '-' && a[1]) { snprintf(err, errlen, "unknown option %s", a); return -1; }
		if (!o->archive) o->archive = a;
		else if (!o->mnt) o->mnt = a;
		else { snprintf(err, errlen, "too many arguments: %s", a); return -1; }
	}
	if (!o->archive || !o->mnt) { snprintf(err, errlen, "an archive and a mount point are needed"); return -1; }
	return 0;
}

/* what is left to decide once the command line is parsed */
static void finish_opts(mount_opts *o)
{
	if (!o->threads) o->threads = zf_default_threads();
}

/* the argument vector libfuse gets: mount point, the flags, and -o ro,... */
#define MAX_FARGS (MAX_FLAGS + 6)
static int build_fuse_args(const mount_opts *o, char **fargv, char *obuf, size_t obuflen)
{
	int n = 0;
	int plain = strchr(o->archive, ',') == NULL && strchr(o->archive, '\\') == NULL;

	fargv[n++] = (char *)"zfrag";
	fargv[n++] = (char *)o->mnt;
	for (int i = 0; i < o->nflags; i++) fargv[n++] = (char *)o->flags[i];
	char mt[32] = "";

#ifdef ZM_HAVE_MAX_THREADS
	if (!o->user_max_threads) snprintf(mt, sizeof mt, ",max_threads=%u", o->threads);
#endif
	snprintf(obuf, obuflen, "ro,subtype=zfrag%s%s%s%s%s", plain ? ",fsname=" : "", plain ? o->archive : "",
		 o->fuse_o[0] ? "," : "", o->fuse_o, mt);
	fargv[n++] = (char *)"-o";
	fargv[n++] = obuf;
	fargv[n] = NULL;
	return n;
}

int zfrag_mount(int argc, char **argv)
{
	mount_opts o;
	char err[512], obuf[2048], *fargv[MAX_FARGS + 1];
	int fargc, rc;

	if (parse_mount_args(argc, argv, &o, err, sizeof err)) {
		fprintf(stderr, "zfrag mount: %s\n\n", err);
		usage(stderr);
		return 2;
	}
	if (o.help) { usage(stdout); return 0; }
	finish_opts(&o);
	if (zt_open(&g_tree, o.archive, o.cache_mib, o.threads, err, sizeof err)) {
		fprintf(stderr, "zfrag mount: %s\n", err);
		return 1;
	}
	g_uid = getuid();
	g_gid = getgid();
	fprintf(stderr, "zfrag mount: %s%s%s%s: %llu files in %llu directories%s, %u decoding threads, frames of %lluK, %zu decoded frames cached\n", o.archive,
		g_tree.ar.series[0] ? " (" : "", g_tree.ar.series, g_tree.ar.series[0] ? ")" : "",
		(unsigned long long)g_tree.nfiles, (unsigned long long)g_tree.ndirs - 1,
		g_tree.hidden ? " (some names hidden: see below)" : "", o.threads, (unsigned long long)(g_tree.ar.frame_raw >> 10),
		fc_slots(g_tree.fc));
	switch (cv_open(&g_cv, &g_tree, err, sizeof err)) {
	case 1:
		fprintf(stderr, "zfrag mount: cdcstore repository%s%s: %llu files from %llu chunks, shown under virtual/ (the archive's files are under native/)\n",
			cv_base(g_cv)[0] ? " in " : "", cv_base(g_cv), (unsigned long long)cv_nfiles(g_cv), (unsigned long long)cv_nchunks(g_cv));
		break;
	case -1:
		fprintf(stderr, "zfrag mount: %s\n", err);
		zt_close(&g_tree);
		return 1;
	default:
		break;
	}
	if (!g_cv) {
		char desc[256], base[PATH_MAX];

		switch (mv_open(&g_tree, desc, sizeof desc, base, sizeof base)) {
		case 1:
			g_mkv = 1;
			fprintf(stderr, "zfrag mount: mjpegstore set%s%s: %zu clips as .mkv files (%s), shown under virtual/ (the archive's files are under native/)\n",
				base[0] ? " in " : "", base, mv_nclips(), desc);
			break;
		case -1:
			zt_close(&g_tree);
			return 1;
		default:
			break;
		}
	}
	if (g_tree.hidden)
		fprintf(stderr, "zfrag mount: %llu archive entries are not shown (a name stored twice, or a path that is both a file\n"
				"             and a directory); the first of each is shown\n", (unsigned long long)g_tree.hidden);
	fargc = build_fuse_args(&o, fargv, obuf, sizeof obuf);
	rc = fuse_main(fargc, fargv, &zm_ops, NULL);
	cv_close(g_cv);
	if (g_mkv) mv_close();
	zt_close(&g_tree);
	return rc;
}
