# Changes since v1.0.0

v1.0.0 is the artifact archived on Zenodo
([10.5281/zenodo.22889936](https://doi.org/10.5281/zenodo.22889936)) and described
by [`paper/MOESI-D Paper.pdf`](../paper/MOESI-D%20Paper.pdf). The PDF is kept
unchanged because that DOI points to it. This page lists what in it no longer
holds, what changed in the implementation, and where the current evidence is.

## Round 10 (2026-10-04): remote-atomics baseline, 32/64 cores, a real graph, hardware cost

No protocol changes. Added `bench/pagerank_push.c` (push PageRank on a SNAP
edge list, fixed-point ranks, serial oracle), `tools/ablate.sh v10` (the
remote-atomics baseline and the 32/64-core mesh phases), `tools/cacti_moesi_d.sh`
with `results/cacti_22nm/`, and `docs/HARDWARE_COST.md`. The results page's
round-10 section carries the numbers; README *Evidence status* summarizes them.

## Round 9 (2026-10-03): the recommended configuration, measured everywhere

No code changes beyond the driver (`tools/ablate.sh v9`). The round-8
recommended knob set was run on every workload at 4, 8 and 16 cores, on the
4×4 mesh and with ROI-only timing, and the executor and hot-word latencies were
varied; README *Evidence status* carries the resulting table. The read-mixed
loss at 4 cores is bounded as a network-round-trip effect (±5 points over
executor latencies 10–30) and becomes a win from 8 cores.

## Round 8 (2026-10-03): pruning, cheap rejection, speculation oracle

Removed, with their results kept on the results page: the distinct-writer gate
(`DSTATE_MIN_WRITERS`, `DSTATE_WRITER_EPOCH` as its epoch; superseded by the
change-rate gate), promotion on writers, the adaptive threshold (measured
+5.5 % vs +5.0 % without it) and the owner-tenure predictor (fixed the sketch at
4 cores, lost the hot-word and read-mixed wins at 16, cost a 1,024-entry table
per L1). Setting a removed knob now fails fast. Added: `DSTATE_GATE_TABLE` (the
gate table is hardware-sized and measured at 256 entries), `DSTATE_REJECT_AS_GETX`
(a delegated update the gate refuses is served as the requester's GETX: one trip
instead of NACK + GETX; retained lines and structural refusals still NACK),
`DSTATE_PROMOTE_CHANGES` (retention on the gate table's evidence, replacing
promotion on writers, which the pruning had removed and which the 16-core
read-mixed win turned out to need), and Sequencer statistics `dstate_spec_right/wrong/nocopy` that
measure, without forwarding anything, whether a load behind a delegated add could
have been served from the local copy plus the delta (the speculation oracle for
the forwarding proposal). The pruned build reproduces round 7 bit for bit on the
repeated rows.

## Round 7 (2026-10-02): admission by owner hit rate

The distinct-writer gate of round 6 could not separate shared lines where the
owner still wins (the Count-Min sketch at 4 cores) from shared lines where
delegation wins (512 scattered lines): both show four writers. Two admission
mechanisms that estimate the owner's hit rate replace it as the primary signal
(the writer gate stays as a knob); see `docs/ARCHITECTURE.md` §4a and the round-7
section of `results/ABLATION_2026-09-30.md`. A gem5 simulation-infrastructure
fix came with it: the L2 and directory controllers now declare a functional-read
priority, so a syscall-emulation read of a line whose only fresh copy is in
flight reads the L2 copy or memory instead of aborting the run (observed at
thread exit while another core polls the exiting thread's TLS line; unrelated to
delegated updates).

## Corrections to the v1.0.0 paper

1. **Software transparency.** v1.0.0 redirected ordinary x86 `LOCK ADD`. The
   current revision restores the upstream `LOCK ADD` macro-ops, including flags
   and every operand width, and exposes the operation only through an explicit
   no-return, no-flags instruction (`AADD`, `0F 38 FC /r`) used by the benchmarks.
   No acceleration of unmodified binaries is claimed.
2. **Early completion.** v1.0.0 completed the issuing atomic as soon as the
   request left the L1 (the `pendingDStateReqs` description), which allows a
   younger store to become visible before the add has been applied. That
   behaviour is withdrawn. The operation now stays outstanding until the update
   has been applied and acknowledged; see *Completion and ordering* below for
   how the core avoids stalling on it.
3. **Operators.** Only 32- and 64-bit modular integer addition is implemented.
   MIN/MAX/bitwise and floating-point operations described in v1.0.0 are not.
4. **Directory D state.** D is an L2-local retention policy; the global directory
   runs the unmodified MOESI_CMP_directory state machine. The directory-level D
   promotion and forwarding path described in v1.0.0 is not part of this design.
5. **Random testing.** The stock Ruby random tester used in v1.0.0 does not issue
   no-return atomics, so it did not exercise the delegated path. Directed CPU
   workloads with path-coverage checks replace that claim (see
   [VALIDATION.md](VALIDATION.md)).
6. **Performance, traffic and energy.** All v1.0.0 speedup, line-migration, byte
   and energy figures are superseded. They were obtained with early completion,
   undeclared packet sizes and zero-cost arithmetic at the remote cache. Current
   same-binary measurements are in
   [`results/ABLATION_2026-09-30.md`](../results/ABLATION_2026-09-30.md); notably,
   the single-hot-line and hot-key cases that v1.0.0 led with are slower than the
   conventional path in this revision.
7. **Workload scope.** The Count-Min Sketch program measures update aggregation
   only (no query or error-bound evaluation). Integer SpMV is a negative control,
   not evidence for general sparse kernels.
8. **Bibliography.** Several v1.0.0 reference entries carry incorrect or
   unverified metadata (patent numbers, assignees, venues, years). Consult the
   primary sources directly; [ARCHITECTURE.md](ARCHITECTURE.md) links the ones
   this revision relies on.

## Implementation changes

### Completion and ordering
- The issuing operation completes only on the terminal ACK of an applied update,
  or after local application on the fallback path. A completion-only sequencer
  callback cannot apply an update twice.
- The `AADD` micro-op is store-class: the reorder buffer retires it at commit and
  the store-queue entry, not the core, waits for the ACK. Under TSO the O3 store
  queue sends one store at a time, so a later flag store cannot overtake the add.
  The LSQ never forwards an atomic request's operand to a younger load.
- `DSTATE_RELAXED_AMO=1` (O3) applies the weaker ordering documented for Intel
  RAO-INT: no-return adds may overlap one another and later stores; software
  fences where it publishes. Correctness tests for that mode use the fenced
  guest binaries; the unfenced ones are contract checks that may observe the
  permitted reorder.
- `DSTATE_FAR_READS`: a load of a line held in D is answered with a snapshot and
  no sharer is recorded; the line stays at the home across reads. Read-heavy
  lines fall back to ordinary cached copies after `DSTATE_READ_DOWNGRADE` reads
  with no update.
- `DSTATE_DELTA_MIN_WORDS`: queued adds to several words of one line are sent as
  one masked delta-line request; bank-side merging is mask-based.
- `DSTATE_RANGE_LO_MB`/`HI_MB`: static per-address placement for oracle runs.
- `DSTATE_EVICT_FOR_DELEGATE`: a delegated request to a line the home does not
  hold, in a full set, evicts a victim instead of being rejected.
- `DSTATE_REQ_COMBINE`: the Sequencer issues queued same-word no-return adds
  from one core as a single summed request once the previous request to that
  line completes; all of them complete on its ACK. Only a consecutive run of
  identical type, address and width is combined, so ordering with respect to
  every other queued request is unchanged.

### Rejection and fallback
- A bank that cannot accept an update replies with a NACK *before* acceptance.
  The L1 keeps the operation, consumes the NACK, and retries it through an
  ordinary exclusive (GETX) acquisition and a local apply. Accepted operations
  are never NACKed, and invalidation cannot discard a pending delegated request.
- Owner-plus-sharer states that could expose a stale reader are rejected. The
  supported retained-read case invalidates every local reader before mutation.
- `DSTATE_FORCE_NACK=1` rejects every request, as a fallback correctness control.

### Modeled cost and finite resources
- Explicit home service time (`DSTATE_EXEC_LATENCY`, default 42 cycles) after
  exclusive acquisition; per-bank admission depth and initiation interval are
  configuration inputs; L1/L2 TBEs and every endpoint/trigger buffer are finite.
- Dedicated packet classes with a derived budget: 16 B for a 32-bit update, 24 B
  for a 64-bit update, 8 B for a terminal ACK/NACK.
- Bank-side mechanisms, each off by default: a hot-word buffer, same-word
  combining with one ACK per combined requester, waiting instead of rejecting
  when the line is busy or when the executor is full (`DSTATE_QUEUE_STALL`),
  and delegation from a read-only L1 copy. See the
  [configuration table](../README.md#configuration).

### Controls and build
- Delegation, persistence and forced rejection are independent switches, so the
  same guest binary runs as the conventional baseline (`DSTATE_ENABLED=0`), home
  execution, and home execution with retention.
- Two defects that prevented the protocol from building or from being exercised
  were fixed: SLICC boolean defaults, and a preprocessor guard that the gem5
  Kconfig build never defined (which had silently routed every operation down
  the conventional path). `tools/run_matrix.py` now refuses a passing result
  unless the mode's required protocol events are nonzero.

### Benchmarks and tooling
- Per-cell and per-row oracles with nonzero exit codes; checked thread creation
  and allocation; a race-free read sink.
- Count-Min Sketch rows use independent seeded mixing; the privatized baseline
  merges in parallel by cache line.
- Integer SpMV accepts only integer/pattern Matrix Market input and rejects
  malformed, truncated or fractional files.
- `tools/export_patch.py` regenerates the three patch views deterministically from
  the canonical sources; `tests/check_artifact.py` checks they agree.
- `tools/run_matrix.py` records commands, environment, simulator/guest/patch
  hashes, configuration, statistics and guest output for every run;
  `tools/ablate.sh` reproduces every table in `results/`.

## Open

Deadlock freedom with finite queues is argued, not proven; full-system execution,
device DMA, fault/interrupt ordering, larger core counts and mesh topologies,
ROI-scoped and repeated measurements, application-level benefit, and any power,
area or RTL evaluation remain to be done. See ARCHITECTURE.md §5.
