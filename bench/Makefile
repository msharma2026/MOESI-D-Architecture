# Benchmarks for MOESI-D evaluation.
#
# Built statically for x86-64 gem5 SE mode. Scatter/SpMV use OpenMP;
# litmus, cms and coherence_regression use pthreads. They select strategies at compile time
# via a MODE_* macro (they #error without one); scatter and spmv select the
# conventional-control build with -DBASELINE (SpMV uses plain single-writer adds).
#
# Target names match the binary names used by tools/ablate.sh.

CC      ?= gcc
CFLAGS  ?= -O2 -static -fopenmp -pthread
CPPFLAGS ?=
LDLIBS ?=
# Optional measurement ROI: CPPFLAGS=-DBENCH_GEM5_ROI -I/path/to/gem5/include
# LDLIBS=/path/to/gem5/util/m5/build/x86/out/libm5.a
# ROI includes synchronization/final merge and excludes serial verification.

DSTATE  := scatter_dstate scatter_nm scatter_multi scatter_rw scatter_phased spmv_dstate
BASE    := scatter_base scatter_base_nm scatter_multi_base scatter_rw_base spmv_base
LITMUS  := litmus_dstate litmus_naive litmus_fc litmus_priv litmus_privstream
CMS     := cms_dstate cms_naive cms_priv cms_privstream
# The AADD instruction itself is unfenced (ordered as a store under TSO). The
# *_fenced builds wrap every AADD in mfence via -DDSTATE_FENCED, reproducing the
# earlier always-fenced macro-op as an ablation column with the same simulator.
ORDER   := ordering_litmus ordering_litmus_fenced ordering_litmus_lockadd
FENCED  := scatter_dstate_fenced scatter_rw_fenced coherence_regression_fenced

all: $(DSTATE) $(BASE) $(LITMUS) $(CMS) $(ORDER) $(FENCED) coherence_regression

$(DSTATE) $(BASE) $(LITMUS) $(CMS) $(ORDER) $(FENCED) coherence_regression: dstate_ops.h

coherence_regression:        coherence_regression.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
coherence_regression_fenced: coherence_regression.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DDSTATE_FENCED $< -o $@ $(LDLIBS)

# --- store-ordering litmus: add x ; store flag  (reader must never see flag before x)
ordering_litmus:         ordering_litmus.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
ordering_litmus_fenced:  ordering_litmus.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DDSTATE_FENCED $< -o $@ $(LDLIBS)
ordering_litmus_lockadd: ordering_litmus.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DLITMUS_LOCK_ADD $< -o $@ $(LDLIBS)
scatter_dstate_fenced:   scatter_bench.c   ; $(CC) $(CPPFLAGS) $(CFLAGS) -DDSTATE_FENCED $< -o $@ $(LDLIBS)
scatter_rw_fenced:       scatter_rw.c      ; $(CC) $(CPPFLAGS) $(CFLAGS) -DDSTATE_FENCED $< -o $@ $(LDLIBS)

# --- scatter: one shared 128 B line of counters ---------------------------
scatter_dstate:     scatter_bench.c    ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
scatter_base:       scatter_bench.c    ; $(CC) $(CPPFLAGS) $(CFLAGS) -DBASELINE $< -o $@ $(LDLIBS)
scatter_nm:         scatter_nomemset.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
scatter_base_nm:    scatter_nomemset.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DBASELINE $< -o $@ $(LDLIBS)

# --- scatter_multi: N counters on N distinct lines (working-set sweep) ----
scatter_multi:      scatter_multi.c      ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
scatter_multi_base: scatter_multi_base.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
# --- scatter_phased: update phase over N lines, then every thread reads them; repeat
scatter_phased:     scatter_phased.c     ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)

# --- scatter_rw: updates interleaved with full-line reads -----------------
scatter_rw:         scatter_rw.c      ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
scatter_rw_base:    scatter_rw_base.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)

# --- Integer SpMV: single-writer negative control; check actual delegation ---
spmv_dstate:        spmv_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)
spmv_base:          spmv_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DBASELINE $< -o $@ $(LDLIBS)

# --- shifting-hot Zipfian, five synchronization strategies ----------------
litmus_dstate:      litmus_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_DSTATE      $< -o $@ $(LDLIBS)
litmus_naive:       litmus_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_NAIVE       $< -o $@ $(LDLIBS)
litmus_fc:          litmus_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_FLATCOMBINE $< -o $@ $(LDLIBS)
litmus_priv:        litmus_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_PRIVATIZE   $< -o $@ $(LDLIBS)
litmus_privstream:  litmus_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_PRIVSTREAM  $< -o $@ $(LDLIBS)

# --- cms: Count-Min Sketch telemetry under DDoS key skew ------------------
cms_dstate:         cms_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_DSTATE     $< -o $@ $(LDLIBS)
cms_naive:          cms_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_NAIVE      $< -o $@ $(LDLIBS)
cms_priv:           cms_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_PRIVATIZE  $< -o $@ $(LDLIBS)
cms_privstream:     cms_bench.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -DMODE_PRIVSTREAM $< -o $@ $(LDLIBS)

clean:
	rm -f $(DSTATE) $(BASE) $(LITMUS) $(CMS) coherence_regression

.PHONY: all clean
