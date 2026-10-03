# Validation actually performed

Two records. The first is the 2026-09-29 source-level validation of the corrected
branch (macOS, no simulator build). The second is the 2026-09-30 build-and-run
validation that followed it, which found two build-blocking defects the first
record could not see, fixed them, and produced the first measurements of the
corrected protocol. Numbers live in [`results/ABLATION_2026-09-30.md`](../results/ABLATION_2026-09-30.md).

## 2026-09-30: full build, directed execution, ablation

Host: WSL2 Ubuntu 24.04, gcc 13.3.0, Python 3.12.3, 24 cores / 19 GB. gem5 base
v25.1.0.1 (`c8222cc`) in a detached worktree with the complete patch applied.
Build: `scons build/X86_MOESI_D/gem5.opt -j10`, about 11 minutes from clean,
918 MB binary. Guest binaries built with the repository Makefile (gcc 13, static,
`-fopenmp -pthread`).

### Two defects that only a build could find

| Defect | Symptom | Fix |
|---|---|---|
| SLICC `bool` parameter defaults written as `true`/`false` in the L1 and L2 machines | SLICC copies the default verbatim into the generated Python SimObject; scons aborts (SIGABRT 134) at a `[SO Param]` step with `NameError: name 'true' is not defined`. Standalone SLICC generation passes. | `:= "True"` / `:= "False"`, as stock protocols write them |
| `#if defined(PROTOCOL_MOESI_D)` around the Sequencer's atomic classification, but gem5's Kconfig build defines no generic `PROTOCOL_<name>` macro (only `PROTOCOL_CHI`) | The branch compiled out. Every AADD reached Ruby as an ordinary store; the functor was applied at the requester by `hitCallback`; **every oracle passed while no delegated event ever fired.** | `env.Append(CPPDEFINES=['PROTOCOL_MOESI_D'])` in `src/mem/ruby/SConscript`, mirroring the CHI block |

Both are now guarded: `tests/check_artifact.py` asserts the SConscript define is
in the patch, and `tools/run_matrix.py` refuses to accept a run whose mode did
not produce its required events (`COVERAGE` table), so a silently conventional
run can no longer pass. Decisive post-build check:
`strings build/X86_MOESI_D/gem5.opt | grep -c 'Issuing ATOMIC NO RETURN'` must be 1.

### Passed

| Check | Result and scope |
|---|---|
| `git apply --check --whitespace=error-all` to a pristine `c8222cc` worktree | Clean, after normalizing two CRLF canonical files that a Windows checkout had produced (`*.h`, `*.mtx` now pinned in `.gitattributes`) |
| `tests/check_artifact.py` | 6/6, including the new protocol-macro guard; passes on Windows and Linux (`export_patch.py` path bug fixed) |
| SLICC and x86 ISA generation from the patched worktree | Pass; `dstateBaseStalls` and 59 `DState_Busy` transitions present in generated C++ |
| `tests/run_native.py`, `tests/dstate_unit.cc` | Pass on Linux (previously only macOS) |
| `coherence_regression` under `run_matrix.py`, 5 CPUs, all four modes, **coverage gate on** | O3 fenced/default, O3 unfenced/default, O3 unfenced with queue-8 / interval-4 / busy-stall, Minor fenced/default: 16/16 modes CORRECT with the required delegated, local, reject and retry events all nonzero |
| `ordering_litmus`, 2 CPUs, O3, 20,000 rounds × 2 variants | Unfenced/default, fenced/default, unfenced/knobs: 59,998 delegated completions each, **0 forbidden observations, 0 lost updates**. Stock `lock addl` and local-placement controls: conventional path, 0 forbidden |
| v1.0.0-style tree (early completion) as negative control | 5,000 rounds × 2, 14,998 delegated, 0 forbidden. Inconclusive by design: a deterministic 2-core crossbar never opened the window. Only a FAIL would have been evidence |
| `scatter`, `scatter_rw`, `scatter_multi` (512 lines) under `run_matrix.py`, O3, Garnet crossbar, 4 banks | Every configuration × mode CORRECT with coverage; see the results file |

### Round 2 (second build of the same day, 16:19): five more mechanisms

Store-class AADD micro-op (`IsStore`, ROB retires at commit, the store-queue entry
holds the ACK), hot-word buffer, same-word combining, relaxed no-return ordering
(`DSTATE_RELAXED_AMO`, O3), and delegate-from-S. Same worktree and base commit;
`export_patch.py` re-run after the build left every canonical file in the
worktree byte-identical (md5 of the mirrored set unchanged), so the patches in
this repository are what was measured.

Two more defects that only execution found:

| Defect | Symptom | Fix |
|---|---|---|
| Minor CPU LSQ copies store data for every store-class request; the store-class atomic carries a functor, not data | `X86MinorCPU` segfault (rc −11) on the first AADD, O3 unaffected | `src/cpu/minor/lsq.cc`: treat `isAtomic()` and the AMO functor alike (no data copy), mirroring the O3 forwarding guard |
| First delegate-from-S design dropped the S copy silently and went to `D_REQ`; the L2 still believed the L1 held S and forwarded `Fwd_GETS`/`Inv` to it | `Invalid transition event: Ack state: IS` in `coherence_regression` | Two new L1 states `D_REQ_S`/`D_RETRY_S` that keep the duties of S (serve `Fwd_GETS`/`Fwd_DMA`, ACK `Fwd_GETX`, drop to `D_REQ`/`D_RETRY` on `Inv`) until the ACK or the fallback |

Passed after the fixes (every mode CORRECT **and** through the coverage gate):

| Check | Result |
|---|---|
| `coherence_regression`, 5 CPUs: O3 hot+merge, Minor hot+merge, O3 from-S, O3 relaxed | 16/16 modes, required delegated / local / reject / retry / merge events nonzero; the from-S run shows `DStateReq_CPU_S` and `D_REQ_S` transitions |
| `ordering_litmus`, 2 CPUs, 20,000 × 2: hot+merge, from-S, fenced+relaxed | 0 forbidden, 0 lost, 59,998 delegated each. Unfenced+relaxed (L7) also 0 forbidden, but that row is a contract check only: the relaxed knob *permits* the reorder and a deterministic 2-core crossbar did not produce it |
| `scatter` ×5, `scatter_rw` ×5, `scatter_multi` 512 ×5, hot-key ×6, 800k-add `long` ×9 | every configuration × mode CORRECT with coverage; numbers in the results file |
| `tests/check_artifact.py` 7/7 (adds the forwarding-guard and macro tests); `tests/dstate_unit.cc` (admission, interval, hot-word LRU/drop, merged arithmetic) | pass |

### Round 3 (2026-10-01 build): requester-side combining and bank back-pressure

Two mechanisms, both default-off: the Sequencer issues a consecutive run of
queued same-word no-return adds as one summed request (`DSTATE_REQ_COMBINE`),
and a request that finds the bank executor full waits for a slot instead of
being NACKed (`DSTATE_QUEUE_STALL`). `tests/check_artifact.py` 8/8 after export;
the built binary carries the new Sequencer code and the `DState_Full` event.

A gate finding that changes how the relaxed column must be read:

| Run | Binary | Knobs | Result |
|---|---|---|---|
| G9u | `coherence_regression` (unfenced) | relaxed + combining + back-pressure | 3/4: persistent mode aborted in a guest check |
| G14 | unfenced | relaxed + combining, no back-pressure | persistent FAIL |
| G13 | unfenced | relaxed + back-pressure, no combining | PASS |
| G12 | `coherence_regression_fenced` | relaxed + combining + back-pressure | PASS |
| G9 | `coherence_regression_fenced` | relaxed + combining + back-pressure, all four modes | **4/4** |

The unfenced regression relies on TSO: its barrier and release stores are
expected to order after the preceding adds. Under `DSTATE_RELAXED_AMO` that is
not promised (the Intel RAO-INT contract), and requester-side combining, which
holds queued adds a little longer, widened the window until the test observed
the reorder. The fenced regression — the one the relaxed contract actually
requires — passes in every mode. Consequently the round-2 result "G8 relaxed
unfenced 4/4" was luck of timing, not evidence; the unfenced regression under
relaxed ordering is now listed as a contract check (G9u), like litmus L7/L12.
Under TSO, combining and back-pressure pass everything: O3 (G11) and Minor (G10)
regressions 4/4, unfenced litmus 0 forbidden on both CPUs (L10, L13), and the
fenced litmus under relaxed knobs 0 forbidden (L11).

Performance matrices of round 3 (`results/ABLATION_2026-09-30.md`, round 3): every
configuration × mode CORRECT with coverage, including the 8- and 16-core runs,
the 2,048- and 8,192-line runs and the phased update/read workload.

One infrastructure finding at 16 cores. With the default 32-entry endpoint
buffers, the **conventional** (`DSTATE_ENABLED=0`) and `remote` modes of the
phased TSO workload on 16 cores and 4 banks aborted after ~0.5 ms of simulated
time with Garnet's `Possible network deadlock in vnet 0` (run P5), while the
`persistent` mode and every relaxed-ordering 16-core run completed. The failing
mode contains no delegated path at all: it is the base protocol's request traffic
saturating finite buffers until a virtual channel stays busy for the 50,000-cycle
detection threshold. All 16-core rows in the results therefore use 256-entry
buffers (`DSTATE_BUFFER_SIZE=256`) in every mode, and completed; the two 32-entry
16-core runs (P4, P5) are kept only as the record of the failure. The 4- and
8-core rows use the default 32 entries. Whether the detector saw a true deadlock
or starvation was not resolved (three diagnostic runs were lost to a host-side
kill); the delegated modes are not implicated either way.

### Round 4 (2026-10-01 build): far reads, delta-line requests, static placement

Mechanisms: far reads (`DSTATE_FAR_READS`: a load of a line held in D gets a
snapshot and no sharer is recorded; `IS + Data_Uncached -> I` at the L1,
`D + L1_GETS_Far -> D` at the L2), delta-line requests (`DSTATE_DELTA_MIN_WORDS`:
one masked multi-word request; mask-based merging at the bank), and static
placement by virtual-address range for oracle runs (`DSTATE_RANGE_LO_MB/HI_MB`,
decided in the Sequencer). `tests/check_artifact.py` 9/9 after export.

Gates on the round-4 build, every mode CORRECT and through the coverage gate:

| Check | Result |
|---|---|
| `coherence_regression_fenced`, O3, relaxed + far reads + delta lines (G18) | 4/4; 2,062 far reads served |
| `coherence_regression`, Minor, TSO + far reads (G19); O3, TSO + far reads (G20) | 4/4 each; 2,093 far reads in G20 |
| `coherence_regression` unfenced, O3, relaxed (G21, contract check) | 4/4 this time; still not a gate |
| `ordering_litmus` unfenced TSO (O3 L20, Minor L22), fenced relaxed (L21), stock `lock addl` (L23), unfenced relaxed contract check (L24) | 0 forbidden, 0 lost in all five |
| re-gate after the static-placement change to the L1 mandatory path (G22–G24, L25–L26) | 4/4, 4/4, 4/4; 0 forbidden |

Three findings that changed the campaign, none of them in the protocol:

1. **The first static-placement knob matched nothing.** It compared Ruby's
   *physical* line address with a range of the benchmark's *virtual* mapping, so
   the oracle runs (Q9, Q11, S10) delegated no line at all and failed coverage.
   The decision moved into the Sequencer, which has the packet's virtual address;
   those runs are archived as superseded and were repeated (M-series).
2. **gem5's Garnet has no unmasked functional read.** A syscall's functional
   access to a line whose only copy is in flight reaches
   `Network::functionalRead(Packet*)`, which Garnet leaves at the base-class
   `fatal("Functional read not implemented")`. The conventional (`DSTATE_ENABLED=0`)
   relaxed-ordering CMS run died this way (A0 local). The patch adds the method
   (same semantics as SimpleNetwork's); the repeat (A6) then reached the next
   missing layer, the per-message masked `functionalRead(Packet, WriteMask)` that
   the MOESI_CMP_directory family never defined, so the protocol's three message
   types now define it too (a `DSTATE_REQ`'s data block is an operand, never line
   data, and is not readable). The second repeat (A11) then failed one layer
   deeper, `Ruby functional read failed for address ...`: at the moment of the
   syscall's functional access no controller or message held a readable copy of
   the line (a store in transit on the conventional relaxed path), which gem5's
   SE mode cannot resolve. That is a simulator limitation, not something the
   patch should paper over, so the CMS relaxed column has no conventional
   baseline; its TSO column (A1) is complete. The delegated modes never hit any
   of the three gaps.
3. **The first mixed-regime program could not test the policy**: its "read then
   add" lines were private to one core, so they never left that core's L1 and the
   placement question never arose. The second version makes region B shared
   read-mostly lines (every thread reads them every iteration, one rare writer);
   results in the results file. The checked-in SpMV fixture is a 4×4 correctness
   case; the SpMV rows use a generated 4096×4096 / 65,536-nnz integer matrix
   (`bench/gen_integer_matrix.py 4096 65536 1`, SHA-256 `e435b27da80f6eca…`).

### Round 5 (2026-10-01): randomized stress, debug binary, ROI timing

`bench/random_stress` gives the protocol what the stock random tester never did:
seeded random no-return adds of both widths (never sharing bytes), loads, full
fences and private stores over a working set twice the L2, with an exact per-word
oracle. Three seeds on O3 relaxed (R4), O3 TSO (T4) and Minor TSO (TH4), four
modes each: **36/36 CORRECT with coverage**. Two more seeds with
`DSTATE_EVICT_FOR_DELEGATE=1` on 8,192 lines and one at 16 cores: all modes
CORRECT. The regression (fenced relaxed, TSO O3, TSO Minor) and one stress seed
on **`gem5.debug`** (assertions enabled, including every SLICC resource and
state assertion): 16/16 CORRECT, no assertion fired.

ROI-timed reruns used guests built against `libm5`
(`CPPFLAGS=-DBENCH_GEM5_ROI`, `--require-roi`); the reported statistics are the
first dump (updates, synchronization and merge; the serial oracle excluded).
Numbers in the results file, round 5.

The `graph_push` application run with the round-4 knobs rejected 85% of its adds
on the 8 MB working set (the home's sets were full and the shipped behaviour is
to reject); the eviction-for-delegate variants are the rows that count there.

### Round 6 (2026-10-02): admission gate, TSO same-line batching, ACK value

Gates on the round-6 build, every mode CORRECT and through the coverage gate:
regression with batching alone (O3 TSO) 4/4; regression with gate (2 writers),
batching, ACK value and promotion-on-writers on O3 TSO, Minor TSO and O3
fenced-relaxed 4/4 each, with both admission paths exercised (hundreds of
rejections, ~15,000 accepted delegations per run). Ordering: the existing litmus
with batching on (L40, L44) 0 forbidden; the new `ordering_litmus2` (two adds to
one line — two words, or one word twice — then a flag store to another line;
the reader must never see the flag before every add) 0 forbidden in 40,000
rounds each with batching on, off, and on the conventional path; the fenced
variant under relaxed ordering with ACK value 0 forbidden.

One defect found by the coverage gate before any performance number was taken:
the first admission gate kept its writer bitmap in the L2 cache entry, and a
line the home does not hold (an L1 owns it: state ILX) has no entry, so rejected
writers were never counted and the gate admitted nothing. The bitmap moved into
a bounded per-bank table in the engine (1,024 lines, oldest dropped), which is
also the more realistic hardware. Runs made with the broken gate are archived
outside the run table.

What the gate cannot do: distinguish shared lines where the owner still wins
(CMS at 4 cores: every hot line has 4 writers, yet most of a core's adds would
hit lines it already holds) from shared lines where delegation wins (512 lines,
also 4 writers each). A threshold above the core count rejects everything and
costs a NACK round trip per add before the migration (+37% on 512 lines); a
threshold below it keeps the many-line win (−17% vs −21% without the gate) but
admits CMS (+22%). The writer count separates single-writer from shared lines,
not the two kinds of shared line; the signal that would is the owner's expected
hit rate, which the home does not observe.

### Round 7 (2026-10-02): change-rate gate, owner-tenure predictor, functional-read priorities

Gates on the round-7 build, every mode CORRECT and through the coverage gate:
regression with the change-rate gate (50 %) plus batching, ACK value and
promotion on O3 TSO; gate alone on Minor TSO; gate plus ACK value on O3
fenced-relaxed; gate with adaptation on O3 TSO; tenure predictor (threshold 1)
with and without probing on O3 TSO, Minor TSO and O3 fenced-relaxed: 4/4 each,
nine matrices, both admission paths exercised in every one (the floor rejects a
line's first touch, 132 per run; 13,000–21,000 delegations accepted). Ordering
litmus with each new knob on: 0 forbidden in every run (L50–L53).

Two infrastructure findings this round, both fixed before any number was taken:

1. **Functional reads of lines in flight.** gem5's syscall emulation reads
   memory functionally; a read of a line whose only fresh copy was a response in
   transit between two L1s found no readable copy and aborted the run
   (`Ruby functional read failed`). The protocol trace shows the mechanism:
   core 0 polls the exiting thread's `tid` word (pthread_join) while core 1,
   exiting, stores into the same TLS line; the GETX takes the line from core 0
   at the instant of the functional read. Nothing delegated is involved. gem5's
   intended fallback is a per-controller `functionalReadPriority()` that lets
   `RubySystem` read a Maybe_Stale copy; MOESI_CMP_directory does not declare
   one, so the L2 (20) and directory (30) now do, as `MESI_Two_Level` does. A
   first attempt that read the backing store from `RubyPort` segfaulted
   (`phys_mem` is NULL unless `--access-backing-store` is set) and was
   reverted; it never shipped in the patch. Three aborted runs were archived
   outside the run table and rerun on the fixed build.
2. **Builds and driver edits during a running chain.** Rebuilding `gem5.opt`
   while a chain launches runs, and editing `tools/ablate.sh` while bash is
   executing it, each corrupted a batch (four 16-core matrices, one phase).
   Those runs were archived and repeated; the rule is now in the driver's
   header comment.

Design finding, recorded because it bounds what the home can do: the change
ratio the home measures is an upper bound on the true migration rate. While a
line is conventional the owner's local hits are invisible to the home; while it
is delegated, requester-side combining folds a core's run of adds into one
request. Both inflate the ratio toward 100 %, which is why the sketch at 4
cores (true ratio 0.21) is still admitted by a 50 % threshold until time decay
rejects its quiet lines. The requester-side tenure predictor sees the true
owner hit rate but only its own, and at 16 cores reads an owner's two or three
adds before the recall as "owner wins"; it is a 4-core refinement, not a
general policy (round-7 section of the results page).

### Round 8 (2026-10-03): pruning, cheap rejection, promotion on the gate's evidence

Gates on the round-8 builds, every mode CORRECT and through the coverage gate:
twelve regression matrices (change gate with GETX-served rejection on O3 TSO,
Minor TSO and O3 fenced-relaxed; gate at 90 %; 256-entry table; delegate-from-S
with ACK value; promotion on changes; the final gate-only GETX service on O3 and
Minor) 4/4 each; litmus with the new knobs 0 forbidden. The GETX-served path is
exercised in every matrix that enables it (`D_REQ` receives `Exclusive_Data`
and `Ack`; L2 `L1_GETX` events on delegated requests).

The pruned build reproduces round 7 bit for bit on five repeated rows (same
simSeconds, flits, rejections, merges), which is the test that deleting the
writer gate, the adaptive threshold and the tenure predictor removed nothing
else. One regression the pruning did cause and the gates did not catch:
promotion on writers (`DSTATE_PROMOTE_WRITERS`) was removed as marginal on the
4-core rows, but the 16-core read-mixed win (1.71×) depended on it, because far
reads only serve a line retained in D and the update-count rule rarely promotes
a line that is read every eight adds. Caught by the round-8 16-core rows
(+45 % instead of 1.71×) and replaced by `DSTATE_PROMOTE_CHANGES`, which
promotes on the gate table's own evidence and reproduces the round-7 row
exactly. Lesson recorded: a knob is marginal only on the rows it was measured
on; every pruning needs the full 16-core set, not the 4-core subset.

Cheap rejection took three versions to get right, each one measured:

1. Every refusal served as the requester's GETX: CMS at 4 cores improved to a
   near tie, but the 16-core read-mixed run lost its far reads entirely
   (297,000 → 0) and went from 1.71× to +44 %.
2. Retained (D) lines excluded: no change at 16 cores; the lines were being
   taken before they were ever retained.
3. GETX service for the gate's own decision only; structural refusals (owned
   elsewhere mid-transition, executor or TBEs full, no L2 slot) NACK as before:
   16-core read-mixed back to 1.71× at both gate settings, 4-core results kept.
   The distinction is principled: the gate says "the owner should have this
   line" (migrate now, one trip); a structural refusal says "not right now"
   (the requester's delayed retry finds the home ready and the line stays
   where it is). Serving the second kind as a migration turned every transient
   refusal into a line movement, which at 16 cores is most refusals.

The speculation oracle counts predictions at every delegated issue, including
the single-request paths inside the combining logic (a first version missed
those and over-counted "no local copy"; corrected and rerun before the numbers
were taken).

### Measurement caveats that apply to every number

- **Whole-program `simSeconds`, no ROI.** `libm5` was not built, so times include
  OpenMP runtime start-up and the serial oracle. Comparisons *between modes of the
  same binary* are meaningful (identical fixed cost); absolute speedups are diluted.
  On `scatter` (8,000 adds) the fixed cost dominates and ±1% differences are noise;
  on `scatter_multi` (4.1M adds) it does not.
- Deterministic single runs, fixed seeds. No confidence intervals are claimed.
- Executor depth/interval/latency values other than the defaults are sensitivity
  points, not a measured datapath.
- Flit counts are a traffic proxy, not energy.

### Not performed / not claimed

- Fault/interrupt ordering on either CPU (the `gem5.debug` build now gates the regression and stress programs).
- Root cause of the 16-core, 32-entry-buffer deadlock report in the base protocol
  (true deadlock vs starvation), and bank-count sweeps at 16 cores.
- A non-deterministic or larger-window litmus for the relaxed knob: L7 shows only that
  this simulator did not reorder, not that it cannot; the contract says it may.
- Model checking, saturation to buffer overflow, deadlock/starvation proof.
- 8,192-line and larger working sets; bank/core sweeps; mesh topology; tail latency.
- ROI-scoped timing, repeats with varied seeds, energy, RTL/PPA.

## 2026-09-29: source-level validation (historical)

Date: 2026-09-29. Host: macOS arm64, Apple Clang 17.0.0, Python 3.14.7.
gem5 base: v25.1.0.1, commit `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`.
Python generation environment: `ply 3.11`, `PyYAML 6.0.3`.

| Check | Result and scope |
|---|---|
| `git apply --check --whitespace=error-all` and actual apply to a fresh pinned gem5 worktree | Complete patch applies cleanly, including new headers, protocol files, configs, fixtures and benchmark Makefile |
| SLICC generation, MOESI_D | Pass, including final D_RETRY/backpressure fix; repeated from fresh patched worktree |
| SLICC generation, base MOESI_CMP_directory in integrated tree | Pass; generic Ruby integration additions do not prevent base source generation |
| x86 ISA generation | Pass; stock macros plus explicit AADD-subset decoding/micro-ops generate |
| x86-64 object compilation of `coherence_regression.c` | Pass with Clang; validates inline-assembly constraints/encoding assembly, not execution in gem5 |
| `python3 tests/check_artifact.py` | Five checks pass: complete-patch mirror equality, stock-ISA structural guard, terminal/fallback contract guard, removed ghost/operator definitions, finite-buffer/ablation configuration |
| `tests/dstate_unit.cc` | Actual helper used by DataBlock passes 32/64-bit wrap and all aligned offsets for 64/128/256-byte lines, neighbor preservation, finite executor reserve/release and ID increment |
| `python3 tests/run_native.py` pthread cases | Ten host-native workloads pass |
| Serial-only loader/oracle checks; SpMV malformed-input checks; Python syntax, runner CLI, Makefile dry run, `git diff --check` | Pass |

That record explicitly did not build or execute gem5. The two defects above are
what that gap concealed; generation success is not a compiler, behavioral or
formal correctness result.

The historical PDF was left unchanged; its SHA-256 remains
`a9f7847a1069023896fb4c107aaa1d39a88836931d30499b12da24d410abe9fb`.
