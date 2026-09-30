#!/usr/bin/env python3
"""Mutation check for the FTL tests.

Each mutant injects one realistic FTL bug into a temporary copy of src/,
rebuilds the test binaries against it and runs them (test_diff with a
reduced matrix). The check passes only if every mutant makes some test fail
or time out, which shows the tests can detect that class of bug.
"""
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, original snippet, mutated snippet)
MUTANTS = [
    ("host write invalidates old copy before programming new one", "src/ftl.c",
     "\toobs_placeholder", None),  # replaced below with a real two-part patch
    ("relocation forgets to update the mapping", "src/ftl.c",
     "\tf->l2p[oob.lpn] = dst;\n", ""),
    ("greedy picks the block with the MOST valid pages", "src/ftl.c",
     "if (f->nand.valid[b] < f->nand.valid[best] ||", "if (f->nand.valid[b] > f->nand.valid[best] ||"),
    ("open frontier blocks are eligible victims", "src/ftl.c",
     "if (f->blk_state[b] != BLK_SEALED || f->nand.valid[b] >= f->ppb)",
     "if (f->blk_state[b] == BLK_FREE || f->nand.valid[b] >= f->ppb)"),
    ("discard unmaps without invalidating the page", "src/ftl.c",
     "\tif (nand_invalidate(&f->nand, ppn) != NAND_OK)\n\t\treturn FTL_EINTERNAL;\n\tf->l2p[lpn] = FTL_UNMAPPED_PPN;",
     "\tf->l2p[lpn] = FTL_UNMAPPED_PPN;"),
    ("host may take the last free block (no GC reserve)", "src/ftl.c",
     "\t       (uint64_t)(f->free_count - 1) * f->ppb + gc_frontier_room(f) >= f->ppb - 1;",
     "\t       1;"),
    ("GC skips relocating the last valid page of a victim", "src/ftl.c",
     "\tfor (i = 0; i < f->ppb; i++) {\n\t\tif (f->nand.state[first + i] != PAGE_VALID)",
     "\tfor (i = 0; i + 1 < f->ppb; i++) {\n\t\tif (f->nand.state[first + i] != PAGE_VALID)"),
    ("unmapped read returns stale buffer instead of zeros", "src/ftl.c",
     "\t\tmemset(data, 0, f->cfg.geo.page_size);\n\t\tf->s.host_reads_unmapped++;",
     "\t\tf->s.host_reads_unmapped++;"),
    ("NAND allows programming out of order", "src/nand.c",
     "\tif (idx != n->write_ptr[blk])\n", "\tif (idx != n->write_ptr[blk] && 0)\n"),
    ("failed host program still remaps the LPN", "src/ftl.c",
     "\t\treturn FTL_EPROGRAM;\t/* mapping untouched: old data still readable */",
     "\t\tf->l2p[lpn] = ppn;\n\t\treturn FTL_EPROGRAM;"),
]

# Mutant 1: move the invalidation of the old copy ahead of the program.
WRITE_ORIG = """	oob.lpn = lpn;
	oob.seq = ++f->write_seq;
	rc = nand_program(&f->nand, ppn, data, &oob);"""
WRITE_MUT = """	oob.lpn = lpn;
	oob.seq = ++f->write_seq;
	if (f->l2p[lpn] != FTL_UNMAPPED_PPN) {
		nand_invalidate(&f->nand, f->l2p[lpn]);
		f->l2p[lpn] = FTL_UNMAPPED_PPN;
		f->mapped--;
	}
	rc = nand_program(&f->nand, ppn, data, &oob);"""
MUTANTS[0] = (MUTANTS[0][0], "src/ftl.c", WRITE_ORIG, WRITE_MUT)

TESTS = [["test_nand"], ["test_ftl"], ["test_diff", "6", "1500"]]


def main():
    tmp = tempfile.mkdtemp(prefix="ftlmut-")
    survived = 0
    print("TAP version 13")
    try:
        for i, (name, path, orig, mut) in enumerate(MUTANTS, 1):
            work = os.path.join(tmp, str(i))
            shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns("build", ".git", "results"))
            p = os.path.join(work, path)
            src = open(p).read()
            if src.count(orig) != 1:
                print(f"not ok {i} - {name}: snippet not found exactly once")
                survived += 1
                continue
            open(p, "w").write(src.replace(orig, mut))
            bdir = os.path.join("build", platform.system().lower())
            b = subprocess.run(["make", "-s", "-C", work] +
                               [os.path.join(bdir, t[0]) for t in TESTS],
                               capture_output=True, text=True)
            if b.returncode:
                print(f"not ok {i} - {name}: mutant does not build\n# {b.stderr[-300:]}")
                survived += 1
                continue
            detected, detail = False, ""
            for t in TESTS:
                try:
                    r = subprocess.run([os.path.join(work, bdir, t[0])] + t[1:],
                                       capture_output=True, text=True, timeout=120)
                    if r.returncode != 0:
                        detected = True
                        lines = [l for l in (r.stderr + r.stdout).splitlines() if "FAIL" in l]
                        detail = f"{t[0]}: {lines[0].strip() if lines else 'failed'}"
                        break
                except subprocess.TimeoutExpired:
                    detected, detail = True, f"{t[0]}: timed out"
                    break
            if detected:
                print(f"ok {i} - killed: {name}\n#   {detail[:200]}")
            else:
                print(f"not ok {i} - SURVIVED: {name}")
                survived += 1
    finally:
        shutil.rmtree(tmp)
    print(f"1..{len(MUTANTS)}")
    print(f"# {len(MUTANTS) - survived}/{len(MUTANTS)} mutants detected")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
