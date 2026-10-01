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
      --out "$OUT/$name" --cores "$cores" --banks 4 --cpu "$cpu" --repeats 1 --timeout 2400 > "$OUT/$name.log" 2>&1
  printf "%-34s %s/4 modes pass  %s\n" "$name" "$(grep -cE ' PASS$' "$OUT/$name.log")" "$(grep -E ' FAIL' "$OUT/$name.log" | tr '\n' ' ')"; }
# direct gem5 run for the ordering litmus (prints PASS/FAIL, not CORRECT)
litmus() { local name=$1 bin=$2; shift 2
  [ -d "$OUT/$name" ] && { echo "$name: exists, skipping"; return; }
  env "$@" "$GEM5" --outdir="$OUT/$name" "$SE" $COMMON --cpu-type=X86O3CPU --num-cpus=2 -c "$W/bench/$bin" > "$OUT/$name.log" 2>&1
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
*) echo "usage: GEM5_TREE=... OUT=... $0 gates|perf|rw|multi|final|report | v2 gates|scatter|rw|multi|hotkey|long" ;;
esac
