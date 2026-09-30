# Design: page-mapped FTL simulator

This is a **simulator**. The NAND is an in-memory array; timing, where shown, comes from
stated constants. The goal is to make address mapping, out-of-place updates and garbage
collection explicit, checkable and measurable.

## 1. NAND model (`src/nand.[ch]`)
- Geometry: `blocks × pages_per_block × page_size`. It stores **real page contents** plus a
  per-page OOB area `{lpn, seq}`, so readback correctness is verifiable and GC has a reverse
  map without extra DRAM.
- Page states: `ERASED`, `VALID`, `INVALID`. Rules enforced in `nand_program`,
  `nand_erase`, `nand_read` and `nand_invalidate`; a violation returns an error and changes
  nothing:
  - program only an `ERASED` page, and only at the block's write pointer (in-order
    programming within a block, as on real NAND);
  - erase whole blocks only;
  - **refuse to erase a block that still holds `VALID` pages**, which catches premature erase
    by the FTL;
  - reading an `ERASED` or `INVALID` page fails, so following a stale mapping is caught
    immediately.
- Physical NAND only knows erased vs programmed. VALID/INVALID is really FTL metadata. It
  lives in the NAND model so these rules can be enforced in one place.
- Fault injection: `nand_inject_program_failure(k)` makes the k-th program from now fail. The
  failed page is consumed (it becomes INVALID and the write pointer advances), as a failed
  program would leave it on real NAND.

Simplifications relative to physical NAND:
- no bit errors or ECC, no read/program disturb, no retention loss;
- no bad blocks or bad-block management (a program failure consumes one page, not the block);
- no multi-plane or multi-die parallelism, no page-pair (MLC/TLC) programming constraints,
  no partial-page programming;
- erase endurance is not enforced (erase counts are only reported).

## 2. FTL (`src/ftl.[ch]`)

### Address space and semantics
- Logical capacity `L = (blocks − spare_blocks) × P` pages (P = pages per block). Spare is
  configured explicitly in blocks. Over-provisioning is reported as `spare / (blocks − spare)`.
- `ftl_write(lpn)`: a full-page, out-of-place write.
  **Order: allocate → program new copy → switch L2P → invalidate old copy.** If the program
  fails, or allocation or GC fails, the mapping is unchanged and the previous data stays
  readable (`t_failed_overwrite_keeps_old_data`, and the fault-injection differential tests).
  The allocation happens *before* reading `l2p[lpn]`, because GC during allocation may
  relocate that very LPN.
- `ftl_read(lpn)`: returns the last successfully written data. A never-written or discarded
  LPN returns **zeros with status `FTL_UNMAPPED`** (deterministic read-zero after TRIM).
- `ftl_discard(lpn)`: unmaps and invalidates the page. It is idempotent, and discarding a
  never-written LPN is a no-op.
- Errors are explicit:
  - `FTL_ERANGE` (lpn ≥ L)
  - `FTL_EINVAL` (NULL buffer, bad config)
  - `FTL_EPROGRAM` (NAND program failed; old data intact)
  - `FTL_ENOSPC` / `FTL_EGC_STUCK` (no reclaimable space; only possible with injected faults
    or an unsafe configuration)
  - `FTL_EINTERNAL` (NAND rejected an FTL operation; never observed)

### Blocks and write frontiers
- Block states: `FREE`, `HOST_OPEN`, `GC_OPEN`, `SEALED`.
- Host writes append to the host frontier; GC relocations append to a **separate GC
  frontier**. Relocated data has survived at least one GC cycle, so it tends to be colder;
  keeping it apart from fresh host writes avoids re-mixing hot and cold data in one block.
- `--single-frontier` puts relocations into the host frontier instead. That is the
  single-write-stream assumption of the analytic WA models, and it is used only for the model
  comparison.
- Free blocks are reused in FIFO order (a ring), which spreads erases somewhat without
  explicit wear levelling.

### Victim selection (`select_victim`)
- **Eligible:** state `SEALED` and at least one invalid page (`valid < P`). Open frontiers are
  never victims. A fully valid block would cost P copies and free nothing, so it is excluded
  for both policies.
- **Greedy:** the fewest valid pages; ties go to the oldest seal order.
- **FIFO:** the oldest seal order among eligible blocks.
- Both are O(blocks) scans per victim, which is fine at 1024 blocks. A real FTL would keep
  valid-count buckets.

### GC capacity, reserve and forward progress
Definitions:
- `F` = free blocks
- `g` = erased pages left in the GC frontier (0 in single-frontier mode)
- **GC capacity** `C = F·P + g`

**Invariant (fault-free): `C ≥ P − 1`.** An eligible victim has at most `P − 1` valid pages,
so GC can always relocate one whole victim.

- **Host rule:** the host may open a new block only if the invariant still holds afterwards:
  `(F − 1)·P + g ≥ P − 1` (`host_may_open`).
- **When GC runs:** it is foreground only. It runs when the host frontier is full and the host
  may not open a block. It reclaims victims until the host may.
- **GC step:** relocate each valid page (program copy → remap → invalidate), then erase the
  victim, which now has 0 valid pages (enforced by NAND).

**Each step makes progress.** A step with a victim holding `v ≤ P − 1` valid pages consumes
`v` pages of capacity and frees `P`, so `C` rises by `P − v ≥ 1`. While GC is running, the
host may not open, so `C < 2P − 1`; GC therefore needs fewer than `2P` steps. The code
enforces a hard bound of `2P + 2` steps and returns `FTL_EGC_STUCK` if it is ever exceeded.

**An eligible victim always exists when GC runs, given `spare ≥ 3` blocks.**
1. Valid pages `V ≤ L = (B − S)·P`.
2. When GC runs, the host frontier is full, and either `F = 0, g ≤ P − 1` or `F = 1, g < P − 1`.
   So erased pages `E ≤ P + g`.
3. Invalid pages `I = BP − V − E ≥ (S − 1)·P − g`.
4. The only invalid pages that cannot be reclaimed sit in the open GC frontier, which holds at
   most `P − g` of them.
5. `I > P − g` whenever `S > 2`, so some SEALED block holds an invalid page.

Hence `FTL_MIN_SPARE_BLOCKS = 3`.
- With `unsafe_allow_low_spare`, a spare of 0–2 is allowed for testing. There the FTL
  terminates with a defined error (`t_low_spare_fails_defined`: spare 0/1 refused about 98%
  of writes once full), and data stays intact.
- **Why capacity is counted in pages, not blocks.** The first version stopped GC only when
  `F ≥ 2` whole free blocks. Pages left in the GC frontier did not count, so after reclaiming
  a nearly empty victim, greedy often had to take a second, nearly full victim just to produce
  a whole free block. Measuring and fixing that is described in INTERVIEW_GUIDE.md.

**Faults are outside the argument.** A failed program consumes a page that the argument does
not account for, so `C` can drop below `P − 1`. The FTL then returns `ENOSPC`/`EGC_STUCK` and
never loses data. It can stay in that state until discards free space: 8 of 280 fault-injected
differential runs ended that way. See LIMITATIONS.md.

### Invariants checked by `ftl_check()`
Tests call it after every operation:
1. Every mapped LPN points to an in-range `VALID` page whose OOB LPN equals it: no mapping to
   erased or invalid pages.
2. Every `VALID` page is the target of its OOB LPN's mapping: no orphaned or duplicate live
   copies.
3. `#mapped LPNs == #VALID pages == ftl.mapped`.
4. Per-block valid counts match the page states.
5. Pages below each write pointer are programmed, and pages at or above it are erased.
6. `FREE` blocks are fully erased, `SEALED` blocks are full, open blocks match the frontier
   pointers (at most one of each), and the free queue holds exactly the `FREE` blocks.
7. The GC capacity invariant (fault-free runs).

## 3. Metrics
- **WA** = NAND page programs (host + GC relocation) / host page writes, as deltas over the
  measurement window only. Failed programs are not counted as programs.
- GC pages moved, GC episodes (one per GC invocation) and victims/erases; per-block erase
  counts (min/max/mean/stddev/16-bin histogram, lifetime).
- **Mapping memory**, as a controller would need it:
  - 4-byte L2P entry per logical page
  - 1 valid bit per physical page
  - 25 bytes per block
  - The OOB reverse map lives in NAND, so it is not DRAM.
  - The simulator's own allocations are larger (page data, one byte of state per page); they
    are not reported as FTL overhead.
- **Simulator time:** wall clock of the measurement phase on the host CPU, reported separately
  and labelled as not SSD performance.
- **Simulated device time:** `programs × 600 µs + GC reads × 50 µs + erases × 3 ms`, single die,
  no parallelism. It is labelled simulated, is not validated against any device, and serves
  only as an alternative view of the same counts.

## 4. Workloads and measurement windows (`src/workload.c`, `src/ftlsim.c`)
- xoshiro256** seeded through splitmix64, with unbiased bounded draws (Lemire).
- The op stream depends only on the workload parameters and seed, never on FTL state, so both
  policies see identical traces. `trace_hash` (FNV-1a of every generated LPN) is compared in
  `summarize.py`.
- Workloads:
  - `seq`: cyclic sequential
  - `uniform`: uniform random overwrite
  - `hotcold`: 80% of writes to 20% of the LPNs, with the hot region at the low addresses
- Phases:
  1. **Precondition:** one sequential fill of all L pages.
  2. **Warm-up:** 4×L workload writes, unmeasured.
  3. **Measurement:** 8×L writes.
- WA is also recorded every 0.25×L writes. A configuration is labelled *steady* if the two
  halves of its measurement windows differ by less than 2% for every seed, and *transient*
  otherwise.
- Separate 40×L runs check that no drift appears later.
- After every run, all L pages are read back and compared with the last version written.

## 5. Reference model and testing
- `src/refmodel.c` is a flat array with a mapped flag, and it implements the documented
  semantics directly. `tests/test_diff.c` runs seeded op sequences against both:
  - 7 geometries (including P = 1 and 2, and minimum spare) × 2 policies × 2 frontier modes ×
    40 seeds × 3000 ops, with about 3% out-of-range addresses mixed in;
  - a second pass that injects program failures, where a refused write must leave the FTL
    agreeing with a reference that never saw it.
- `tests/mutation_check.py` injects 10 FTL/NAND bugs and requires each one to fail a test.
  One mutant (greedy choosing the *most* valid pages) initially survived, because the tests
  checked correctness but not policy; `t_victim_selection` was added for it.
