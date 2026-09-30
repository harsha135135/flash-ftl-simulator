# Limitations

## It is a simulator
- The NAND is an in-memory array. There are no bit errors, ECC, read/program disturb,
  retention effects, bad blocks, multi-plane/multi-die parallelism, MLC/TLC paired-page
  constraints, partial-page programs, or endurance limits (erase counts are only reported).
- **Timing.** "Simulated device time" multiplies operation counts by assumed constants
  (tR 50 µs, tPROG 600 µs, tBERS 3 ms, one die, no overlap). It has not been validated
  against any device and is not SSD performance. "Simulator speed" is how fast this program
  runs on the host CPU.
- Single-threaded; there is no host command queue, and no concurrent host I/O during GC.

## FTL features not implemented
- **No wear levelling.** Greedy GC under hot/cold traffic leaves cold blocks rarely erased
  (measured max/mean erase count up to about 1.8; RESULTS.md). Static wear levelling (swapping
  cold data into worn blocks) is the natural next step.
- **Foreground GC only.** There is no background/idle GC and no GC watermark tuning; GC
  happens inside the host write that needs space.
- **No bad-block management.** A program failure consumes one page and is reported. Real
  FTLs retire the block and remap it from a bad-block reserve.
- **No power-loss recovery.** The OOB area records `{lpn, write sequence}` per page, so a
  scan-based rebuild of the L2P table is possible (the newest sequence wins), but it is not
  implemented or tested. Discards are not persisted, so after such a rebuild a discarded page
  could reappear. This optional extension was not attempted.
- Page-level mapping only: no hybrid/block mapping, no DFTL-style cached mapping table, no
  compression or deduplication.
- Full-page writes only. There is no sub-page read-modify-write.

## Known behaviour under faults
- The forward-progress argument (DESIGN.md) holds for fault-free operation. Injected program
  failures consume pages outside that argument, so the FTL can hit `ENOSPC`/`EGC_STUCK`.
- In the differential fault runs:
  - 280 runs hit at least one fault; 8 ended with at least 100 consecutive refused writes;
  - in every case, all data verified against the reference and all invariants held.
- The persistent-refusal state resembles an SSD going read-only after exhausting its spare
  capacity. A dedicated fault/bad-block reserve would reduce it.

## Test and measurement scope
- Sanitizers: ASan/UBSan/LeakSanitizer ran on Linux (gcc 13, in the VM). On this macOS
  version the ASan runtime hangs before `main()` even for an empty program (both Apple clang
  and Homebrew LLVM), so no macOS sanitizer results are claimed. The normal test suite passes
  on both macOS (clang) and Linux (gcc).
- The differential tests use small geometries so that `ftl_check()` can run after every
  operation. The 1024-block experiment geometry is covered by the end-of-run full readback
  and a final `ftl_check()`, not per-operation checks.
- The workloads are synthetic: sequential, uniform and one hot/cold split (80/20). No real
  block traces were replayed.
- The mutation check covers 10 injected bugs. That shows the tests can detect those bug
  classes, not that the suite is complete.

## Realistic next steps
1. Implement power-loss recovery from the OOB `{lpn, seq}` records, plus a persisted discard
   log, with an injected-interruption test harness.
2. Add static wear levelling and measure its WA cost against the erase-spread benefit.
3. Add a bad-block reserve so program failures retire blocks instead of reducing GC capacity.
4. Add cost-benefit victim selection (age × invalid / valid) as a third policy, plus a
   hot/cold-aware write frontier.
5. Replay public block I/O traces.
