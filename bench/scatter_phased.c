/* Phased update/read microbenchmark: the workload the D (retention) policy is
 * for. In each phase every thread adds 1 to every one of N lines (its own
 * 128 B line each), then every thread reads every line. Reads pull each line
 * into the readers' L1s; the next update phase measures whether retaining the
 * home copy (persistent mode) beats re-acquiring it (remote mode). */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
typedef struct { int32_t v; char pad[124]; } line_t;
#define MAXN 16384
static line_t ctr[MAXN] __attribute__((aligned(128)));
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s threads iters lines phases\n", argv[0]);
        return 1;
    }
    int th = bench_int(argv[1], 1, 64);
    int it = bench_int(argv[2], 1, 1 << 20);
    int nl = bench_int(argv[3], 1, MAXN);
    int ph = bench_int(argv[4], 1, 1 << 10);
    omp_set_dynamic(0); omp_set_num_threads(th);
    memset(ctr, 0, (size_t)nl * sizeof(*ctr));
    long long sink = 0;
    bench_roi_begin();
    for (int p = 0; p < ph; ++p) {
        #pragma omp parallel
        { for (int i = 0; i < it; ++i) for (int k = 0; k < nl; ++k) dstate_add32(&ctr[k].v, 1); }
        /* implicit barrier: all adds of this phase are complete before any read */
        #pragma omp parallel reduction(+:sink)
        { for (int k = 0; k < nl; ++k) sink += __atomic_load_n(&ctr[k].v, __ATOMIC_RELAXED); }
    }
    bench_roi_end();
    int ok = 1;
    for (int k = 0; k < nl; ++k) ok &= ctr[k].v == th * it * ph;
    /* read phase p (1-based) sees th*it*p in each of nl lines, read by th threads */
    long long expect = (long long)th * th * it * nl * ph * (ph + 1) / 2;
    ok &= sink == expect;
    printf("per-cell oracle: %s (%d cells, %d phases) read_sink=%lld expected=%lld\n",
           ok ? "CORRECT" : "WRONG", nl, ph, sink, expect);
    return ok ? 0 : 2;
}
