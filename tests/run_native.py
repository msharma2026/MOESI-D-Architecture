#!/usr/bin/env python3
"""Host-only oracle/datapath smoke tests. Does NOT execute gem5 or delegated ISA."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which("clang") or shutil.which("gcc")
CXX = shutil.which("clang++") or shutil.which("g++")
if not CC or not CXX:
    raise SystemExit("C and C++ compilers required")

with tempfile.TemporaryDirectory(prefix="moesi-d-native-") as scratch:
    scratch = Path(scratch)
    def run(argv):
        subprocess.run([str(x) for x in argv], check=True, timeout=60, cwd=ROOT)
    run([CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Iprotocol",
         "tests/dstate_unit.cc", "-o", scratch / "datapath"])
    run([scratch / "datapath"])
    cases = [("coherence_regression", None, [])]
    cases += [("litmus_bench", mode, ["4", "1001", "128", "17"])
              for mode in ("DSTATE", "NAIVE", "PRIVATIZE", "PRIVSTREAM", "FLATCOMBINE")]
    cases += [("cms_bench", mode, ["4", "1001", "17"])
              for mode in ("DSTATE", "NAIVE", "PRIVATIZE", "PRIVSTREAM")]
    for source, mode, args in cases:
        binary = scratch / (source + (mode or ""))
        cmd = [CC, "-std=c11", "-O2", "-pthread", "-DDSTATE_NATIVE"]
        if mode: cmd += ["-DMODE_" + mode]
        run([*cmd, "bench/" + source + ".c", "-o", binary])
        run([binary, *args])
    # These serial-only checks exercise loaders/oracles, NOT OpenMP concurrency.
    for source, args in [
        ("scatter_bench", ["1", "101", "7"]),
        ("scatter_nomemset", ["1", "101", "7"]),
        ("scatter_multi", ["1", "101", "33"]),
        ("scatter_rw", ["1", "101", "7", "3"]),
        ("spmv_bench", ["1", "7", "bench/integer_fixture.mtx"]),
    ]:
        binary = scratch / source
        run([CC, "-std=c11", "-O2", "-DDSTATE_NATIVE", "-Itests/serial_omp",
             "-Wno-unknown-pragmas", "bench/" + source + ".c", "-o", binary])
        run([binary, *args])
        bad = subprocess.run([str(binary)], cwd=ROOT, capture_output=True)
        if bad.returncode == 0:
            raise SystemExit(f"{source} silently accepted missing arguments")
        if source == "spmv_bench":
            for fixture in ("fractional", "truncated", "real"):
                bad = subprocess.run([str(binary), "1", "1", f"tests/fixtures/{fixture}.mtx"], cwd=ROOT, capture_output=True)
                if bad.returncode != 1:
                    raise SystemExit(f"SpMV failed to reject {fixture} input correctly")
    print("PASS: datapath, 10 pthread workloads, 5 serial-only loader/oracle checks; no gem5/OpenMP validation claim")
