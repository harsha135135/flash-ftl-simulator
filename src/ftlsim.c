// SPDX-License-Identifier: MIT
/*
 * ftlsim - workload runner for the FTL simulator. Prints one JSON document.
 *
 * Phases (all sizes are multiples of the logical capacity L):
 *   precondition  one sequential pass writing every logical page (unmeasured)
 *   warm-up       --warmup-x * L workload writes (unmeasured)
 *   measurement   --measure-x * L workload writes; all headline numbers are
 *                 deltas over exactly this phase
 * WA is also recorded for every --window-x * L writes across warm-up and
 * measurement, so convergence (or its absence) is visible in the output.
 *
 * At the end every logical page is read back and compared with the last
 * version written, so every experiment is also a data-integrity check.
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ftl.h"
#include "workload.h"

/* Simulated device timing: order-of-magnitude TLC figures, single die, no
 * parallelism, no controller overhead. Used only for the labelled
 * "simulated_device" section; never mixed with simulator wall time. */
#define T_READ_US	50.0
#define T_PROG_US	600.0
#define T_ERASE_US	3000.0

struct opts {
	struct ftl_config cfg;
	enum wl_kind wl;
	uint64_t seed;
	double hot_frac, hot_weight;
	double warmup_x, measure_x, window_x;
	int precondition;
};

struct window {
	uint64_t end_writes;
	int measured;
	double wa;
	uint64_t moved, erases, gc_invocations;
};

static void usage(void)
{
	fprintf(stderr,
"usage: ftlsim [options]\n"
"  --blocks N (1024)  --ppb N (64)  --page-size N (4096)  --spare-blocks N (72)\n"
"  --policy greedy|fifo (greedy)  --single-frontier\n"
"  --workload seq|uniform|hotcold (uniform)  --hot-frac F (0.2)  --hot-weight F (0.8)\n"
"  --seed N (1)  --warmup-x F (2)  --measure-x F (8)  --window-x F (0.25)\n"
"  --no-precondition\n");
	exit(2);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned long ul(const char *s)
{
	char *e;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &e, 0);
	if (errno || *e)
		usage();
	return v;
}

static double dbl(const char *s)
{
	char *e;
	double v = strtod(s, &e);

	if (*e || v < 0)
		usage();
	return v;
}

static void parse(struct opts *o, int argc, char **argv)
{
	static const struct option lo[] = {
		{ "blocks", 1, 0, 'B' }, { "ppb", 1, 0, 'P' }, { "page-size", 1, 0, 'Z' },
		{ "spare-blocks", 1, 0, 'S' }, { "policy", 1, 0, 'p' },
		{ "single-frontier", 0, 0, '1' }, { "workload", 1, 0, 'w' },
		{ "hot-frac", 1, 0, 'f' }, { "hot-weight", 1, 0, 'h' }, { "seed", 1, 0, 's' },
		{ "warmup-x", 1, 0, 'u' }, { "measure-x", 1, 0, 'm' }, { "window-x", 1, 0, 'W' },
		{ "no-precondition", 0, 0, 'n' }, { 0, 0, 0, 0 },
	};
	int c;

	memset(o, 0, sizeof(*o));
	o->cfg.geo = (struct nand_geometry){ .page_size = 4096, .pages_per_block = 64,
					     .blocks = 1024 };
	o->cfg.spare_blocks = 72;
	o->cfg.policy = FTL_GC_GREEDY;
	o->wl = WL_UNIFORM;
	o->seed = 1;
	o->hot_frac = 0.2;
	o->hot_weight = 0.8;
	o->warmup_x = 2;
	o->measure_x = 8;
	o->window_x = 0.25;
	o->precondition = 1;
	while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (c) {
		case 'B': o->cfg.geo.blocks = (uint32_t)ul(optarg); break;
		case 'P': o->cfg.geo.pages_per_block = (uint32_t)ul(optarg); break;
		case 'Z': o->cfg.geo.page_size = (uint32_t)ul(optarg); break;
		case 'S': o->cfg.spare_blocks = (uint32_t)ul(optarg); break;
		case 'p':
			if (!strcmp(optarg, "greedy")) o->cfg.policy = FTL_GC_GREEDY;
			else if (!strcmp(optarg, "fifo")) o->cfg.policy = FTL_GC_FIFO;
			else usage();
			break;
		case '1': o->cfg.single_frontier = true; break;
		case 'w': if (wl_parse(optarg, &o->wl)) usage(); break;
		case 'f': o->hot_frac = dbl(optarg); break;
		case 'h': o->hot_weight = dbl(optarg); break;
		case 's': o->seed = ul(optarg); break;
		case 'u': o->warmup_x = dbl(optarg); break;
		case 'm': o->measure_x = dbl(optarg); break;
		case 'W': o->window_x = dbl(optarg); break;
		case 'n': o->precondition = 0; break;
		default: usage();
		}
	}
	if (optind != argc || o->window_x <= 0 || o->measure_x <= 0)
		usage();
}

static void die_rc(const char *what, int rc)
{
	fprintf(stderr, "ftlsim: %s: %s (%d)\n", what, ftl_strerror(rc), rc);
	exit(1);
}

static uint64_t programs(const struct ftl *f)
{
	return f->s.nand_programs_host + f->s.nand_programs_gc;
}

int main(int argc, char **argv)
{
	struct opts o;
	struct ftl f;
	struct workload wl;
	struct ftl_mem mem;
	struct ftl_stats m0, m1;
	struct window *win = NULL;
	uint32_t *version, lpn, L, B;
	uint8_t *buf;
	uint64_t warm, meas, total, i, next_win, win_len, nwin = 0, win_cap;
	uint64_t w_host = 0, w_prog = 0, w_moved = 0, w_erase = 0, w_gc = 0;
	uint64_t mismatches = 0, emin = UINT64_MAX, emax = 0, hist[16] = { 0 };
	double t0 = 0, t1 = 0, esum = 0, esq = 0, emean, estd, busy_s;
	int rc;

	parse(&o, argc, argv);
	rc = ftl_create(&f, &o.cfg);
	if (rc)
		die_rc("ftl_create", rc);
	L = f.logical_pages;
	B = o.cfg.geo.blocks;
	if (wl_init(&wl, o.wl, L, o.seed, o.hot_frac, o.hot_weight))
		usage();
	version = calloc(L, sizeof(*version));
	buf = malloc(o.cfg.geo.page_size);
	warm = (uint64_t)llround(o.warmup_x * L);
	meas = (uint64_t)llround(o.measure_x * L);
	win_len = (uint64_t)llround(o.window_x * L);
	if (win_len == 0)
		win_len = 1;
	total = warm + meas;
	win_cap = total / win_len + 2;
	win = calloc(win_cap, sizeof(*win));
	if (!version || !buf || !win)
		die_rc("alloc", FTL_ENOMEM);

	if (o.precondition) {
		for (lpn = 0; lpn < L; lpn++) {
			page_fill(buf, o.cfg.geo.page_size, lpn, ++version[lpn]);
			rc = ftl_write(&f, lpn, buf);
			if (rc)
				die_rc("precondition write", rc);
		}
	}

	next_win = win_len;
	m0 = f.s;	/* re-taken at the warm-up/measurement boundary below */
	t0 = now_s();
	for (i = 0; i < total; i++) {
		if (i == warm) {
			m0 = f.s;
			t0 = now_s();
		}
		lpn = wl_next(&wl);
		page_fill(buf, o.cfg.geo.page_size, lpn, ++version[lpn]);
		rc = ftl_write(&f, lpn, buf);
		if (rc)
			die_rc("workload write", rc);
		if (i + 1 == next_win || i + 1 == total) {
			struct window *w = &win[nwin++];

			w->end_writes = i + 1;
			w->measured = i + 1 > warm;
			w->wa = (double)(programs(&f) - w_prog) / (double)(f.s.host_writes - w_host);
			w->moved = f.s.gc_pages_moved - w_moved;
			w->erases = f.s.erases - w_erase;
			w->gc_invocations = f.s.gc_invocations - w_gc;
			w_host = f.s.host_writes;
			w_prog = programs(&f);
			w_moved = f.s.gc_pages_moved;
			w_erase = f.s.erases;
			w_gc = f.s.gc_invocations;
			next_win += win_len;
		}
	}
	t1 = now_s();
	m1 = f.s;

	/* Integrity: every logical page must hold its latest version. */
	for (lpn = 0; lpn < L; lpn++) {
		rc = ftl_read(&f, lpn, buf);
		if (version[lpn] == 0 ? rc != FTL_UNMAPPED
				      : rc != FTL_OK ||
					page_check(buf, o.cfg.geo.page_size, lpn, version[lpn]))
			mismatches++;
	}
	{
		char msg[256];

		if (ftl_check(&f, msg, sizeof(msg))) {
			fprintf(stderr, "ftlsim: invariant violated: %s\n", msg);
			mismatches++;
		}
	}

	for (i = 0; i < B; i++) {
		uint64_t e = f.nand.erase_count[i];

		emin = e < emin ? e : emin;
		emax = e > emax ? e : emax;
		esum += (double)e;
		esq += (double)e * (double)e;
	}
	emean = esum / B;
	estd = sqrt(esq / B - emean * emean > 0 ? esq / B - emean * emean : 0);
	for (i = 0; i < B; i++) {
		/* 16 equal-width bins over [emin, emax]. */
		uint64_t e = f.nand.erase_count[i];
		uint64_t bin = emax > emin ? (e - emin) * 16 / (emax - emin + 1) : 0;

		hist[bin]++;
	}
	ftl_mem_model(&f, &mem);

	{
		uint64_t host = m1.host_writes - m0.host_writes;
		uint64_t prog = (m1.nand_programs_host + m1.nand_programs_gc) -
				(m0.nand_programs_host + m0.nand_programs_gc);
		uint64_t moved = m1.gc_pages_moved - m0.gc_pages_moved;
		uint64_t gcinv = m1.gc_invocations - m0.gc_invocations;
		uint64_t erases = m1.erases - m0.erases;
		uint64_t gcreads = m1.nand_reads_gc - m0.nand_reads_gc;

		busy_s = ((double)prog * T_PROG_US + (double)gcreads * T_READ_US +
			  (double)erases * T_ERASE_US) / 1e6;
		printf("{\n");
		printf("  \"config\": {\"blocks\": %u, \"pages_per_block\": %u, \"page_size\": %u, "
		       "\"spare_blocks\": %u, \"policy\": \"%s\", \"single_frontier\": %s, "
		       "\"workload\": \"%s\", \"hot_frac\": %.3f, \"hot_weight\": %.3f, "
		       "\"seed\": %llu, \"precondition\": %s, \"warmup_x\": %.3f, "
		       "\"measure_x\": %.3f, \"window_x\": %.3f},\n",
		       B, f.ppb, o.cfg.geo.page_size, o.cfg.spare_blocks,
		       ftl_policy_name(o.cfg.policy), o.cfg.single_frontier ? "true" : "false",
		       wl_name(o.wl), o.hot_frac, o.hot_weight, (unsigned long long)o.seed,
		       o.precondition ? "true" : "false", o.warmup_x, o.measure_x, o.window_x);
		printf("  \"capacity\": {\"physical_pages\": %u, \"logical_pages\": %u, "
		       "\"spare_pct_of_physical\": %.3f, \"op_pct_of_logical\": %.3f},\n",
		       f.nand.total_pages, L, 100.0 * o.cfg.spare_blocks / B,
		       100.0 * o.cfg.spare_blocks / (B - o.cfg.spare_blocks));
		printf("  \"trace_hash\": \"%016llx\",\n", (unsigned long long)wl.hash);
		printf("  \"measure\": {\"host_writes\": %llu, \"nand_programs\": %llu, "
		       "\"write_amplification\": %.5f, \"gc_pages_moved\": %llu, "
		       "\"gc_invocations\": %llu, \"gc_per_1k_host_writes\": %.4f, "
		       "\"erases\": %llu, \"gc_max_steps_lifetime\": %llu},\n",
		       (unsigned long long)host, (unsigned long long)prog,
		       host ? (double)prog / (double)host : 0.0, (unsigned long long)moved,
		       (unsigned long long)gcinv, host ? 1000.0 * (double)gcinv / (double)host : 0.0,
		       (unsigned long long)erases, (unsigned long long)m1.gc_max_steps);
		printf("  \"erase_count\": {\"scope\": \"lifetime, all phases\", \"min\": %llu, "
		       "\"max\": %llu, \"mean\": %.3f, \"stddev\": %.3f, \"max_over_mean\": %.4f, "
		       "\"hist16\": [",
		       (unsigned long long)emin, (unsigned long long)emax, emean, estd,
		       emean > 0 ? (double)emax / emean : 0.0);
		for (i = 0; i < 16; i++)
			printf("%llu%s", (unsigned long long)hist[i], i < 15 ? ", " : "");
		printf("]},\n");
		printf("  \"mapping_memory\": {\"l2p_bytes\": %llu, \"valid_bitmap_bytes\": %llu, "
		       "\"block_meta_bytes\": %llu, \"total_bytes\": %llu, "
		       "\"pct_of_logical_capacity\": %.5f},\n",
		       (unsigned long long)mem.l2p_bytes, (unsigned long long)mem.valid_bitmap_bytes,
		       (unsigned long long)mem.block_meta_bytes, (unsigned long long)mem.total_bytes,
		       100.0 * (double)mem.total_bytes / ((double)L * o.cfg.geo.page_size));
		printf("  \"simulator\": {\"measure_wall_s\": %.4f, \"host_writes_per_s\": %.0f, "
		       "\"note\": \"simulator execution speed on the host CPU; not SSD performance\"},\n",
		       t1 - t0, t1 > t0 ? (double)host / (t1 - t0) : 0.0);
		printf("  \"simulated_device\": {\"assumptions\": \"single die, no parallelism, "
		       "tR=%.0fus tPROG=%.0fus tBERS=%.0fus\", \"busy_s\": %.3f, "
		       "\"mean_us_per_host_write\": %.1f},\n",
		       T_READ_US, T_PROG_US, T_ERASE_US, busy_s,
		       host ? busy_s * 1e6 / (double)host : 0.0);
		printf("  \"windows\": [");
		for (i = 0; i < nwin; i++)
			printf("%s\n    {\"end_writes\": %llu, \"measured\": %s, \"wa\": %.5f, "
			       "\"moved\": %llu, \"erases\": %llu, \"gc_invocations\": %llu}",
			       i ? "," : "", (unsigned long long)win[i].end_writes,
			       win[i].measured ? "true" : "false", win[i].wa,
			       (unsigned long long)win[i].moved, (unsigned long long)win[i].erases,
			       (unsigned long long)win[i].gc_invocations);
		printf("\n  ],\n");
		printf("  \"verify\": {\"pages_checked\": %u, \"mismatches\": %llu, \"ok\": %s}\n}\n",
		       L, (unsigned long long)mismatches, mismatches ? "false" : "true");
	}
	free(win);
	free(buf);
	free(version);
	ftl_destroy(&f);
	return mismatches ? 1 : 0;
}
