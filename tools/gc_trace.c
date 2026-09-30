// SPDX-License-Identifier: MIT
/*
 * gc_trace - reproduce the GC over-collection investigation (INTERVIEW_GUIDE).
 * 12 blocks x 8 pages, spare 3; fill all 72 logical pages, then rewrite one
 * LPN per block ((i % 9) * 8) 400 times. Prints per-block valid counts after
 * each of the first GC episodes and the totals.
 * Usage: gc_trace [0=greedy|1=fifo] [1=block-granular reserve]
 */
#include <stdio.h>
#include <stdlib.h>

#include "ftl.h"

int main(int argc, char **argv)
{
	struct ftl f;
	struct ftl_config c = { 0 };
	uint8_t page[16] = { 0 };
	uint64_t victims = 0, moved = 0;
	uint32_t lpn, k;
	int i;

	c.geo.page_size = 16;
	c.geo.pages_per_block = 8;
	c.geo.blocks = 12;
	c.spare_blocks = 3;
	c.policy = argc > 1 && atoi(argv[1]) ? FTL_GC_FIFO : FTL_GC_GREEDY;
	c.reserve_whole_blocks = argc > 2 && atoi(argv[2]);
	if (ftl_create(&f, &c))
		return 1;
	for (lpn = 0; lpn < f.logical_pages; lpn++)
		ftl_write(&f, lpn, page);
	for (i = 0; i < 400; i++) {
		if (ftl_write(&f, (uint32_t)(i % 9) * 8, page))
			return 1;
		if (f.s.gc_victims != victims) {
			if (i < 60) {
				printf("write %3d: +%llu victims, +%llu moved | valid/state:", i,
				       (unsigned long long)(f.s.gc_victims - victims),
				       (unsigned long long)(f.s.gc_pages_moved - moved));
				for (k = 0; k < c.geo.blocks; k++)
					printf(" %u%c", f.nand.valid[k], "FHGS"[f.blk_state[k]]);
				printf("\n");
			}
			victims = f.s.gc_victims;
			moved = f.s.gc_pages_moved;
		}
	}
	printf("%s, %s reserve: %llu pages moved, %llu victims over 400 writes "
	       "(F=free H=host-open G=gc-open S=sealed)\n", ftl_policy_name(c.policy),
	       c.reserve_whole_blocks ? "block-granular" : "page-granular",
	       (unsigned long long)moved, (unsigned long long)victims);
	ftl_destroy(&f);
	return 0;
}
