// SPDX-License-Identifier: MIT
/*
 * Deterministic FTL tests: configuration and boundary handling, read/write/
 * discard semantics, GC behaviour on tiny geometries, failure handling.
 * ftl_check() runs after every mutating operation in these tests.
 */
#include <stdint.h>
#include <stdlib.h>

#include "ftl.h"
#include "refmodel.h"
#include "simalloc.h"
#include "testutil.h"
#include "workload.h"

#define PS 16

static char msg[256];

static struct ftl_config cfg(uint32_t blocks, uint32_t ppb, uint32_t spare,
			     enum ftl_gc_policy pol, bool single)
{
	struct ftl_config c = { 0 };

	c.geo.page_size = PS;
	c.geo.pages_per_block = ppb;
	c.geo.blocks = blocks;
	c.spare_blocks = spare;
	c.policy = pol;
	c.single_frontier = single;
	return c;
}

#define CHECK_INV(f) EXPECT(ftl_check((f), msg, sizeof(msg)) == 0, "%s", msg)

/* Write lpn with version v and mirror it in a version array. */
static int wr(struct ftl *f, uint32_t *ver, uint32_t lpn)
{
	uint8_t b[PS];
	int rc;

	page_fill(b, PS, lpn, ver[lpn] + 1);
	rc = ftl_write(f, lpn, b);
	if (rc == FTL_OK)
		ver[lpn]++;
	return rc;
}

static int rd_ok(struct ftl *f, const uint32_t *ver, uint32_t lpn)
{
	uint8_t b[PS];
	int rc = ftl_read(f, lpn, b);

	if (ver[lpn] == 0)
		return rc == FTL_UNMAPPED;
	return rc == FTL_OK && page_check(b, PS, lpn, ver[lpn]) == 0;
}

static void t_config_validation(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_GREEDY, false);

	EXPECT(ftl_create(&f, NULL) == FTL_EINVAL);
	c.spare_blocks = 2;
	EXPECT(ftl_create(&f, &c) == FTL_EINVAL, "spare below minimum");
	c.spare_blocks = 8;
	c.unsafe_allow_low_spare = true;
	EXPECT(ftl_create(&f, &c) == FTL_EINVAL, "spare >= blocks");
	c = cfg(8, 4, 3, (enum ftl_gc_policy)7, false);
	EXPECT(ftl_create(&f, &c) == FTL_EINVAL, "unknown policy");
	c = cfg(8, 0, 3, FTL_GC_GREEDY, false);
	EXPECT(ftl_create(&f, &c) == FTL_EINVAL, "zero ppb");
	c = cfg(8, 4, 0, FTL_GC_GREEDY, false);
	c.unsafe_allow_low_spare = true;
	EXPECT(ftl_create(&f, &c) == FTL_OK, "unsafe mode allows zero spare");
	ftl_destroy(&f);
	c = cfg(8, 4, 3, FTL_GC_GREEDY, false);
	EXPECT(ftl_create(&f, &c) == FTL_OK);
	EXPECT(f.logical_pages == 20, "logical = (8 - 3) * 4, got %u", f.logical_pages);
	CHECK_INV(&f);
	ftl_destroy(&f);
}

static void t_create_alloc_failure_cleans_up(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_FIFO, false);
	unsigned long k;
	int rc;

	for (k = 1;; k++) {
		sim_alloc_fail_after(k);
		rc = ftl_create(&f, &c);
		sim_alloc_fail_after(0);
		if (rc == FTL_OK)
			break;
		EXPECT(rc == FTL_ENOMEM, "k=%lu rc=%d", k, rc);
	}
	EXPECT(k >= 8, "expected >= 8 allocations, saw %lu", k);
	ftl_destroy(&f);
}

static void t_boundaries_and_bad_input(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_GREEDY, false);
	uint8_t b[PS];
	uint32_t L;

	ftl_create(&f, &c);
	L = f.logical_pages;
	page_fill(b, PS, L - 1, 1);
	EXPECT(ftl_write(&f, L - 1, b) == FTL_OK);
	EXPECT(ftl_write(&f, L, b) == FTL_ERANGE);
	EXPECT(ftl_write(&f, UINT32_MAX, b) == FTL_ERANGE);
	EXPECT(ftl_write(&f, 0, NULL) == FTL_EINVAL);
	EXPECT(ftl_read(&f, L, b) == FTL_ERANGE);
	EXPECT(ftl_read(&f, 0, NULL) == FTL_EINVAL);
	EXPECT(ftl_discard(&f, L) == FTL_ERANGE);
	EXPECT(ftl_discard(&f, UINT32_MAX) == FTL_ERANGE);
	EXPECT(ftl_read(&f, L - 1, b) == FTL_OK && page_check(b, PS, L - 1, 1) == 0);
	EXPECT(f.s.host_writes == 1 && f.mapped == 1, "rejected ops must not change state");
	CHECK_INV(&f);
	ftl_destroy(&f);
}

static void t_unmapped_reads_zero(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_GREEDY, false);
	uint8_t b[PS], z[PS] = { 0 };

	ftl_create(&f, &c);
	memset(b, 0xee, PS);
	EXPECT(ftl_read(&f, 3, b) == FTL_UNMAPPED && !memcmp(b, z, PS));
	ftl_destroy(&f);
}

static void t_discard_read_rewrite(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_FIFO, false);
	uint32_t ver[32] = { 0 };
	uint8_t b[PS], z[PS] = { 0 };

	ftl_create(&f, &c);
	EXPECT(wr(&f, ver, 5) == FTL_OK && rd_ok(&f, ver, 5));
	EXPECT(ftl_discard(&f, 5) == FTL_OK);
	CHECK_INV(&f);
	EXPECT(ftl_read(&f, 5, b) == FTL_UNMAPPED && !memcmp(b, z, PS), "discarded -> zeros");
	EXPECT(ftl_discard(&f, 5) == FTL_OK && f.s.host_discards == 1, "idempotent");
	EXPECT(ftl_discard(&f, 6) == FTL_OK && f.s.host_discards == 1, "never-written lpn");
	EXPECT(f.mapped == 0 && f.nand.valid[0] == 0);
	EXPECT(wr(&f, ver, 5) == FTL_OK && rd_ok(&f, ver, 5), "rewrite after discard");
	CHECK_INV(&f);
	ftl_destroy(&f);
}

static void t_repeated_writes_one_lpn(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_GREEDY, false);
	uint32_t ver[32] = { 0 }, i;

	ftl_create(&f, &c);
	for (i = 0; i < 20 * f.nand.total_pages; i++) {
		EXPECT(wr(&f, ver, 0) == FTL_OK, "write %u", i);
		EXPECT(rd_ok(&f, ver, 0), "read after write %u", i);
		CHECK_INV(&f);
	}
	EXPECT(f.s.gc_victims > 0, "GC never ran");
	/* Only one live page ever exists, so a victim holds at most one valid page. */
	EXPECT(f.s.gc_pages_moved <= f.s.gc_victims);
	ftl_destroy(&f);
}

static void t_sequential_overwrite_moves_nothing(void)
{
	struct ftl f;
	struct ftl_config c = cfg(16, 8, 3, FTL_GC_GREEDY, false);
	uint32_t ver[128] = { 0 }, pass, lpn;

	ftl_create(&f, &c);
	for (pass = 0; pass < 6; pass++)
		for (lpn = 0; lpn < f.logical_pages; lpn++) {
			EXPECT(wr(&f, ver, lpn) == FTL_OK);
			CHECK_INV(&f);
		}
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		EXPECT(rd_ok(&f, ver, lpn), "lpn %u", lpn);
	/* Whole blocks are invalidated in order: greedy always finds empty victims. */
	EXPECT(f.s.gc_victims > 0 && f.s.gc_pages_moved == 0,
	       "victims %llu moved %llu", (unsigned long long)f.s.gc_victims,
	       (unsigned long long)f.s.gc_pages_moved);
	ftl_destroy(&f);
}

/* Random overwrites on a full device, invariants after every op. */
static void soak(uint32_t blocks, uint32_t ppb, uint32_t spare, enum ftl_gc_policy pol,
		 bool single, uint64_t writes, uint64_t seed, uint64_t *moved_out,
		 bool *saw_partial_gc_frontier)
{
	struct ftl f;
	struct ftl_config c = cfg(blocks, ppb, spare, pol, single);
	struct rng r;
	uint32_t *ver, lpn;
	uint64_t i;

	EXPECT(ftl_create(&f, &c) == FTL_OK);
	ver = calloc(f.logical_pages, sizeof(*ver));
	rng_seed(&r, seed);
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		EXPECT(wr(&f, ver, lpn) == FTL_OK, "fill %u", lpn);
	for (i = 0; i < writes; i++) {
		lpn = (uint32_t)rng_below(&r, f.logical_pages);
		EXPECT(wr(&f, ver, lpn) == FTL_OK, "write %llu", (unsigned long long)i);
		CHECK_INV(&f);
		if (saw_partial_gc_frontier && f.gc_frontier != FTL_NO_BLOCK &&
		    f.nand.write_ptr[f.gc_frontier] > 0 && f.nand.write_ptr[f.gc_frontier] < ppb)
			*saw_partial_gc_frontier = true;
	}
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		EXPECT(rd_ok(&f, ver, lpn), "lpn %u", lpn);
	EXPECT(f.s.gc_max_steps <= 2ULL * ppb + 1, "GC episode took %llu steps",
	       (unsigned long long)f.s.gc_max_steps);
	if (moved_out)
		*moved_out = f.s.gc_pages_moved;
	free(ver);
	ftl_destroy(&f);
}

static void t_minimal_spare_tiny_geometries(void)
{
	static const uint32_t geos[][2] = { { 4, 1 }, { 4, 2 }, { 5, 3 }, { 6, 4 }, { 8, 8 } };
	bool partial = false;
	uint64_t moved = 0;
	size_t i;
	int pol, single;

	/* Spare = 3 blocks (the minimum): logical capacity is as full as allowed. */
	for (i = 0; i < sizeof(geos) / sizeof(geos[0]); i++)
		for (pol = 0; pol < 2; pol++)
			for (single = 0; single < 2; single++) {
				soak(geos[i][0], geos[i][1], 3, (enum ftl_gc_policy)pol,
				     single, 3000, 11 + i, &moved, &partial);
				if (t_failures)
					return;
			}
	EXPECT(moved > 0, "tiny devices should relocate data");
	EXPECT(partial, "dual-frontier runs never had a partially filled GC frontier");
}

static void t_repeated_gc_larger(void)
{
	uint64_t moved = 0;

	soak(32, 16, 4, FTL_GC_GREEDY, false, 40000, 3, &moved, NULL);
	EXPECT(moved > 0);
	soak(32, 16, 4, FTL_GC_FIFO, false, 40000, 3, &moved, NULL);
	EXPECT(moved > 0);
}

static void t_data_preserved_through_relocation(void)
{
	int pol;

	/* Cold data: every page written once. Hot data: one page per block
	 * (lpn % 8 == 0) rewritten repeatedly, so every block that holds cold
	 * data also receives invalidations and GC must move the cold pages. */
	for (pol = 0; pol < 2; pol++) {
		struct ftl f;
		struct ftl_config c = cfg(12, 8, 3, (enum ftl_gc_policy)pol, false);
		uint32_t ver[96] = { 0 }, lpn, i;

		ftl_create(&f, &c);
		for (lpn = 0; lpn < f.logical_pages; lpn++)
			wr(&f, ver, lpn);
		for (i = 0; i < 5000; i++) {
			EXPECT(wr(&f, ver, (i % 9) * 8) == FTL_OK);
			CHECK_INV(&f);
		}
		EXPECT(f.s.gc_pages_moved > 0, "%s: no relocation happened",
		       ftl_policy_name(c.policy));
		for (lpn = 0; lpn < f.logical_pages; lpn++)
			EXPECT(rd_ok(&f, ver, lpn), "lpn %u lost after relocation", lpn);
		printf("  %s: %llu cold pages relocated, all %u pages verified\n",
		       ftl_policy_name(c.policy), (unsigned long long)f.s.gc_pages_moved,
		       f.logical_pages);
		ftl_destroy(&f);
	}
}

static void t_failed_overwrite_keeps_old_data(void)
{
	struct ftl f;
	struct ftl_config c = cfg(8, 4, 3, FTL_GC_GREEDY, false);
	uint32_t ver[32] = { 0 }, lpn;
	uint8_t b[PS];

	ftl_create(&f, &c);
	EXPECT(wr(&f, ver, 5) == FTL_OK);
	nand_inject_program_failure(&f.nand, 1);
	page_fill(b, PS, 5, 999);
	EXPECT(ftl_write(&f, 5, b) == FTL_EPROGRAM);
	EXPECT(rd_ok(&f, ver, 5), "old version must still be readable");
	CHECK_INV(&f);
	EXPECT(wr(&f, ver, 5) == FTL_OK && rd_ok(&f, ver, 5), "next write succeeds");

	/* Failure while GC is relocating: fill, then fail programs during GC. */
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		EXPECT(wr(&f, ver, lpn) == FTL_OK);
	for (lpn = 0; lpn < 200; lpn++) {
		int rc;

		if (lpn % 7 == 0)
			nand_inject_program_failure(&f.nand, 1 + lpn % 3);
		rc = wr(&f, ver, (lpn * 5) % f.logical_pages);
		EXPECT(rc == FTL_OK || rc == FTL_EPROGRAM || rc == FTL_EGC_STUCK ||
		       rc == FTL_ENOSPC, "rc=%d", rc);
		CHECK_INV(&f);
	}
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		EXPECT(rd_ok(&f, ver, lpn), "lpn %u", lpn);
	EXPECT(f.s.program_failures > 0);
	ftl_destroy(&f);
}

static void t_low_spare_fails_defined(void)
{
	uint32_t spare;

	/* Below the minimum spare the FTL may run out of reclaimable space, but it
	 * must report a defined error, terminate, and never corrupt mapped data. */
	for (spare = 0; spare < FTL_MIN_SPARE_BLOCKS; spare++) {
		struct ftl f;
		struct ftl_config c = cfg(6, 4, spare, FTL_GC_GREEDY, false);
		struct rng r;
		uint32_t ver[32] = { 0 }, i, lpn;
		int rc = FTL_OK, errors = 0;

		c.unsafe_allow_low_spare = true;
		EXPECT(ftl_create(&f, &c) == FTL_OK);
		rng_seed(&r, spare);
		for (i = 0; i < 2000; i++) {
			lpn = (uint32_t)rng_below(&r, f.logical_pages);
			rc = wr(&f, ver, lpn);
			EXPECT(rc == FTL_OK || rc == FTL_EGC_STUCK || rc == FTL_ENOSPC,
			       "spare %u: rc=%d (%s)", spare, rc, ftl_strerror(rc));
			errors += rc != FTL_OK;
			CHECK_INV(&f);
		}
		for (lpn = 0; lpn < f.logical_pages; lpn++)
			EXPECT(rd_ok(&f, ver, lpn), "spare %u lpn %u", spare, lpn);
		EXPECT(spare == 2 || errors > 0, "spare %u never hit the limit", spare);
		printf("  spare=%u: %d of 2000 writes refused with a defined error\n", spare, errors);
		ftl_destroy(&f);
	}
}

static void t_mem_model(void)
{
	struct ftl f;
	struct ftl_config c = cfg(1024, 64, 72, FTL_GC_GREEDY, false);
	struct ftl_mem m;

	c.geo.page_size = 4096;
	EXPECT(ftl_create(&f, &c) == FTL_OK);
	ftl_mem_model(&f, &m);
	EXPECT(m.l2p_bytes == (uint64_t)f.logical_pages * 4);
	EXPECT(m.valid_bitmap_bytes == 1024 * 64 / 8);
	EXPECT(m.total_bytes == m.l2p_bytes + m.valid_bitmap_bytes + m.block_meta_bytes);
	ftl_destroy(&f);
}

int main(void)
{
	RUN(t_config_validation);
	RUN(t_create_alloc_failure_cleans_up);
	RUN(t_boundaries_and_bad_input);
	RUN(t_unmapped_reads_zero);
	RUN(t_discard_read_rewrite);
	RUN(t_repeated_writes_one_lpn);
	RUN(t_sequential_overwrite_moves_nothing);
	RUN(t_minimal_spare_tiny_geometries);
	RUN(t_repeated_gc_larger);
	RUN(t_data_preserved_through_relocation);
	RUN(t_failed_overwrite_keeps_old_data);
	RUN(t_low_spare_fails_defined);
	RUN(t_mem_model);
	DONE();
}
