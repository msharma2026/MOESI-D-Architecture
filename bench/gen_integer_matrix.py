#!/usr/bin/env python3
"""Deterministic synthetic integer sparse matrix in Matrix Market coordinate form,
for spmv_bench (the checked-in fixture is a 4x4 correctness case, not a workload).

    python3 gen_integer_matrix.py ROWS NNZ SEED > synth.mtx

Values are small positive integers so the per-row oracle stays exact; the
pattern is uniform random with no duplicate coordinates. Record ROWS/NNZ/SEED
and the file's SHA-256 (tools/run_matrix.py --input hashes it) with any result.
"""
import random
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    rows, nnz, seed = (int(a) for a in sys.argv[1:])
    if rows < 1 or nnz < 1 or nnz > rows * rows:
        sys.exit("need 1 <= NNZ <= ROWS*ROWS")
    rng = random.Random(seed)
    seen = set()
    while len(seen) < nnz:
        seen.add((rng.randrange(rows), rng.randrange(rows)))
    out = sys.stdout
    out.write("%%MatrixMarket matrix coordinate integer general\n")
    out.write(f"% synthetic: rows={rows} nnz={nnz} seed={seed} (bench/gen_integer_matrix.py)\n")
    out.write(f"{rows} {rows} {nnz}\n")
    for r, c in sorted(seen):
        out.write(f"{r + 1} {c + 1} {rng.randint(1, 9)}\n")


if __name__ == "__main__":
    main()
