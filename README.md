# SSD flash translation layer simulator

A page-mapped FTL in C over a **simulated** NAND array. It demonstrates:
- logical-to-physical mapping and out-of-place updates;
- garbage collection with two interchangeable victim policies (greedy, FIFO);
- how policy, spare capacity and workload affect write amplification and erase distribution.

Page contents are stored for real, so every experiment also verifies that data survived GC.
This is not a model of any specific SSD, and its timing numbers are either simulator CPU speed
or explicitly *simulated* device time.

- **NAND model** (`src/nand.c`): erase-before-program, in-order programming within a block,
  whole-block erase, refusal to erase blocks holding valid data, OOB `{lpn, seq}`, and
  program-failure injection.
- **FTL** (`src/ftl.c`):
  - L2P table, separate host and GC write frontiers, greedy/FIFO GC;
  - a page-granular GC reserve with a bounded, provably progressing GC loop;
  - defined errors for invalid input and exhausted space;
  - `ftl_check()` for all structural invariants.
- **Runner** (`src/ftlsim.c`): seeded workloads (sequential, uniform, hot/cold) and
  precondition, warm-up and measurement phases. It records WA per window, erase distribution,
  mapping memory and simulator time, does a full readback check, and prints JSON.
- **Tests**:
  - NAND rules, FTL edge cases on tiny geometries, and victim selection;
  - fault injection, and differential testing against a reference model (4.2 M compared
    operations);
  - ASan/UBSan/LSan (Linux), and a mutation check (10/10 injected bugs detected).

See [DESIGN.md](DESIGN.md) (mapping, GC, invariants, progress argument),
[RESULTS.md](RESULTS.md), [LIMITATIONS.md](LIMITATIONS.md) and
[INTERVIEW_GUIDE.md](INTERVIEW_GUIDE.md).

## Layout
```
src/nand.[ch]      NAND array model          src/ftl.[ch]        FTL + GC + ftl_check
src/refmodel.[ch]  reference model           src/workload.[ch]   PRNG, workloads, page patterns
src/ftlsim.c       CLI runner (JSON)         src/simalloc.[ch]   allocation-failure injection
tests/             test_nand.c test_ftl.c test_diff.c mutation_check.py
bench/             run_experiments.py (raw JSON)  summarize.py (tables + CSV)
results/           raw/{main,model,long}/*.json  summary.md  summary_runs.csv  windows.csv
```

## Build and test (macOS or Linux; C11, make, Python 3 stdlib only)
```sh
make all                      # build/<os>/ftlsim and test binaries
make check                    # test_nand, test_ftl, test_diff (full differential matrix)
make check-asan               # same under ASan+UBSan(+LSan); run on Linux (see LIMITATIONS)
python3 tests/mutation_check.py
```
Warnings are errors (`-Wall -Wextra -Wconversion -Werror`).

## Run
```sh
build/$(uname -s | tr A-Z a-z)/ftlsim --blocks 1024 --ppb 64 --page-size 4096 \
    --spare-blocks 72 --policy greedy --workload hotcold --seed 1 \
    --warmup-x 4 --measure-x 8 --window-x 0.25 > run.json
```
Other options: `--policy fifo`, `--workload seq|uniform|hotcold`, `--hot-frac`, `--hot-weight`,
`--single-frontier`, `--no-precondition`.

## Reproduce the experiments
```sh
make all
python3 bench/run_experiments.py      # 112 runs, ~2 min on an M4 Pro; results/raw/
python3 bench/summarize.py            # results/summary.md, summary_runs.csv, windows.csv
```

## Demo
```sh
B=build/$(uname -s | tr A-Z a-z)
for p in greedy fifo; do
  $B/ftlsim --workload hotcold --policy $p --spare-blocks 72 --measure-x 4 |
    python3 -c "import json,sys; d=json.load(sys.stdin); m=d['measure']; e=d['erase_count'];
print('$p', 'WA', round(m['write_amplification'],3), 'erase max/mean', round(e['max_over_mean'],2),
      'verify', d['verify']['ok'], 'trace', d['trace_hash'])"
done
```
