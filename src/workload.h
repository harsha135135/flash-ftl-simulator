/* SPDX-License-Identifier: MIT */
/*
 * Deterministic workload generation. The op stream depends only on the
 * workload parameters and seed, never on FTL state, so two FTLs driven with
 * the same parameters see byte-identical traces (checked via trace_hash).
 */
#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stdint.h>

/* xoshiro256** seeded through splitmix64. */
struct rng {
	uint64_t s[4];
};

void rng_seed(struct rng *r, uint64_t seed);
uint64_t rng_next(struct rng *r);
/* Uniform in [0, n) without modulo bias (Lemire's method). */
uint64_t rng_below(struct rng *r, uint64_t n);
/* Uniform double in [0, 1). */
double rng_unit(struct rng *r);

enum wl_kind { WL_SEQ, WL_UNIFORM, WL_HOTCOLD };

struct workload {
	enum wl_kind kind;
	uint32_t pages;		/* logical address space */
	double hot_frac;	/* hot/cold: fraction of the space that is hot */
	double hot_weight;	/* hot/cold: fraction of writes to the hot part */
	uint32_t hot_pages;
	uint64_t cursor;	/* sequential position */
	struct rng rng;
	uint64_t hash;		/* FNV-1a over every generated LPN */
};

int wl_init(struct workload *w, enum wl_kind kind, uint32_t pages, uint64_t seed,
	    double hot_frac, double hot_weight);
uint32_t wl_next(struct workload *w);
const char *wl_name(enum wl_kind k);
int wl_parse(const char *s, enum wl_kind *k);

/* Fill a page with a pattern unique to (lpn, version), and check it. */
void page_fill(void *buf, uint32_t page_size, uint32_t lpn, uint64_t version);
int page_check(const void *buf, uint32_t page_size, uint32_t lpn, uint64_t version);

#endif
