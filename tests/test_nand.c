// SPDX-License-Identifier: MIT
/* NAND model rules: program/erase/read constraints and fault injection. */
#include <stdint.h>
#include <stdlib.h>

#include "nand.h"
#include "simalloc.h"
#include "testutil.h"

#define PS 32

static const struct nand_geometry G = { .page_size = PS, .pages_per_block = 4, .blocks = 3 };

static void fill(uint8_t *b, uint8_t v)
{
	memset(b, v, PS);
}

static void t_bad_geometry(void)
{
	struct nand n;
	struct nand_geometry g = G;

	g.page_size = 0;
	EXPECT(nand_init(&n, &g) == NAND_EINVAL);
	g = G;
	g.pages_per_block = 0;
	EXPECT(nand_init(&n, &g) == NAND_EINVAL);
	g = G;
	g.blocks = 0;
	EXPECT(nand_init(&n, &g) == NAND_EINVAL);
	EXPECT(nand_init(&n, NULL) == NAND_EINVAL);
}

static void t_fresh_device_is_erased(void)
{
	struct nand n;
	uint8_t buf[PS];
	uint32_t p;

	EXPECT(nand_init(&n, &G) == NAND_OK);
	for (p = 0; p < n.total_pages; p++)
		EXPECT(n.state[p] == PAGE_ERASED && nand_read(&n, p, buf, NULL) == NAND_EERASED);
	EXPECT(n.data[0] == 0xff && n.data[(size_t)n.total_pages * PS - 1] == 0xff);
	nand_free(&n);
}

static void t_program_read_roundtrip_and_rules(void)
{
	struct nand n;
	struct nand_oob oob = { .lpn = 7, .seq = 99 }, got;
	uint8_t in[PS], out[PS];

	nand_init(&n, &G);
	fill(in, 0xa5);
	EXPECT(nand_program(&n, 0, in, &oob) == NAND_OK);
	EXPECT(nand_read(&n, 0, out, &got) == NAND_OK && !memcmp(in, out, PS));
	EXPECT(got.lpn == 7 && got.seq == 99);
	EXPECT(n.valid[0] == 1 && n.write_ptr[0] == 1);

	/* Reprogramming a programmed page is forbidden (no in-place update). */
	EXPECT(nand_program(&n, 0, in, &oob) == NAND_ENOT_ERASED);
	/* Pages within a block must be programmed in order. */
	EXPECT(nand_program(&n, 2, in, &oob) == NAND_EORDER);
	EXPECT(n.state[2] == PAGE_ERASED && n.write_ptr[0] == 1);
	/* Other blocks have independent write pointers. */
	EXPECT(nand_program(&n, 4, in, &oob) == NAND_OK);
	EXPECT(nand_program(&n, n.total_pages, in, &oob) == NAND_ERANGE);
	EXPECT(n.c.programs == 2);
	nand_free(&n);
}

static void t_invalidate_and_stale_read(void)
{
	struct nand n;
	struct nand_oob oob = { 0 };
	uint8_t b[PS];

	nand_init(&n, &G);
	fill(b, 1);
	EXPECT(nand_invalidate(&n, 0) == NAND_ESTATE);	/* erased page */
	nand_program(&n, 0, b, &oob);
	EXPECT(nand_invalidate(&n, 0) == NAND_OK && n.valid[0] == 0);
	EXPECT(nand_invalidate(&n, 0) == NAND_ESTATE);	/* already invalid */
	EXPECT(nand_read(&n, 0, b, NULL) == NAND_ESTALE);
	EXPECT(nand_invalidate(&n, n.total_pages) == NAND_ERANGE);
	nand_free(&n);
}

static void t_erase_rules(void)
{
	struct nand n;
	struct nand_oob oob = { 0 };
	uint8_t b[PS], out[PS];
	uint32_t i;

	nand_init(&n, &G);
	fill(b, 0x3c);
	for (i = 0; i < 4; i++)
		EXPECT(nand_program(&n, i, b, &oob) == NAND_OK);
	/* Live data blocks the erase, and nothing changes. */
	nand_invalidate(&n, 0);
	nand_invalidate(&n, 1);
	EXPECT(nand_erase(&n, 0) == NAND_EHAS_VALID);
	EXPECT(nand_read(&n, 2, out, NULL) == NAND_OK && !memcmp(b, out, PS));
	EXPECT(n.erase_count[0] == 0);
	nand_invalidate(&n, 2);
	nand_invalidate(&n, 3);
	EXPECT(nand_erase(&n, 0) == NAND_OK);
	EXPECT(n.erase_count[0] == 1 && n.write_ptr[0] == 0 && n.valid[0] == 0);
	for (i = 0; i < 4; i++)
		EXPECT(n.state[i] == PAGE_ERASED && n.data[(size_t)i * PS] == 0xff);
	/* Erased block is programmable again from page 0. */
	EXPECT(nand_program(&n, 0, b, &oob) == NAND_OK);
	EXPECT(nand_erase(&n, G.blocks) == NAND_ERANGE);
	/* Erasing an already-empty block is allowed and counted. */
	EXPECT(nand_erase(&n, 2) == NAND_OK && n.erase_count[2] == 1);
	nand_free(&n);
}

static void t_program_failure_consumes_page(void)
{
	struct nand n;
	struct nand_oob oob = { 0 };
	uint8_t b[PS];

	nand_init(&n, &G);
	fill(b, 9);
	nand_inject_program_failure(&n, 2);
	EXPECT(nand_program(&n, 0, b, &oob) == NAND_OK);
	EXPECT(nand_program(&n, 1, b, &oob) == NAND_EPROGRAM_FAIL);
	EXPECT(n.state[1] == PAGE_INVALID && n.write_ptr[0] == 2 && n.valid[0] == 1);
	EXPECT(nand_read(&n, 1, b, NULL) == NAND_ESTALE);
	EXPECT(nand_program(&n, 2, b, &oob) == NAND_OK);	/* one-shot */
	EXPECT(n.c.program_failures == 1 && n.c.programs == 2);
	nand_free(&n);
}

static void t_init_alloc_failure_cleans_up(void)
{
	struct nand n;
	unsigned long k;
	int rc;

	/* Fail each allocation in turn; LeakSanitizer reports any leak. */
	for (k = 1;; k++) {
		sim_alloc_fail_after(k);
		rc = nand_init(&n, &G);
		sim_alloc_fail_after(0);
		if (rc == NAND_OK)
			break;
		EXPECT(rc == NAND_ENOMEM, "k=%lu rc=%d", k, rc);
		EXPECT(n.data == NULL && n.state == NULL);
	}
	EXPECT(k > 1);
	nand_free(&n);
}

int main(void)
{
	RUN(t_bad_geometry);
	RUN(t_fresh_device_is_erased);
	RUN(t_program_read_roundtrip_and_rules);
	RUN(t_invalidate_and_stale_read);
	RUN(t_erase_rules);
	RUN(t_program_failure_consumes_page);
	RUN(t_init_alloc_failure_cleans_up);
	DONE();
}
