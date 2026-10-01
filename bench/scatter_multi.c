/* Working-set capacity / bank-pressure update microbenchmark. */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
typedef struct { int32_t v; char pad[124]; } line_t;
#define MAXN 16384
static line_t ctr[MAXN] __attribute__((aligned(128)));
static inline void add(int32_t *p) {
#ifdef BASELINE
    (void)conventional_add32(p, 1);
#else
    dstate_add32(p, 1);
#endif
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s threads iters lines\n", argv[0]); return 1; }
    int th = bench_int(argv[1], 1, 64);
    int it = bench_int(argv[2], 1, INT_MAX / th);
    int nl = bench_int(argv[3], 1, MAXN);
    omp_set_dynamic(0); omp_set_num_threads(th);
    memset(ctr, 0, (size_t)nl * sizeof(*ctr));
    bench_roi_begin();
    #pragma omp parallel
    { for (int i = 0; i < it; ++i) for (int k = 0; k < nl; ++k) add(&ctr[k].v); }
    bench_roi_end();
    int ok = 1;
    for (int k = 0; k < nl; ++k) ok &= ctr[k].v == th * it;
    printf("per-cell oracle: %s (%d cells)\n", ok ? "CORRECT" : "WRONG", nl);
    return ok ? 0 : 2;
}
