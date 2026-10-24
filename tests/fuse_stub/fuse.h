/* A minimal stand-in for libfuse3's <fuse.h>, only for compiling and testing mount.c
 * without FUSE installed (tests/glue_test.sh). It declares just what mount.c uses, with the libfuse3
 * (3.x, FUSE_USE_VERSION 31) signatures. A real build uses the real header, never this one. */
#ifndef ZFRAG_FUSE_STUB_H
#define ZFRAG_FUSE_STUB_H
#define FUSE_MAKE_VERSION(maj, min)  ((maj) * 100 + (min))
#define FUSE_VERSION FUSE_MAKE_VERSION(3, 14)           /* what the real header says for libfuse 3.14 */
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

struct fuse_file_info {
	int flags;
	unsigned int writepage : 1, direct_io : 1, keep_cache : 1, flush : 1, nonseekable : 1;
	uint64_t fh;
};
struct fuse_conn_info { unsigned proto_major, proto_minor, capable, want; };
struct fuse_config {
	double entry_timeout, negative_timeout, attr_timeout;
	int kernel_cache;
};
enum fuse_readdir_flags { FUSE_READDIR_PLUS = (1 << 0) };
enum fuse_fill_dir_flags { FUSE_FILL_DIR_PLUS = (1 << 1) };
typedef int (*fuse_fill_dir_t)(void *buf, const char *name, const struct stat *stbuf, off_t off,
			       enum fuse_fill_dir_flags flags);
struct fuse_operations {
	int (*getattr)(const char *, struct stat *, struct fuse_file_info *);
	int (*readlink)(const char *, char *, size_t);
	int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, enum fuse_readdir_flags);
	int (*open)(const char *, struct fuse_file_info *);
	int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
	void *(*init)(struct fuse_conn_info *, struct fuse_config *);
};
int fuse_main(int argc, char *argv[], const struct fuse_operations *op, void *user_data);
#endif
