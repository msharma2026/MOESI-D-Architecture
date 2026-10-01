#!/usr/bin/env python3
"""Record a SAME-BINARY ablation with complete per-run provenance.

Does not build gem5, download workloads, claim correctness from checksums alone,
or publish results. The output directory must not already exist.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
MODES = {
    "local": {"DSTATE_ENABLED": "0", "DSTATE_PERSISTENCE": "0", "DSTATE_FORCE_NACK": "0"},
    "remote": {"DSTATE_ENABLED": "1", "DSTATE_PERSISTENCE": "0", "DSTATE_FORCE_NACK": "0"},
    "persistent": {"DSTATE_ENABLED": "1", "DSTATE_PERSISTENCE": "1", "DSTATE_FORCE_NACK": "0"},
    "forced-nack": {"DSTATE_ENABLED": "1", "DSTATE_PERSISTENCE": "1", "DSTATE_FORCE_NACK": "1"},
}


# Events each mode must exercise (aggregate controller counters in stats.txt). A correct
# checksum on the conventional path proves nothing about the delegated machinery -- a build
# that compiled the Sequencer atomic branch out passed every oracle while every one of these
# stayed at zero. Requiring them is the runner's coverage gate.
COVERAGE = {
    "local": ["L1Cache_Controller.LocalAtomic_CPU"],
    # An accepted operation completes as DState_Complete, or as DState_CompleteMerged
    # when other requesters were combined into it; either proves the delegated path.
    "remote": ["L1Cache_Controller.DStateReq_CPU",
               "L2Cache_Controller.DState_Complete|L2Cache_Controller.DState_CompleteMerged"],
    "persistent": ["L1Cache_Controller.DStateReq_CPU",
                   "L2Cache_Controller.DState_Complete|L2Cache_Controller.DState_CompleteMerged"],
    "forced-nack": ["L1Cache_Controller.DStateReq_CPU", "L2Cache_Controller.DState_Reject",
                    "L1Cache_Controller.DState_Retry"],
}


def event_counts(stats_path):
    """Sum each system.ruby.* counter across controller instances.

    gem5 prints per-instance event counts in a compact row: `name | v pct pct | v pct pct ...`;
    scalars are `name value`. Both are reduced to one number per name.
    """
    counts = {}
    with open(stats_path, errors="replace") as f:
        for line in f:
            if not line.startswith("system.ruby."):
                continue
            name, _, rest = line.partition(" ")
            total = 0.0
            for segment in rest.split("|")[1:] or [rest]:
                tokens = segment.split()
                if tokens:
                    try:
                        total += float(tokens[0])
                    except ValueError:
                        pass
            counts[name[len("system.ruby."):]] = total
    return counts


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5-tree", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--workload-args", default="")
    parser.add_argument("--input", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cores", type=int, default=4)
    parser.add_argument("--banks", type=int, default=4)
    parser.add_argument("--cpu", choices=["X86O3CPU", "X86MinorCPU"], default="X86O3CPU")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--require-roi", action="store_true")
    parser.add_argument("--extra", default="", help="Additional gem5 config options, saved verbatim")
    parser.add_argument("--gem5-binary", type=Path, default=None,
                        help="gem5 binary to run (default: <tree>/build/X86_MOESI_D/gem5.opt)")
    parser.add_argument("--modes", default=",".join(MODES),
                        help="Comma-separated subset of " + ",".join(MODES))
    args = parser.parse_args()
    if args.cores < 1 or args.banks < 1 or args.banks & (args.banks - 1) or args.repeats < 1 or args.timeout < 1:
        parser.error("positive cores/repeats/timeout and power-of-two banks required")
    tree, binary, out = args.gem5_tree.resolve(), args.binary.resolve(), args.out.resolve()
    simulator = args.gem5_binary or (tree / "build/X86_MOESI_D/gem5.opt")
    config = tree / "configs/deprecated/example/se.py"
    for path in [simulator, config, binary, *args.input]:
        if not path.is_file(): parser.error(f"missing file: {path}")
    out.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    env = os.environ.copy()
    env.setdefault("DSTATE_BUFFER_SIZE", "32")
    env.setdefault("DSTATE_TBES", "16")
    env.setdefault("DSTATE_EXEC_LATENCY", "42")
    env.setdefault("DSTATE_THRESHOLD", "4")
    env.setdefault("DSTATE_READ_DOWNGRADE", "3")
    failed = False
    selected = [m.strip() for m in args.modes.split(",") if m.strip()]
    unknown = sorted(set(selected) - set(MODES))
    if unknown:
        parser.error("unknown mode(s): " + ", ".join(unknown))
    for mode, settings in MODES.items():
        if mode not in selected:
            continue
        for repeat in range(args.repeats):
            dest = out / f"{mode}-{repeat}"
            dest.mkdir()
            run_env = env | settings
            cmd = [str(simulator), "--outdir=" + str(dest), str(config), "--ruby",
                   "--cpu-type=" + args.cpu, f"--num-cpus={args.cores}",
                   f"--num-l2caches={args.banks}", "--cacheline_size=128",
                   "--l1d_size=32kB", "--l1i_size=32kB", "--l2_size=256kB",
                   "--mem-size=1GB", "--network=garnet", "--topology=Crossbar",
                   "--cmd=" + str(binary), "--options=" + args.workload_args,
                   *shlex.split(args.extra)]
            manifest = {
                "artifact_commit": revision, "host": platform.platform(),
                "mode": mode, "repeat": repeat, "command": cmd,
                "environment": {k: v for k, v in run_env.items() if k.startswith("DSTATE_") or k.startswith("OMP_")},
                "hashes": {str(p): digest(p) for p in [simulator, binary, ROOT / "gem5-moesi-d.patch", *args.input]},
                "gem5_commit": subprocess.check_output(["git", "-C", str(tree), "rev-parse", "HEAD"], text=True).strip(),
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }
            path = dest / "manifest.json"
            path.write_text(json.dumps(manifest, indent=2) + "\n")
            started = time.monotonic()
            with (dest / "run.log").open("w") as log:
                proc = subprocess.Popen(cmd, cwd=tree, env=run_env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    status = proc.wait(timeout=args.timeout)
                    timed_out = False
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    status = proc.wait()
                    timed_out = True
            log_text = (dest / "run.log").read_text(errors="replace")
            ok = status == 0 and not timed_out and "CORRECT" in log_text and "WRONG" not in log_text
            ok &= (dest / "config.ini").is_file() and (dest / "stats.txt").is_file()
            if args.require_roi:
                ok &= "ROI_BEGIN:" in log_text and "ROI_END" in log_text
            counts = event_counts(dest / "stats.txt") if (dest / "stats.txt").is_file() else {}
            # "a|b" means either counter satisfies the requirement; report their sum.
            coverage = {event: sum(counts.get(name, 0.0) for name in event.split("|"))
                        for event in COVERAGE[mode]}
            missing = [event for event, n in coverage.items() if n == 0]
            ok &= not missing
            manifest.update(return_code=status, timed_out=timed_out, accepted=ok,
                            elapsed_wall_seconds=time.monotonic() - started,
                            path_coverage=coverage, coverage_missing=missing,
                            limitation="Oracle pass plus nonzero path coverage is necessary, not a coherence/liveness proof; inspect transition coverage.")
            path.write_text(json.dumps(manifest, indent=2) + "\n")
            verdict = "PASS" if ok else ("FAIL (path not exercised: %s)" % ", ".join(missing) if missing else "FAIL")
            print(dest.name, verdict, flush=True)
            failed |= not ok
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
