/* SPDX-License-Identifier: MIT */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tar.h"

#define BLK 512
#define OCT11_MAX 077777777777ULL       /* the most an 11-digit octal field holds */

static void put_oct(char *dst, size_t width, uint64_t v)
{
	/* width includes the terminating NUL: width-1 octal digits */
	for (size_t i = width - 1; i-- > 0; ) {
		dst[i] = (char)('0' + (v & 7));
		v >>= 3;
	}
	dst[width - 1] = '\0';
}

static void finish_header(uint8_t *h)
{
	unsigned sum = 0;

	memset(h + 148, ' ', 8);
	for (int i = 0; i < BLK; i++) sum += h[i];
	snprintf((char *)h + 148, 8, "%06o", sum);
	h[154] = '\0';
	h[155] = ' ';
}

static void fill_common(uint8_t *h, uint64_t size, int64_t mtime, unsigned mode, char type)
{
	char *c = (char *)h;

	if (mtime < 0) mtime = 0;
	if ((uint64_t)mtime > OCT11_MAX) mtime = (int64_t)OCT11_MAX;
	put_oct(c + 100, 8, mode & 07777);
	put_oct(c + 108, 8, 0);                 /* uid */
	put_oct(c + 116, 8, 0);                 /* gid */
	put_oct(c + 124, 12, size > OCT11_MAX ? 0 : size);
	put_oct(c + 136, 12, (uint64_t)mtime);
	c[156] = type;
	memcpy(c + 257, "ustar", 6);             /* magic with its NUL */
	memcpy(c + 263, "00", 2);                /* version */
}

/* a pax record: "<len> <key>=<value>\n", where len counts the whole record */
static size_t pax_record(char *dst, size_t cap, const char *key, const char *val, size_t vlen)
{
	size_t base = 1 + strlen(key) + 1 + vlen + 1;   /* space key '=' value '\n' */
	size_t digits = 1, len;

	for (;;) {
		len = base + digits;
		size_t d = 1;
		for (size_t t = len; t >= 10; t /= 10) d++;
		if (d == digits) break;
		digits = d;
	}
	if (len > cap) return 0;
	{
		int n = snprintf(dst, cap, "%zu %s=", len, key);
		memcpy(dst + n, val, vlen);
		dst[n + vlen] = '\n';
	}
	return len;
}

size_t tar_header(uint8_t *out, size_t cap, const char *path, size_t plen,
		  uint64_t size, int64_t mtime, unsigned mode)
{
	uint8_t uh[BLK];
	char recs[TAR_MAX_HDR];
	size_t nrec = 0, total = 0;
	size_t prefix_len = 0, name_start = 0;
	int need_path = 0;

	memset(uh, 0, sizeof uh);
	if (plen <= 100) {
		memcpy(uh, path, plen);
	} else {
		/* try to split at a '/' so that prefix <= 155 and name <= 100 */
		size_t k, found = 0;
		for (k = plen; k-- > 0; ) {
			if (path[k] != '/') continue;
			if (k <= 155 && plen - k - 1 <= 100 && plen - k - 1 > 0) { found = k; break; }
		}
		if (found) {
			prefix_len = found;
			name_start = found + 1;
			memcpy(uh + 345, path, prefix_len);
			memcpy(uh, path + name_start, plen - name_start);
		} else {
			need_path = 1;
			memcpy(uh, path, 100);
		}
	}
	if (size > OCT11_MAX) {
		char num[32];
		size_t r;

		snprintf(num, sizeof num, "%" PRIu64, size);
		r = pax_record(recs + nrec, sizeof recs - nrec, "size", num, strlen(num));
		if (!r) return 0;
		nrec += r;
	}
	if (need_path) {
		size_t r = pax_record(recs + nrec, sizeof recs - nrec, "path", path, plen);
		if (!r) return 0;
		nrec += r;
	}
	if (nrec) {
		uint8_t ph[BLK];
		size_t padded = (nrec + BLK - 1) / BLK * BLK;

		if (cap < BLK + padded) return 0;
		memset(ph, 0, sizeof ph);
		memcpy(ph, "PaxHeaders/zfrag", 16);
		fill_common(ph, nrec, mtime, 0644, 'x');
		finish_header(ph);
		memcpy(out, ph, BLK);
		memset(out + BLK, 0, padded);
		memcpy(out + BLK, recs, nrec);
		total = BLK + padded;
	}
	if (cap < total + BLK) return 0;
	fill_common(uh, size, mtime, mode, '0');
	finish_header(uh);
	memcpy(out + total, uh, BLK);
	return total + BLK;
}

/* ---- parsing ---------------------------------------------------------- */

static int parse_oct(const uint8_t *p, size_t width, uint64_t *v)
{
	uint64_t r = 0;
	size_t i = 0;

	while (i < width && (p[i] == ' ' || p[i] == '\0')) i++;
	for (; i < width && p[i] >= '0' && p[i] <= '7'; i++) r = r * 8 + (uint64_t)(p[i] - '0');
	for (; i < width; i++) if (p[i] != ' ' && p[i] != '\0') return -1;
	*v = r;
	return 0;
}

static int check_sum(const uint8_t *h)
{
	unsigned sum = 0;
	uint64_t want;

	for (int i = 0; i < BLK; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
	if (parse_oct(h + 148, 8, &want)) return -1;
	return sum == want ? 0 : -1;
}

int tar_parse(const uint8_t *buf, size_t len, tar_info *ti)
{
	const uint8_t *h = buf;
	char paxpath[TAR_NAME_MAX];
	uint64_t paxsize = 0;
	int have_path = 0, have_size = 0;
	size_t used = 0;

	memset(ti, 0, sizeof *ti);
	if (len < BLK || check_sum(h)) return -1;
	if (h[156] == 'x') {
		uint64_t n;
		size_t padded;
		const char *p, *end;

		if (parse_oct(h + 124, 12, &n) || n > TAR_MAX_HDR) return -1;
		padded = ((size_t)n + BLK - 1) / BLK * BLK;
		if (len < BLK + padded + BLK) return -1;
		p = (const char *)h + BLK;
		end = p + n;
		while (p < end) {
			char *e;
			unsigned long rl = strtoul(p, &e, 10);
			const char *kv, *eq, *recend;

			if (e == p || *e != ' ' || rl == 0 || rl > (size_t)(end - p)) return -1;
			recend = p + rl;
			kv = e + 1;
			eq = memchr(kv, '=', (size_t)(recend - kv));
			if (!eq || recend[-1] != '\n') return -1;
			if ((size_t)(eq - kv) == 4 && !memcmp(kv, "path", 4)) {
				size_t vl = (size_t)(recend - 1 - (eq + 1));
				if (vl >= sizeof paxpath) return -1;
				memcpy(paxpath, eq + 1, vl);
				paxpath[vl] = '\0';
				have_path = 1;
			} else if ((size_t)(eq - kv) == 4 && !memcmp(kv, "size", 4)) {
				paxsize = strtoull(eq + 1, NULL, 10);
				have_size = 1;
			}
			p = recend;
		}
		used = BLK + padded;
		h = buf + used;
	}
	if (len < used + BLK || check_sum(h)) return -1;
	ti->type = h[156];
	{
		uint64_t sz;
		if (parse_oct(h + 124, 12, &sz)) return -1;
		ti->size = have_size ? paxsize : sz;
	}
	if (have_path) {
		snprintf(ti->name, sizeof ti->name, "%s", paxpath);
	} else {
		char name[101], prefix[156];

		memcpy(name, h, 100); name[100] = '\0';
		memcpy(prefix, h + 345, 155); prefix[155] = '\0';
		if (prefix[0]) snprintf(ti->name, sizeof ti->name, "%s/%s", prefix, name);
		else snprintf(ti->name, sizeof ti->name, "%s", name);
	}
	ti->hdr_len = used + BLK;
	return 0;
}

int tar_norm_name(const char *p, const char **out, size_t *len)
{
	const char *s = p;
	size_t n;

	for (;;) {
		if (*s == '/') s++;
		else if (s[0] == '.' && s[1] == '/') s += 2;
		else break;
	}
	n = strlen(s);
	if (!n) return -1;
	for (const char *q = s; *q; ) {                 /* any ".." component? */
		const char *e = strchr(q, '/');
		size_t l = e ? (size_t)(e - q) : strlen(q);

		if (l == 2 && q[0] == '.' && q[1] == '.') return -1;
		q += l;
		if (*q == '/') q++;
	}
	*out = s;
	*len = n;
	return 0;
}

int tar_strip_name(const char **name, size_t *len, unsigned n)
{
	const char *p = *name;
	size_t l = *len;

	while (n--) {
		const char *sl = memchr(p, '/', l);

		if (!sl) return -1;                     /* the name was a single component: nothing left */
		l -= (size_t)(sl + 1 - p);
		p = sl + 1;
		while (l && *p == '/') { p++; l--; }
	}
	if (!l) return -1;
	*name = p;
	*len = l;
	return 0;
}
