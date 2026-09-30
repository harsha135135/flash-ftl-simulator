# Résumé bullets

- Built a page-mapped SSD flash translation layer **simulator** in C: a NAND model enforcing
  erase-before-program and in-order page programming, and greedy/FIFO garbage collection with
  a bounded GC loop and a documented forward-progress argument. Writes use program-then-remap
  ordering, so failed writes keep the old data. Validated with
  differential testing against a reference model (4.2M operations, invariants checked after
  every operation), NAND fault injection, ASan/UBSan, and a mutation check (10/10 injected
  bugs caught).
- Measured write amplification on identical seeded traces for a simulated 256 MiB device.
  Greedy GC lowered WA 9.6–11.6% vs FIFO at 7.6% over-provisioning (6.72 vs 7.43 uniform)
  but raised max/mean block erases to 1.8× under hot/cold traffic. Matched an analytic FIFO
  model within 0.1% after identifying and correcting for a reserve-block effect.

Every number is from RESULTS.md (revision `66f539e`, raw data in `results/raw/`).
