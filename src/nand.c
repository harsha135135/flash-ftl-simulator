// SPDX-License-Identifier: MIT
#include "nand.h"

#include <stdlib.h>
#include <string.h>

#include "simalloc.h"

int nand_init(struct nand *n, const struct nand_geometry *g)
{
	uint64_t pages, bytes;

	memset(n, 0, sizeof(*n));
	if (!g || g->page_size == 0 || g->pages_per_block == 0 || g->blocks == 0)
		return NAND_EINVAL;
	pages = (uint64_t)g->pages_per_block * g->blocks;
	bytes = pages * g->page_size;
	if (pages > UINT32_MAX - 1 || bytes > ((uint64_t)1 << 40))
		return NAND_EINVAL;

	n->g = *g;
	n->total_pages = (uint32_t)pages;
	n->data = sim_calloc((size_t)bytes, 1);
	n->state = sim_calloc(pages, sizeof(*n->state));
	n->oob = sim_calloc(pages, sizeof(*n->oob));
	n->write_ptr = sim_calloc(g->blocks, sizeof(*n->write_ptr));
	n->valid = sim_calloc(g->blocks, sizeof(*n->valid));
	n->erase_count = sim_calloc(g->blocks, sizeof(*n->erase_count));
	if (!n->data || !n->state || !n->oob || !n->write_ptr || !n->valid ||
	    !n->erase_count) {
		nand_free(n);
		return NAND_ENOMEM;
	}
	/* Factory state: every page erased (all-ones). */
	memset(n->data, 0xff, (size_t)bytes);
	return NAND_OK;
}

void nand_free(struct nand *n)
{
	free(n->data);
	free(n->state);
	free(n->oob);
	free(n->write_ptr);
	free(n->valid);
	free(n->erase_count);
	memset(n, 0, sizeof(*n));
}

void nand_inject_program_failure(struct nand *n, uint64_t k)
{
	n->fail_program_at = k ? n->c.programs + n->c.program_failures + k : 0;
}

int nand_program(struct nand *n, uint32_t ppn, const void *data, const struct nand_oob *oob)
{
	uint32_t blk, idx;

	if (ppn >= n->total_pages || !data || !oob)
		return NAND_ERANGE;
	if (n->state[ppn] != PAGE_ERASED)
		return NAND_ENOT_ERASED;
	blk = nand_block_of(n, ppn);
	idx = ppn - nand_first_page(n, blk);
	if (idx != n->write_ptr[blk])
		return NAND_EORDER;

	if (n->fail_program_at &&
	    n->c.programs + n->c.program_failures + 1 == n->fail_program_at) {
		/*
		 * A failed program still consumes the page: it is no longer
		 * erased, holds no valid data, and the write pointer moves on.
		 */
		n->fail_program_at = 0;
		n->state[ppn] = PAGE_INVALID;
		n->write_ptr[blk]++;
		n->c.program_failures++;
		return NAND_EPROGRAM_FAIL;
	}

	memcpy(n->data + (size_t)ppn * n->g.page_size, data, n->g.page_size);
	n->oob[ppn] = *oob;
	n->state[ppn] = PAGE_VALID;
	n->write_ptr[blk]++;
	n->valid[blk]++;
	n->c.programs++;
	return NAND_OK;
}

int nand_read(struct nand *n, uint32_t ppn, void *data, struct nand_oob *oob)
{
	if (ppn >= n->total_pages)
		return NAND_ERANGE;
	if (n->state[ppn] == PAGE_ERASED)
		return NAND_EERASED;
	if (n->state[ppn] == PAGE_INVALID)
		return NAND_ESTALE;
	if (data)
		memcpy(data, n->data + (size_t)ppn * n->g.page_size, n->g.page_size);
	if (oob)
		*oob = n->oob[ppn];
	n->c.reads++;
	return NAND_OK;
}

int nand_invalidate(struct nand *n, uint32_t ppn)
{
	if (ppn >= n->total_pages)
		return NAND_ERANGE;
	if (n->state[ppn] != PAGE_VALID)
		return NAND_ESTATE;
	n->state[ppn] = PAGE_INVALID;
	n->valid[nand_block_of(n, ppn)]--;
	return NAND_OK;
}

int nand_erase(struct nand *n, uint32_t block)
{
	uint32_t first;

	if (block >= n->g.blocks)
		return NAND_ERANGE;
	if (n->valid[block] != 0)
		return NAND_EHAS_VALID;
	first = nand_first_page(n, block);
	memset(n->state + first, PAGE_ERASED, n->g.pages_per_block);
	memset(n->data + (size_t)first * n->g.page_size, 0xff,
	       (size_t)n->g.pages_per_block * n->g.page_size);
	memset(n->oob + first, 0, (size_t)n->g.pages_per_block * sizeof(*n->oob));
	n->write_ptr[block] = 0;
	n->erase_count[block]++;
	n->c.erases++;
	return NAND_OK;
}

const char *nand_strerror(int rc)
{
	switch (rc) {
	case NAND_OK: return "ok";
	case NAND_ERANGE: return "address out of range";
	case NAND_ENOT_ERASED: return "program of non-erased page";
	case NAND_EORDER: return "program out of order within block";
	case NAND_EHAS_VALID: return "erase of block with valid pages";
	case NAND_EERASED: return "read of erased page";
	case NAND_ESTALE: return "read of invalid page";
	case NAND_ESTATE: return "invalidate of non-valid page";
	case NAND_EPROGRAM_FAIL: return "program failure";
	case NAND_EINVAL: return "invalid argument";
	case NAND_ENOMEM: return "out of memory";
	default: return "unknown";
	}
}
