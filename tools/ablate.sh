#!/bin/bash
# Ablation driver for the corrected MOESI-D build. Phases:
#   gates   correctness: coherence_regression matrices (O3 x3, Minor x1; 5 CPUs) then ordering_litmus (2 CPUs)
#   perf    scatter (one hot line): six executor configurations x local/remote/persistent/forced-nack
#   rw      scatter_rw (read-interleaved): fenced/default vs unfenced/full knobs
#   multi   scatter_multi (512 and 2048 hot lines): where ownership no longer batches
#   final   read-interleaved at 20-cycle service; 2048-line point at reduced iteration count
#   report  one table row per (run, mode) from stats.txt
#
# One simulator binary serves every column; columns differ only by DSTATE_* knobs and by which
# guest binary (fenced or unfenced AADD) is run. Each matrix goes through tools/run_matrix.py,
# which records hashes/manifests and refuses to PASS a mode whose required delegated / local /
# reject / retry events are zero. Phases are grouped into waves of at most ~4 concurrent 4-5 core
# Garnet runs (~2 GB each) so a 19 GB host is not oversubscribed.
#
# Required:  GEM5_TREE  patched gem5 v25.1.0.1 worktree containing build/X86_MOESI_D/gem5.opt
#            OUT        results directory (created; existing run names are skipped, never overwritten)
# Optional:  MOESI_D_SRC (this repository; default: parent of this script), BENCH_DIR (where
#            bench/ binaries were built; default: MOESI_D_SRC), SCATTER_ARGS, RW_ARGS.
set -u
SRC=${MOESI_D_SRC:-$(cd "$(dirname "$0")/.." && pwd)}
# Never rebuild gem5.opt or edit this script while a chain is running: bash reads
# the script incrementally, and a gem5 process exec-ed while the binary is being
# written crashes. Archive affected runs and repeat them.
TREE=${GEM5_TREE:?set GEM5_TREE to the patched gem5 worktree}
W=${BENCH_DIR:-$SRC}
OUT=${OUT:?set OUT to a results directory}; mkdir -p "$OUT"
GEM5=$TREE/build/X86_MOESI_D/gem5.opt; SE=$TREE/configs/deprecated/example/se.py
SCATTER_ARGS=${SCATTER_ARGS:-"4 2000 32"}; RW_ARGS=${RW_ARGS:-"4 2000 32 8"}
COMMON="--ruby --num-l2caches=4 --cacheline_size=128 --l1d_size=32kB --l1i_size=32kB --l2_size=256kB --mem-size=1GB --network=garnet --topology=Crossbar"
KN_QUEUE="DSTATE_QUEUE_DEPTH=8"
KN_PIPE="DSTATE_QUEUE_DEPTH=8 DSTATE_INIT_INTERVAL=4"
KN_FULL="DSTATE_QUEUE_DEPTH=8 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1"

# run_matrix: local / remote / persistent / forced-nack for one (binary, engine-env)
matrix() { local name=$1 bin=$2 args=$3 cores=$4 cpu=$5; shift 5
  [ -d "$OUT/$name" ] && { echo "$name: exists, skipping"; return; }
  env "$@" python3 "$SRC/tools/run_matrix.py" --gem5-tree "$TREE" --binary "$W/bench/$bin" --workload-args "$args" \
      --out "$OUT/$name" --cores "$cores" --banks "${BANKS:-4}" --extra="${EXTRA:-}" ${REQROI:+--require-roi} ${GEM5BIN:+--gem5-binary "$GEM5BIN"} --cpu "$cpu" --repeats 1 --timeout "${TIMEOUT:-2400}" --modes "${MODES:-local,remote,persistent,forced-nack}" > "$OUT/$name.log" 2>&1
  printf "%-34s %s/%s modes pass  %s\n" "$name" "$(grep -cE ' PASS$' "$OUT/$name.log")" "$(echo "${MODES:-local,remote,persistent,forced-nack}" | tr ',' '\n' | wc -l)" "$(grep -E ' FAIL' "$OUT/$name.log" | tr '\n' ' ')"; }
# direct gem5 run for the ordering litmus (prints PASS/FAIL, not CORRECT)
litmus() { local name=$1 bin=$2; shift 2
  [ -d "$OUT/$name" ] && { echo "$name: exists, skipping"; return; }
  env "$@" "$GEM5" --outdir="$OUT/$name" "$SE" $COMMON --cpu-type="${LITMUS_CPU:-X86O3CPU}" --num-cpus=2 -c "$W/bench/$bin" > "$OUT/$name.log" 2>&1
  printf "%-34s %s\n" "$name" "$(grep -E 'variant=|^PASS|^FAIL|LOST|panic:|fatal:' "$OUT/$name.log" | tr '\n' '|' | cut -c1-140)"; }

case ${1:-help} in
gates)
  matrix G1_regress_fenced_default   coherence_regression_fenced "" 5 X86O3CPU    &
  matrix G2_regress_unfenced_default coherence_regression        "" 5 X86O3CPU    &
  matrix G3_regress_unfenced_knobs   coherence_regression        "" 5 X86O3CPU $KN_FULL &
  matrix G4_regress_fenced_minor     coherence_regression_fenced "" 5 X86MinorCPU &
  wait   # wave 1: four 5-core matrices; wave 2: five 2-core litmus runs
  litmus L1_litmus_unfenced_default  ordering_litmus         DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 &
  litmus L2_litmus_fenced_default    ordering_litmus_fenced  DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 &
  litmus L3_litmus_lockadd_stock     ordering_litmus_lockadd DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 &
  litmus L4_litmus_unfenced_knobs    ordering_litmus         DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_FULL &
  litmus L5_litmus_unfenced_local    ordering_litmus         DSTATE_ENABLED=0 DSTATE_PERSISTENCE=0 &
  wait ;;
perf)
  matrix P0_scatter_fenced_default    scatter_dstate_fenced "$SCATTER_ARGS" 4 X86O3CPU &
  matrix P2_scatter_unfenced_default  scatter_dstate        "$SCATTER_ARGS" 4 X86O3CPU &
  matrix P3_scatter_unfenced_queue8   scatter_dstate        "$SCATTER_ARGS" 4 X86O3CPU $KN_QUEUE &
  matrix P4_scatter_unfenced_pipe4    scatter_dstate        "$SCATTER_ARGS" 4 X86O3CPU $KN_PIPE &
  matrix P5_scatter_unfenced_full     scatter_dstate        "$SCATTER_ARGS" 4 X86O3CPU $KN_FULL &
  matrix P6_scatter_unfenced_full_l20 scatter_dstate        "$SCATTER_ARGS" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
  wait ;;
rw)
  matrix R0_rw_fenced_default  scatter_rw_fenced "$RW_ARGS" 4 X86O3CPU &
  matrix R5_rw_unfenced_full   scatter_rw        "$RW_ARGS" 4 X86O3CPU $KN_FULL &
  wait ;;
multi)
  # Many hot lines on distinct 128 B lines: ownership no longer batches. 4 x 2000 x L adds.
  for L in 512 2048; do
    matrix M0_multi${L}_fenced_default    scatter_multi "4 2000 $L" 4 X86O3CPU &
    matrix M2_multi${L}_unfenced_default  scatter_multi "4 2000 $L" 4 X86O3CPU &
    matrix M5_multi${L}_unfenced_full     scatter_multi "4 2000 $L" 4 X86O3CPU $KN_FULL &
    matrix M6_multi${L}_unfenced_full_l20 scatter_multi "4 2000 $L" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    wait   # one wave per line count
  done ;;
final)
  matrix R6_rw_unfenced_full_l20        scatter_rw    "$RW_ARGS"    4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20
  matrix M2_multi2048_unfenced_default  scatter_multi "4 250 2048" 4 X86O3CPU &
  matrix M6_multi2048_unfenced_full_l20 scatter_multi "4 250 2048" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
  wait ;;
v2)
  # Round two: store-class micro-op (always on in this build), hot-word buffer,
  # combining, relaxed ordering, delegate-from-S. The defaults column isolates the
  # store-class change against the previous build's P2/R5/M2 rows.
  #   usage: ablate.sh v2 gates|scatter|rw|multi|hotkey|long
  KN_HOT="$KN_FULL DSTATE_HOTWORDS=8 DSTATE_HIT_LATENCY=4"
  KN_HOTM="$KN_HOT DSTATE_MERGE_LIMIT=8"
  KN_HOTMR="$KN_HOTM DSTATE_RELAXED_AMO=1"
  KN_HOTMS="$KN_HOTM DSTATE_DELEGATE_FROM_S=1"
  KN_HOT32="$KN_FULL DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4"   # buffer covers the 32-counter working set
  KN_HOT32M="$KN_HOT32 DSTATE_MERGE_LIMIT=8"
  KN_HOT32MR="$KN_HOT32M DSTATE_RELAXED_AMO=1"
  case ${2:-all} in
  gates)
    matrix G5_regress_hotmerge_o3      coherence_regression "" 5 X86O3CPU    $KN_HOTM  &
    matrix G6_regress_hotmerge_minor   coherence_regression "" 5 X86MinorCPU $KN_HOTM  &
    matrix G7_regress_fromS_o3         coherence_regression "" 5 X86O3CPU    $KN_HOTMS &
    matrix G8_regress_relaxed_o3       coherence_regression "" 5 X86O3CPU    $KN_HOTMR &
    wait
    # L7 is a contract check, not a pass/fail gate: unfenced adds under relaxed
    # ordering MAY be observed out of order; L8 (fenced) under the same knobs must pass.
    litmus L6_litmus_unfenced_hotmerge  ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_HOTM  &
    litmus L7_litmus_unfenced_RELAXED   ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_HOTMR &
    litmus L8_litmus_fenced_relaxed     ordering_litmus_fenced DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_HOTMR &
    litmus L9_litmus_unfenced_fromS     ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_HOTMS &
    wait ;;
  scatter)
    matrix V0_scatter_default          scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU &
    matrix V1_scatter_full_l20         scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix V2_scatter_hot              scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU $KN_HOT &
    matrix V3_scatter_hotmerge         scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU $KN_HOTM &
    matrix V4_scatter_hotmerge_relaxed scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU $KN_HOTMR &
    wait ;;
  rw)
    matrix V0_rw_default          scatter_rw "$RW_ARGS" 4 X86O3CPU &
    matrix V1_rw_full_l20         scatter_rw "$RW_ARGS" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix V3_rw_hotmerge         scatter_rw "$RW_ARGS" 4 X86O3CPU $KN_HOTM &
    matrix V4_rw_hotmerge_relaxed scatter_rw "$RW_ARGS" 4 X86O3CPU $KN_HOTMR &
    matrix V5_rw_hotmerge_fromS   scatter_rw "$RW_ARGS" 4 X86O3CPU $KN_HOTMS &
    wait ;;
  multi)
    matrix V0_multi512_default          scatter_multi "4 2000 512" 4 X86O3CPU &
    matrix V1_multi512_full_l20         scatter_multi "4 2000 512" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix V2_multi512_hot              scatter_multi "4 2000 512" 4 X86O3CPU $KN_HOT &
    matrix V3_multi512_hotmerge         scatter_multi "4 2000 512" 4 X86O3CPU $KN_HOTM &
    matrix V4_multi512_hotmerge_relaxed scatter_multi "4 2000 512" 4 X86O3CPU $KN_HOTMR &
    wait ;;
  hotkey)
    # One counter hammered by all four cores: same-word contention, the pattern combining
    # exists for (the CMS/telemetry hot-key case). 8,000 adds: start-up dominated, so these
    # rows are correctness/coverage evidence; `long` has the timed versions.
    matrix H0_hotkey_default             scatter_dstate "4 2000 1" 4 X86O3CPU &
    matrix H1_hotkey_full_l20            scatter_dstate "4 2000 1" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix H2_hotkey_hot32               scatter_dstate "4 2000 1" 4 X86O3CPU $KN_HOT32 &
    matrix H3_hotkey_hot32_merge         scatter_dstate "4 2000 1" 4 X86O3CPU $KN_HOT32M &
    matrix H4_hotkey_hot32_merge_relaxed scatter_dstate "4 2000 1" 4 X86O3CPU $KN_HOT32MR &
    matrix H5_scatter32_hot32_merge      scatter_dstate "$SCATTER_ARGS" 4 X86O3CPU $KN_HOT32M &
    wait ;;
  long)
    # The single-line workloads at 100x the adds (800k), so the ~150 us process start-up no
    # longer dominates simSeconds. Three 4-core matrices per wave.
    matrix K0_hotkey800k_default              scatter_dstate "4 200000 1"    4 X86O3CPU &
    matrix K1_hotkey800k_full_l20             scatter_dstate "4 200000 1"    4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix K2_hotkey800k_hot32_merge          scatter_dstate "4 200000 1"    4 X86O3CPU $KN_HOT32M &
    wait
    matrix K3_hotkey800k_hot32_merge_relaxed  scatter_dstate "4 200000 1"    4 X86O3CPU $KN_HOT32MR &
    matrix K4_scatter800k_full_l20            scatter_dstate "4 200000 32"   4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix K5_scatter800k_hot32_merge_relaxed scatter_dstate "4 200000 32"   4 X86O3CPU $KN_HOT32MR &
    wait
    matrix K6_rw800k_full_l20                 scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_FULL DSTATE_EXEC_LATENCY=20 &
    matrix K7_rw800k_hot32_merge              scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_HOT32M &
    matrix K8_rw800k_hot32_merge_relaxed      scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_HOT32MR &
    wait ;;
  esac ;;
v3)
  # Round three: requester-side combining (DSTATE_REQ_COMBINE: queued same-word adds
  # from one core issued as one summed request) and bank back-pressure
  # (DSTATE_QUEUE_STALL: wait for an executor slot instead of NACKing). Both are
  # CPU/bank mechanisms on top of the round-2 knobs. Columns:
  #   T  = TSO,      20-cycle service, no hot-word buffer   (round-2 best TSO + new knobs)
  #   TH = TSO,      42-cycle service, 32 hot words          (single-line TSO comparison)
  #   R  = relaxed,  42/4-cycle service, 32 hot words        (round-2 best relaxed + new knobs)
  #   usage: ablate.sh v3 gates|perf|ws|scale|phased
  KN_V3="DSTATE_QUEUE_DEPTH=16 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1 DSTATE_REQ_COMBINE=16 DSTATE_MERGE_LIMIT=64"
  KN_V3T="$KN_V3 DSTATE_EXEC_LATENCY=20"
  KN_V3TH="$KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4"
  KN_V3R="$KN_V3TH DSTATE_RELAXED_AMO=1"
  PERF3="local,remote,persistent"
  KN_HOT="$KN_FULL DSTATE_HOTWORDS=8 DSTATE_HIT_LATENCY=4"; KN_HOTM="$KN_HOT DSTATE_MERGE_LIMIT=8"; KN_HOTMR="$KN_HOTM DSTATE_RELAXED_AMO=1"
  case ${2:-all} in
  gates)
    # Relaxed column: the FENCED regression is the gate. The unfenced one (G9u) is a
    # contract check only: its barrier/release stores may overtake an in-flight add
    # under relaxed ordering, and requester combining widens that window.
    matrix G9_regress_v3R_fenced_o3   coherence_regression_fenced "" 5 X86O3CPU $KN_V3R &
    matrix G9u_regress_v3R_unfenced_o3 coherence_regression       "" 5 X86O3CPU $KN_V3R &
    matrix G10_regress_v3T_minor  coherence_regression "" 5 X86MinorCPU $KN_V3TH &
    matrix G11_regress_v3T_o3     coherence_regression "" 5 X86O3CPU    $KN_V3T  &
    wait
    # L10/L13 (TSO, unfenced) must pass: combining may not reorder a flag store past the adds.
    # L11 (fenced, relaxed) must pass. L12 is the relaxed contract check (may reorder).
    litmus L10_litmus_unfenced_v3T        ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_V3T &
    litmus L11_litmus_fenced_v3R          ordering_litmus_fenced DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_V3R &
    litmus L12_litmus_unfenced_v3R_RELAXED ordering_litmus       DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_V3R &
    LITMUS_CPU=X86MinorCPU litmus L13_litmus_unfenced_v3T_minor ordering_litmus DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_V3TH &
    wait ;;
  perf)
    # 512 lines: isolate back-pressure (W1) and combining (W2) on the round-2 relaxed
    # best, then everything (W3); W0 is the TSO column.
    matrix W0_multi512_v3T               scatter_multi "4 2000 512" 4 X86O3CPU $KN_V3T &
    matrix W1_multi512_hotmerge_relaxed_qstall scatter_multi "4 2000 512" 4 X86O3CPU $KN_HOTMR DSTATE_QUEUE_STALL=1 &
    matrix W2_multi512_hotmerge_relaxed_combine scatter_multi "4 2000 512" 4 X86O3CPU $KN_HOTMR DSTATE_REQ_COMBINE=16 DSTATE_QUEUE_DEPTH=16 &
    matrix W3_multi512_v3R               scatter_multi "4 2000 512" 4 X86O3CPU $KN_V3R &
    wait
    # single line, 800k adds: the regime requester-side combining is for
    matrix X0_hotkey800k_v3TH            scatter_dstate "4 200000 1"    4 X86O3CPU $KN_V3TH &
    matrix X1_hotkey800k_v3R             scatter_dstate "4 200000 1"    4 X86O3CPU $KN_V3R &
    matrix X2_scatter800k_v3R            scatter_dstate "4 200000 32"   4 X86O3CPU $KN_V3R &
    matrix X3_rw800k_v3T                 scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_V3T &
    wait
    matrix X4_rw800k_v3R                 scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_V3R &
    matrix X5_hotkey800k_v3R_combine64   scatter_dstate "4 200000 1"    4 X86O3CPU $KN_V3R DSTATE_REQ_COMBINE=64 &
    wait ;;
  ws)
    # working set at 4 cores, op count held at 4.096M
    MODES=$PERF3 matrix Y0_multi2048_v3T  scatter_multi "4 500 2048" 4 X86O3CPU $KN_V3T &
    MODES=$PERF3 matrix Y1_multi2048_v3R  scatter_multi "4 500 2048" 4 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix Y2_multi8192_v3T  scatter_multi "4 125 8192" 4 X86O3CPU $KN_V3T &
    MODES=$PERF3 matrix Y3_multi8192_v3R  scatter_multi "4 125 8192" 4 X86O3CPU $KN_V3R &
    wait ;;
  scale)
    # 8 and 16 cores, 4 banks; adds held constant (512 lines: 4.096M; single line: 800k)
    export TIMEOUT=7200
    MODES=$PERF3 matrix Z0_multi512_8c_v3T   scatter_multi  "8 1000 512"   8 X86O3CPU $KN_V3T &
    MODES=$PERF3 matrix Z1_multi512_8c_v3R   scatter_multi  "8 1000 512"   8 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix Z2_hotkey800k_8c_v3R scatter_dstate "8 100000 1"   8 X86O3CPU $KN_V3R &
    wait
    MODES=$PERF3 matrix Z3_hotkey800k_8c_v3TH scatter_dstate "8 100000 1"  8 X86O3CPU $KN_V3TH &
    MODES=$PERF3 matrix Z4_rw800k_8c_v3T     scatter_rw     "8 100000 32 8" 8 X86O3CPU $KN_V3T &
    MODES=$PERF3 matrix Z5_rw800k_8c_v3R     scatter_rw     "8 100000 32 8" 8 X86O3CPU $KN_V3R &
    wait
    # 16 cores on 4 banks: with the default 32-entry endpoint buffers the BASE protocol
    # (conventional mode, no delegation) trips Garnet's deadlock detector on the request
    # vnet under TSO. Every 16-core row therefore uses 256-entry buffers (BUF16), in all
    # modes alike; see docs/VALIDATION.md.
    BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
    MODES=$PERF3 matrix Z6_multi512_16c_v3T   scatter_multi  "16 500 512"   16 X86O3CPU $KN_V3T $BUF16 &
    MODES=$PERF3 matrix Z7_multi512_16c_v3R   scatter_multi  "16 500 512"   16 X86O3CPU $KN_V3R $BUF16 &
    wait
    MODES=$PERF3 matrix Z8_hotkey800k_16c_v3R scatter_dstate "16 50000 1"   16 X86O3CPU $KN_V3R $BUF16 &
    MODES=$PERF3 matrix Z9_rw800k_16c_v3R     scatter_rw     "16 50000 32 8" 16 X86O3CPU $KN_V3R $BUF16 &
    wait
    MODES=$PERF3 matrix Z10_phased512_16c_v3T scatter_phased "16 25 512 8"  16 X86O3CPU $KN_V3T $BUF16 &
    MODES=$PERF3 matrix Z11_phased512_16c_v3R scatter_phased "16 25 512 8"  16 X86O3CPU $KN_V3R $BUF16 &
    wait ;;
  phased)
    # update phase over N lines, then every thread reads every line; repeat. The
    # read phases are what the D retention policy is for: persistent vs remote.
    MODES=$PERF3 matrix P0_phased512_v3T    scatter_phased "4 50 512 8"   4 X86O3CPU $KN_V3T &
    MODES=$PERF3 matrix P1_phased512_v3R    scatter_phased "4 50 512 8"   4 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix P2_phased32_v3R     scatter_phased "4 5000 32 8"  4 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix P3_phased512_default scatter_phased "4 50 512 8"  4 X86O3CPU &
    wait
    # P4/P5 ran 16 cores with the default 32-entry buffers: P5's conventional and remote
    # modes hit the Garnet deadlock detector (see VALIDATION.md). The 16-core phased rows
    # that count are Z10/Z11 in `scale`, which use 256-entry buffers in every mode.
    export TIMEOUT=7200
    MODES=$PERF3 matrix P4_phased512_16c_v3R scatter_phased "16 25 512 8" 16 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix P5_phased512_16c_v3T scatter_phased "16 25 512 8" 16 X86O3CPU $KN_V3T &
    wait ;;
  esac ;;
v4)
  # Round four, one campaign on one build. New mechanisms (all default-off):
  #   DSTATE_FAR_READS      a load of a line held in D gets a snapshot and no sharer is recorded
  #   DSTATE_DELTA_MIN_WORDS queued adds to >= N different words of a line go as one delta-line request
  #   DSTATE_RANGE_LO/HI_MB  static per-address placement (oracle runs on scatter_mixed)
  #   DSTATE_MAX_OUTSTANDING per-core Ruby request limit (sweep at 16 cores)
  # Columns: T/TH/R as in v3 (round-3 knobs, for isolation), T4/TH4/R4 = same + far reads
  # (+ delta line in R4). Read-downgrade raised to 64 in the far-read columns so a read run
  # between update bursts does not hand out cached copies.
  #   usage: ablate.sh v4 gates|perf|apps|scale|mixed|regate|all
  KN_V3="DSTATE_QUEUE_DEPTH=16 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1 DSTATE_REQ_COMBINE=16 DSTATE_MERGE_LIMIT=64"
  KN_V3T="$KN_V3 DSTATE_EXEC_LATENCY=20"
  KN_V3TH="$KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4"
  KN_V3R="$KN_V3TH DSTATE_RELAXED_AMO=1"
  FAR="DSTATE_FAR_READS=1 DSTATE_READ_DOWNGRADE=64"
  KN_T4="$KN_V3T $FAR"
  KN_TH4="$KN_V3TH $FAR"
  KN_R4="$KN_V3R $FAR DSTATE_DELTA_MIN_WORDS=2"
  ORACLE_A="DSTATE_RANGE_LO_MB=512 DSTATE_RANGE_HI_MB=768"   # scatter_mixed: delegate the shared region only
  PERF3="local,remote,persistent"
  MTX=/home/ubuntu/moesi-d-work/bench/integer_fixture.mtx
  case ${2:-all} in
  gates)
    matrix G18_regress_R4_fenced_o3    coherence_regression_fenced "" 5 X86O3CPU    $KN_R4  &
    matrix G19_regress_TH4_minor       coherence_regression        "" 5 X86MinorCPU $KN_TH4 &
    matrix G20_regress_T4_o3           coherence_regression        "" 5 X86O3CPU    $KN_T4  &
    matrix G21_regress_R4_unfenced_o3  coherence_regression        "" 5 X86O3CPU    $KN_R4  &
    wait
    # far reads sit on the litmus reader's load of x; TSO rows must pass, L24 is the relaxed contract check
    litmus L20_litmus_unfenced_T4         ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 &
    litmus L21_litmus_fenced_R4           ordering_litmus_fenced DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_R4 &
    LITMUS_CPU=X86MinorCPU litmus L22_litmus_unfenced_TH4_minor ordering_litmus DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 &
    litmus L23_litmus_lockadd_T4          ordering_litmus_lockadd DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 &
    litmus L24_litmus_unfenced_R4_RELAXED ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_R4 &
    wait ;;
  perf)
    # read-interleaved single line: far reads are aimed here (v3 columns for isolation)
    matrix Q0_rw800k_T4                scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_T4 &
    matrix Q1_rw800k_R4                scatter_rw     "4 200000 32 8" 4 X86O3CPU $KN_R4 &
    matrix Q2_scatter800k_R4           scatter_dstate "4 200000 32"   4 X86O3CPU $KN_R4 &
    matrix Q3_hotkey800k_R4            scatter_dstate "4 200000 1"    4 X86O3CPU $KN_R4 &
    wait
    matrix Q4_multi512_T4              scatter_multi  "4 2000 512"    4 X86O3CPU $KN_T4 &
    matrix Q5_multi512_R4              scatter_multi  "4 2000 512"    4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix Q6_phased512_T4 scatter_phased "4 50 512 8"   4 X86O3CPU $KN_T4 &
    MODES=$PERF3 matrix Q7_phased512_R4 scatter_phased "4 50 512 8"   4 X86O3CPU $KN_R4 &
    wait
    MODES=$PERF3 matrix Q13_hotkey800k_8c_R4 scatter_dstate "8 100000 1"   8 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix Q14_rw800k_8c_T4     scatter_rw     "8 100000 32 8" 8 X86O3CPU $KN_T4 &
    wait ;;
  mixed)
    # Mixed regime: region A write-only shared lines, region B read-mostly shared lines
    # (every thread reads all of B each iteration, one add per 8 iterations). The
    # oracle delegates A only (virtual-address range, decided in the Sequencer); the
    # dynamic policy may delegate anything and relies on far reads + read-downgrade.
    # (The earlier Q8-Q12 runs used a first version of the program whose private
    # lines never left their owner's L1, and a physical-address range that matched
    # nothing; they are superseded by these.)
    MODES=$PERF3 matrix M0_mixed_R4_all      scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix M1_mixed_R4_oracleA  scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_R4 $ORACLE_A &
    MODES=$PERF3 matrix M2_mixed_T4_all      scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_T4 &
    MODES=$PERF3 matrix M3_mixed_T4_oracleA  scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_T4 $ORACLE_A &
    wait
    MODES=$PERF3 matrix M4_mixed_R3_all      scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_V3R &
    MODES=$PERF3 matrix M5_mixed_R4_nofar    scatter_mixed "4 20000 64 32 8" 4 X86O3CPU $KN_V3R DSTATE_DELTA_MIN_WORDS=2 &
    wait
    export TIMEOUT=7200
    BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
    MODES=$PERF3 matrix M6_mixed_16c_R4_all     scatter_mixed "16 5000 64 32 8" 16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix M7_mixed_16c_R4_oracleA scatter_mixed "16 5000 64 32 8" 16 X86O3CPU $KN_R4 $BUF16 $ORACLE_A &
    wait ;;
  regate)
    # the static-placement change touched the L1 mandatory path: re-gate on the final build
    matrix G22_regress_R4_fenced_o3_final coherence_regression_fenced "" 5 X86O3CPU    $KN_R4  &
    matrix G23_regress_T4_o3_final        coherence_regression        "" 5 X86O3CPU    $KN_T4  &
    matrix G24_regress_TH4_minor_final    coherence_regression        "" 5 X86MinorCPU $KN_TH4 &
    wait
    litmus L25_litmus_unfenced_T4_final   ordering_litmus        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 &
    litmus L26_litmus_fenced_R4_final     ordering_litmus_fenced DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_R4 &
    wait ;;
  apps)
    # the v1.0.0 paper's programs on the corrected protocol (first time)
    MODES=$PERF3 matrix A0_cms_R4       cms_dstate    "4 20000"       4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix A1_cms_T4       cms_dstate    "4 20000"       4 X86O3CPU $KN_T4 &
    MODES=$PERF3 matrix A2_zipf_R4      litmus_dstate "4 4000 2048"   4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix A3_zipf_T4      litmus_dstate "4 4000 2048"   4 X86O3CPU $KN_T4 &
    wait
    for col in R4 T4; do
      kn=$KN_R4; [ $col = T4 ] && kn=$KN_T4
      name=A4_spmv_$col; [ $col = T4 ] && name=A5_spmv_$col
      [ -d "$OUT/$name" ] || env $kn python3 "$SRC/tools/run_matrix.py" --gem5-tree "$TREE" --binary "$W/bench/spmv_dstate" \
          --workload-args "4 20 $MTX" --input "$MTX" --out "$OUT/$name" --cores 4 --banks 4 --cpu X86O3CPU --repeats 1 \
          --timeout 2400 --modes $PERF3 > "$OUT/$name.log" 2>&1 &
    done
    wait
    for n in A4_spmv_R4 A5_spmv_T4; do printf "%-34s %s/3 modes pass\n" $n "$(grep -cE ' PASS$' "$OUT/$n.log")"; done ;;
  tune)
    # Far-read transitions at low downgrade thresholds (correctness), the promotion
    # threshold on the read-interleaved line under TSO (far reads did not engage there:
    # the line rarely reaches D), and a 4x4 mesh instead of the crossbar at 16 cores.
    matrix F0_regress_T4_down1_o3        coherence_regression        "" 5 X86O3CPU    $KN_T4  DSTATE_READ_DOWNGRADE=1 &
    matrix F1_regress_R4_fenced_down2_o3 coherence_regression_fenced "" 5 X86O3CPU    $KN_R4  DSTATE_READ_DOWNGRADE=2 &
    matrix F2_regress_TH4_down1_minor    coherence_regression        "" 5 X86MinorCPU $KN_TH4 DSTATE_READ_DOWNGRADE=1 &
    MODES=$PERF3 matrix F3_rw800k_T4_thr1 scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_T4 DSTATE_THRESHOLD=1 &
    wait
    MODES=$PERF3 matrix F4_rw800k_T4_thr2 scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_T4 DSTATE_THRESHOLD=2 &
    MODES=$PERF3 matrix F5_rw800k_R4_thr1 scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_R4 DSTATE_THRESHOLD=1 &
    wait
    export TIMEOUT=7200
    BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
    # Mesh_XY needs the controller count to be a multiple of the router count (16):
    # 16 L1 + 8 L2 + 8 directories. The crossbar twin with the same counts is the control.
    MODES=$PERF3 BANKS=8 EXTRA="--num-dirs=8 --topology=Mesh_XY --mesh-rows=4" matrix F6b_multi512_16c_R4_mesh8 scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 BANKS=8 EXTRA="--num-dirs=8" matrix F6c_multi512_16c_R4_xbar8d scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix F7_rw800k_16c_T4_thr1 scatter_rw "16 50000 32 8" 16 X86O3CPU $KN_T4 $BUF16 DSTATE_THRESHOLD=1 &
    wait ;;
  apps2)
    # CMS again (its conventional relaxed run hit gem5's missing Garnet functional
    # read, fixed in the patch), the Zipfian program at 25x the adds, and SpMV on a
    # synthetic 4096x4096 / 65,536-nnz integer matrix (bench/gen_integer_matrix.py
    # 4096 65536 1); SpMV partitions rows per thread, so it is the single-writer
    # negative control.
    SYN=$W/bench/synth_4096_65536_s1.mtx
    [ -f "$SYN" ] || python3 "$SRC/bench/gen_integer_matrix.py" 4096 65536 1 > "$SYN"
    MODES=$PERF3 matrix A6_cms_R4_fr       cms_dstate    "4 20000"        4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix A7_zipf100k_R4     litmus_dstate "4 100000 2048"  4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix A8_zipf100k_T4     litmus_dstate "4 100000 2048"  4 X86O3CPU $KN_T4 &
    wait
    for col in R4 T4; do
      kn=$KN_R4; [ $col = T4 ] && kn=$KN_T4
      name=A9_spmv_synth_$col; [ $col = T4 ] && name=A10_spmv_synth_$col
      [ -d "$OUT/$name" ] || env $kn python3 "$SRC/tools/run_matrix.py" --gem5-tree "$TREE" --binary "$W/bench/spmv_dstate" \
          --workload-args "4 50 $SYN" --input "$SYN" --out "$OUT/$name" --cores 4 --banks 4 --cpu X86O3CPU --repeats 1 \
          --timeout 2400 --modes $PERF3 > "$OUT/$name.log" 2>&1 &
    done
    wait
    for n in A9_spmv_synth_R4 A10_spmv_synth_T4; do printf "%-34s %s/3 modes pass\n" $n "$(grep -cE ' PASS$' "$OUT/$n.log")"; done ;;
  scale)
    export TIMEOUT=7200
    BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
    # is the 16-core relaxed ceiling the per-core request limit or the banks?
    MODES=$PERF3 matrix S0_multi512_16c_R4            scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix S1_multi512_16c_R4_out32      scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 DSTATE_MAX_OUTSTANDING=32 &
    wait
    MODES=$PERF3 matrix S2_multi512_16c_R4_out64      scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 DSTATE_MAX_OUTSTANDING=64 &
    MODES=$PERF3 BANKS=8 matrix S3_multi512_16c_R4_8banks scatter_multi "16 500 512" 16 X86O3CPU $KN_R4 $BUF16 &
    wait
    MODES=$PERF3 matrix S4_hotkey800k_16c_TH4         scatter_dstate "16 50000 1"    16 X86O3CPU $KN_TH4 $BUF16 &
    MODES=$PERF3 matrix S5_rw800k_16c_R4              scatter_rw     "16 50000 32 8" 16 X86O3CPU $KN_R4  $BUF16 &
    wait
    MODES=$PERF3 matrix S6_rw800k_16c_T4              scatter_rw     "16 50000 32 8" 16 X86O3CPU $KN_T4  $BUF16 &
    MODES=$PERF3 matrix S7_phased512_16c_R4           scatter_phased "16 25 512 8"   16 X86O3CPU $KN_R4  $BUF16 &
    wait
    MODES=$PERF3 matrix S8_phased512_16c_T4           scatter_phased "16 25 512 8"   16 X86O3CPU $KN_T4  $BUF16 &
    MODES=$PERF3 matrix S11_hotkey800k_16c_R4         scatter_dstate "16 50000 1"    16 X86O3CPU $KN_R4  $BUF16 &
    wait ;;
  all)
    for ph in gates perf apps scale mixed regate; do echo "--- $ph $(date +%H:%M)"; bash "$0" v4 $ph; done ;;
  esac ;;
v5)
  # Round five: stress seeds, promotion-policy grid, applications at 16 cores, the
  # graph kernel, ROI-timed reruns, 32/64-core meshes, debug-binary gates.
  #   usage: ablate.sh v5 stress|policy|apps3|roi|mesh|debug|evict
  KN_V3="DSTATE_QUEUE_DEPTH=16 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1 DSTATE_REQ_COMBINE=16 DSTATE_MERGE_LIMIT=64"
  FAR="DSTATE_FAR_READS=1 DSTATE_READ_DOWNGRADE=64"
  KN_T4="$KN_V3 DSTATE_EXEC_LATENCY=20 $FAR"
  KN_TH4="$KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4 $FAR"
  KN_R4="$KN_TH4 DSTATE_RELAXED_AMO=1 DSTATE_DELTA_MIN_WORDS=2"
  PERF3="local,remote,persistent"
  BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
  case ${2:-all} in
  stress)
    # 4096 lines = 2x the L2: retained lines are evicted under the random traffic
    for seed in 1 2 3; do
      matrix N${seed}_stress_R4_o3     random_stress "4 30000 4096 $seed" 4 X86O3CPU    $KN_R4  &
      matrix N${seed}_stress_T4_o3     random_stress "4 30000 4096 $seed" 4 X86O3CPU    $KN_T4  &
      matrix N${seed}_stress_TH4_minor random_stress "4 30000 4096 $seed" 4 X86MinorCPU $KN_TH4 &
      wait
    done
    matrix N4_stress_R4_evict_o3       random_stress "4 30000 8192 4"     4 X86O3CPU    $KN_R4 DSTATE_EVICT_FOR_DELEGATE=1 &
    matrix N5_stress_T4_evict_o3       random_stress "4 30000 8192 5"     4 X86O3CPU    $KN_T4 DSTATE_EVICT_FOR_DELEGATE=1 &
    matrix N6_stress_R4_16c            random_stress "16 10000 4096 6"    16 X86O3CPU   $KN_R4 $BUF16 &
    wait ;;
  policy)
    for thr in 1 2 8; do for dn in 8 64; do
      MODES=$PERF3 matrix P_phased512_R4_thr${thr}_dn${dn} scatter_phased "4 50 512 8" 4 X86O3CPU $KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4 DSTATE_RELAXED_AMO=1 DSTATE_DELTA_MIN_WORDS=2 DSTATE_FAR_READS=1 DSTATE_THRESHOLD=$thr DSTATE_READ_DOWNGRADE=$dn &
    done; wait; done ;;
  apps3)
    export TIMEOUT=7200
    MODES=$PERF3 matrix B0_graph_R4_4c_wide   graph_push "4 1000000 65536 1" 4 X86O3CPU $KN_R4 &
    MODES=$PERF3 matrix B1_graph_T4_4c_wide   graph_push "4 1000000 65536 1" 4 X86O3CPU $KN_T4 &
    MODES=$PERF3 matrix B2_graph_R4_4c_dense  graph_push "4 1000000 4096 1"  4 X86O3CPU $KN_R4 &
    wait
    MODES=$PERF3 matrix B3_cms_16c_R4         cms_dstate "16 5000"            16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix B4_cms_16c_T4         cms_dstate "16 5000"            16 X86O3CPU $KN_T4 $BUF16 &
    wait
    MODES=$PERF3 matrix B5_graph_R4_16c_wide  graph_push "16 2000000 65536 1" 16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix B6_graph_T4_16c_wide  graph_push "16 2000000 65536 1" 16 X86O3CPU $KN_T4 $BUF16 &
    wait ;;
  roi)
    # ROI-instrumented guests (BENCH_DIR points at the libm5 build); stats = first dump
    export REQROI=1 TIMEOUT=7200
    MODES=$PERF3 matrix I0_multi512_T4_roi     scatter_multi  "4 2000 512"    4 X86O3CPU  $KN_T4 &
    MODES=$PERF3 matrix I1_multi512_R4_roi     scatter_multi  "4 2000 512"    4 X86O3CPU  $KN_R4 &
    MODES=$PERF3 matrix I2_hotkey800k_R4_roi   scatter_dstate "4 200000 1"    4 X86O3CPU  $KN_R4 &
    MODES=$PERF3 matrix I3_rw800k_R4_roi       scatter_rw     "4 200000 32 8" 4 X86O3CPU  $KN_R4 &
    wait
    MODES=$PERF3 matrix I4_hotkey800k_16c_R4_roi scatter_dstate "16 50000 1"    16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix I5_rw800k_16c_R4_roi     scatter_rw     "16 50000 32 8" 16 X86O3CPU $KN_R4 $BUF16 &
    wait
    MODES=$PERF3 matrix I6_phased512_16c_R4_roi  scatter_phased "16 25 512 8"   16 X86O3CPU $KN_R4 $BUF16 &
    MODES=$PERF3 matrix I7_multi512_16c_R4_roi   scatter_multi  "16 500 512"    16 X86O3CPU $KN_R4 $BUF16 &
    wait ;;
  mesh)
    # controllers must be a multiple of the router count: L1s + banks + directories
    export TIMEOUT=28800
    MODES=local,persistent BANKS=16 EXTRA="--num-dirs=16 --topology=Mesh_XY --mesh-rows=4" matrix H32_multi512_32c_R4_mesh scatter_multi "32 250 512" 32 X86O3CPU $KN_R4 DSTATE_BUFFER_SIZE=512 &
    wait
    MODES=local,persistent BANKS=32 EXTRA="--num-dirs=32 --topology=Mesh_XY --mesh-rows=8" matrix H64_multi512_64c_R4_mesh scatter_multi "64 125 512" 64 X86O3CPU $KN_R4 DSTATE_BUFFER_SIZE=512 &
    wait ;;
  debug)
    # assertion-checking binary on the regression and one stress seed
    export GEM5BIN=$TREE/build/X86_MOESI_D/gem5.debug TIMEOUT=14400
    matrix D0_regress_R4_fenced_debug coherence_regression_fenced "" 5 X86O3CPU $KN_R4 &
    matrix D1_regress_T4_debug        coherence_regression        "" 5 X86O3CPU $KN_T4 &
    wait
    matrix D2_stress_R4_debug         random_stress "4 5000 1024 7" 4 X86O3CPU $KN_R4 &
    matrix D3_regress_TH4_minor_debug coherence_regression "" 5 X86MinorCPU $KN_TH4 &
    wait ;;
  evict)
    # capacity rejections past the L2 (8,192 lines) with eviction-for-delegate
    MODES=$PERF3 matrix E0_multi8192_R4_evict scatter_multi "4 125 8192" 4 X86O3CPU $KN_R4 DSTATE_EVICT_FOR_DELEGATE=1 &
    MODES=$PERF3 matrix E1_multi8192_T4_evict scatter_multi "4 125 8192" 4 X86O3CPU $KN_T4 DSTATE_EVICT_FOR_DELEGATE=1 &
    wait ;;
  esac ;;
v6)
  # Round six: admission gate (distinct writers, epoch decay), promotion on writers,
  # ACK-with-value, TSO same-line batching. Columns add to the round-4 knob sets:
  #   G  = gate (MIN_WRITERS=6, epoch 64)   S = TSO same-line batching   V = ACK value
  #   usage: ablate.sh v6 gates|strict|reads|regress16|all
  KN_V3="DSTATE_QUEUE_DEPTH=16 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1 DSTATE_REQ_COMBINE=16 DSTATE_MERGE_LIMIT=64"
  FAR="DSTATE_FAR_READS=1 DSTATE_READ_DOWNGRADE=64 DSTATE_EVICT_FOR_DELEGATE=1"
  KN_T4="$KN_V3 DSTATE_EXEC_LATENCY=20 $FAR"
  KN_TH4="$KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4 $FAR"
  KN_R4="$KN_TH4 DSTATE_RELAXED_AMO=1 DSTATE_DELTA_MIN_WORDS=2"
  GATE="DSTATE_MIN_WRITERS=6 DSTATE_WRITER_EPOCH=64"
  GATE2="DSTATE_MIN_WRITERS=2 DSTATE_WRITER_EPOCH=16"
  GATE3="DSTATE_MIN_WRITERS=3 DSTATE_WRITER_EPOCH=64"
  SL="DSTATE_TSO_SAMELINE=1 DSTATE_DELTA_MIN_WORDS=2"
  ACKV="DSTATE_ACK_VALUE=1"
  PW="DSTATE_PROMOTE_WRITERS=2"
  PERF3="local,remote,persistent"
  BUF16="DSTATE_BUFFER_SIZE=256"
  case ${2:-all} in
  gates)
    matrix G30_regress_TH4_SGV_o3     coherence_regression        "" 5 X86O3CPU    $KN_TH4 $GATE2 $SL $ACKV $PW &
    matrix G31_regress_TH4_SGV_minor  coherence_regression        "" 5 X86MinorCPU $KN_TH4 $GATE2 $ACKV $PW &
    matrix G32_regress_R4_GV_fenced   coherence_regression_fenced "" 5 X86O3CPU    $KN_R4  $GATE2 $ACKV $PW &
    matrix G33_regress_T4_S_o3        coherence_regression        "" 5 X86O3CPU    $KN_T4  $SL &
    wait
    # TSO litmus with same-line batching must pass; litmus2 exercises the rule directly
    litmus L40_litmus_unfenced_T4_S          ordering_litmus         DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 $SL &
    litmus L41_litmus2_unfenced_T4_S         ordering_litmus2        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 $SL &
    litmus L42_litmus2_unfenced_T4_noS       ordering_litmus2        DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_T4 &
    litmus L43_litmus2_unfenced_T4_S_local   ordering_litmus2        DSTATE_ENABLED=0 DSTATE_PERSISTENCE=0 $KN_T4 $SL &
    litmus L44_litmus_unfenced_T4_SGV        ordering_litmus         DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 $GATE2 $SL $ACKV $PW &
    litmus L45_litmus2_fenced_R4_V           ordering_litmus2_fenced DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_R4 $ACKV &
    wait ;;
  strict)
    # the strict-ordering single-line losses: hot key, 32-counter line, CMS at 4 cores
    MODES=$PERF3 matrix U0_hotkey800k_TH4_S        scatter_dstate "4 200000 1"  4 X86O3CPU $KN_TH4 $SL &
    MODES=$PERF3 matrix U1_hotkey800k_TH4_G        scatter_dstate "4 200000 1"  4 X86O3CPU $KN_TH4 $GATE &
    MODES=$PERF3 matrix U2_hotkey800k_TH4_SG       scatter_dstate "4 200000 1"  4 X86O3CPU $KN_TH4 $SL $GATE &
    MODES=$PERF3 matrix U3_scatter800k_TH4_S       scatter_dstate "4 200000 32" 4 X86O3CPU $KN_TH4 $SL &
    wait
    MODES=$PERF3 matrix U4_scatter800k_T4_S        scatter_dstate "4 200000 32" 4 X86O3CPU $KN_T4 $SL &
    MODES=$PERF3 matrix U5_cms_T4_SG               cms_dstate     "4 20000"     4 X86O3CPU $KN_T4 $SL $GATE &
    MODES=$PERF3 matrix U6_cms_T4_G                cms_dstate     "4 20000"     4 X86O3CPU $KN_T4 $GATE &
    MODES=$PERF3 matrix U7_multi512_T4_SG          scatter_multi  "4 2000 512"  4 X86O3CPU $KN_T4 $SL $GATE &
    wait
    MODES=$PERF3 matrix U8_hotkey800k_8c_TH4_SG    scatter_dstate "8 100000 1"  8 X86O3CPU $KN_TH4 $SL $GATE &
    MODES=$PERF3 matrix U9_multi512_R4_G           scatter_multi  "4 2000 512"  4 X86O3CPU $KN_R4 $GATE &
    MODES=$PERF3 matrix U10_hotkey800k_R4_G        scatter_dstate "4 200000 1"  4 X86O3CPU $KN_R4 $GATE &
    wait
    # a gate of 3 admits every 4-writer line at 4 cores: what it costs CMS and what it keeps
    MODES=$PERF3 matrix U12_multi512_T4_SG3        scatter_multi  "4 2000 512"  4 X86O3CPU $KN_T4 $SL $GATE3 &
    MODES=$PERF3 matrix U13_cms_T4_SG3             cms_dstate     "4 20000"     4 X86O3CPU $KN_T4 $SL $GATE3 &
    MODES=$PERF3 matrix U14_hotkey800k_TH4_SG3     scatter_dstate "4 200000 1"  4 X86O3CPU $KN_TH4 $SL $GATE3 &
    wait ;;
  reads)
    # read-after-add under strict ordering: ACK value, promotion on writers, batching
    MODES=$PERF3 matrix V0_rw800k_T4_V             scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_T4 $ACKV &
    MODES=$PERF3 matrix V1_rw800k_T4_VP            scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_T4 $ACKV $PW &
    MODES=$PERF3 matrix V2_rw800k_T4_SVP           scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_T4 $SL $ACKV $PW &
    MODES=$PERF3 matrix V3_rw800k_R4_V             scatter_rw "4 200000 32 8" 4 X86O3CPU $KN_R4 $ACKV &
    wait
    export TIMEOUT=7200
    MODES=$PERF3 matrix V4_rw800k_16c_T4_SVP       scatter_rw "16 50000 32 8" 16 X86O3CPU $KN_T4 $SL $ACKV $PW $BUF16 &
    MODES=$PERF3 matrix V5_rw800k_16c_R4_V         scatter_rw "16 50000 32 8" 16 X86O3CPU $KN_R4 $ACKV $BUF16 &
    wait ;;
  regress16)
    # the gate must not cost the 16-core wins
    export TIMEOUT=7200
    MODES=$PERF3 matrix W16_hotkey800k_16c_TH4_SG  scatter_dstate "16 50000 1"  16 X86O3CPU $KN_TH4 $SL $GATE $BUF16 &
    MODES=$PERF3 matrix W17_multi512_16c_R4_G      scatter_multi  "16 500 512"  16 X86O3CPU $KN_R4 $GATE $BUF16 &
    wait
    MODES=$PERF3 matrix W18_phased512_16c_R4_GV    scatter_phased "16 25 512 8" 16 X86O3CPU $KN_R4 $GATE $ACKV $BUF16 &
    MODES=$PERF3 matrix W19_hotkey800k_16c_R4_G    scatter_dstate "16 50000 1"  16 X86O3CPU $KN_R4 $GATE $BUF16 &
    wait ;;
  all)
    for ph in gates strict reads regress16; do echo "--- $ph $(date +%H:%M)"; bash "$0" v6 $ph; done ;;
  esac ;;
v7)
  # Round 7: change-rate admission gate (would-be ownership changes per update, counted at
  # the home in both modes), time-based decay of the gate table, adaptive threshold.
  # Phases: gates | strict | sixteen | all
  KN_V3="DSTATE_QUEUE_DEPTH=16 DSTATE_INIT_INTERVAL=4 DSTATE_BUSY_STALL=1 DSTATE_QUEUE_STALL=1 DSTATE_REQ_COMBINE=16 DSTATE_MERGE_LIMIT=64"
  FAR="DSTATE_FAR_READS=1 DSTATE_READ_DOWNGRADE=64 DSTATE_EVICT_FOR_DELEGATE=1"
  KN_T4="$KN_V3 DSTATE_EXEC_LATENCY=20 $FAR"
  KN_TH4="$KN_V3 DSTATE_HOTWORDS=32 DSTATE_HIT_LATENCY=4 $FAR"
  KN_R4="$KN_TH4 DSTATE_RELAXED_AMO=1 DSTATE_DELTA_MIN_WORDS=2"
  SL="DSTATE_TSO_SAMELINE=1 DSTATE_DELTA_MIN_WORDS=2"
  ACKV="DSTATE_ACK_VALUE=1"
  PW="DSTATE_PROMOTE_WRITERS=2"
  BUF16="DSTATE_BUFFER_SIZE=${BUF16:-256}"
  IDLE="DSTATE_WRITER_IDLE=200000"
  CG="DSTATE_MIN_CHANGE_PCT=50 DSTATE_MIN_CHANGES=2 $IDLE"
  CG30="DSTATE_MIN_CHANGE_PCT=30 DSTATE_MIN_CHANGES=2 $IDLE"
  CGA="$CG DSTATE_ADAPTIVE_GATE=1"
  PERF3="local,remote,persistent"
  case ${2:-all} in
  gates)
    matrix G40_regress_TH4_SCV_o3     coherence_regression        "" 5 X86O3CPU    $KN_TH4 $SL $CG $ACKV $PW &
    matrix G41_regress_TH4_C_minor    coherence_regression        "" 5 X86MinorCPU $KN_TH4 $CG &
    matrix G42_regress_R4_CV_fenced   coherence_regression_fenced "" 5 X86O3CPU    $KN_R4 $CG $ACKV &
    matrix G43_regress_TH4_SCA_o3     coherence_regression        "" 5 X86O3CPU    $KN_TH4 $SL $CGA &
    wait
    litmus L50_litmus_unfenced_T4_SC          ordering_litmus  DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 $SL $CG &
    litmus L51_litmus2_unfenced_T4_SC         ordering_litmus2 DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 $SL $CG &
    wait ;;
  strict)
    # the test the writer gate failed: keep the 512-line win AND refuse CMS, at 4 cores
    MODES=$PERF3 matrix C0_cms_T4_SC                cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $CG &
    MODES=$PERF3 matrix C1_multi512_T4_SC           scatter_multi  "4 2000 512"     4 X86O3CPU $KN_T4 $SL $CG &
    MODES=$PERF3 matrix C2_hotkey800k_TH4_SC        scatter_dstate "4 200000 1"     4 X86O3CPU $KN_TH4 $SL $CG &
    wait
    MODES=$PERF3 matrix C3_rw800k_T4_SCVP           scatter_rw     "4 200000 32 8"  4 X86O3CPU $KN_T4 $SL $CG $ACKV $PW &
    MODES=$PERF3 matrix C4_cms_T4_SCA               cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $CGA &
    MODES=$PERF3 matrix C5_multi512_T4_SCA          scatter_multi  "4 2000 512"     4 X86O3CPU $KN_T4 $SL $CGA &
    wait
    MODES=$PERF3 matrix C6_cms_T4_SC30              cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $CG30 &
    MODES=$PERF3 matrix C7_rw800k_T4_SC30VP         scatter_rw     "4 200000 32 8"  4 X86O3CPU $KN_T4 $SL $CG30 $ACKV $PW &
    MODES=$PERF3 matrix C8_multi512_R4_C            scatter_multi  "4 2000 512"     4 X86O3CPU $KN_R4 $CG &
    wait ;;
  sixteen)
    export TIMEOUT=7200
    MODES=$PERF3 matrix C16_multi512_16c_R4_C       scatter_multi  "16 500 512"     16 X86O3CPU $KN_R4 $CG $BUF16 &
    MODES=$PERF3 matrix C17_hotkey800k_16c_TH4_SC   scatter_dstate "16 50000 1"     16 X86O3CPU $KN_TH4 $SL $CG $BUF16 &
    wait
    MODES=$PERF3 matrix C18_rw800k_16c_T4_SCVP      scatter_rw     "16 50000 32 8"  16 X86O3CPU $KN_T4 $SL $CG $ACKV $PW $BUF16 &
    MODES=$PERF3 matrix C19_phased512_16c_R4_C      scatter_phased "16 25 512 8"    16 X86O3CPU $KN_R4 $CG $BUF16 &
    wait
    MODES=$PERF3 matrix C20_cms_16c_T4_SC           cms_dstate     "16 5000"        16 X86O3CPU $KN_T4 $SL $CG $BUF16 &
    wait ;;
  tenure)
    # requester-side owner-tenure predictor: the owner hit-rate signal the home lacks.
    # The change-rate gate here runs without idle decay (DSTATE_WRITER_IDLE shifts
    # timing into a gem5 SE functional-read limitation on the CMS binary).
    CG0="DSTATE_MIN_CHANGE_PCT=50 DSTATE_MIN_CHANGES=2"
    TEN="DSTATE_MAX_TENURE=1"
    matrix G44_regress_TH4_ST_o3      coherence_regression        "" 5 X86O3CPU    $KN_TH4 $SL $TEN $ACKV $PW &
    matrix G45_regress_R4_T_fenced    coherence_regression_fenced "" 5 X86O3CPU    $KN_R4 $TEN &
    litmus L52_litmus_unfenced_T4_ST          ordering_litmus  DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 $SL $TEN &
    wait
    MODES=$PERF3 matrix T0_cms_T4_ST                cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $TEN &
    MODES=$PERF3 matrix T1_multi512_T4_ST           scatter_multi  "4 2000 512"     4 X86O3CPU $KN_T4 $SL $TEN &
    MODES=$PERF3 matrix T2_rw800k_T4_STVP           scatter_rw     "4 200000 32 8"  4 X86O3CPU $KN_T4 $SL $TEN $ACKV $PW &
    wait
    MODES=$PERF3 matrix T3_hotkey800k_TH4_ST        scatter_dstate "4 200000 1"     4 X86O3CPU $KN_TH4 $SL $TEN &
    MODES=$PERF3 matrix T4_cms_T4_STC0              cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $TEN $CG0 &
    MODES=$PERF3 matrix T5_cms_T4_SC0               cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $CG0 &
    wait
    # 16-core tenure runs are in the probe phase: without probing the predictor never samples
    wait ;;
  probe)
    # tenure predictor with probing: a line with no recorded tenure is owned first
    # so the tenure can be measured; records expire (re-probe) after 1M cycles.
    CG0="DSTATE_MIN_CHANGE_PCT=50 DSTATE_MIN_CHANGES=2"
    PR="DSTATE_MAX_TENURE=1 DSTATE_TENURE_PROBE=1 DSTATE_TENURE_IDLE=1000000"
    matrix G46_regress_TH4_SPV_o3     coherence_regression        "" 5 X86O3CPU    $KN_TH4 $SL $PR $ACKV $PW &
    matrix G47_regress_R4_P_fenced    coherence_regression_fenced "" 5 X86O3CPU    $KN_R4 $PR &
    matrix G48_regress_TH4_P_minor    coherence_regression        "" 5 X86MinorCPU $KN_TH4 $PR &
    litmus L53_litmus_unfenced_T4_SP          ordering_litmus  DSTATE_ENABLED=1 DSTATE_PERSISTENCE=1 $KN_TH4 $SL $PR &
    wait
    MODES=$PERF3 matrix P0_cms_T4_SP                cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $PR &
    MODES=$PERF3 matrix P1_multi512_T4_SP           scatter_multi  "4 2000 512"     4 X86O3CPU $KN_T4 $SL $PR &
    MODES=$PERF3 matrix P2_rw800k_T4_SPVP           scatter_rw     "4 200000 32 8"  4 X86O3CPU $KN_T4 $SL $PR $ACKV $PW &
    wait
    MODES=$PERF3 matrix P3_hotkey800k_TH4_SP        scatter_dstate "4 200000 1"     4 X86O3CPU $KN_TH4 $SL $PR &
    MODES=$PERF3 matrix P4_cms_T4_SPC0              cms_dstate     "4 20000"        4 X86O3CPU $KN_T4 $SL $PR $CG0 &
    MODES=$PERF3 matrix P5_multi512_R4_P            scatter_multi  "4 2000 512"     4 X86O3CPU $KN_R4 $PR &
    wait
    MODES=$PERF3 matrix P6_scatter800k_T4_SP        scatter_dstate "4 200000 32"    4 X86O3CPU $KN_T4 $SL $PR &
    MODES=$PERF3 matrix P7_phased512_R4_P           scatter_phased "4 100 512 8"    4 X86O3CPU $KN_R4 $PR &
    wait
    export TIMEOUT=7200
    MODES=$PERF3 matrix P16_multi512_16c_R4_P       scatter_multi  "16 500 512"     16 X86O3CPU $KN_R4 $PR $BUF16 &
    MODES=$PERF3 matrix P17_rw800k_16c_T4_SPVP      scatter_rw     "16 50000 32 8"  16 X86O3CPU $KN_T4 $SL $PR $ACKV $PW $BUF16 &
    wait
    MODES=$PERF3 matrix P18_hotkey800k_16c_TH4_SP   scatter_dstate "16 50000 1"     16 X86O3CPU $KN_TH4 $SL $PR $BUF16 &
    MODES=$PERF3 matrix P19_cms_16c_T4_SP           cms_dstate     "16 5000"        16 X86O3CPU $KN_T4 $SL $PR $BUF16 &
    wait ;;
  all)
    for ph in gates strict sixteen tenure probe; do echo "--- $ph $(date +%H:%M)"; bash "$0" v7 $ph; done ;;
  esac ;;
report)
  # One row per (run, mode). Update/response message counts come from the L1's terminal
  # ACK/NACK counters (Garnet emits no per-size-class msg_count); newBytes uses the derived
  # 16 B / 8 B classes, oldBytes what the earlier 32 B / 24 B classes would have charged.
  printf "%-32s %-11s %9s %8s %8s %6s %6s %6s %6s %7s %6s %6s %7s %7s\n" run mode simSec flits pkts Rej Busy Cold Hot Compl Merge CmplM updMsg rspMsg
  for d in $(ls -d "$OUT"/*/ 2>/dev/null | sort); do name=$(basename "$d")
    for m in "$d"/*-0/ "$d"/; do [ -f "$m/stats.txt" ] || continue
      awk -v n="$name" -v mode="$(basename "$m" | sed 's/-0$//')" '
        # Ruby prints multi-instance counters as `name | v pct pct | v pct pct ...`; sum them.
        function vsum(line,   n, parts, i, t, s) { n = split(line, parts, "|"); s = 0;
          if (n == 1) { split(line, t, " "); return t[2] + 0 }
          for (i = 2; i <= n; i++) { split(parts[i], t, " "); s += t[1] + 0 } return s }
        /End Simulation Statistics/{exit}
        /^simSeconds/{s=$2}
        /L2Cache_Controller\.DState_Reject /{r=vsum($0)} /L2Cache_Controller\.DState_Busy /{b=vsum($0)}
        /L2Cache_Controller\.DState_Cold /{c=vsum($0)}   /L2Cache_Controller\.DState_Hot /{h=vsum($0)}
        /L2Cache_Controller\.DState_Complete /{k=vsum($0)}
        /L2Cache_Controller\.DState_Merge /{mg=vsum($0)} /L2Cache_Controller\.DState_CompleteMerged /{km=vsum($0)}
        /network\.flits_injected::total/{fl=$2} /network\.packets_injected::total/{pk=$2}
        /L1Cache_Controller\.D_REQ\.DState_ACK /{u32+=vsum($0); rs+=vsum($0)}
        /L1Cache_Controller\.D_REQ\.DState_NACK /{u32+=vsum($0); rs+=vsum($0)}
        # Compl = plain + merged completions; Merge = requests folded into another op
        END{ if (mode==n) mode="-"; printf "%-32s %-11s %9s %8s %8s %6d %6d %6d %6d %7d %6d %6d %7d %7d\n",
             n, mode, s, fl, pk, r, b, c, h, k+km, mg, km, u32, rs }' "$m/stats.txt"
    done
  done ;;
*) echo "usage: GEM5_TREE=... OUT=... $0 gates|perf|rw|multi|final|report | v2 gates|scatter|rw|multi|hotkey|long | v3 gates|perf|ws|scale|phased | v4 gates|perf|apps|scale|mixed|regate|all" ;;
esac
