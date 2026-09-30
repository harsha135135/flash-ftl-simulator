# Results

All numbers come from the **simulator**. WA, pages moved and erase counts are exact counts of
simulated NAND operations. "Simulator speed" is how fast this program runs on the host CPU.
"Simulated device time" multiplies counts by assumed NAND timings. Neither is SSD
performance.

## Environment and revision
- Experiments ran on a MacBook Pro (Apple M4 Pro), macOS 26.5.2, Apple clang 17.0.0, `-O2`.
- Code revision `66f539e` (recorded in `results/raw/environment.json`); the tree was clean
  apart from the regenerated `results/`.
- Tests also ran on Ubuntu 24.04 (arm64 VM), gcc 13.3.0.

## Correctness checks run
| Check | Where | Result | Log |
|---|---|---|---|
| `make check`: NAND rules, FTL edge cases, differential matrix (4.2 M ops compared, 918k GC victims, 753k relocations) | macOS clang, Linux gcc | pass | [macOS](results/logs/check_macos.txt), [Linux](results/logs/check_linux.txt) |
| Fault-injection differential pass: 18,856 injected program failures hit across 280 runs. No data loss or invariant violation; 8 runs ended in a persistent refused-write state (see LIMITATIONS.md) | both | pass | same |
| `make check-asan`: ASan + UBSan + LeakSanitizer; unit tests, a reduced differential matrix (0.7 M ops), and a sanitized `ftlsim` run | Linux gcc | pass | [log](results/logs/check_asan_linux.txt) |
| Mutation check: 10 injected FTL/NAND bugs must each fail a test | Linux | 10/10 detected | [log](results/logs/mutation_check_linux.txt) |
| End-of-run readback of every logical page in every experiment | macOS | 112/112 runs verified | `verify` in each raw JSON |

The macOS ASan runtime hangs before `main()` on this OS version, even for an empty program,
so no macOS sanitizer results are claimed.

## Experiment setup
Command: `python3 bench/run_experiments.py && python3 bench/summarize.py`.

- Raw data: [results/raw/](results/raw/) (one JSON per run, including every WA window).
  Tables: [results/summary.md](results/summary.md). CSV:
  [summary_runs.csv](results/summary_runs.csv), [windows.csv](results/windows.csv).
- Geometry: 1024 blocks × 64 pages × 4 KiB (256 MiB physical). Spare: 72 blocks (OP 7.6% of
  logical) or 224 blocks (OP 28%).
- Workloads:
  - `seq`: cyclic sequential;
  - `uniform`: uniform random overwrite;
  - `hotcold`: 80% of writes to 20% of the logical pages.
- Phases: precondition with one sequential fill, warm up with 4× logical capacity of workload
  writes (unmeasured), then measure over the next 8× logical capacity. All metrics except
  lifetime erase counts are deltas over the measurement phase.
- Greedy and FIFO received **identical traces**: the same generator and seed, verified by an
  identical `trace_hash` for every (workload, spare, seed). They also had identical geometry,
  spare, preconditioning and windows. 5 seeds per configuration; cells are median [min–max].
- **Steady state:** for every configuration and seed, mean WA over the two halves of the
  measurement phase differed by less than 2%, so every row below is labelled *steady*.
  Separate 40× runs show WA flat from the first 2× window onward (table at the end).

## Greedy vs FIFO (dual write frontier)
| workload | spare (OP) | policy | WA | GC pages moved / host write | blocks erased / 1k writes | erase max/mean | erase stddev |
|---|---|---|---|---|---|---|---|
| seq | 72 (7.6%) | greedy / fifo | 1.000 / 1.000 | 0 / 0 | 15.6 / 15.6 | 1.08 / 1.08 | 0.3 / 0.3 |
| uniform | 72 (7.6%) | greedy | **6.716** [6.710–6.718] | 5.72 | 104.9 | 1.10 | 2.5 |
| uniform | 72 (7.6%) | fifo | **7.433** [7.420–7.442] | 6.43 | 116.2 | 1.01 | 0.5 |
| uniform | 224 (28%) | greedy | **2.415** [2.414–2.416] | 1.42 | 37.7 | 1.13 | 0.9 |
| uniform | 224 (28%) | fifo | **2.495** [2.493–2.498] | 1.50 | 39.0 | 1.00 | 0.2 |
| hotcold | 72 (7.6%) | greedy | **6.793** [6.787–6.797] | 5.79 | 106.1 | 1.47 [1.46–1.67] | 11.4 |
| hotcold | 72 (7.6%) | fifo | **7.680** [7.672–7.681] | 6.68 | 120.0 | 1.02 | 1.1 |
| hotcold | 224 (28%) | greedy | **2.608** [2.605–2.620] | 1.61 | 40.8 | 1.83 [1.75–1.88] | 5.8 |
| hotcold | 224 (28%) | fifo | **2.835** [2.833–2.837] | 1.84 | 44.3 | 1.00 [1.00–1.04] | 0.1 |

Observations:
- **Sequential** writes invalidate whole blocks in order, so neither policy moves any data
  (WA exactly 1).
- **Greedy has lower WA** in every random workload: 9.6% lower than FIFO for uniform at 7.6%
  OP (6.72 vs 7.43), and 11.6% lower for hot/cold (6.79 vs 7.68).
- **Spare capacity dominates policy.** Going from 7.6% to 28% OP cuts WA by about 2.8×
  (6.72 → 2.42 greedy, uniform).
- **Wear:** FIFO erases nearly uniformly (max/mean 1.00–1.02). Greedy under hot/cold
  concentrates erases (max/mean up to 1.83), because cold blocks full of valid data are
  rarely chosen. No wear levelling is implemented.
- **Hot/cold does not help either policy here.** Both see slightly higher WA than under
  uniform traffic, because hot and cold host writes share one host frontier. Separating GC
  writes from host writes does not separate hot from cold host writes.

## Sanity check: analytic FIFO model
The model assumes uniform random overwrites, one write stream and FIFO cleaning. It is
`u = exp(−α(1−u))`, `WA = 1/(1−u)`, with α = physical/logical pages. It was compared against
runs with `--single-frontier` (3 seeds each), the configuration that matches those
assumptions.

| spare | OP | α | model WA | model, 1 reserve block excluded | measured FIFO (single frontier) | vs model | vs corrected |
|---|---|---|---|---|---|---|---|
| 40 | 4.1% | 1.0407 | 12.971 | 13.286 | 13.270 | +2.3% | −0.1% |
| 72 | 7.6% | 1.0756 | 7.286 | 7.379 | 7.378 | +1.3% | −0.0% |
| 128 | 14.3% | 1.1429 | 4.182 | 4.209 | 4.215 | +0.8% | +0.1% |
| 224 | 28.0% | 1.2800 | 2.481 | 2.489 | 2.490 | +0.4% | +0.0% |

- **Unexpected result:** the measured WA was systematically above the model, and the gap grew
  as spare shrank.
- **Explanation:** in steady state this FTL always holds one erased block in reserve. That
  block holds no data and is outside the write cycle, so the effective α is (B−1)/(B−S).
  With that single correction, all four points agree within ±0.1%.
- Greedy (single frontier) is below the FIFO curve at every OP (11.14 vs 13.27 at 4.1%).
  With the dual frontier, uniform-traffic WA is within 1% of the single-frontier value for
  both policies.

## GC reserve rule
The original rule (the host opens a block only if two whole blocks are free) vs the
page-granular reserve (`host_may_open`); 3 seeds, spare 72:

| workload | policy | WA page-granular | WA whole-block | difference |
|---|---|---|---|---|
| uniform | greedy | 6.7164 | 6.7207 | +0.06% |
| uniform | fifo | 7.4233 | 7.4253 | +0.03% |
| hotcold | greedy | 6.7952 | 6.8002 | +0.07% |
| hotcold | fifo | 7.6807 | 7.6811 | +0.01% |

- At this geometry the rule barely matters.
- On a 12-block, minimum-spare device it matters a lot. `tools/gc_trace.c` shows greedy
  relocating 384 pages per 400 writes under the old rule vs 256 under the new one (FIFO: 63
  under both). See INTERVIEW_GUIDE.md.

## Long runs (40× logical capacity, spare 72, seed 1, no separate warm-up)
| workload | policy | WA window at 2× | 4× | 8× | 12× | 20× | 30× | 40× |
|---|---|---|---|---|---|---|---|---|
| uniform | greedy | 6.737 | 6.731 | 6.687 | 6.677 | 6.733 | 6.670 | 6.729 |
| uniform | fifo | 7.441 | 7.412 | 7.425 | 7.376 | 7.445 | 7.399 | 7.431 |
| hotcold | greedy | 6.800 | 6.769 | 6.817 | 6.838 | 6.821 | 6.781 | 6.697 |
| hotcold | fifo | 7.698 | 7.628 | 7.680 | 7.685 | 7.645 | 7.691 | 7.635 |

Windows are 0.5× logical capacity each. No trend beyond window-to-window noise (±1%), so
the 4× warm-up used in the main runs is sufficient for these workloads.

## Mapping-table memory (modelled controller DRAM)
| spare | L2P | valid bitmap | per-block metadata | total | % of logical capacity |
|---|---|---|---|---|---|
| 72 | 243,712 B | 8,192 B | 25,600 B | 277,504 B | 0.111% |
| 224 | 204,800 B | 8,192 B | 25,600 B | 238,592 B | 0.114% |

## Simulator speed and simulated device time
- **Simulator speed (host CPU):** about 3.15 M host writes/s for sequential; 0.53 M (FIFO)
  to 0.57 M (greedy) for uniform at 7.6% OP. The cost is dominated by 4 KiB page copies. Faster
  greedy runs mean fewer relocations, not a faster algorithm.
- **Simulated device busy time** (tPROG 600 µs, tR 50 µs, tBERS 3 ms, one die, no
  parallelism, *assumed*): 647 µs per host write for sequential; 4,630 µs (greedy) vs
  5,130 µs (FIFO) for uniform at 7.6% OP. This is a restatement of WA in time units, not a
  performance prediction.
