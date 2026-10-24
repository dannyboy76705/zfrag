/* SPDX-License-Identifier: MIT */
/* Drives the callbacks in mount.c directly, with a stand-in fuse.h (no FUSE needed):
 *   glue_harness ARCHIVE getattr PATH      "mode=%o nlink=%u size=%llu uid=%u mtime=%lld" or "-ENOENT"...
 *   glue_harness ARCHIVE readdir PATH      the names the filler receives, in order
 *   glue_harness ARCHIVE open PATH [wr]    0, or the errno it returns (wr = open for writing)
 *   glue_harness ARCHIVE read PATH OFF LEN [nofh]   bytes via open+read (nofh: read without the handle)
 *   glue_harness ARCHIVE readlink PATH     the link target, or the errno
 *   glue_harness ARCHIVE init              the timeouts init sets
 *   glue_harness args mount ARG...         the parsed mount options, or the error (argv[0] is "mount", as zfrag_mount gets it)
 *   glue_harness defthreads | threadsfor N | threadflag mount ARG...   the thread-count rule, and what a command line resolves to
 *   glue_harness fuseargs mount ARG...     what libfuse would be called with (a stub fuse_main records it)
 * Built by glue_test.sh. */
#define ZFRAG_WITH_FUSE 1
#include "../mount.c"

static int fill(void *buf, const char *name, const struct stat *st, off_t off, enum fuse_fill_dir_flags fl)
{
	(void)buf; (void)st; (void)off; (void)fl;
	printf("%s\n", name);
	return 0;
}

static int fuse_main_calls;
int fuse_main(int argc, char *argv[], const struct fuse_operations *op, void *user_data)
{
	(void)op; (void)user_data;
	fuse_main_calls++;
	for (int i = 0; i < argc; i++) printf("[%s]", argv[i]);
	printf("\n");
	return 0;
}

static const char *errname(int e)
{
	switch (-e) {
	case ENOENT: return "-ENOENT";
	case ENOTDIR: return "-ENOTDIR";
	case EISDIR: return "-EISDIR";
	case EACCES: return "-EACCES";
	case EIO: return "-EIO";
	case EINVAL: return "-EINVAL";
	default: return "-E?";
	}
}

int main(int argc, char **argv)
{
	char err[512];
	struct fuse_file_info fi;

	/* the code under test talks on stderr (warnings, reasons); tests that want it set HARNESS_VERBOSE=1 */
	if (!getenv("HARNESS_VERBOSE") && !freopen("/dev/null", "w", stderr)) return 2;
	if (argc >= 2 && !strcmp(argv[1], "args")) {
		mount_opts o;
		if (parse_mount_args(argc - 2, argv + 2, &o, err, sizeof err)) { printf("ERROR: %s\n", err); return 1; }
		char cache[32];
		if (o.cache_mib == ZT_CACHE_AUTO) snprintf(cache, sizeof cache, "auto"); else snprintf(cache, sizeof cache, "%zu", o.cache_mib);
		printf("archive=%s mnt=%s cache=%s fuse_o=%s flags=%d help=%d\n", o.archive ? o.archive : "-", o.mnt ? o.mnt : "-",
		       cache, o.fuse_o, o.nflags, o.help);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "usage")) {                 /* the help text of zfrag mount */
		usage(stdout);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "defthreads")) {           /* the default rule on this machine */
		printf("cpus=%u threads=%u\n", zf_cpu_count(), zf_default_threads());
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "threadsfor")) {           /* the rule for a given number of CPUs */
		printf("%u\n", zf_threads_for((unsigned)atoi(argv[2])));
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "threadflag")) {           /* what a command line resolves to */
		mount_opts o;
		if (parse_mount_args(argc - 2, argv + 2, &o, err, sizeof err)) { printf("ERROR: %s\n", err); return 1; }
		finish_opts(&o);
		printf("threads=%u\n", o.threads);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "fuseargs")) {
		mount_opts o;
		char *fargv[MAX_FARGS + 1], obuf[2048];
		if (parse_mount_args(argc - 2, argv + 2, &o, err, sizeof err)) { printf("ERROR: %s\n", err); return 1; }
		finish_opts(&o);
		int n = build_fuse_args(&o, fargv, obuf, sizeof obuf);
		return fuse_main(n, fargv, &zm_ops, NULL);
	}
	if (argc < 3) { fprintf(stderr, "usage: glue_harness ARCHIVE COMMAND ...\n"); return 2; }
	g_uid = 4242; g_gid = 4343;
	if (zt_open(&g_tree, argv[1], 4, 4, err, sizeof err)) { printf("OPEN FAILED: %s\n", err); return 1; }
	memset(&fi, 0, sizeof fi);
	if (cv_open(&g_cv, &g_tree, err, sizeof err) < 0) { printf("CDC FAILED: %s\n", err); return 1; }
	if (!g_cv) {                                    /* what zfrag_mount decides: an mjpegstore set, if there is one */
		char desc[256], base[PATH_MAX];
		int r = mv_open(&g_tree, desc, sizeof desc, base, sizeof base);
		if (r < 0) { printf("MKV FAILED\n"); return 1; }
		g_mkv = r;
	}
	if (!strcmp(argv[2], "getattr") && argc > 3) {
		struct stat st;
		int r = zm_getattr(argv[3], &st, NULL);
		if (r) puts(errname(r));
		else printf("%s mode=%o nlink=%u size=%llu blocks=%llu uid=%u gid=%u mtime=%lld\n", S_ISDIR(st.st_mode) ? "dir" : "file",
			    (unsigned)(st.st_mode & 07777), (unsigned)st.st_nlink, (unsigned long long)st.st_size,
			    (unsigned long long)st.st_blocks, (unsigned)st.st_uid, (unsigned)st.st_gid, (long long)st.st_mtime);
	} else if (!strcmp(argv[2], "readdir") && argc > 3) {
		int r = zm_readdir(argv[3], NULL, fill, 0, NULL, 0);
		if (r) puts(errname(r));
	} else if (!strcmp(argv[2], "open") && argc > 3) {
		fi.flags = argc > 4 && !strcmp(argv[4], "wr") ? O_WRONLY : O_RDONLY;
		int r = zm_open(argv[3], &fi);
		if (r) puts(errname(r)); else printf("0 keep_cache=%u fh=%s\n", fi.keep_cache, fi.fh ? "set" : "unset");
	} else if (!strcmp(argv[2], "read") && argc > 5) {
		uint64_t off = strtoull(argv[4], NULL, 10);
		size_t len = (size_t)strtoull(argv[5], NULL, 10);
		char *b = malloc(len ? len : 1);
		int r;
		fi.flags = O_RDONLY;
		if (!(argc > 6 && !strcmp(argv[6], "nofh"))) {
			r = zm_open(argv[3], &fi);
			if (r) { puts(errname(r)); return 0; }
		}
		r = zm_read(argv[3], b, len, (off_t)off, &fi);
		if (r < 0) puts(errname(r)); else fwrite(b, 1, (size_t)r, stdout);
		free(b);
	} else if (!strcmp(argv[2], "readlink") && argc > 3) {
		char t[4096];
		int r = zm_readlink(argv[3], t, sizeof t);
		if (r) puts(errname(r)); else puts(t);
	} else if (!strcmp(argv[2], "init")) {
		struct fuse_conn_info ci = {0};
		struct fuse_config cfg = {0};
		zm_init(&ci, &cfg);
		printf("entry=%.0f attr=%.0f negative=%.0f kernel_cache=%d\n", cfg.entry_timeout, cfg.attr_timeout, cfg.negative_timeout, cfg.kernel_cache);
	} else {
		fprintf(stderr, "unknown command\n");
		return 2;
	}
	zt_close(&g_tree);
	return 0;
}
