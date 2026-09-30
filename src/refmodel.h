/* SPDX-License-Identifier: MIT */
/*
 * Reference model for differential testing: a flat array of logical pages
 * with no mapping, no GC and no NAND rules. It implements the documented
 * host semantics directly (write stores, discard unmaps, reads of unmapped
 * pages return zeros), so any divergence from the FTL is an FTL bug.
 */
#ifndef REFMODEL_H
#define REFMODEL_H

#include <stdbool.h>
#include <stdint.h>

struct refmodel {
	uint32_t pages;
	uint32_t page_size;
	uint8_t *data;
	bool *mapped;
};

int ref_init(struct refmodel *r, uint32_t pages, uint32_t page_size);
void ref_free(struct refmodel *r);
/* Same return conventions as ftl_write/ftl_read/ftl_discard. */
int ref_write(struct refmodel *r, uint32_t lpn, const void *data);
int ref_read(const struct refmodel *r, uint32_t lpn, void *data);
int ref_discard(struct refmodel *r, uint32_t lpn);

#endif
