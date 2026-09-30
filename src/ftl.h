/* SPDX-License-Identifier: MIT */
/*
 * Page-mapped flash translation layer over the simulated NAND in nand.h.
 *
 * Host interface (all addresses are logical page numbers, LPNs):
 *   ftl_write    out-of-place write of one full page
 *   ftl_read     returns the last written data, or zeros + FTL_UNMAPPED if the
 *                page was never written or has been discarded
 *   ftl_discard  (TRIM) unmaps the page; idempotent; later reads return zeros
 *
 * Capacity: logical pages = (blocks - spare_blocks) * pages_per_block.
 * spare_blocks must be >= FTL_MIN_SPARE_BLOCKS; see DESIGN.md for why three
 * blocks are the minimum for the garbage collector to always make progress.
 */
#ifndef FTL_H
#define FTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nand.h"

#define FTL_UNMAPPED_PPN	UINT32_MAX
#define FTL_NO_BLOCK		UINT32_MAX
#define FTL_MIN_SPARE_BLOCKS	3U

enum ftl_rc {
	FTL_OK = 0,
	FTL_UNMAPPED = 1,	/* read status: page not mapped, zeros returned */
	FTL_EINVAL = -1,	/* bad argument or configuration */
	FTL_ERANGE = -2,	/* LPN >= logical capacity */
	FTL_ENOSPC = -3,	/* no free block where one is required */
	FTL_EGC_STUCK = -4,	/* GC found no reclaimable block / hit its bound */
	FTL_EPROGRAM = -5,	/* NAND program failed; previous data preserved */
	FTL_ENOMEM = -6,
	FTL_EINTERNAL = -7,	/* NAND rejected an operation: FTL bug */
};

enum ftl_gc_policy {
	FTL_GC_GREEDY,		/* sealed block with the fewest valid pages */
	FTL_GC_FIFO,		/* oldest sealed block (by seal order) */
};

enum ftl_block_state {
	BLK_FREE = 0,
	BLK_HOST_OPEN,		/* host write frontier */
	BLK_GC_OPEN,		/* GC relocation frontier */
	BLK_SEALED,		/* fully programmed; GC candidate */
};

struct ftl_config {
	struct nand_geometry geo;
	uint32_t spare_blocks;
	enum ftl_gc_policy policy;
	/*
	 * true: GC relocations are appended to the host frontier (one write
	 * stream, as assumed by the classic analytic WA models).
	 * false (default): separate host and GC frontiers.
	 */
	bool single_frontier;
	/* Tests only: allow spare_blocks < FTL_MIN_SPARE_BLOCKS. */
	bool unsafe_allow_low_spare;
};

struct ftl_stats {
	uint64_t host_writes;		/* successful host page writes */
	uint64_t host_write_errors;
	uint64_t host_reads;
	uint64_t host_reads_unmapped;
	uint64_t host_discards;		/* discards of mapped pages */
	uint64_t nand_programs_host;
	uint64_t nand_programs_gc;
	uint64_t nand_reads_gc;
	uint64_t gc_invocations;	/* foreground GC episodes */
	uint64_t gc_victims;		/* blocks reclaimed */
	uint64_t gc_pages_moved;	/* valid pages relocated */
	uint64_t gc_max_steps;		/* most victims needed by one episode */
	uint64_t erases;
	uint64_t program_failures;
};

struct ftl {
	struct ftl_config cfg;
	struct nand nand;
	uint32_t ppb;
	uint32_t logical_pages;
	uint32_t *l2p;			/* logical_pages entries */
	uint8_t *blk_state;		/* enum ftl_block_state per block */
	uint64_t *seal_seq;		/* per block: order in which it was sealed */
	uint32_t *free_q;		/* ring of free block ids (FIFO reuse) */
	uint32_t free_head;
	uint32_t free_count;
	uint32_t host_frontier;		/* FTL_NO_BLOCK when none open */
	uint32_t gc_frontier;
	uint64_t next_seal_seq;
	uint64_t write_seq;		/* stored in OOB: newest copy wins on recovery */
	uint64_t mapped;		/* number of mapped LPNs */
	uint8_t *buf;			/* one page, for GC relocation */
	bool faults_seen;		/* a NAND fault was injected and hit */
	struct ftl_stats s;
};

int ftl_create(struct ftl *f, const struct ftl_config *cfg);
void ftl_destroy(struct ftl *f);

int ftl_write(struct ftl *f, uint32_t lpn, const void *data);
int ftl_read(struct ftl *f, uint32_t lpn, void *data);
int ftl_discard(struct ftl *f, uint32_t lpn);

/*
 * Check every structural invariant (see DESIGN.md "Invariants"). Returns 0,
 * or -1 and writes a description of the first violation into msg.
 */
int ftl_check(const struct ftl *f, char *msg, size_t msglen);

/* Bytes of FTL metadata a real controller would keep in DRAM for this config. */
struct ftl_mem {
	uint64_t l2p_bytes;
	uint64_t valid_bitmap_bytes;
	uint64_t block_meta_bytes;
	uint64_t total_bytes;
};
void ftl_mem_model(const struct ftl *f, struct ftl_mem *m);

const char *ftl_strerror(int rc);
const char *ftl_policy_name(enum ftl_gc_policy p);

#endif
