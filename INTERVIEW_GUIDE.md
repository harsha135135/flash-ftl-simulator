# Interview guide: FTL simulator

## Walkthrough (follow a host write)
1. `ftl_write` (`src/ftl.c`) validates the input and calls `host_alloc_page`.
2. `host_alloc_page` returns the next page of the host frontier. If the frontier is full, it
   seals it (`seal`) and calls `gc_until_host_can_open`, then opens a new block
   (`open_frontier`).
3. `gc_until_host_can_open` loops `gc_step` while `host_may_open` is false:
   - `gc_step`: `select_victim` (greedy/FIFO), then `relocate` each valid page, then
     `nand_erase`, then `free_push`.
   - `relocate`: read the page with its OOB (`nand_read`), check that the OOB LPN maps back to
     the page, then `gc_alloc_page` → `nand_program` (retry on an injected failure) → update
     `l2p` → `nand_invalidate` the old copy.
4. Back in `ftl_write`: `nand_program` the new data with OOB `{lpn, ++write_seq}`. Only after
   it succeeds does the code read the old mapping, point `l2p[lpn]` at the new page and
   invalidate the old page.
5. `ftl_check` verifies every structural invariant; the tests call it after each operation.

Everything in `nand.c` refuses illegal operations (`NAND_ENOT_ERASED`, `NAND_EORDER`,
`NAND_EHAS_VALID`, `NAND_ESTALE`), so an FTL bug surfaces as an error, not as silent
corruption.

## Likely questions

**1. Why can't flash be updated in place, and what does the FTL do instead?**
A page can only be programmed when erased, and erase works on whole blocks (64 pages here).
Updating in place would mean erasing and rewriting a whole block. `ftl_write` instead writes
to the next free page, remaps `l2p[lpn]` and marks the old page INVALID. `nand_program`
enforces the erased-only and in-order rules.

**2. What exactly is write amplification here?**
NAND page programs (host + GC relocations) divided by host page writes, both as deltas over
the measurement phase only (`ftlsim.c`: `m0`/`m1` snapshots). Measured values:
- sequential: exactly 1.000 (whole blocks become invalid in order, so nothing is moved);
- uniform, 7.6% OP: 6.72 (greedy) and 7.43 (FIFO);
- uniform, 28% OP: 2.42 and 2.50.

**3. Greedy vs FIFO?**
- Greedy takes the sealed block with the fewest valid pages, which minimises copies per
  reclaim: lower WA in every random workload (6.72 vs 7.43 uniform, 6.79 vs 7.68 hot/cold at
  7.6% OP).
- FIFO takes the oldest block, which gives nearly perfect wear spread (erase max/mean about
  1.00–1.02) but copies more.
- Greedy under hot/cold concentrates erases: max/mean 1.47 at 7.6% OP and 1.83 at 28%.
  That is the classic argument for adding wear levelling or cost-benefit selection.

**4. How do you guarantee GC terminates and never deadlocks?**
- Invariant `C = F·P + g ≥ P − 1` (free blocks × P + GC-frontier pages). An eligible victim
  has at most P − 1 valid pages, so one reclaim always fits.
- The host may open a block only if the invariant still holds afterwards (`host_may_open`).
- Each GC step raises C by P − v ≥ 1, and while GC runs C < 2P − 1, so it needs fewer than
  2P steps.
- An eligible victim exists whenever spare ≥ 3 blocks (pigeonhole on invalid pages; see
  DESIGN.md). The loop is also bounded in code (`limit = 2P + 2` → `FTL_EGC_STUCK`), so a
  flawed argument or injected faults yield an error, not a hang.
- `gc_max_steps` is asserted ≤ 2P + 1 in `soak()`.

**5. Why must a victim have at least one invalid page, and why never an open block?**
- A fully valid victim costs P copies and frees nothing, so capacity does not grow and the
  progress argument fails.
- An open frontier is still being written. Reclaiming it would mean relocating pages into
  itself or racing its write pointer.
- A mutant that let open blocks be victims was caught by `ftl_check` ("frontier bookkeeping
  mismatch").

**6. What happens if power were lost mid-relocation?**
It is not implemented (LIMITATIONS.md), but the ordering is designed for it. The new copy is
programmed with OOB `{lpn, seq}` before the old copy is invalidated, so at any instant at
least one complete copy exists. A scan rebuild would take the highest `seq` per LPN.
Discards are not logged, so a discarded page could reappear after such a rebuild. I would
need a persisted TRIM log to fix that.

**7. What if a program fails during a host overwrite?**
`nand_program` fails before `l2p` is touched, so `ftl_write` returns `FTL_EPROGRAM` and the
old version is still mapped and readable (`t_failed_overwrite_keeps_old_data`). A mutant that
remapped on failure was caught. During GC, `relocate` retries on the next page; the victim is
not erased until it has zero valid pages.

**8. How did you test correctness beyond hand-written cases?**
Differential testing (`tests/test_diff.c`):
- the same seeded sequence of write/read/discard (including out-of-range LPNs) runs against
  the FTL and `refmodel.c`, a flat array;
- every read's status and bytes must match, and `ftl_check()` runs after every op;
- 7 geometries (including 1 and 2 pages per block, and minimum spare) × 2 policies × 2
  frontier modes × 40 seeds, 4.2 M ops in total;
- a fault-injection pass where refused writes must leave the FTL agreeing with a reference
  that never saw them.

**9. How do you know the tests can actually fail?**
`tests/mutation_check.py` injects 10 realistic bugs, among them:
- invalidating the old copy before programming the new one;
- forgetting to remap on relocation;
- discard without invalidate;
- no GC reserve;
- NAND allowing out-of-order programming.

All 10 are detected. The first run caught only 8/10. One mutant only failed to compile and
was fixed. The other, greedy choosing the *most* valid pages, survived, because the tests
checked correctness and never checked which block a policy picks. I added
`t_victim_selection`: a constructed state where greedy must reclaim the empty block 5 and
FIFO the oldest block 0.

**10. Why separate host and GC write frontiers?**
Relocated pages have survived at least one GC cycle and are statistically colder. Mixing them
with fresh host writes makes blocks whose pages die at very different times. The analytic
model assumes one stream, so `--single-frontier` exists to compare against it. Under uniform
traffic, separation has almost no effect (FIFO 7.43 dual vs 7.38 single); there is nothing
hot or cold to separate.

**11. How does your measured WA compare with theory?**
- The FIFO/uniform model solves u = e^(−α(1−u)), WA = 1/(1−u). Against it, measured
  single-frontier FIFO was systematically high: +0.4% at 28% OP up to +2.3% at 4% OP.
- Hypothesis: the always-free reserve block holds no data and sits outside the write cycle,
  so the effective α is (B−1)/(B−S), not B/(B−S).
- With that correction, all four spare levels match within ±0.1%.
- Greedy is below the FIFO curve, as expected for finite blocks.

**12. How much DRAM does the mapping need?**
A 4-byte PPN per 4 KiB logical page (243,712 B for 238 MiB logical), plus 1 valid bit per
physical page (8 KiB) and 25 B per block: 0.11% of capacity. The ~1 GB per 1 TB rule of thumb
for page-mapped FTLs follows from the 4 B / 4 KiB ratio. The reverse map is not DRAM: it
lives in the NAND OOB and is read during relocation.

**13. How did you decide the runs were long enough?**
- Precondition with a full sequential fill, warm up with 4× logical capacity, measure over 8×.
- WA is recorded per 0.25× window. A configuration counts as steady only if the two halves of
  the measurement phase differ by less than 2% for every seed; all 12 configurations passed.
- Separate 40× runs show WA flat from the 2× window onward (uniform greedy: 6.74 at 2×,
  6.73 at 40×).

**14. Why is "GC episodes per 1k writes" 15.6 in every configuration?**
GC is foreground, and in steady state it runs exactly once each time the host needs a new
block: 1000/64 = 15.6. The informative frequency metric is blocks *erased* per host write,
which rises with WA. The summary reports that instead.

**15. What's the simulator throughput, and is it SSD speed?**
- It is not SSD speed. It is how fast this C program runs on an M4 Pro: about 3.15 M host
  writes/s for sequential, 0.53–0.57 M for uniform at 7.6% OP (dominated by generating and
  copying 4 KiB pages).
- Simulated device time uses assumed NAND timings and is labelled simulated.

## Real problems encountered

### 1. GC over-collection from block-granular reserve accounting
- **Symptom:** in `t_data_preserved_through_relocation`, greedy relocated 4,984 pages in
  5,000 writes while FIFO relocated 63. Data was intact, but greedy was doing far more work.
- **Reproduction:** a 30-line harness printed each GC step's free count, victim and valid
  count. Every episode reclaimed *two* victims:
  - a hot block with 1 valid page, whose relocation needed a fresh GC block, so the free-block
    count stayed at 1;
  - then a cold block with 7 valid pages, just to reach 2 whole free blocks.
  The 7 erased pages left in the GC frontier were invisible to the stop condition
  (`free_count < 2`).
- **Fix:** account for the reserve in pages (`host_may_open`: `(F−1)·P + g ≥ P − 1`) and
  update the invariant in `ftl_check`. Relocations in the same trace dropped from 384 to 256
  per 400 writes. The progress argument was rewritten in terms of C = F·P + g.
- **Reproduce:** `build/<os>/gc_trace 0 1` (old rule, kept as `reserve_whole_blocks` for
  comparison) vs `gc_trace 0 0`. The repository was initialised after this fix, so the old
  rule survives as an option, not in git history.
- **Honest scope:** at the 1024-block experiment geometry, the two rules give WA within about
  0.1% (RESULTS.md, "GC reserve rule"). The fix matters on small, minimal-spare devices,
  where a single extra victim per episode is a large fraction of the work.
- **Tradeoff it exposed:** the tighter reserve leaves no slack for program failures. The
  fault-injection differential test then produced `ENOSPC`, and 8/280 fault runs end in a
  persistent refused state. That is documented, not hidden.

### 2. Measurement start overwritten when warm-up is zero
- The 40× long runs use `--warmup-x 0`. A leftover line (`if (warm == 0) t0 = t1`) would have
  reported 0 s simulator time for them.
- gcc's `-Wmaybe-uninitialized` flagged the related snapshot variables during the Linux
  ASan build, and `-Werror` stopped the build. The fix was to snapshot `m0`/`t0` before the
  loop. It was caught before any results were produced with it.

### 3. A test that could not fail the way it claimed
- `t_data_preserved_through_relocation` originally rewrote LPNs 0–7, which all live in block 0.
  Hot and cold data never shared a block, so greedy never moved cold data. The test's own
  `gc_pages_moved > 0` assertion caught this.
- It now rewrites one LPN per block (`(i % 9) * 8`), so every cold block receives
  invalidations.
