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
path. With the mechanisms below, on 512 to 8,192 hot lines every update delegates
with about 7× fewer flits: **20–21% faster** than the matched conventional path
under TSO at 4, 8 and 16 cores, and **3.0–3.35× faster** when both sides use the
relaxed no-return ordering of Intel RAO-INT (`DSTATE_RELAXED_AMO=1`); 2.3–2.5× at
16 cores on 4–8 banks. A single hot word is a tie at 4 cores and **1.4× (TSO) to
1.6× (relaxed) faster at 16 cores**. The retention policy (D) earns its keep
through **far reads**: the read-interleaved single line, 3.2× slower in round 3,
is 18% slower at 4 cores and **2.2× faster at 16 cores**; phased update/read
workloads gain 8–28% from retention at 16 cores. On a 4×4 mesh the 16-core
many-line gain is **4.95×** (migrations pay for hops, home execution does not);
32 and 64 cores on larger meshes give 4.9× and 3.2×. Correctness: directed
regression, ordering litmus, a seeded randomized stress program on both CPU
models, and the assertion-checking `gem5.debug` build all pass with the delegated
path exercised. Whole-program and ROI-timed single deterministic runs on
microbenchmarks and small kernels; not an application speedup.

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
| `DSTATE_FAR_READS` | 0 | 1: a load of a line held in D is answered with a snapshot and no sharer is recorded, so the next update needs no invalidation; after `DSTATE_READ_DOWNGRADE` reads with no update the line takes the ordinary cached path |
| `DSTATE_DELTA_MIN_WORDS` | 0 | Queued adds to at least this many different words of one line are sent as one masked delta-line request (32–144 B by word count); 0 disables |
| `DSTATE_RANGE_LO_MB` / `_HI_MB` | 0 / 0 | When hi > lo, only adds to virtual addresses in [lo MB, hi MB) are delegated (static placement for oracle runs) |
| `DSTATE_MAX_OUTSTANDING` | 16 | Per-core outstanding Ruby requests (gem5 default); applies to every mode |
| `DSTATE_EVICT_FOR_DELEGATE` | 0 | 1: a delegated request to a line the home does not hold, in a full set, evicts a victim (as an L1 GETX does) instead of being rejected |
| `DSTATE_MIN_WRITERS` / `DSTATE_WRITER_EPOCH` | 0 / 64 | Admission gate: accept a delegated update only once this many distinct cores have written the line in the current epoch of updates; otherwise reject it and the requester owns the line. 0 disables |
| `DSTATE_PROMOTE_WRITERS` | 0 | Promote to D once this many distinct writers are seen in the epoch (0: the update-count rule) |
| `DSTATE_ACK_VALUE` | 0 | 1: the terminal ACK carries the updated word (16 B response); a load of that word waiting behind the add completes from it |
| `DSTATE_MIN_CHANGE_PCT` / `DSTATE_MIN_CHANGES` | 0 / 2 | Change-rate gate at the home: accept a delegated update only when the line's would-be ownership changes (a GETX from a non-owner when conventional, an update from a core other than the previous writer when delegated) are at least this percentage of its updates, and at least this many; counters halve every `DSTATE_WRITER_EPOCH` updates. 0 disables |
| `DSTATE_WRITER_IDLE` | 0 | Time-based decay of the gate table: a line idle for this many cycles restarts as single-writer (0: the update-count epoch) |
| `DSTATE_ADAPTIVE_GATE` | 0 | 1: per-bank adaptation of `DSTATE_MIN_CHANGE_PCT` in steps of 10 with an 8-window cooling-off period |
| `DSTATE_MAX_TENURE` / `DSTATE_TENURE_IDLE` / `DSTATE_TENURE_PROBE` | 0 / 0 / 0 | Requester-side owner-tenure predictor (L1): delegate a line only if, the last time this L1 owned it, it performed at most this many adds before losing it; a record expires after the idle cycles; with probe=1 a line with no record is owned first so the tenure can be measured. 0 disables |
| `DSTATE_TSO_SAMELINE` | 0 | 1 (O3 only): under TSO a younger no-return add to the same line may issue while older ones are in flight; the home applies the batch atomically, stores to other lines wait for all of them |

Recommended set after the 2026-10 ablations: the `full` executor knobs (`DSTATE_QUEUE_DEPTH=16
DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1`), `DSTATE_REQ_COMBINE=16
DSTATE_MERGE_LIMIT=64 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4 DSTATE_FAR_READS=1
DSTATE_READ_DOWNGRADE=64 DSTATE_EVICT_FOR_DELEGATE=1 DSTATE_DELTA_MIN_WORDS=2`, plus
`DSTATE_TSO_SAMELINE=1` under TSO or `DSTATE_RELAXED_AMO=1` under relaxed ordering, and
for admission `DSTATE_MIN_CHANGE_PCT=50 DSTATE_WRITER_IDLE=200000` (the change-rate gate
with time decay: it cost no measured win at 4 or 16 cores and takes the Count-Min sketch
at 4 cores from +24 % to +5 %). The distinct-writer gate (`DSTATE_MIN_WRITERS`) and the
owner-tenure predictor (`DSTATE_MAX_TENURE=1 DSTATE_TENURE_PROBE=1`, which closes the
sketch to a tie at 4 cores but loses the hot word and read-mixed wins at 16) remain
available as knobs. The defaults stay at the conservative as-shipped values.
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
