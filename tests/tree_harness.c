/* SPDX-License-Identifier: MIT */
/* Test harness for the mount's model (tree and frame cache), with no FUSE:
 *   tree_harness [-c MIB|auto] [-t THREADS] ARCHIVE COMMAND [args]      (default: 64 MiB, 8 threads)
 *     walk                 every file: "size mode mtime /path"
 *     ls PATH              the entries of a directory: "d NAME" or "f NAME SIZE"
 *     stat PATH            "dir MODE SIZE MTIME", "file MODE SIZE MTIME" or "ENOENT"
 *     read PATH OFF LEN    bytes of a file to stdout (LEN may be "all"); "ENOENT" on stderr
 *     readall              reads every file front to back in odd-sized pieces
 *     stats                files, directories, hidden entries, cache slots and hits/misses
 *     stress THREADS READS SEED   random reads from many threads, each checked against a copy
 * Built by mount_test.sh. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree.h"

static zt_tree T;

static void walk(const zt_node *d, char *path, size_t plen)
{
	for (uint32_t i = 0; i < d->nkids; i++) {
		const zt_node *k = d->kids[i];
		size_t l = plen + 1 + strlen(k->name);

		if (l >= 16000) { fprintf(stderr, "path too long for the harness\n"); exit(1); }
		path[plen] = '/';
		strcpy(path + plen + 1, k->name);
		if (k->is_dir) walk(k, path, l);
		else printf("%llu %o %lld %s\n", (unsigned long long)k->size, k->mode, (long long)k->mtime, path);
		path[plen] = '\0';
	}
}

static void collect(const zt_node *d, const zt_node ***v, size_t *n, size_t *cap)
{
	for (uint32_t i = 0; i < d->nkids; i++) {
		if (d->kids[i]->is_dir) { collect(d->kids[i], v, n, cap); continue; }
		if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *v = realloc(*v, *cap * sizeof **v); }
		(*v)[(*n)++] = d->kids[i];
	}
}

static const zt_node **files;
static uint8_t **content;
static size_t nfiles_total;
static unsigned long long mismatches, errors;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int stress_reads;
static unsigned stress_seed;

static void *stress_thread(void *arg)
{
	unsigned seed = stress_seed + (unsigned)(size_t)arg;
	unsigned long long bad = 0, err = 0;
	uint8_t *buf = malloc(300000);

	for (int i = 0; i < stress_reads; i++) {
		size_t f = (size_t)rand_r(&seed) % nfiles_total;
		const zt_node *n = files[f];
		uint64_t off = n->size ? (uint64_t)rand_r(&seed) % (n->size + 2) : 0;   /* sometimes past the end */
		size_t len = (size_t)rand_r(&seed) % 200000;
		ssize_t r = zt_read(&T, n, off, buf, len);
		uint64_t want = off >= n->size ? 0 : (len > n->size - off ? n->size - off : len);

		if (r < 0) { err++; continue; }
		if ((uint64_t)r != want || (want && memcmp(buf, content[f] + off, (size_t)want))) bad++;
	}
	free(buf);
	pthread_mutex_lock(&mu);
	mismatches += bad; errors += err;
	pthread_mutex_unlock(&mu);
	return NULL;
}

int main(int argc, char **argv)
{
	size_t mib = 64;
	unsigned threads = 8;
	int i = 1;
	char err[512];

	while (i < argc && argv[i][0] == '-' && argv[i][1]) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) { i++; mib = !strcmp(argv[i], "auto") ? ZT_CACHE_AUTO : (size_t)atol(argv[i]); }
		else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
		i++;
	}
	if (argc - i < 2) { fprintf(stderr, "usage: tree_harness [-c MIB|auto] [-t THREADS] ARCHIVE COMMAND ...\n"); return 2; }
	if (zt_open(&T, argv[i], mib, threads, err, sizeof err)) { printf("OPEN FAILED: %s\n", err); return 1; }
	const char *cmd = argv[i + 1];

	if (!strcmp(cmd, "walk")) {
		{ static char path[16384]; path[0] = '\0'; walk(T.top, path, 0); }
	} else if (!strcmp(cmd, "ls") && i + 2 < argc) {
		const zt_node *d = zt_lookup(&T, argv[i + 2]);
		if (!d) { puts("ENOENT"); return 0; }
		if (!d->is_dir) { puts("ENOTDIR"); return 0; }
		for (uint32_t k = 0; k < d->nkids; k++) {
			if (d->kids[k]->is_dir) printf("d %s\n", d->kids[k]->name);
			else printf("f %s %llu\n", d->kids[k]->name, (unsigned long long)d->kids[k]->size);
		}
	} else if (!strcmp(cmd, "stat") && i + 2 < argc) {
		const zt_node *n = zt_lookup(&T, argv[i + 2]);
		if (!n) puts("ENOENT");
		else printf("%s %o %llu %lld\n", n->is_dir ? "dir" : "file", n->mode, (unsigned long long)n->size, (long long)n->mtime);
	} else if (!strcmp(cmd, "read") && i + 4 < argc) {
		const zt_node *n = zt_lookup(&T, argv[i + 2]);
		uint64_t off = strtoull(argv[i + 3], NULL, 10);
		size_t len = !strcmp(argv[i + 4], "all") ? (size_t)-1 : (size_t)strtoull(argv[i + 4], NULL, 10);
		if (!n) { fprintf(stderr, "ENOENT\n"); return 1; }
		if (len == (size_t)-1) len = n->size;
		uint8_t *b = malloc(len ? len : 1);
		ssize_t r = zt_read(&T, n, off, b, len);
		if (r < 0) { fprintf(stderr, "error %zd\n", r); return 1; }
		fwrite(b, 1, (size_t)r, stdout);
	} else if (!strcmp(cmd, "readall")) {
		const zt_node **v = NULL; size_t n = 0, cap = 0; unsigned long long total = 0;
		uint8_t *b = malloc(100003);
		collect(T.top, &v, &n, &cap);
		for (size_t k = 0; k < n; k++)
			for (uint64_t off = 0; off < v[k]->size; ) {
				ssize_t r = zt_read(&T, v[k], off, b, 100003);
				if (r <= 0) { printf("read error at %s\n", v[k]->name); return 1; }
				off += (uint64_t)r; total += (uint64_t)r;
			}
		printf("read %zu files, %llu bytes\n", n, total);
	} else if (!strcmp(cmd, "stats")) {
		uint64_t h, m, w;
		fc_stats(T.fc, &h, &m, &w);
		printf("files=%llu dirs=%llu hidden=%llu slots=%zu frames=%llu\n", (unsigned long long)T.nfiles,
		       (unsigned long long)T.ndirs, (unsigned long long)T.hidden, fc_slots(T.fc), (unsigned long long)T.ar.nframes);
	} else if (!strcmp(cmd, "stress") && i + 4 < argc) {
		int nthreads = atoi(argv[i + 2]);
		pthread_t th[64];
		uint64_t h, m, w;

		stress_reads = atoi(argv[i + 3]);
		stress_seed = (unsigned)atol(argv[i + 4]);
		collect(T.top, &files, &nfiles_total, &(size_t){0});
		/* a reference copy of every file, read once, front to back, before the threads start */
		content = calloc(nfiles_total, sizeof *content);
		for (size_t k = 0; k < nfiles_total; k++) {
			content[k] = malloc(files[k]->size + 1);
			for (uint64_t off = 0; off < files[k]->size; ) {
				ssize_t r = zt_read(&T, files[k], off, content[k] + off, files[k]->size - off);
				if (r <= 0) { puts("reference read failed"); return 1; }
				off += (uint64_t)r;
			}
		}
		for (int k = 0; k < nthreads && k < 64; k++) pthread_create(&th[k], NULL, stress_thread, (void *)(size_t)k);
		for (int k = 0; k < nthreads && k < 64; k++) pthread_join(th[k], NULL);
		fc_stats(T.fc, &h, &m, &w);
		printf("stress: threads=%d reads=%d files=%zu mismatches=%llu errors=%llu slots=%zu\n", nthreads, stress_reads,
		       nfiles_total, mismatches, errors, fc_slots(T.fc));
		return (mismatches || errors) ? 1 : 0;
	} else {
		fprintf(stderr, "unknown command\n");
		return 2;
	}
	if (!strcmp(cmd, "readall")) {
		uint64_t h, m, w;
		fc_stats(T.fc, &h, &m, &w);
		printf("cache: hits=%llu misses=%llu frames=%llu slots=%zu\n", (unsigned long long)h, (unsigned long long)m,
		       (unsigned long long)T.ar.nframes, fc_slots(T.fc));
	}
	zt_close(&T);
	return 0;
}
