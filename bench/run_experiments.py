#!/usr/bin/env python3
"""Run the FTL experiments and save raw JSON (one file per run).

main     workloads {seq, uniform, hotcold 80/20} x policies {greedy, fifo} x
         spare {72, 224 of 1024 blocks} x seeds 1..5, dual write frontier.
         Greedy and FIFO receive byte-identical traces (same generator and
         seed; verified by trace_hash in summarize.py), identical geometry,
         spare, preconditioning and measurement windows.
model    uniform workload, single write frontier (the assumption of the
         analytic model), both policies, spare {40, 72, 128, 224}, seeds 1..3.
reserve  page-granular GC reserve (default) vs the original whole-block rule
         (--reserve-whole-blocks), uniform/hotcold, spare 72, seeds 1..3.
long     uniform and hotcold, spare 72, seed 1, 40x logical capacity of
         writes, to check whether the main runs reached steady state.

Geometry for all: 1024 blocks x 64 pages x 4096 B (256 MiB physical).
Runs are sequential so the simulator timing numbers are not distorted by
co-scheduled runs.
"""
import argparse
import json
import os
import platform
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "build", platform.system().lower(), "ftlsim")
RAW = os.path.join(ROOT, "results", "raw")
GEO = ["--blocks", "1024", "--ppb", "64", "--page-size", "4096"]


def plan(seeds):
    runs = []
    for wl in ("seq", "uniform", "hotcold"):
        for spare in (72, 224):
            for pol in ("greedy", "fifo"):
                for s in range(1, seeds + 1):
                    runs.append(("main", f"{wl}_s{spare}_{pol}_seed{s}",
                                 GEO + ["--spare-blocks", str(spare), "--policy", pol,
                                        "--workload", wl, "--seed", str(s),
                                        "--warmup-x", "4", "--measure-x", "8",
                                        "--window-x", "0.25"]))
    for spare in (40, 72, 128, 224):
        for pol in ("greedy", "fifo"):
            for s in range(1, 4):
                runs.append(("model", f"uniform_s{spare}_{pol}_single_seed{s}",
                             GEO + ["--spare-blocks", str(spare), "--policy", pol,
                                    "--single-frontier", "--workload", "uniform",
                                    "--seed", str(s), "--warmup-x", "4", "--measure-x", "8",
                                    "--window-x", "0.25"]))
    for wl in ("uniform", "hotcold"):
        for pol in ("greedy", "fifo"):
            for rule in ("page", "block"):
                for s in range(1, 4):
                    runs.append(("reserve", f"{wl}_s72_{pol}_{rule}_seed{s}",
                                 GEO + ["--spare-blocks", "72", "--policy", pol, "--workload", wl,
                                        "--seed", str(s), "--warmup-x", "4", "--measure-x", "8"] +
                                 (["--reserve-whole-blocks"] if rule == "block" else [])))
    for wl in ("uniform", "hotcold"):
        for pol in ("greedy", "fifo"):
            runs.append(("long", f"{wl}_s72_{pol}_seed1",
                         GEO + ["--spare-blocks", "72", "--policy", pol, "--workload", wl,
                                "--seed", "1", "--warmup-x", "0", "--measure-x", "40",
                                "--window-x", "0.5"]))
    return runs


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    runs = plan(args.seeds)
    if args.only:
        runs = [r for r in runs if r[0] in args.only]
    os.makedirs(RAW, exist_ok=True)
    env = {
        "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": platform.platform(),
        "machine": platform.machine(),
        "cpu": sh("sysctl -n machdep.cpu.brand_string 2>/dev/null || lscpu | grep 'Model name'"),
        "compiler": sh("cc --version | head -1"),
        "git_rev": sh(f"git -C {ROOT} rev-parse --short HEAD"),
        "git_dirty": bool(sh(f"git -C {ROOT} status --porcelain --untracked-files=no")),
    }
    with open(os.path.join(RAW, "environment.json"), "w") as f:
        json.dump(env, f, indent=2)
    for i, (exp, name, a) in enumerate(runs, 1):
        os.makedirs(os.path.join(RAW, exp), exist_ok=True)
        out = os.path.join(RAW, exp, name + ".json")
        r = subprocess.run([SIM] + a, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{name} failed ({r.returncode}): {r.stderr}")
        d = json.loads(r.stdout)
        d["argv"] = a
        with open(out, "w") as f:
            json.dump(d, f, indent=1)
        print(f"[{i}/{len(runs)}] {exp}/{name}: WA {d['measure']['write_amplification']:.3f} "
              f"verify={d['verify']['ok']} ({d['simulator']['measure_wall_s']:.1f}s)", flush=True)


if __name__ == "__main__":
    main()
