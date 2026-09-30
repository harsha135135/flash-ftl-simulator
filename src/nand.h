/* SPDX-License-Identifier: MIT */
/*
 * Simulated NAND flash array.
 *
 * Geometry: `blocks` erase blocks of `pages_per_block` pages of `page_size`
 * bytes. Each page also has an out-of-band (spare) area holding the logical
 * page number and a write sequence number, as real FTLs store in the NAND
 * spare area; GC uses it as the reverse (physical -> logical) map.
 *
 * Rules enforced (violations return an error and change nothing):
 *   - a page can be programmed only while ERASED, and only at the block's
 *     write pointer (pages within a block are programmed in order);
 *   - erase works on whole blocks and resets every page to ERASED;
 *   - a block that still holds VALID pages cannot be erased (simulator
 *     safety rule that catches premature erases by the FTL);
 *   - reading an ERASED page fails; reading an INVALID (stale) page fails,
 *     so an FTL that follows a stale mapping is caught immediately.
 *
 * Physical NAND only knows erased vs programmed. The VALID/INVALID
 * distinction is FTL metadata; it is tracked here, next to the page, so that
 * the rules above can be enforced in one place. See DESIGN.md for the other
 * simplifications (no bit errors, no read disturb, no multi-plane, etc.).
 */
#ifndef NAND_H
#define NAND_H

#include <stdbool.h>
#include <stdint.h>

enum nand_page_state {
	PAGE_ERASED = 0,
	PAGE_VALID,
	PAGE_INVALID,
};

enum nand_rc {
	NAND_OK = 0,
	NAND_ERANGE = -1,	/* page/block number out of range */
	NAND_ENOT_ERASED = -2,	/* program of a non-erased page */
	NAND_EORDER = -3,	/* program not at the block's write pointer */
	NAND_EHAS_VALID = -4,	/* erase of a block with valid pages */
	NAND_EERASED = -5,	/* read of an erased page */
	NAND_ESTALE = -6,	/* read of an invalid page */
	NAND_ESTATE = -7,	/* invalidate of a non-valid page */
	NAND_EPROGRAM_FAIL = -8,/* injected program failure (page consumed) */
	NAND_EINVAL = -9,	/* bad geometry or argument */
	NAND_ENOMEM = -10,
};

struct nand_geometry {
	uint32_t page_size;
	uint32_t pages_per_block;
	uint32_t blocks;
};

struct nand_oob {
	uint32_t lpn;
	uint32_t pad;
	uint64_t seq;
};

struct nand_counters {
	uint64_t programs;
	uint64_t program_failures;
	uint64_t reads;
	uint64_t erases;
};

struct nand {
	struct nand_geometry g;
	uint32_t total_pages;
	uint8_t *data;			/* total_pages * page_size */
	uint8_t *state;			/* enum nand_page_state per page */
	struct nand_oob *oob;		/* per page */
	uint32_t *write_ptr;		/* per block: next page index to program */
	uint32_t *valid;		/* per block: number of VALID pages */
	uint32_t *erase_count;		/* per block */
	struct nand_counters c;
	uint64_t fail_program_at;	/* fault injection: fail when programs+1 == this */
};

int nand_init(struct nand *n, const struct nand_geometry *g);
void nand_free(struct nand *n);

static inline uint32_t nand_block_of(const struct nand *n, uint32_t ppn)
{
	return ppn / n->g.pages_per_block;
}

static inline uint32_t nand_first_page(const struct nand *n, uint32_t block)
{
	return block * n->g.pages_per_block;
}

int nand_program(struct nand *n, uint32_t ppn, const void *data, const struct nand_oob *oob);
int nand_read(struct nand *n, uint32_t ppn, void *data, struct nand_oob *oob);
int nand_invalidate(struct nand *n, uint32_t ppn);
int nand_erase(struct nand *n, uint32_t block);

/* Make the program operation that is `k` operations from now (1 = next) fail. */
void nand_inject_program_failure(struct nand *n, uint64_t k);

const char *nand_strerror(int rc);

#endif
