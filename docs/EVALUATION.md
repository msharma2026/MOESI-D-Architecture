# Validation and evaluation gates

The corrected code needs new evidence. Follow this order; do not tune a fast
configuration until the correctness gates pass.

## 1. Build and source checks

Apply the complete patch to clean gem5 v25.1.0.1. Build `gem5.debug` first for
assertions, then `gem5.opt` for measurement. Build the guest benchmarks with an
x86-64 Linux toolchain. Record exact compiler/build command and binary hash.
`tests/check_artifact.py` checks patch/source consistency and structural guards;
`tests/run_native.py` tests the arithmetic helper and pthread harnesses using
conventional host atomics. Neither is a gem5 protocol test.

SLICC/ISA generation can be checked without building the simulator, from gem5:

```sh
PYTHONPATH=src/mem:build_tools python3 -m slicc.main --tb -C generated-check \
  src/mem/ruby/protocol/MOESI_D.slicc
mkdir -p generated-isa-check
PYTHONPATH=src/arch:build_tools python3 -c \
  'from isa_parser import ISAParser; ISAParser("generated-isa-check").parse_isa_desc("src/arch/x86/isa/main.isa")'
```

Use an isolated Python environment with gem5's required dependencies (generation
here used `ply` and `PyYAML`). Generated code still requires C++ compilation.

## 2. Correctness before performance

Run `bench/coherence_regression` on both O3 and Minor timing CPUs, with D enabled,
disabled, persistence disabled and forced NACK. It includes mixed aligned ADD32
and ADD64, wraparound/neighbor preservation, ordinary stores, multiple readers,
publication and standard byte/word LOCK ADD CF/ZF checks on x86. It checks every
random-update cell before the next phase overwrites it. Host-native execution
does not establish that D paths were exercised.

Trace required transitions using gem5 protocol debugging and controller event
statistics. A final-value pass without nonzero relevant path coverage does not
close a finding. Force each of the following, with watchdog timeouts:

| Case | Required evidence |
|---|---|
| I/M/D acceptance and ILX/ILOX owner pull | One apply followed by one terminal ACK per ID; no early CPU completion |
| Force NACK, full executor, low TBEs/buffers, full cache set | NACK consumed, D_RETRY injects GETX, exactly one local apply; no lost update |
| OLSX retained reads and owner-plus-sharer rejection | All stale readers invalidated before mutation; rejected path uses ordinary GETX |
| Eviction/PUTX races, late writeback, data/ACK reordering | Correct retained value and no invalid transition or stuck TBE |
| Same/different addresses and false sharing | Correct per-cell values; completion cannot be mistaken for another ID |
| Publication, fences, loads/stores, full-width flags/forms | No forbidden outcomes under both O3 and Minor; verify fault paths separately |
| Long hot-bank overload, finite routing buffers | Progress of older requests, no response/request circular wait, bounded-resource occupancy |

Missing from the current directed program: deterministic forcing of every home
state/race, misalignment/fault handling, interrupt/drain, full-system operations,
DMA, randomized message delays, reset/loss, cache maintenance and memory-model
enumeration. Add a Ruby protocol-level injector or model checker for these;
the stock random RubyTester is only a base read/write compatibility check.
Provide a dependency graph and bounded invariant/liveness model before asserting
deadlock freedom. Include fairness assumptions explicitly.

## 3. Matched controls

Use the **same explicit-operation guest binary** and simulator build:

| Variant | DSTATE_ENABLED | DSTATE_PERSISTENCE | DSTATE_FORCE_NACK |
|---|---:|---:|---:|
| Local placement | 0 | 0 | 0 |
| Home placement without retention | 1 | 0 | 0 |
| Home placement with D retention | 1 | 1 | 0 |
| Rejection/fallback correctness control | 1 | 1 | 1 |

Local vs home estimates placement benefit under the same software contract.
Home vs persistent isolates the incremental D policy. Forced NACK is not a
fair performance baseline: it deliberately adds a rejection round trip.

Two further axes use the same simulator build. The guest binary selects the
fence contract: `*_fenced` targets wrap every AADD in `mfence` (the earlier
always-fenced macro-op); the plain targets rely on TSO store ordering. The bank
executor knobs select the service model, and their defaults are the conservative
one-slot case:

| Column | DSTATE_QUEUE_DEPTH | DSTATE_INIT_INTERVAL | DSTATE_BUSY_STALL |
|---|---:|---:|---:|
| One-slot, non-pipelined (default) | 1 | 0 (= latency) | 0 |
| Bounded queue, non-pipelined | 8 | 0 | 0 |
| Bounded queue, pipelined | 8 | 4 | 0 |
| Pipelined, same-line requests wait | 8 | 4 | 1 |

Sweep `DSTATE_EXEC_LATENCY` (20/42/80) across the last row. Report every column;
a queue depth above one or an interval below the latency is a sensitivity point
for a pipelined datapath, not a measured design.

Round-two mechanisms, each a knob defaulting off, layered on the last row:

| Column | Adds | Models |
|---|---|---|
| hot-word | `DSTATE_HOTWORDS=8 DSTATE_HIT_LATENCY=4` | recently updated words register-resident at the bank; misses still pay `DSTATE_EXEC_LATENCY` |
| combining | `+ DSTATE_MERGE_LIMIT=8` | same-word updates folded into one accepted operation, one ACK each |
| relaxed | `+ DSTATE_RELAXED_AMO=1` (O3 only) | Intel RAO-INT weak ordering; `ordering_litmus` unfenced is *expected* to reorder, fenced must pass |
| from-S | `+ DSTATE_DELEGATE_FROM_S=1` | an L1 holding the line read-only delegates and drops its copy |
| hot32 variants | `DSTATE_HOTWORDS=32` in place of 8 | buffer sized to cover the 32-counter working set; used for the hot-key and 800k-add `long` rows |

Round-three mechanisms, layered on the round-two columns (`ablate.sh v3`):

| Column | Adds | Models |
|---|---|---|
| back-pressure | `DSTATE_QUEUE_STALL=1` | a request that finds the executor full waits for a slot instead of falling back to GETX |
| requester combining | `DSTATE_REQ_COMBINE=16` | queued same-word adds from one core issued as one summed request; only reachable when the core keeps several adds in flight |
| deeper executor | `DSTATE_QUEUE_DEPTH=16 DSTATE_MERGE_LIMIT=64` | sensitivity points for the two queues |

`run_matrix.py --modes` selects a subset of the four modes; `ablate.sh v3 scale`
uses `local,remote,persistent` at 8 and 16 cores, and `v3 phased` runs
`bench/scatter_phased` (update every line, then every thread reads every line,
repeated), the read/update-phase workload the retention policy is for.

`DSTATE_RELAXED_AMO` changes the CPU, not the protocol, and `run_matrix.py` applies
it to the `local` mode as well; a relaxed row's baseline is the relaxed `local` row.
Single-line runs of 8,000 adds are dominated by process start-up (~150 µs); the
`ablate.sh v2 long` rows repeat them at 800,000 adds for timing.

The store-class micro-op is not a knob: it is on in every build after 2026-09-30.
Compare a build's default column against the previous build's to isolate it. Thread-context note for SE mode:
each pthread needs its own CPU, so `coherence_regression` (4 workers + main) needs
`--num-cpus=5` and `ordering_litmus` (writer on main + reader) needs 2.
Conventional LOCK XADD, plain single-writer operations, private accumulation,
streaming flush and combining are additional software controls, not substitutes
for the matched-ISA ablation. Compare equal freshness/visibility requirements.

## 4. Measured region and provenance

Build gem5's x86 `libm5` under `util/m5` using its supplied build instructions.
Rebuild guest benchmarks with `CPPFLAGS='-DBENCH_GEM5_ROI -I/path/to/gem5/include'`
and `LDLIBS=/path/to/gem5/util/m5/build/x86/out/libm5.a`. The Makefile accepts these
variables. The explicit ROI includes thread/synchronization and final merge
costs where applicable; verification is outside it. State startup/cache-state
and whole-program costs separately, since this is not an application-speedup ROI.

Use `tools/run_matrix.py --require-roi` for performance runs. Preserve the
**first explicit ROI statistics dump** (before final process-exit statistics),
the whole raw stats file, `config.ini`, `config.json`, guest output and manifest.
Do not parse a convenient last matching statistic and accidentally include the
serial oracle. A smoke run without ROI is valid only as a smoke run.

The runner hashes simulator, guest binary, patch and every `--input` file, stores
commands/relevant environment and refuses existing output directories. Input
paths inside `--workload-args` must be absolute; pass those files separately with
`--input` for hashing. Preserve compiler version/command alongside the manifests.
Current workload PRNG seeds are fixed/documented in source; repetitions do not
constitute independent input seeds. Add varied seeds before claiming confidence
intervals across workloads, rather than treating repeated deterministic runs
as independent samples.

## 5. Sensitivity and decision criteria

- Sweep 1/2/4/8/16 banks independently of 1/2/4/8/16/32/64 cores where feasible;
  include mesh/chiplet-like distance, not just a crossbar.
- Sweep service latency (e.g. 20/42/80 cycles), queue/TBE counts and line sizes.
  A 20-cycle case is a sensitivity point, not a measured physical datapath.
- Sweep concentrated vs bank-spread addresses, capacity above/below L2,
  update/read phases, false sharing, skew changes and conventional-atomic mixes.
- Record throughput, end-to-end and ROI latency, tails, NACK/fallback rate,
  D admissions/residency, ownership traffic, request/ACK bytes and bank occupancy.
  Event names changed: do not reuse the old `DSTATE_HOT` counter interpretation.
- Include a real query/update application and tuned parallel software baselines.
  The corrected CMS program tests update aggregation, not sketch error bounds
  or a production telemetry decision loop. Integer SpMV is a negative control.
- Report regressions and crossover points, not only wins. Byte/flit counts are
  traffic proxies, not joules. A power claim requires an appropriate power model
  and hardware cost estimate.

Performance statements should rest on repeated, ROI-scoped runs with varied
seeds once the correctness coverage above is complete. A hardware claim
additionally requires the obligations in ARCHITECTURE.md §5.
