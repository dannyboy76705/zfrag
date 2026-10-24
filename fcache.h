/* SPDX-License-Identifier: MIT */
/* fcache.h - a cache of decoded frames, safe to use from many threads at once.
 *
 * A mount reads files in pieces of 128 KiB or so; each piece lives in a frame of up to
 * 32 MiB that must be decoded whole. The cache keeps the most recently used decoded
 * frames so a front-to-back read decodes each frame once, and concurrent readers of the
 * same frame wait for one decode instead of each doing their own.
 *
 * A reader holds at most one slot at a time (it copies what it needs and lets go before
 * asking for the next frame), so waiting for a free slot can never deadlock, however few
 * slots there are. Slots allocate their buffers on first use. */
#ifndef ZFRAG_FCACHE_H
#define ZFRAG_FCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "archive.h"

typedef struct fcache fcache;

#define FC_MIN_SLOTS 2

#define FC_MAX_DECODERS 64

/* `nslots` is clamped to at least FC_MIN_SLOTS. `ndecoders` is how many frames may be decoded at once (the
 * thread count): decoders are created when first needed, and no more than there are slots can ever be in use.
 * The archive must outlive the cache. */
fcache *fc_new(const zf_archive *a, size_t nslots, size_t ndecoders);
void fc_free(fcache *c);

/* Copies bytes [off, off+len) of the tar stream into dst. Thread-safe. Returns 0, or -1
 * with the reason in err (and nothing guaranteed about dst). */
int fc_read(fcache *c, uint64_t off, uint64_t len, uint8_t *dst, char *err, size_t errlen);

size_t fc_slots(const fcache *c);
void fc_stats(fcache *c, uint64_t *hits, uint64_t *misses, uint64_t *waits);

#endif
