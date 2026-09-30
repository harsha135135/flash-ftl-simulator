// SPDX-License-Identifier: MIT
/*
 * Page-mapped FTL: out-of-place writes, foreground garbage collection with
 * greedy or FIFO victim selection, and separate host/GC write frontiers.
 *
 * Space rules (the forward-progress argument is in DESIGN.md):
 *   - GC capacity C = free_blocks * P + g, where P = pages per block and g =
 *     erased pages left in the dedicated GC frontier (0 in single-frontier
 *     mode). Invariant: C >= P - 1, i.e. GC can always relocate the valid
 *     pages of any eligible victim (at most P - 1 of them).
 *   - The host may open a new block only if the invariant still holds
 *     afterwards: (free_blocks - 1) * P + g >= P - 1.
 *   - GC runs only when the host needs a new block and may not open one. It
 *     reclaims victims until the host may open a block (or, in single-frontier
 *     mode, until GC has left room in the shared frontier).
 *   - A victim must be SEALED (never an open frontier) and contain at least
 *     one invalid page (valid < pages_per_block); a fully valid block would
 *     cost a whole block of copies and free nothing.
 *   - Each GC episode is bounded; exceeding the bound returns FTL_EGC_STUCK
 *     instead of looping.
 *
 * Update ordering, for host writes and GC relocation alike: program the new
 * copy, then switch the mapping, then invalidate the old copy. A failure
 * before the mapping switch leaves the previous data mapped and readable.
 */
#include "ftl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simalloc.h"

enum stream { STREAM_HOST, STREAM_GC };

const char *ftl_strerror(int rc)
{
	switch (rc) {
	case FTL_OK: return "ok";
	case FTL_UNMAPPED: return "unmapped";
	case FTL_EINVAL: return "invalid argument";
	case FTL_ERANGE: return "logical page out of range";
	case FTL_ENOSPC: return "no free block";
	case FTL_EGC_STUCK: return "garbage collection cannot make progress";
	case FTL_EPROGRAM: return "program failed";
	case FTL_ENOMEM: return "out of memory";
	case FTL_EINTERNAL: return "internal inconsistency";
	default: return "unknown";
	}
}

const char *ftl_policy_name(enum ftl_gc_policy p)
{
	return p == FTL_GC_GREEDY ? "greedy" : p == FTL_GC_FIFO ? "fifo" : "?";
}

/* ---- construction ---- */

void ftl_destroy(struct ftl *f)
{
	nand_free(&f->nand);
	free(f->l2p);
	free(f->blk_state);
	free(f->seal_seq);
	free(f->free_q);
	free(f->buf);
	memset(f, 0, sizeof(*f));
}

int ftl_create(struct ftl *f, const struct ftl_config *cfg)
{
	const struct nand_geometry *g;
	uint32_t i;
	int rc;

	memset(f, 0, sizeof(*f));
	if (!cfg)
		return FTL_EINVAL;
	g = &cfg->geo;
	if (g->page_size == 0 || g->pages_per_block == 0 || g->blocks == 0 ||
	    cfg->spare_blocks >= g->blocks ||
	    (cfg->policy != FTL_GC_GREEDY && cfg->policy != FTL_GC_FIFO))
		return FTL_EINVAL;
	if (cfg->spare_blocks < FTL_MIN_SPARE_BLOCKS && !cfg->unsafe_allow_low_spare)
		return FTL_EINVAL;
	if ((uint64_t)(g->blocks - cfg->spare_blocks) * g->pages_per_block >= FTL_UNMAPPED_PPN)
		return FTL_EINVAL;

	rc = nand_init(&f->nand, g);
	if (rc)
		return rc == NAND_ENOMEM ? FTL_ENOMEM : FTL_EINVAL;
	f->cfg = *cfg;
	f->ppb = g->pages_per_block;
	f->logical_pages = (g->blocks - cfg->spare_blocks) * g->pages_per_block;
	f->l2p = sim_calloc(f->logical_pages, sizeof(*f->l2p));
	f->blk_state = sim_calloc(g->blocks, sizeof(*f->blk_state));
	f->seal_seq = sim_calloc(g->blocks, sizeof(*f->seal_seq));
	f->free_q = sim_calloc(g->blocks, sizeof(*f->free_q));
	f->buf = sim_calloc(g->page_size, 1);
	if (!f->l2p || !f->blk_state || !f->seal_seq || !f->free_q || !f->buf) {
		ftl_destroy(f);
		return FTL_ENOMEM;
	}
	for (i = 0; i < f->logical_pages; i++)
		f->l2p[i] = FTL_UNMAPPED_PPN;
	for (i = 0; i < g->blocks; i++)
		f->free_q[i] = i;
	f->free_count = g->blocks;
	f->host_frontier = f->gc_frontier = FTL_NO_BLOCK;
	return FTL_OK;
}

/* ---- block allocation ---- */

static uint32_t free_pop(struct ftl *f)
{
	uint32_t b = f->free_q[f->free_head];

	f->free_head = (f->free_head + 1) % f->cfg.geo.blocks;
	f->free_count--;
	return b;
}

static void free_push(struct ftl *f, uint32_t b)
{
	f->free_q[(f->free_head + f->free_count) % f->cfg.geo.blocks] = b;
	f->free_count++;
}

static bool has_room(const struct ftl *f, uint32_t blk)
{
	return blk != FTL_NO_BLOCK && f->nand.write_ptr[blk] < f->ppb;
}

static void seal(struct ftl *f, uint32_t *frontier)
{
	f->blk_state[*frontier] = BLK_SEALED;
	f->seal_seq[*frontier] = f->next_seal_seq++;
	*frontier = FTL_NO_BLOCK;
}

static uint32_t *frontier_of(struct ftl *f, enum stream s)
{
	return (s == STREAM_GC && !f->cfg.single_frontier) ? &f->gc_frontier
							    : &f->host_frontier;
}

/* Next page of an open frontier (caller checked has_room). */
static uint32_t frontier_page(const struct ftl *f, uint32_t blk)
{
	return nand_first_page(&f->nand, blk) + f->nand.write_ptr[blk];
}

static int open_frontier(struct ftl *f, uint32_t *frontier, enum stream s)
{
	uint32_t b;

	if (f->free_count == 0)
		return FTL_ENOSPC;
	b = free_pop(f);
	f->blk_state[b] = (s == STREAM_GC && !f->cfg.single_frontier) ? BLK_GC_OPEN
								      : BLK_HOST_OPEN;
	*frontier = b;
	return FTL_OK;
}

/* ---- garbage collection ---- */

static uint32_t select_victim(const struct ftl *f)
{
	uint32_t b, best = FTL_NO_BLOCK;

	for (b = 0; b < f->cfg.geo.blocks; b++) {
		if (f->blk_state[b] != BLK_SEALED || f->nand.valid[b] >= f->ppb)
			continue;	/* ineligible: open/free, or nothing to gain */
		if (best == FTL_NO_BLOCK) {
			best = b;
		} else if (f->cfg.policy == FTL_GC_GREEDY) {
			if (f->nand.valid[b] < f->nand.valid[best] ||
			    (f->nand.valid[b] == f->nand.valid[best] &&
			     f->seal_seq[b] < f->seal_seq[best]))
				best = b;
		} else if (f->seal_seq[b] < f->seal_seq[best]) {
			best = b;
		}
	}
	return best;
}

/* Page for a GC relocation. May take the last free block (the reserve). */
static int gc_alloc_page(struct ftl *f, uint32_t *ppn)
{
	uint32_t *fr = frontier_of(f, STREAM_GC);
	int rc;

	if (!has_room(f, *fr)) {
		if (*fr != FTL_NO_BLOCK)
			seal(f, fr);
		rc = open_frontier(f, fr, STREAM_GC);
		if (rc)
			return rc;
	}
	*ppn = frontier_page(f, *fr);
	return FTL_OK;
}

/* Move one valid page out of a victim: program copy, remap, invalidate. */
static int relocate(struct ftl *f, uint32_t src)
{
	struct nand_oob oob, noob = { 0 };
	uint32_t dst;
	int rc;

	if (nand_read(&f->nand, src, f->buf, &oob) != NAND_OK)
		return FTL_EINTERNAL;
	f->s.nand_reads_gc++;
	if (oob.lpn >= f->logical_pages || f->l2p[oob.lpn] != src)
		return FTL_EINTERNAL;	/* reverse map disagrees with forward map */

	for (;;) {
		rc = gc_alloc_page(f, &dst);
		if (rc)
			return rc;	/* victim keeps its remaining valid pages */
		noob.lpn = oob.lpn;
		noob.seq = ++f->write_seq;
		rc = nand_program(&f->nand, dst, f->buf, &noob);
		if (rc == NAND_OK)
			break;
		if (rc != NAND_EPROGRAM_FAIL)
			return FTL_EINTERNAL;
		f->s.program_failures++;	/* page consumed; retry on the next one */
		f->faults_seen = true;
	}
	f->l2p[oob.lpn] = dst;
	if (nand_invalidate(&f->nand, src) != NAND_OK)
		return FTL_EINTERNAL;
	f->s.nand_programs_gc++;
	f->s.gc_pages_moved++;
	return FTL_OK;
}

static int gc_step(struct ftl *f)
{
	uint32_t victim = select_victim(f), first, i;
	int rc;

	if (victim == FTL_NO_BLOCK)
		return FTL_EGC_STUCK;
	first = nand_first_page(&f->nand, victim);
	for (i = 0; i < f->ppb; i++) {
		if (f->nand.state[first + i] != PAGE_VALID)
			continue;
		rc = relocate(f, first + i);
		if (rc)
			return rc;
	}
	if (nand_erase(&f->nand, victim) != NAND_OK)
		return FTL_EINTERNAL;	/* would mean live data was still present */
	f->blk_state[victim] = BLK_FREE;
	free_push(f, victim);
	f->s.erases++;
	f->s.gc_victims++;
	return FTL_OK;
}

/* Erased pages GC can still use in its own frontier (dual-frontier mode only). */
static uint64_t gc_frontier_room(const struct ftl *f)
{
	if (f->cfg.single_frontier || !has_room(f, f->gc_frontier))
		return 0;
	return f->ppb - f->nand.write_ptr[f->gc_frontier];
}

/* May the host take a free block without breaking the GC capacity invariant? */
static bool host_may_open(const struct ftl *f)
{
	if (f->cfg.reserve_whole_blocks)
		return f->free_count >= 2;
	return f->free_count > 0 &&
	       (uint64_t)(f->free_count - 1) * f->ppb + gc_frontier_room(f) >= f->ppb - 1;
}

/*
 * Reclaim until the host may open a block. While the host may not, the GC
 * capacity C = free * P + g is below 2P - 1; each step raises C by
 * P - valid(victim) >= 1, so a correct configuration needs fewer than 2P
 * steps. The bound turns any violation of that argument (injected faults,
 * unsafe spare) into a defined error instead of a livelock.
 */
static int gc_until_host_can_open(struct ftl *f)
{
	uint64_t steps = 0, limit = 2ULL * f->ppb + 2;
	int rc;

	if (host_may_open(f))
		return FTL_OK;
	f->s.gc_invocations++;
	while (!host_may_open(f)) {
		/* Single frontier: GC may have opened the shared frontier for us. */
		if (f->cfg.single_frontier && has_room(f, f->host_frontier))
			break;
		if (steps == limit)
			return FTL_EGC_STUCK;
		rc = gc_step(f);
		if (rc)
			return rc;
		steps++;
	}
	if (steps > f->s.gc_max_steps)
		f->s.gc_max_steps = steps;
	return FTL_OK;
}

static int host_alloc_page(struct ftl *f, uint32_t *ppn)
{
	uint32_t *fr = &f->host_frontier;
	int rc;

	if (!has_room(f, *fr)) {
		if (*fr != FTL_NO_BLOCK)
			seal(f, fr);
		rc = gc_until_host_can_open(f);
		if (rc)
			return rc;
		/* In single-frontier mode GC may have opened the shared frontier. */
		if (!has_room(f, *fr)) {
			if (*fr != FTL_NO_BLOCK)
				seal(f, fr);
			rc = open_frontier(f, fr, STREAM_HOST);
			if (rc)
				return rc;
		}
	}
	*ppn = frontier_page(f, *fr);
	return FTL_OK;
}

/* ---- host operations ---- */

int ftl_write(struct ftl *f, uint32_t lpn, const void *data)
{
	struct nand_oob oob = { 0 };
	uint32_t ppn, old;
	int rc;

	if (!data)
		return FTL_EINVAL;
	if (lpn >= f->logical_pages)
		return FTL_ERANGE;
	/* Allocate first: GC may relocate this LPN, so read l2p afterwards. */
	rc = host_alloc_page(f, &ppn);
	if (rc) {
		f->s.host_write_errors++;
		return rc;
	}
	oob.lpn = lpn;
	oob.seq = ++f->write_seq;
	rc = nand_program(&f->nand, ppn, data, &oob);
	if (rc == NAND_EPROGRAM_FAIL) {
		f->s.program_failures++;
		f->s.host_write_errors++;
		f->faults_seen = true;
		return FTL_EPROGRAM;	/* mapping untouched: old data still readable */
	}
	if (rc)
		return FTL_EINTERNAL;
	old = f->l2p[lpn];
	f->l2p[lpn] = ppn;
	if (old == FTL_UNMAPPED_PPN)
		f->mapped++;
	else if (nand_invalidate(&f->nand, old) != NAND_OK)
		return FTL_EINTERNAL;
	f->s.host_writes++;
	f->s.nand_programs_host++;
	return FTL_OK;
}

int ftl_read(struct ftl *f, uint32_t lpn, void *data)
{
	struct nand_oob oob;
	uint32_t ppn;

	if (!data)
		return FTL_EINVAL;
	if (lpn >= f->logical_pages)
		return FTL_ERANGE;
	f->s.host_reads++;
	ppn = f->l2p[lpn];
	if (ppn == FTL_UNMAPPED_PPN) {
		memset(data, 0, f->cfg.geo.page_size);
		f->s.host_reads_unmapped++;
		return FTL_UNMAPPED;
	}
	if (nand_read(&f->nand, ppn, data, &oob) != NAND_OK || oob.lpn != lpn)
		return FTL_EINTERNAL;
	return FTL_OK;
}

int ftl_discard(struct ftl *f, uint32_t lpn)
{
	uint32_t ppn;

	if (lpn >= f->logical_pages)
		return FTL_ERANGE;
	ppn = f->l2p[lpn];
	if (ppn == FTL_UNMAPPED_PPN)
		return FTL_OK;
	if (nand_invalidate(&f->nand, ppn) != NAND_OK)
		return FTL_EINTERNAL;
	f->l2p[lpn] = FTL_UNMAPPED_PPN;
	f->mapped--;
	f->s.host_discards++;
	return FTL_OK;
}

/* ---- invariants ---- */

#define FAIL(...)                                          \
	do {                                               \
		if (msg && msglen)                         \
			snprintf(msg, msglen, __VA_ARGS__); \
		return -1;                                 \
	} while (0)

int ftl_check(const struct ftl *f, char *msg, size_t msglen)
{
	const struct nand *n = &f->nand;
	uint64_t valid_total = 0, mapped = 0;
	uint32_t b, i, lpn, nfree = 0, nhost = 0, ngc = 0;

	for (b = 0; b < n->g.blocks; b++) {
		uint32_t first = nand_first_page(n, b), wp = n->write_ptr[b], v = 0;

		if (wp > f->ppb)
			FAIL("block %u: write pointer %u > %u", b, wp, f->ppb);
		for (i = 0; i < f->ppb; i++) {
			uint32_t ppn = first + i;
			uint8_t st = n->state[ppn];

			if (i < wp && st == PAGE_ERASED)
				FAIL("block %u page %u: erased below write pointer %u", b, i, wp);
			if (i >= wp && st != PAGE_ERASED)
				FAIL("block %u page %u: programmed at/above write pointer %u", b, i, wp);
			if (st != PAGE_VALID)
				continue;
			v++;
			if (n->oob[ppn].lpn >= f->logical_pages ||
			    f->l2p[n->oob[ppn].lpn] != ppn)
				FAIL("ppn %u (lpn %u in OOB) is VALID but not mapped to it",
				     ppn, n->oob[ppn].lpn);
		}
		if (v != n->valid[b])
			FAIL("block %u: valid count %u, actual %u", b, n->valid[b], v);
		valid_total += v;

		switch (f->blk_state[b]) {
		case BLK_FREE:
			nfree++;
			if (wp != 0)
				FAIL("free block %u is not erased (wp %u)", b, wp);
			break;
		case BLK_SEALED:
			if (wp != f->ppb)
				FAIL("sealed block %u not full (wp %u)", b, wp);
			break;
		case BLK_HOST_OPEN:
			nhost++;
			if (b != f->host_frontier)
				FAIL("block %u HOST_OPEN but host frontier is %u", b, f->host_frontier);
			break;
		case BLK_GC_OPEN:
			ngc++;
			if (b != f->gc_frontier || f->cfg.single_frontier)
				FAIL("block %u GC_OPEN but gc frontier is %u", b, f->gc_frontier);
			break;
		default:
			FAIL("block %u: bad state %u", b, f->blk_state[b]);
		}
	}
	if (nhost > 1 || ngc > 1)
		FAIL("%u host and %u gc frontiers open", nhost, ngc);
	if ((f->host_frontier != FTL_NO_BLOCK) != (nhost == 1) ||
	    (f->gc_frontier != FTL_NO_BLOCK) != (ngc == 1))
		FAIL("frontier bookkeeping mismatch");
	if (nfree != f->free_count)
		FAIL("free_count %u, FREE blocks %u", f->free_count, nfree);
	for (i = 0; i < f->free_count; i++) {
		b = f->free_q[(f->free_head + i) % n->g.blocks];
		if (b >= n->g.blocks || f->blk_state[b] != BLK_FREE)
			FAIL("free queue entry %u (block %u) is not a free block", i, b);
	}

	for (lpn = 0; lpn < f->logical_pages; lpn++) {
		uint32_t ppn = f->l2p[lpn];

		if (ppn == FTL_UNMAPPED_PPN)
			continue;
		mapped++;
		if (ppn >= n->total_pages)
			FAIL("lpn %u maps to out-of-range ppn %u", lpn, ppn);
		if (n->state[ppn] != PAGE_VALID)
			FAIL("lpn %u maps to %s page %u", lpn,
			     n->state[ppn] == PAGE_ERASED ? "an erased" : "an invalid", ppn);
		if (n->oob[ppn].lpn != lpn)
			FAIL("lpn %u maps to ppn %u whose OOB says lpn %u", lpn, ppn,
			     n->oob[ppn].lpn);
	}
	if (mapped != valid_total || mapped != f->mapped)
		FAIL("mapped %llu, valid pages %llu, counter %llu", (unsigned long long)mapped,
		     (unsigned long long)valid_total, (unsigned long long)f->mapped);
	/* GC capacity invariant, after every completed operation in a fault-free run. */
	if (!f->faults_seen && f->cfg.spare_blocks >= FTL_MIN_SPARE_BLOCKS &&
	    (uint64_t)f->free_count * f->ppb + gc_frontier_room(f) < f->ppb - 1)
		FAIL("GC capacity invariant violated: %u free blocks, %llu GC frontier pages",
		     f->free_count, (unsigned long long)gc_frontier_room(f));
	return 0;
}

void ftl_mem_model(const struct ftl *f, struct ftl_mem *m)
{
	uint64_t blocks = f->cfg.geo.blocks;

	/* 4-byte PPN per logical page; 1 valid bit per physical page; per block:
	 * valid count (4), state (1), seal order (8), erase count (4), write
	 * pointer (4), free-queue slot (4). OOB reverse map lives in NAND. */
	m->l2p_bytes = (uint64_t)f->logical_pages * sizeof(uint32_t);
	m->valid_bitmap_bytes = ((uint64_t)f->nand.total_pages + 7) / 8;
	m->block_meta_bytes = blocks * (4 + 1 + 8 + 4 + 4 + 4);
	m->total_bytes = m->l2p_bytes + m->valid_bitmap_bytes + m->block_meta_bytes;
}
