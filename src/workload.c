// SPDX-License-Identifier: MIT
#include "workload.h"

#include <string.h>

static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

void rng_seed(struct rng *r, uint64_t seed)
{
	int i;

	for (i = 0; i < 4; i++)
		r->s[i] = splitmix64(&seed);
}

static uint64_t rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

uint64_t rng_next(struct rng *r)
{
	uint64_t *s = r->s, res = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = rotl(s[3], 45);
	return res;
}

uint64_t rng_below(struct rng *r, uint64_t n)
{
	__uint128_t m = (__uint128_t)rng_next(r) * n;
	uint64_t lo = (uint64_t)m, t;

	if (lo < n) {
		t = -n % n;
		while (lo < t) {
			m = (__uint128_t)rng_next(r) * n;
			lo = (uint64_t)m;
		}
	}
	return (uint64_t)(m >> 64);
}

double rng_unit(struct rng *r)
{
	return (double)(rng_next(r) >> 11) * 0x1.0p-53;
}

int wl_init(struct workload *w, enum wl_kind kind, uint32_t pages, uint64_t seed,
	    double hot_frac, double hot_weight)
{
	memset(w, 0, sizeof(*w));
	if (pages == 0 || hot_frac <= 0 || hot_frac >= 1 || hot_weight < 0 || hot_weight > 1)
		return -1;
	w->kind = kind;
	w->pages = pages;
	w->hot_frac = hot_frac;
	w->hot_weight = hot_weight;
	w->hot_pages = (uint32_t)(hot_frac * pages);
	if (w->hot_pages == 0)
		w->hot_pages = 1;
	if (w->hot_pages >= pages)
		w->hot_pages = pages - 1;
	rng_seed(&w->rng, seed);
	w->hash = 0xcbf29ce484222325ULL;
	return 0;
}

uint32_t wl_next(struct workload *w)
{
	uint32_t lpn;

	switch (w->kind) {
	case WL_SEQ:
		lpn = (uint32_t)(w->cursor++ % w->pages);
		break;
	case WL_UNIFORM:
		lpn = (uint32_t)rng_below(&w->rng, w->pages);
		break;
	default:	/* WL_HOTCOLD: hot region is [0, hot_pages) */
		if (rng_unit(&w->rng) < w->hot_weight)
			lpn = (uint32_t)rng_below(&w->rng, w->hot_pages);
		else
			lpn = w->hot_pages + (uint32_t)rng_below(&w->rng, w->pages - w->hot_pages);
		break;
	}
	w->hash = (w->hash ^ lpn) * 0x100000001b3ULL;
	return lpn;
}

const char *wl_name(enum wl_kind k)
{
	return k == WL_SEQ ? "seq" : k == WL_UNIFORM ? "uniform" : "hotcold";
}

int wl_parse(const char *s, enum wl_kind *k)
{
	if (!strcmp(s, "seq"))
		*k = WL_SEQ;
	else if (!strcmp(s, "uniform"))
		*k = WL_UNIFORM;
	else if (!strcmp(s, "hotcold"))
		*k = WL_HOTCOLD;
	else
		return -1;
	return 0;
}

/* memcpy keeps these valid for buffers of any alignment. */
void page_fill(void *buf, uint32_t page_size, uint32_t lpn, uint64_t version)
{
	uint64_t x = ((uint64_t)lpn << 32) ^ version ^ 0x5bd1e995ULL, v;
	uint8_t *p = buf;
	uint32_t i;

	for (i = 0; i < page_size / 8; i++) {
		v = splitmix64(&x);
		memcpy(p + (size_t)i * 8, &v, 8);
	}
	for (i = page_size & ~7U; i < page_size; i++)
		((uint8_t *)buf)[i] = (uint8_t)(lpn + version + i);
}

int page_check(const void *buf, uint32_t page_size, uint32_t lpn, uint64_t version)
{
	uint64_t x = ((uint64_t)lpn << 32) ^ version ^ 0x5bd1e995ULL, v;
	const uint8_t *p = buf;
	uint32_t i;

	for (i = 0; i < page_size / 8; i++) {
		memcpy(&v, p + (size_t)i * 8, 8);
		if (v != splitmix64(&x))
			return -1;
	}
	for (i = page_size & ~7U; i < page_size; i++)
		if (((const uint8_t *)buf)[i] != (uint8_t)(lpn + version + i))
			return -1;
	return 0;
}
