/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "archive.h"
#include "outset.h"

#define MAX_PARTS 10000

struct outset {
	char *base;
	int multi, committed;
	uint64_t part_size, part_start;
	unsigned width;
	int fd;
	size_t n;                       /* parts started */
	char *tmp[MAX_PARTS], *fin[MAX_PARTS];
	uint64_t bytes[MAX_PARTS];
	volatile sig_atomic_t live;     /* temporary files that exist (for the signal handler) */
};

static outset *g_active;

static void on_signal(int sig)
{
	outset *o = g_active;

	if (o) for (sig_atomic_t i = 0; i < o->live; i++) unlink(o->tmp[i]);
	_exit(128 + sig);
}

void outset_install_signals(outset *o)
{
	struct sigaction sa;

	g_active = o;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
}

static int pwrite_all(int fd, const void *buf, size_t n, uint64_t off)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t r = pwrite(fd, p, n, (off_t)off);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; off += (uint64_t)r; n -= (size_t)r;
	}
	return 0;
}

static uint64_t max_parts(const outset *o)
{
	uint64_t m = 1;

	for (unsigned i = 0; i < o->width && m < MAX_PARTS; i++) m *= 10;
	m -= 1;
	return m > MAX_PARTS ? MAX_PARTS : m;
}

static int open_part(outset *o, char *err, size_t errlen)
{
	char name[4096];
	size_t i = o->n;

	if (o->multi && i >= max_parts(o)) {
		snprintf(err, errlen, "that would be more than %llu parts: use a larger part size", (unsigned long long)max_parts(o));
		return -1;
	}
	if (o->multi) zf_part_name(name, sizeof name, o->base, (unsigned)(ZF_PART_FIRST + i), o->width);
	else snprintf(name, sizeof name, "%s", o->base);
	o->fin[i] = strdup(name);
	snprintf(name + strlen(name), sizeof name - strlen(name), ".tmp");
	o->tmp[i] = strdup(name);
	if (!o->fin[i] || !o->tmp[i]) { snprintf(err, errlen, "out of memory"); return -1; }
	o->fd = open(o->tmp[i], O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (o->fd < 0) { snprintf(err, errlen, "%.300s: %s", o->tmp[i], strerror(errno)); return -1; }
	o->n = i + 1;
	o->live = (sig_atomic_t)o->n;
	return 0;
}

static int close_part(outset *o, char *err, size_t errlen)
{
	int fd = o->fd;

	o->fd = -1;
	if (fd < 0) return 0;
	if (fsync(fd) || close(fd)) { snprintf(err, errlen, "%.300s: %s", o->tmp[o->n - 1], strerror(errno)); return -1; }
	return 0;
}

static int remove_existing(const char *base, int force, int multi, int do_remove, int keep_base, char *err, size_t errlen)
{
	char **v = NULL;
	size_t n = 0;
	int exists = !keep_base && access(base, F_OK) == 0;

	if (!multi) {
		if (exists && !force) { snprintf(err, errlen, "%.300s exists; use --force to overwrite", base); return -1; }
		return 0;
	}
	if (zf_find_series_files(base, &v, &n)) { snprintf(err, errlen, "cannot look for existing parts of %.300s", base); return -1; }
	if ((exists || n) && !force) {
		if (exists) snprintf(err, errlen, "%.300s exists; use --force to replace it", base);
		else snprintf(err, errlen, "parts named %.300s.* exist (e.g. %.300s); use --force to replace them", base, v[0]);
		zf_free_paths(v, n);
		return -1;
	}
	/* --force: a single file of the same name would shadow the new parts, and leftover parts from an
	 * older, longer series would be joined onto the new one, so both go */
	if (do_remove) {
		if (exists) unlink(base);
		for (size_t i = 0; i < n; i++) unlink(v[i]);
	}
	zf_free_paths(v, n);
	return 0;
}

int outset_precheck(const char *base, int multi, int force, int keep_base, char *err, size_t errlen)
{
	return remove_existing(base, force, multi, 0, keep_base, err, errlen);
}

outset *outset_new(const char *base, int multi, uint64_t part_size, unsigned width, int force, int keep_base,
		   char *err, size_t errlen)
{
	outset *o;

	if (remove_existing(base, force, multi, 1, keep_base, err, errlen)) return NULL;
	o = calloc(1, sizeof *o);
	if (!o) { snprintf(err, errlen, "out of memory"); return NULL; }
	o->base = strdup(base);
	o->multi = multi;
	o->part_size = part_size;
	o->width = width ? width : ZF_PART_WIDTH;
	o->fd = -1;
	if (!o->base || open_part(o, err, errlen)) { outset_abort(o); return NULL; }
	return o;
}

static int roll(outset *o, uint64_t off, char *err, size_t errlen)
{
	o->bytes[o->n - 1] = off - o->part_start;
	if (close_part(o, err, errlen)) return -1;
	o->part_start = off;
	return open_part(o, err, errlen);
}

int outset_start_part(outset *o, uint64_t off, char *err, size_t errlen)
{
	if (off == o->part_start) return 0;                     /* the current part is still empty */
	return roll(o, off, err, errlen);
}

int outset_write(outset *o, const void *buf, size_t len, uint64_t off, char *err, size_t errlen)
{
	uint64_t in = off - o->part_start;

	if (o->multi && o->part_size) {
		if (len > o->part_size) {
			snprintf(err, errlen, "a piece of %llu bytes cannot fit in a part of %llu: use a larger part size",
				 (unsigned long long)len, (unsigned long long)o->part_size);
			return -1;
		}
		if (in > 0 && in + len > o->part_size) {
			if (roll(o, off, err, errlen)) return -1;
			in = 0;
		}
	}
	if (pwrite_all(o->fd, buf, len, in)) { snprintf(err, errlen, "write error on %.300s: %s", o->tmp[o->n - 1], strerror(errno)); return -1; }
	if (in + len > o->bytes[o->n - 1]) o->bytes[o->n - 1] = in + len;
	return 0;
}

int outset_commit(outset *o, char *err, size_t errlen)
{
	sigset_t block, old;
	size_t i;

	if (close_part(o, err, errlen)) return -1;
	/* a series is renamed into place in one go: no signal may land half way and leave parts 001..k */
	sigemptyset(&block);
	sigaddset(&block, SIGINT); sigaddset(&block, SIGTERM); sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, &old);
	for (i = 0; i < o->n; i++) {
		if (rename(o->tmp[i], o->fin[i])) {
			snprintf(err, errlen, "cannot rename %.300s: %s", o->tmp[i], strerror(errno));
			for (size_t j = 0; j < i; j++) unlink(o->fin[j]);      /* never leave a partial series */
			sigprocmask(SIG_SETMASK, &old, NULL);
			return -1;
		}
	}
	o->committed = 1;
	o->live = 0;
	if (g_active == o) g_active = NULL;
	sigprocmask(SIG_SETMASK, &old, NULL);
	return 0;
}

void outset_abort(outset *o)
{
	if (!o) return;
	if (o->fd >= 0) { close(o->fd); o->fd = -1; }
	if (!o->committed) for (size_t i = 0; i < o->n; i++) unlink(o->tmp[i]);
	o->live = 0;
	if (g_active == o) g_active = NULL;
	outset_free(o);
}

void outset_free(outset *o)
{
	if (!o) return;
	if (g_active == o) g_active = NULL;
	for (size_t i = 0; i < o->n; i++) { free(o->tmp[i]); free(o->fin[i]); }
	free(o->base);
	free(o);
}

size_t outset_nparts(const outset *o) { return o->n; }
const char *outset_part_path(const outset *o, size_t i) { return o->fin[i]; }
uint64_t outset_part_bytes(const outset *o, size_t i) { return o->bytes[i]; }
