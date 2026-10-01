# Correctness-first architecture

Revision: 2026-09-30. This is an executable simulator design specification,
not a verified server coherence protocol, physical implementation, or ISA
compatibility certification. Differences from v1.0.0 are listed in
[CHANGES.md](CHANGES.md).

## 1. Architectural contract

The software interface is an explicit **no-return, no-flags, modular integer
addition** to a naturally aligned 4- or 8-byte memory operand. It returns no old
value and does not modify condition codes. Overflow wraps at the operand width;
neighboring bytes are unchanged. The operation may execute locally or at home;
that choice must not change its architectural result or completion guarantee.

The research decoder uses the AADD encoding (`0F 38 FC /r`, REX.W for 64 bits),
with a memory destination and register source. `bench/dstate_ops.h` emits it
explicitly. Operand-size override, REP, REPNE, LOCK and register destinations
are rejected by the selected decoder; unsupported size/alignment faults before
the memory operation. The ModRM predecoder table is updated as well as the main
decoder. All original `ADD_LOCKED` macro-ops are unchanged.

**This is not full Intel RAO-INT support.** No CPUID feature is advertised.
Only x86-64 timing-CPU SE experiments on ordinary cacheable user memory are
supported. Other RAO operations, TSX, memory-type semantics, MMIO, virtualization,
full-system execution, device DMA and checkpoint/recovery behavior are not
certified. The supplied config rejects full-system/device DMA use. Do not use
this encoding on physical hardware without a proper ISA-specific implementation
and feature detection.

The AADD macro-op is a bare `stula` micro-op with no fences, classified as a
**store** (`IsStore`) whose memory request carries `ATOMIC_NO_RETURN_OP`. Like any
store it executes speculatively, commits without waiting for memory, and is sent
from the store queue only after commit, so a wrong-path add never leaves the
core; the O3 LSQ holds the entry until the terminal ACK. It is ordered as a store
under TSO: the O3 LSQ writes back one store at a time
(`src/cpu/o3/lsq_unit.cc`, `needsTSO`/`storeInFlight`), so a younger store is not
sent until this operation's terminal ACK has returned. That is what forbids a
later flag store from becoming visible before the add (`bench/ordering_litmus.c`).
Because a store-class entry would otherwise forward its data to a younger load
of the same address, the LSQ's forwarding check also treats any *request* flagged
atomic as non-forwardable (the add's "data" is its operand). With
`DSTATE_RELAXED_AMO=1` (O3) no-return atomics neither wait for nor hold the TSO
in-flight slot -- Intel's documented RAO-INT contract -- and software fences where
it publishes; `ordering_litmus` is *expected* to observe reordering unfenced and
must pass fenced. As for any store, TSO permits a younger *load* to
pass it; software that needs load-after-add ordering adds a fence, and
`-DDSTATE_FENCED` in `bench/dstate_ops.h` wraps every AADD in `mfence` to
reproduce the previous always-fenced macro-op as an ablation column on the same
simulator build. The outstanding memory operation is retained until local
application or remote execution ACK; this revision does not restore the old
early-completion callback and does not invent a new speculative-execution fix.
It is not a proof of all x86 ordering, fault, or interrupt behavior; directed
CPU tests and memory-model validation remain required.

Intel describes no-return arithmetic operations separately from ordinary
locked instructions. WG21 P3111R2 illustrates why an unused result alone does
not permit weakening surrounding synchronization. These motivate an explicit
interface and conservative completion, not a claim of a new ISA invention.
[Intel specification](https://cdrdv2-public.intel.com/843860/architecture-instruction-set-extensions-programming-reference-dec-24.pdf),
[P3111R2](https://open-std.org/JTC1/SC22/WG21/docs/papers/2024/p3111r2.html).

## 2. Transaction lifetime and NACK meaning

```
CPU atomic remains outstanding
  -> L1 invalid: reserve placeholder/TBE, send (address, ID, width, operand)
       -> pre-acceptance NACK: keep operation -> GETX -> local apply -> complete
       -> accept at home: acquire/drain -> service timer -> apply -> ACK -> complete
  -> L1 already valid: ordinary ownership upgrade/local atomic -> complete
```

`D_REQ` no longer completes the CPU. It records access metadata and a per-L1
64-bit transaction ID. The sequencer retains the original functor; that is the
authoritative retry operand. On NACK, L1 enters `D_RETRY` and immediately consumes
the response. A separate timer-driven transition injects GETX and enters `IM_AMO`
when request-network space is available. This avoids a new response-to-request
buffer dependency under saturation. It does not reread a mandatory-queue entry
that has already been popped. It does
not deallocate the operation or acknowledge success. On ACK, the new
`atomicRemoteCallback` removes the outstanding request without calling its
functor, preventing double application. Responses must match ID and home.

NACK means **not accepted, not applied, and not scheduled for future execution**.
L2 consumes the rejected message in a transition that never allocates an
execution transaction. Resource pressure, unsupported home states and the
forced-rejection test hook all use this path. Once accepted, an operation
cannot NACK; it holds its resources through application and response enqueue.

Invalidation of a pending L1 placeholder sends the required coherence ACK but
does not destroy its TBE. Ordinary CPU accesses/replacement wait. An ACK means
the authoritative value has already changed, not merely that a queue accepted
the request. Runtime assertions enforce exclusive local ownership at apply.

Exactly-once reasoning is conditional on gem5's reliable transport, one active
operation per requester/line and non-reused live IDs. There is **no timeout
replay** of accepted operations. IDs detect mismatches; they are not a durable
deduplication log. A lossy fabric, controller reset, wraparound/restart or
cross-socket retry scheme needs an explicit epoch/dedup/recovery protocol.

## 3. Ownership and the meaning of D

D is a **home-L2-local policy state**, globally represented by ordinary MOESI
exclusive ownership. The directory file is restored to the upstream protocol,
including its consistency assertions. No second directory D state, special
promotion message or unused forwarding path remains.

| Home state at admission | Action |
|---|---|
| M or D | Reserve executor/TBE; service then apply under existing exclusivity |
| I, with a reservable cache block | Reserve block immediately; GETX; wait for data and all global ACKs |
| ILX or ILOX (exclusive local owner, no other local readers) | Pull owner data, remove owner only after response, then service |
| OLSX descended from D, below read-run limit | Invalidate local readers, wait for all ACKs, then service |
| ILO/ILOS/ILOSX, other shared states, transients, unavailable resources | NACK before acceptance; ordinary MOESI ownership path handles it |

This removes the previous path that pulled an owner and cleared its sharer
metadata without invalidating those readers. `DX_DRAIN` retains the data and
counts actual acknowledgments before clearing readers. Late writeback requests
after revocation use the base M-style writeback-NACK behavior in D too.

An I miss reserves its destination before acquisition, avoiding an accepted
operation with no cache slot when data returns. Forwarded requests and
replacement are recycled while an accepted operation acquires/services the
line. Stable D uses base M paths for ownership transfer, dirty eviction and
forwarded requests. Reusing these paths is a design choice, not evidence that
every race is validated. Full-system support is explicitly not enabled.

### What persistence changes, distinct from placement

M already accepts remote operations when delegation is enabled. D is not needed
to execute a remote add. Its distinct policy is on a **local read**: instead of
giving a core exclusive ownership, retain the authoritative L2 copy, supply
shared data, and record the readers in OLSX. An eligible later delegated update
invalidates those readers locally and returns to D, without a fresh global GETX.
This needs measurement against remote execution with persistence disabled.

Promotion requires a saturating update threshold and evidence of two distinct
requesters in the current cache residency. Metadata uses a first-requester ID,
seen/multiple-requester bits, counters, and a retention bit, not an N-core bitmap.
It measures L2-observed requests, not all CPU reads or contention. A read-run
threshold refuses retained-owner reacquisition; it is not a global popularity
predictor. Cache eviction/transfer naturally discards residency metadata.

## 4. Finite resources and timing

The bank executor admits up to `DSTATE_QUEUE_DEPTH` accepted transactions per L2
bank (default 1) -- at most one per line, since the TBE is per address -- and
spaces successive starts by `DSTATE_INIT_INTERVAL` cycles (default: equal to the
service latency, i.e. non-pipelined). Reservation covers acquisition,
invalidation and arithmetic service, so the engine cannot admit an unbounded
backlog. With `DSTATE_BUSY_STALL=1`, a request for a line that is already
acquiring or executing, or that sits in one of the 53 transient states where the
base protocol itself parks an `L1_GETX` with `stall_and_wait`, waits in the finite
input buffer and is woken by the same `wakeUpAllBuffers(address)` calls instead
of being NACKed into the ordinary GETX path; by default it is rejected as before.
The stall therefore inherits the base protocol's liveness argument rather than
adding a new one. The measured reason for the knob: under read-interleaving,
3,623 of 3,625 rejections came from `IFLOXX` alone and 2 of 8,000 updates were
delegated. Depths above 1 and intervals below the latency are sensitivity points
standing in for a pipelined datapath, not a claimed design.
Existing L1/L2 TBEs and all configured endpoint/trigger queues
are finite. Default capacity is 16 TBEs/controller and 32 messages/buffer.
SLICC-generated transition resource checks reserve required enqueue capacity
before actions execute. Response backpressure therefore stalls the apply+ACK
transition as a unit. [gem5 SLICC documentation](https://www.gem5.org/documentation/general_docs/ruby/slicc/).

After exclusive acquisition, an explicit timer charges 42 L2-controller cycles
by default before the mutation. This is a conservative **assumption** resembling
20-cycle read + 2-cycle arithmetic + 20-cycle write, not a measured SRAM/ALU
pipeline. Ordinary L2 accesses still do not contend with this helper for a
physically modeled cache-array port. Sweep service latency and bank count; do
not call this a complete contention, power or area model.

Dedicated network size classes charge 16 bytes for a 32-bit update, 24 bytes for
a 64-bit update and 8 bytes for a terminal ACK/NACK. The budget is derived in
`RubySlicc_Exports.sm`: a 63-bit header (41-bit line address, 5-bit word offset,
size, opcode, 6-bit requester, 8-bit tag) plus the operand, and a response that
carries only tag, requester and status because the requester keys on the tag. A
32-bit update therefore fits one 16-byte Garnet flit. The earlier 32/24-byte
classes and the original 8-byte control category are both sensitivity points;
none of the three is a measured physical framing.
One conceptual 32-byte request budget is address(8), ID(8), operand(8),
requester/route metadata(2), opcode(1), offset(2), width(1), flags(2).
ACK/NACK can omit operand/operation fields. These are modeled budgets, not
serialized wire formats or proof of a physical packet implementation. Garnet
flit counts depend on configured link width and routing. The simulator still
stores operands inside DataBlocks; their host memory footprint is not the
modeled traffic cost.

Three further bank-side mechanisms are modeled, each off by default and each
counted in Ruby statistics. A **hot-word buffer** (`DSTATE_HOTWORDS` entries per
bank, LRU, dropped when the line leaves the bank) charges `DSTATE_HIT_LATENCY`
instead of the full service latency when the target word was updated recently.
**Combining** (`DSTATE_MERGE_LIMIT`): while an update executes, later accepted
requests for the same word and width add their operands to it instead of waiting;
when the combined operation applies, the bank sends one terminal ACK per
requester from a trigger loop (`D_ACKING_M`/`D_ACKING_D`).
Each logical update still completes only after the value that includes it has
been written, so the completion contract above is unchanged. **Delegate from S**
(`DSTATE_DELEGATE_FROM_S`): an L1 that holds the line read-only sends the update
to the home instead of upgrading. It keeps serving forwarded reads and
acknowledging invalidations for its copy (`D_REQ_S`, `D_RETRY_S`) until the ACK
arrives or the fallback starts, because the L2 still lists it as a sharer.

Two requester/bank mechanisms address limits that the round-2 measurements
exposed, both default-off. **Requester-side combining** (`DSTATE_REQ_COMBINE`):
gem5's Sequencer queues every request to a line behind the one already
outstanding, so a core never has more than one delegated add per line in flight
whatever the ordering mode allows. When that request completes, the Sequencer
now issues the consecutive run of queued no-return adds to the *same word* as one
request whose operand is their modular sum; all of them complete on its ACK.
Only a consecutive run of identical type, address and width is combined, so the
order of every other queued request is unchanged, and the request is still one
operand of the original width (one 16 B or 24 B message). If the L1 applies the
request locally instead, it executes only the front request's own functor and
re-issues the rest, so nothing is applied twice or lost; the fallback path
applies the saved summed operand once and completes the whole group. This is the
delegated-path counterpart of the batching an owning core gets for free.
**Back-pressure** (`DSTATE_QUEUE_STALL`): a request that finds the bank executor
at capacity is parked in the finite input buffer with `stall_and_wait` and
re-analysed when any executor slot is released, instead of being NACKed into a
full ownership migration. Accepted operations complete through the timer and
the response network, never through the request buffer, so the stall adds no
new dependency to the liveness argument.

Separate virtual networks and priority for completion/responses help preserve
progress but do **not** prove deadlock freedom with finite queues. Required
remaining work includes dependency graphs, adversarial interleavings and
bounded model checking, followed by randomized CPU-generated atomic traffic.

## 5. Open implementation obligations

Do not bypass these obligations by calling the design a small ALU addition:

- Model arbitration with real cache ports, MSHRs, ECC read/modify/write and
  coherence directory capacity, plus area and clock closure.
- Define fairness/age bounds and QoS so a hot line or adversarial tenant cannot
  monopolize a bank. The present engine falls back on pressure; it is not a
  production QoS mechanism.
- Prove ordering and precise faults, migration/TLB interactions, interrupt and
  drain behavior, accepted-request recovery and all ownership races.
- Specify device coherence, CPU atomics mixed with DMA, persistent memory,
  cache-maintenance instructions and multi-socket protocol boundaries.
- Measure useful application work, including reads/queries, versus modern
  software sharding/combining and the same-ISA placement/persistence controls.

Home serialization can be slower than local execution, and on a single
contended line it is: an owning core applies successive adds at L1 speed, which
neither the hot-word buffer nor combining matches in the measured configurations
([results](../results/ABLATION_2026-09-30.md)). The benefit measured so far comes
from many distinct hot lines, where the conventional path migrates every line.
