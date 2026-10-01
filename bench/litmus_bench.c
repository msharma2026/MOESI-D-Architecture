/* Shifting-hot-window update microbenchmark (not a memory-ordering litmus).
 * Explicit ADD32, conventional atomics, combining and private accumulation.
 * All output cells are checked by deterministic serial stream replay.
 * See coherence_regression.c for directed correctness/ordering tests. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "dstate_ops.h"
#include <string.h>
#include <pthread.h>

#if !defined(MODE_NAIVE) && !defined(MODE_DSTATE) && \
    !defined(MODE_FLATCOMBINE) && !defined(MODE_PRIVATIZE) && !defined(MODE_PRIVSTREAM)
#error "define one of MODE_NAIVE / MODE_DSTATE / MODE_FLATCOMBINE / MODE_PRIVATIZE / MODE_PRIVSTREAM"
#endif

/* ---- workload shape (shifting hot window) ------------------------------- */
#define HOT_WIDTH     8      /* # of consecutive counters hot at any moment   */
#define SHIFT_PERIOD  128    /* advance the hot window every this many iters   */
#define SHIFT_STRIDE  HOT_WIDTH
#define MAXK          8192

#define CACHELINE     128    /* matches --cacheline_size=128                   */

/* ---- explicitly selected primitives ----------------------------------- */
static inline void atomic_add_noret(int32_t *p, int32_t d)
{ dstate_add32(p, (uint32_t)d); }
static inline int32_t atomic_fetch_add(int32_t *p, int32_t d)
{ return conventional_add32(p, d); }

/* ---- shared state ------------------------------------------------------- */
static int32_t counters[MAXK] __attribute__((aligned(CACHELINE)));
static int      g_threads, g_iters, g_ncounters;
static int g_window = 0;
#if defined(MODE_PRIVATIZE)
static int32_t *private_rows[64];
#endif

/* deterministic per-thread PRNG (xorshift32) */
static inline uint32_t xrng(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

/* Zipf(s=1) CDF over HOT_WIDTH ranks, precomputed once. */
static double zipf_cdf[HOT_WIDTH];
static void zipf_init(void) {
    double h = 0.0, acc = 0.0;
    for (int k = 1; k <= HOT_WIDTH; k++) h += 1.0 / k;
    for (int k = 1; k <= HOT_WIDTH; k++) { acc += (1.0 / k) / h; zipf_cdf[k-1] = acc; }
}
static inline int zipf_rank(uint32_t *s) {
    double u = (double)(xrng(s) & 0xFFFFFF) / (double)0x1000000; /* [0,1) */
    for (int k = 0; k < HOT_WIDTH; k++) if (u < zipf_cdf[k]) return k;
    return HOT_WIDTH - 1;
}

/* All threads share the SAME window schedule keyed on the local iteration
 * index j, so at step j every thread targets the same shifting hot window
 * -> genuine multi-writer contention that moves over time. Zipf concentrates
 * the hits on the hottest 1-2 indices of that window. */
static inline int pick_index(int j, uint32_t *s) {
    int base = (j / SHIFT_PERIOD) * SHIFT_STRIDE;
    return (base + zipf_rank(s)) % g_ncounters;
}

/* ---- tiny sense-reversing barrier (avoids libc barrier quirks in SE) ---- */
static volatile int bar_count = 0, bar_sense = 0;
static void barrier(int *local_sense) {
    *local_sense = !*local_sense;
    int arrived = __atomic_add_fetch(&bar_count, 1, __ATOMIC_ACQ_REL);
    if (arrived == g_threads) {
        __atomic_store_n(&bar_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&bar_sense, *local_sense, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&bar_sense, __ATOMIC_ACQUIRE) != *local_sense) ;
    }
}

/* ======================================================================== */
#if defined(MODE_FLATCOMBINE)
/* Classic flat combining. Each thread owns one padded publication record.
 * A thread publishes {idx,delta}, then tries to become the combiner (single
 * TAS lock). The combiner applies EVERY pending record to the counters with
 * plain adds -> counters are only ever touched by the current combiner, so the
 * hot counter lines never ping-pong. Only the combiner-lock line and the
 * record lines move between cores. Backpressure: a thread waits for its own
 * previous record to be consumed before republishing (and helps combine while
 * it waits), which also guarantees liveness. */
typedef struct {
    volatile int32_t pending;   /* 1 = has an unapplied request               */
    int32_t idx, delta;
    char pad[CACHELINE - 3 * (int)sizeof(int32_t)];
} rec_t;
static rec_t recs[64] __attribute__((aligned(CACHELINE)));
static volatile int32_t comb_lock __attribute__((aligned(CACHELINE))) = 0;

static inline void cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
/* test-and-test-and-set: only issue the contended xchg when the lock READS free,
 * so a held lock is polled from a shared (cached) copy instead of ping-ponging. */
static inline int try_lock(void) {
    if (__atomic_load_n(&comb_lock, __ATOMIC_ACQUIRE)) return 0;
    return __atomic_exchange_n(&comb_lock, 1, __ATOMIC_ACQUIRE) == 0;
}
static inline void unlock(void)  { __atomic_store_n(&comb_lock, 0, __ATOMIC_RELEASE); }

static void combine_all(void) {
    for (int i = 0; i < g_threads; i++) {
        if (__atomic_load_n(&recs[i].pending, __ATOMIC_ACQUIRE)) {
            counters[recs[i].idx] += recs[i].delta;          /* combiner-exclusive */
            __atomic_store_n(&recs[i].pending, 0, __ATOMIC_RELEASE);
        }
    }
}
static inline void fc_publish(int tid, int idx) {
    /* backpressure: wait for my previous request to be consumed; help combine */
    while (__atomic_load_n(&recs[tid].pending, __ATOMIC_ACQUIRE)) {
        if (try_lock()) { combine_all(); unlock(); }
        else cpu_relax();
    }
    recs[tid].idx = idx; recs[tid].delta = 1;
    __atomic_store_n(&recs[tid].pending, 1, __ATOMIC_RELEASE);
    if (try_lock()) { combine_all(); unlock(); } /* else fire-and-forget */
}
#endif

/* ======================================================================== */
typedef struct { int tid; } arg_t;

static void *worker(void *a) {
    int tid = ((arg_t *)a)->tid;
    uint32_t s = 0x9E3779B9u ^ (uint32_t)(tid * 2654435761u); /* per-thread seed */
    if (!s) s = 1;
    int ls = 0;

#if defined(MODE_PRIVATIZE)
    int32_t *priv = bench_calloc(g_ncounters, sizeof(int32_t));   /* private, no sharing */
#endif
#if defined(MODE_PRIVSTREAM)
    /* streaming privatization: accumulate locally, but with NO clean merge point
     * we must flush deltas to the shared state every g_window ops to bound
     * staleness. Flush uses a CONVENTIONAL atomic (lock xadd) — i.e. this is
     * privatization on a machine WITHOUT D-state. A per-thread dirty list
     * (epoch-marked, O(1) reset) keeps each flush proportional to touched
     * counters, not to K. */
    int32_t *priv  = bench_calloc(g_ncounters, sizeof(int32_t));
    int     *dirty = malloc((size_t)g_ncounters * sizeof(int));
    uint32_t *dmark = bench_calloc(g_ncounters, sizeof(uint32_t));
    int ndirty = 0; uint32_t epoch = 1;
#endif

    for (int j = 0; j < g_iters; j++) {
        int idx = pick_index(j, &s);
#if defined(MODE_NAIVE)
        (void)atomic_fetch_add(&counters[idx], 1);
#elif defined(MODE_DSTATE)
        atomic_add_noret(&counters[idx], 1);
#elif defined(MODE_FLATCOMBINE)
        fc_publish(tid, idx);
#elif defined(MODE_PRIVATIZE)
        priv[idx] += 1;                                     /* zero contention */
#elif defined(MODE_PRIVSTREAM)
        priv[idx] += 1;
        if (dmark[idx] != epoch) { dmark[idx] = epoch; dirty[ndirty++] = idx; }
        if (g_window > 0 && ((j + 1) % g_window) == 0) {    /* windowed flush */
            for (int d = 0; d < ndirty; d++) { int ix = dirty[d];
                (void)atomic_fetch_add(&counters[ix], priv[ix]); priv[ix] = 0; }
            ndirty = 0; epoch++;
        }
#endif
    }

#if defined(MODE_PRIVSTREAM)
    for (int d = 0; d < ndirty; d++) { int ix = dirty[d];   /* final flush */
        (void)atomic_fetch_add(&counters[ix], priv[ix]); priv[ix] = 0; }
    free(priv); free(dirty); free(dmark);
#endif

    #if defined(MODE_PRIVATIZE)
    private_rows[tid] = priv;
    #endif
    barrier(&ls);   /* all workers finished the update loop */

#if defined(MODE_FLATCOMBINE)
    if (tid == 0) { combine_all(); combine_all(); }         /* drain residual  */
#elif defined(MODE_PRIVATIZE)
    int chunk = ((g_ncounters + g_threads - 1) / g_threads + 31) / 32 * 32;
    int end = (tid + 1) * chunk;
    if (end > g_ncounters) end = g_ncounters;
    for (int k = tid * chunk; k < end; ++k)
        for (int t = 0; t < g_threads; ++t) counters[k] += private_rows[t][k];
    barrier(&ls);
    free(priv);
#endif
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <threads> <iters_per_thread> <num_counters>\n", argv[0]);
        return 1;
    }
    g_threads   = bench_int(argv[1], 1, 64);
    g_iters     = bench_int(argv[2], 1, INT_MAX / g_threads);
    g_ncounters = bench_int(argv[3], HOT_WIDTH, MAXK);
    if (argc >= 5) g_window = bench_int(argv[4], 0, INT_MAX);   /* PRIVSTREAM flush window */
    if (g_threads < 1 || g_threads > 64) { fprintf(stderr, "threads 1..64\n"); return 1; }
    if (g_ncounters < HOT_WIDTH || g_ncounters > MAXK) {
        fprintf(stderr, "counters %d..%d\n", HOT_WIDTH, MAXK); return 1;
    }

    memset(counters, 0, sizeof(counters));
    zipf_init();

#if defined(MODE_NAIVE)
    const char *mode = "NAIVE(lock-xadd)";
#elif defined(MODE_DSTATE)
    const char *mode = "DSTATE(explicit-add32)";
#elif defined(MODE_FLATCOMBINE)
    const char *mode = "FLATCOMBINE(software)";
#elif defined(MODE_PRIVATIZE)
    const char *mode = "PRIVATIZE(software)";
#elif defined(MODE_PRIVSTREAM)
    const char *mode = "PRIVSTREAM(software)";
#endif
    long total = (long)g_threads * g_iters;
    printf("litmus_bench: threads=%d iters/thread=%d counters=%d mode=%s window=%d\n",
           g_threads, g_iters, g_ncounters, mode, g_window);
    printf("total_ops=%ld hot_width=%d shift_period=%d\n", total, HOT_WIDTH, SHIFT_PERIOD);

    bench_roi_begin();
    pthread_t th[64]; arg_t args[64];
    for (int t = 0; t < g_threads; t++) { args[t].tid = t; }
    for (int t = 1; t < g_threads; t++) bench_thread_check(pthread_create(&th[t], NULL, worker, &args[t]));
    worker(&args[0]);                          /* main thread is worker 0 */
    for (int t = 1; t < g_threads; t++) bench_thread_check(pthread_join(th[t], NULL));

    bench_roi_end();
    int64_t sum = 0;
    for (int k = 0; k < g_ncounters; k++) sum += counters[k];
    int correct = (sum == (int64_t)total);
    int32_t *oracle = bench_calloc(g_ncounters, sizeof(*oracle));
    for (int t = 0; t < g_threads; ++t) {
        uint32_t seed = 0x9E3779B9u ^ (uint32_t)(t * 2654435761u);
        if (!seed) seed = 1;
        for (int j = 0; j < g_iters; ++j) oracle[pick_index(j, &seed)]++;
    }
    for (int k = 0; k < g_ncounters; ++k) correct &= counters[k] == oracle[k];
    free(oracle);
    printf("checksum=%lld expected=%ld %s\n",
           (long long)sum, total, correct ? "CORRECT" : "WRONG");
    return correct ? 0 : 2;
}
