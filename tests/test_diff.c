// SPDX-License-Identifier: MIT
/*
 * Differential testing: seeded random sequences of write/read/discard run
 * against the FTL and the reference model; every read result (status and
 * bytes) must match, and ftl_check() must pass after every operation.
 *
 * Matrix: geometries (including pages_per_block of 1 and 2 and minimum
 * spare) x {greedy, fifo} x {dual, single frontier} x seeds. A second pass
 * injects NAND program failures: a failed write must leave the FTL agreeing
 * with a reference model that never saw that write.
 *
 * Usage: test_diff [seeds [ops]]   (defaults: 40 seeds, 3000 ops)
 */
#include <stdint.h>
#include <stdlib.h>

#include "ftl.h"
#include "refmodel.h"
#include "testutil.h"
#include "workload.h"

struct geo {
	uint32_t blocks, ppb, page_size, spare;
};

static const struct geo GEOS[] = {
	{ 4, 1, 16, 3 },	/* one page per block */
	{ 5, 2, 16, 3 },
	{ 6, 4, 16, 3 },	/* minimum spare, tiny */
	{ 10, 4, 32, 3 },
	{ 16, 8, 64, 4 },
	{ 32, 16, 64, 5 },
	{ 64, 8, 128, 12 },
};

static uint64_t total_ops, total_moved, total_victims, total_failures, total_refused;
static uint64_t runs_faulted, runs_wedged;
static char msg[256];

/* Returns 0 on success. */
static int run_one(const struct geo *g, enum ftl_gc_policy pol, bool single, uint64_t seed,
		   uint32_t ops, bool faults)
{
	struct ftl f;
	struct refmodel ref;
	struct ftl_config c = { 0 };
	struct rng r;
	uint8_t *a, *b;
	uint32_t i, lpn, L, hot, refused_streak = 0;
	int rf, rr;

	c.geo = (struct nand_geometry){ g->page_size, g->ppb, g->blocks };
	c.spare_blocks = g->spare;
	c.policy = pol;
	c.single_frontier = single;
	if (ftl_create(&f, &c) != FTL_OK)
		return -1;
	L = f.logical_pages;
	hot = L / 4 ? L / 4 : 1;
	if (ref_init(&ref, L, g->page_size) != FTL_OK)
		return -1;
	a = malloc(g->page_size);
	b = malloc(g->page_size);
	rng_seed(&r, seed);

	for (i = 0; i < ops; i++) {
		uint64_t op = rng_below(&r, 100);

		/* Skewed addresses so blocks end up with varied valid counts;
		 * ~3% out-of-range addresses exercise the error path too. */
		if (rng_below(&r, 100) < 3)
			lpn = L + (uint32_t)rng_below(&r, 4);
		else if (rng_below(&r, 2))
			lpn = (uint32_t)rng_below(&r, hot);
		else
			lpn = (uint32_t)rng_below(&r, L);

		if (faults && rng_below(&r, 40) == 0)
			nand_inject_program_failure(&f.nand, 1 + rng_below(&r, 4));

		if (op < 60) {
			page_fill(a, g->page_size, lpn, rng_next(&r));
			rf = ftl_write(&f, lpn, a);
			/*
			 * With injected faults a write may be refused: EPROGRAM (the
			 * program itself failed) or, because failed programs consume
			 * pages the progress argument does not account for, ENOSPC /
			 * EGC_STUCK. The reference never sees a refused write, so any
			 * data loss or corruption still shows up as a mismatch.
			 */
			if (faults && f.faults_seen &&
			    (rf == FTL_EPROGRAM || rf == FTL_ENOSPC || rf == FTL_EGC_STUCK)) {
				rr = rf;
				total_refused++;
				refused_streak++;
			} else {
				rr = ref_write(&ref, lpn, a);
				if (rf == FTL_OK)
					refused_streak = 0;
			}
			if (rf != rr) {
				snprintf(msg, sizeof(msg), "op %u write lpn %u: ftl %d ref %d", i,
					 lpn, rf, rr);
				goto fail;
			}
		} else if (op < 85) {
			rf = ftl_read(&f, lpn, a);
			rr = ref_read(&ref, lpn, b);
			if (rf != rr || (rf >= 0 && memcmp(a, b, g->page_size))) {
				snprintf(msg, sizeof(msg), "op %u read lpn %u: ftl %d ref %d%s", i,
					 lpn, rf, rr, rf == rr ? " (data differs)" : "");
				goto fail;
			}
		} else {
			rf = ftl_discard(&f, lpn);
			rr = ref_discard(&ref, lpn);
			if (rf != rr) {
				snprintf(msg, sizeof(msg), "op %u discard lpn %u: ftl %d ref %d", i,
					 lpn, rf, rr);
				goto fail;
			}
		}
		if (ftl_check(&f, msg, sizeof(msg)))
			goto fail;
	}
	/* Final full comparison. */
	for (lpn = 0; lpn < L; lpn++) {
		rf = ftl_read(&f, lpn, a);
		rr = ref_read(&ref, lpn, b);
		if (rf != rr || memcmp(a, b, g->page_size)) {
			snprintf(msg, sizeof(msg), "final read lpn %u: ftl %d ref %d", lpn, rf, rr);
			goto fail;
		}
	}
	total_ops += ops;
	if (f.faults_seen) {
		runs_faulted++;
		/* Wedged: the last 100 or more write attempts were all refused. */
		runs_wedged += refused_streak >= 100;
	}
	total_moved += f.s.gc_pages_moved;
	total_victims += f.s.gc_victims;
	total_failures += f.s.program_failures;
	free(a);
	free(b);
	ref_free(&ref);
	ftl_destroy(&f);
	return 0;
fail:
	fprintf(stderr, "  geo %ux%u spare %u %s %s seed %llu%s: %s\n", g->blocks, g->ppb,
		g->spare, ftl_policy_name(pol), single ? "single" : "dual",
		(unsigned long long)seed, faults ? " (faults)" : "", msg);
	free(a);
	free(b);
	ref_free(&ref);
	ftl_destroy(&f);
	return -1;
}

static uint32_t n_seeds = 40, n_ops = 3000;

static void t_differential(void)
{
	size_t gi;
	uint64_t s;
	int pol, single;

	for (gi = 0; gi < sizeof(GEOS) / sizeof(GEOS[0]); gi++)
		for (pol = 0; pol < 2; pol++)
			for (single = 0; single < 2; single++)
				for (s = 1; s <= n_seeds; s++)
					EXPECT(run_one(&GEOS[gi], (enum ftl_gc_policy)pol, single,
						       s * 1000 + gi, n_ops, false) == 0);
	EXPECT(total_moved > 0 && total_victims > 0, "GC never ran");
}

static void t_differential_with_program_failures(void)
{
	size_t gi;
	uint64_t s;
	int pol;

	for (gi = 0; gi < sizeof(GEOS) / sizeof(GEOS[0]); gi++)
		for (pol = 0; pol < 2; pol++)
			for (s = 1; s <= n_seeds / 2; s++)
				EXPECT(run_one(&GEOS[gi], (enum ftl_gc_policy)pol, false,
					       s * 7919 + gi, n_ops, true) == 0);
	EXPECT(total_failures > 0, "no program failure was ever hit");
}

int main(int argc, char **argv)
{
	if (argc > 1)
		n_seeds = (uint32_t)strtoul(argv[1], NULL, 0);
	if (argc > 2)
		n_ops = (uint32_t)strtoul(argv[2], NULL, 0);
	RUN(t_differential);
	RUN(t_differential_with_program_failures);
	printf("  %llu ops compared, %llu GC victims, %llu pages relocated, "
	       "%llu injected program failures hit, %llu writes refused after faults\n",
	       (unsigned long long)total_ops, (unsigned long long)total_victims,
	       (unsigned long long)total_moved, (unsigned long long)total_failures,
	       (unsigned long long)total_refused);
	printf("  fault runs: %llu hit a fault, %llu ended wedged (>=100 consecutive refused writes)\n",
	       (unsigned long long)runs_faulted, (unsigned long long)runs_wedged);
	DONE();
}
