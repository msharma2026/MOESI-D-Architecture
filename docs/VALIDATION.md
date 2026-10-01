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

- `gem5.debug` build; fault/interrupt ordering on either CPU.
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
