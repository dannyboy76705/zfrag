/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "fcache.h"

enum { S_EMPTY, S_LOADING, S_READY };

typedef struct {
	int64_t frame;                  /* the frame held or being loaded, or -1 */
	int state;
	unsigned refs;                  /* readers copying from it right now */
	uint64_t stamp;                 /* LRU tick of the last use */
	uint8_t *buf;                   /* allocated on first load */
} slot;


struct fcache {
	const zf_archive *a;
	slot *slots;
	size_t n;
	zf_decoder decs[FC_MAX_DECODERS];       /* a decoder is only needed while a frame is being decoded, */
	int dec_busy[FC_MAX_DECODERS];          /* so a few are shared by all slots (created when first needed) */
	size_t ndec;
	uint64_t tick, hits, misses, waits;
	pthread_mutex_t mu;
	pthread_cond_t cv;
};

fcache *fc_new(const zf_archive *a, size_t nslots, size_t ndecoders)
{
	fcache *c = calloc(1, sizeof *c);

	if (!c) return NULL;
	if (nslots < FC_MIN_SLOTS) nslots = FC_MIN_SLOTS;
	c->slots = calloc(nslots, sizeof *c->slots);
	if (!c->slots) { free(c); return NULL; }
	c->a = a;
	c->n = nslots;
	if (ndecoders < 1) ndecoders = 1;
	if (ndecoders > FC_MAX_DECODERS) ndecoders = FC_MAX_DECODERS;
	c->ndec = nslots < ndecoders ? nslots : ndecoders;
	for (size_t i = 0; i < nslots; i++) c->slots[i].frame = -1;
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv, NULL);
	return c;
}

void fc_free(fcache *c)
{
	if (!c) return;
	for (size_t i = 0; i < c->ndec; i++) zf_decoder_free(&c->decs[i]);
	for (size_t i = 0; i < c->n; i++) free(c->slots[i].buf);
	pthread_mutex_destroy(&c->mu);
	pthread_cond_destroy(&c->cv);
	free(c->slots);
	free(c);
}

size_t fc_slots(const fcache *c) { return c->n; }

void fc_stats(fcache *c, uint64_t *hits, uint64_t *misses, uint64_t *waits)
{
	pthread_mutex_lock(&c->mu);
	if (hits) *hits = c->hits;
	if (misses) *misses = c->misses;
	if (waits) *waits = c->waits;
	pthread_mutex_unlock(&c->mu);
}

/* A decoder for the caller's exclusive use until release_decoder. Waits if all are busy; the ones
 * in use belong to loaders that are decoding and wait for nothing, so this cannot deadlock. */
static int take_decoder(fcache *c)
{
	pthread_mutex_lock(&c->mu);
	for (;;) {
		for (size_t k = 0; k < c->ndec; k++) {
			if (c->dec_busy[k]) continue;
			if (!c->decs[k].dctx && zf_decoder_init(&c->decs[k])) { pthread_mutex_unlock(&c->mu); return -1; }
			c->dec_busy[k] = 1;
			pthread_mutex_unlock(&c->mu);
			return (int)k;
		}
		pthread_cond_wait(&c->cv, &c->mu);
	}
}

static void release_decoder(fcache *c, int k)
{
	pthread_mutex_lock(&c->mu);
	c->dec_busy[k] = 0;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
}

/* Returns the slot holding frame i, ready, with a reference taken; or NULL with err set. */
static slot *get_frame(fcache *c, uint64_t i, char *err, size_t errlen)
{
	pthread_mutex_lock(&c->mu);
	for (;;) {
		slot *victim = NULL;
		int waiting = 0;

		for (size_t k = 0; k < c->n; k++) {
			slot *s = &c->slots[k];

			if (s->frame != (int64_t)i) continue;
			if (s->state == S_READY) {
				s->refs++;
				s->stamp = ++c->tick;
				c->hits++;
				pthread_mutex_unlock(&c->mu);
				return s;
			}
			waiting = 1;                    /* someone is decoding it: wait for them */
		}
		if (waiting) {
			c->waits++;
			pthread_cond_wait(&c->cv, &c->mu);
			continue;
		}
		/* a miss: take an empty slot, else the least recently used one nobody is reading */
		for (size_t k = 0; k < c->n; k++) {
			slot *s = &c->slots[k];

			if (s->state == S_EMPTY) { victim = s; break; }
			if (s->state == S_READY && s->refs == 0 && (!victim || s->stamp < victim->stamp)) victim = s;
		}
		if (!victim) {                          /* every slot is being read or loaded: wait for one */
			c->waits++;
			pthread_cond_wait(&c->cv, &c->mu);
			continue;
		}
		victim->state = S_LOADING;
		victim->frame = (int64_t)i;
		victim->refs = 1;
		c->misses++;
		pthread_mutex_unlock(&c->mu);

		int failed = 0, d = -1;

		if (!victim->buf) victim->buf = malloc((size_t)c->a->frame_raw);
		if (!victim->buf) { snprintf(err, errlen, "out of memory"); failed = 1; }
		if (!failed) {
			d = take_decoder(c);
			if (d < 0) { snprintf(err, errlen, "out of memory"); failed = 1; }
			else {
				failed = zf_decode_frame(c->a, i, &c->decs[d], victim->buf, err, errlen) != 0;
				release_decoder(c, d);
			}
		}
		if (failed) {
			pthread_mutex_lock(&c->mu);
			victim->state = S_EMPTY;
			victim->frame = -1;
			victim->refs = 0;
			pthread_cond_broadcast(&c->cv);
			pthread_mutex_unlock(&c->mu);
			return NULL;
		}
		pthread_mutex_lock(&c->mu);
		victim->state = S_READY;
		victim->stamp = ++c->tick;
		pthread_cond_broadcast(&c->cv);
		pthread_mutex_unlock(&c->mu);
		return victim;
	}
}

static void put_frame(fcache *c, slot *s)
{
	pthread_mutex_lock(&c->mu);
	if (--s->refs == 0) pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
}

int fc_read(fcache *c, uint64_t off, uint64_t len, uint8_t *dst, char *err, size_t errlen)
{
	const zf_archive *a = c->a;

	if (off > a->stream_len || len > a->stream_len - off) {
		snprintf(err, errlen, "read past the end of the tar stream");
		return -1;
	}
	while (len) {
		uint64_t fi = off / a->frame_raw, in = off % a->frame_raw;
		uint64_t flen = zf_frame_len(a, fi);
		size_t take = flen - in < len ? (size_t)(flen - in) : (size_t)len;
		slot *s = get_frame(c, fi, err, errlen);

		if (!s) return -1;
		memcpy(dst, s->buf + in, take);
		put_frame(c, s);                        /* let go before asking for the next frame */
		dst += take; off += take; len -= take;
	}
	return 0;
}
