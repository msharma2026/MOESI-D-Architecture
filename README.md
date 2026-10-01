# MOESI-D: acknowledged home-cache reductions and persistent ownership

A gem5 research implementation of explicit no-return integer ADD32/ADD64
operations executed at the home L2 cache, with acknowledged completion, ordinary
MOESI fallback, optional bank-side combining, and an optional L2-local
read-retention (D) policy. It is pinned to gem5 **v25.1.0.1**
(`c8222cc67a399bfc01e8658dd14b30d5bfd634f9`). It is a simulator model, not a
hardware design.

**Documentation:** [architecture and instruction contract](docs/ARCHITECTURE.md),
[evaluation method](docs/EVALUATION.md),
[validation record](docs/VALIDATION.md),
[results](results/ABLATION_2026-09-30.md),
and [changes since v1.0.0](docs/CHANGES.md).

**Status (2026-10-01):** the branch builds and runs on gem5; directed regression
and store-ordering tests pass on O3 and Minor with the delegated path exercised
in every mode ([validation record](docs/VALIDATION.md)). Same-binary ablations
are in [`results/ABLATION_2026-09-30.md`](results/ABLATION_2026-09-30.md). The
shipped one-op-per-bank admission made delegation slower than the conventional
path. With the executor knobs below (bounded admission, pipelining,
back-pressure, combining), on 512 to 8,192 hot lines every update delegates with
about 7× fewer flits: **20–21% faster** than the matched conventional path under
TSO at 4, 8 and 16 cores, and **3.0–3.35× faster** when both sides use the relaxed
no-return ordering of Intel RAO-INT (`DSTATE_RELAXED_AMO=1`) — 2.3× at 16 cores,
where four banks become the limit. On a single hot word, requester-side combining
takes the delegated path from 37% slower to a tie at 4 cores and **1.6× faster at
16 cores** under relaxed ordering; under TSO the owner stays ahead (+13% at 8
cores). The retention policy (D) helps only on phased update/read workloads:
1–7% at 4 cores and 24–33% at 16 cores over home execution without retention.
Whole-program, single deterministic runs on a 4-bank model; not an application
speedup.

## What changed since v1.0.0

Summary; [CHANGES.md](docs/CHANGES.md) has the full list and the corrections
to the v1.0.0 paper.

- Stock x86 `LOCK ADD` is restored, including flags and operand widths.
  Benchmarks explicitly request the experimental operation; no transparent
  acceleration of existing binaries is claimed.
- The issuing atomic remains outstanding until application is acknowledged.
  Remote completion never applies the update a second time.
- A pre-acceptance NACK retains the original operation and takes GETX/local-AMO
  fallback. Accepted operations never issue NACKs. Invalidation cannot discard
  an outstanding delegated transaction.
- Potential owner-plus-sharer hazards fall back. The supported persistent
  read-sharing case drains all local readers before mutation.
- D is an L2-local ownership policy. The global directory is the upstream MOESI
  state machine, not an unconnected second D implementation.
- Delegation, persistence, and forced rejection are independently configurable.
  Buffers and executor capacity are finite; service latency and packet sizes
  are explicit modeling assumptions.
- Benchmarks have stronger oracles, nonzero failure exits, parallel private
  merges, corrected sketch hashing, and strict integer SpMV input handling.

## Evidence status

The [paper PDF](paper/MOESI-D%20Paper.pdf) and the
[v1.0.0 DOI](https://doi.org/10.5281/zenodo.22889936) describe the earlier
artifact. Their performance and correctness claims are **not evidence for this
revision**; [CHANGES.md](docs/CHANGES.md) lists the corrections. The PDF is kept
unchanged as the record that DOI points to.

Build, directed execution and ablation runs are recorded in
[VALIDATION.md](docs/VALIDATION.md). The only performance numbers asserted for
this revision are the same-binary microbenchmark ablations in `results/`;
coherence/liveness verification, server-scale experiments, application-level
speedups, energy and RTL/PPA remain open. Every run behind those tables is
listed with its knobs, statistics and simulator/guest/patch hashes in
[`results/runs_2026-09-30.tsv`](results/runs_2026-09-30.tsv).

## Build

Use a fresh checkout; do not layer this patch over the historical patch.

```sh
git clone --branch v25.1.0.1 https://github.com/gem5/gem5.git
cd gem5
git apply --check /path/to/MOESI-D-Architecture/gem5-moesi-d.patch
git apply /path/to/MOESI-D-Architecture/gem5-moesi-d.patch
scons build/X86_MOESI_D/gem5.opt -j4
make -C bench
```

Build the guest workloads with an **x86-64 Linux** toolchain. Do not execute the
default guest binaries on an ordinary host: they contain an experimental
encoding. The `DSTATE_NATIVE` backend is for host oracle tests only.
Only timing-CPU syscall-emulation experiments on normal user memory are in scope.
Full-system/device DMA use is rejected by the supplied configuration.

```sh
# Run in THIS artifact repository; no gem5 build is needed for these checks.
python3 tests/check_artifact.py
python3 tests/run_native.py

# After building gem5 and the guest benchmark:
python3 tools/run_matrix.py --gem5-tree /path/to/gem5 \
  --binary /path/to/gem5/bench/scatter_dstate \
  --workload-args '4 2000 16' --out /path/to/new-results-directory
```

The runner saves commands, hashes, selected environment, guest output, gem5
configuration and statistics for every mode/repeat. It does not invent results
or interpret an oracle pass as a coherence proof. See the evaluation plan for
ROI instrumentation and performance acceptance criteria.

## Configuration

| Variable | Default | Meaning |
|---|---:|---|
| `DSTATE_ENABLED` | 1 | 0 sends the same operation directly through local ownership acquisition |
| `DSTATE_PERSISTENCE` | 1 | 0 allows home execution but disables promotion/read-retention policy |
| `DSTATE_FORCE_NACK` | 0 | Reject every delegated request before acceptance; correctness control |
| `DSTATE_EXEC_LATENCY` | 42 | Home service cycles per update, after exclusive acquisition |
| `DSTATE_QUEUE_DEPTH` | 1 | Accepted updates in flight per L2 bank (one per line); more is a sensitivity point |
| `DSTATE_INIT_INTERVAL` | 0 = latency | Cycles between successive update starts at a bank; below the latency models a pipelined datapath |
| `DSTATE_BUSY_STALL` | 0 | 1: a request for a line already acquiring/executing waits for it instead of being NACKed to the GETX path |
| `DSTATE_HIT_LATENCY` | 0 = latency | Service cycles when the target word is in the bank's hot-word buffer |
| `DSTATE_HOTWORDS` | 0 | Hot-word buffer entries per bank (register-resident recent words); 0 disables |
| `DSTATE_MERGE_LIMIT` | 0 | Same-word updates folded into one accepted operation (combining); 0 disables |
| `DSTATE_DELEGATE_FROM_S` | 0 | 1: an L1 holding the line read-only delegates the update and drops its copy instead of upgrading |
| `DSTATE_RELAXED_AMO` | 0 | 1 (O3 only): no-return atomics bypass TSO's one-store-in-flight rule, Intel RAO-INT style; fence where you publish |
| `DSTATE_REQ_COMBINE` | 1 | Queued same-word no-return adds a core issues as one summed request once the previous request to that line completes; 1 disables |
| `DSTATE_QUEUE_STALL` | 0 | 1: a request that finds the bank executor full waits in the input buffer for a released slot instead of being NACKed to the GETX path |
| `DSTATE_BUFFER_SIZE` | 32 | Entries per configured endpoint/trigger buffer |
| `DSTATE_TBES` | 16 | L1/L2 controller TBE count |
| `DSTATE_THRESHOLD` | 4 | Saturating accepted-update count needed for promotion |
| `DSTATE_READ_DOWNGRADE` | 3 | L2-visible read-run limit for persistent read-sharing reacquisition |

Promotion additionally requires at least two distinct requesters within the
current residency. L1 hits are not observed by this policy. By default one
accepted operation per L2 bank is allowed, including acquisition and service;
the queue/interval/stall knobs above relax that for ablation only.

The AADD instruction is unfenced and ordered as a store under TSO (see
`docs/ARCHITECTURE.md`). `bench/Makefile` builds `*_fenced` variants with
`-DDSTATE_FENCED`, which wrap every AADD in `mfence` to reproduce the earlier
always-fenced macro-op on the same simulator build; `bench/ordering_litmus.c`
is the store-ordering regression for both.

## Source maintenance

Canonical files live in `protocol/`, `configs/`, `bench/` and `build_opts/`.
The ISA and Ruby integration patches contain the supporting gem5 edits.
After modifying integration files in a disposable patched gem5 checkout, run:

```sh
python3 tools/export_patch.py /path/to/disposable-gem5
python3 tests/check_artifact.py
```

This regenerates all three patch views and includes the benchmark Makefile and
headers. The complete patch is sufficient for applying the artifact; do not
apply its component patches a second time.

## Licensing

This repository is BSD-3-Clause (see `LICENSE`), matching gem5.

The files under `protocol/` are derivative works of gem5's `MOESI_CMP_directory`
protocol and **retain their original copyright headers** (ARM Limited; Mark D. Hill and
David A. Wood) as those licenses require. Note in particular ARM's clause that the
license extends only to copyright in the software and does not grant rights to any
hardware implementation of the described functionality.

## Citing

Cite this development branch by its repository commit; it has no new DOI release.
The following citation is **only for the historical v1.0.0 artifact**.

Archived on Zenodo. Cite the concept DOI —
[10.5281/zenodo.22889935](https://doi.org/10.5281/zenodo.22889935) — which always
resolves to the latest version. To pin the exact snapshot these results came from, use
the v1.0.0 version DOI, [10.5281/zenodo.22889936](https://doi.org/10.5281/zenodo.22889936).
GitHub renders a "Cite this repository" button from `CITATION.cff`.

```bibtex
@software{sharma_moesi_d_2026,
  author    = {Sharma, Manav and Alkhameri, Yaseen},
  title     = {{MOESI-D}: A Persistent Sole-Writer Coherence State for
               Commutative No-Return Atomics},
  year      = {2026},
  version   = {1.0.0},
  doi       = {10.5281/zenodo.22889935},
  url       = {https://github.com/msharma2026/MOESI-D-Architecture}
}
```

## Authors

Manav Sharma and Yaseen Alkhameri.
