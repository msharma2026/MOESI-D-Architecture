/* Shared-line ADD32 microbenchmark. Default: explicit fenced no-return
 * instruction. BASELINE: conventional returning atomic (secondary control).
 * Primary comparison: the SAME default binary with DSTATE_ENABLED=0/1 and
 * DSTATE_PERSISTENCE=0/1. All final cells checked, failures exit nonzero. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>

/* D-state path: completion-acknowledged add, no return value */
static inline void atomic_add_noret(int32_t *p, int32_t d)
{ dstate_add32(p, (uint32_t)d); }

/* Baseline path: fetch-add returns old value, forces conventional GetX */
static inline int32_t atomic_fetch_add_baseline(int32_t *p, int32_t d)
{ return conventional_add32(p, d); }

/*
 * 16 counters = 64 B, fits in one 128-byte cache line.
 * Aligned to 128-byte boundary so all counters share exactly one line.
 * (max 32 so the array is always <= 128 B)
 */
static int32_t counters[32] __attribute__((aligned(128)));

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <threads> <iters_per_thread> <num_counters>\n",
                argv[0]);
        return 1;
    }

    int threads   = bench_int(argv[1], 1, 64);
    int iters     = bench_int(argv[2], 1, INT_MAX / threads);
    int ncounters = bench_int(argv[3], 1, 32);

    if (ncounters < 1 || ncounters > 32) {
        fprintf(stderr, "num_counters must be 1..32\n");
        return 1;
    }

    omp_set_dynamic(0);
    omp_set_num_threads(threads);
    /* counters are BSS zero-initialized; no memset (avoids pre-owning the line) */

    int total = threads * iters;

#ifdef BASELINE
    const char *mode = "baseline(lock-xadd)";
#else
    const char *mode = "dstate(explicit-add32)";
#endif

    printf("scatter_bench: threads=%d iters/thread=%d counters=%d mode=%s\n",
           threads, iters, ncounters, mode);
    printf("total_ops=%d line_size_bytes=%d\n",
           total, ncounters * (int)sizeof(int32_t));

    /*
     * schedule(static): each thread gets a contiguous block of iters.
     * All threads cycle through counters 0..ncounters-1, so every L2
     * writes to every counter — maximising cross-L2 contention for
     * D-state promotion.
     */
    bench_roi_begin();
#pragma omp parallel for schedule(static)
    for (int i = 0; i < total; i++) {
        int idx = i % ncounters;
#ifdef BASELINE
        (void)atomic_fetch_add_baseline(&counters[idx], 1);
#else
        atomic_add_noret(&counters[idx], 1);
#endif
    }

    bench_roi_end();
    int64_t sum = 0;
    for (int i = 0; i < ncounters; i++)
        sum += counters[i];

    int expected = total;
    int correct = (sum == (int64_t)expected);
    for (int k = 0; k < ncounters; ++k)
        correct &= counters[k] == total / ncounters + (k < total % ncounters);

    printf("checksum=%lld expected=%d %s\n",
           (long long)sum, expected,
           correct ? "CORRECT" : "WRONG");

    return correct ? 0 : 2;
}
