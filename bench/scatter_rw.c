/* Updates plus ordinary atomic loads; these reads do not force an L2 access. */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
static int32_t ctr[32] __attribute__((aligned(128)));
static inline void add(int32_t *p) {
#ifdef BASELINE
    (void)conventional_add32(p, 1);
#else
    dstate_add32(p, 1);
#endif
}
int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) { fprintf(stderr, "usage: %s threads iters counters [read_freq]\n", argv[0]); return 1; }
    int th = bench_int(argv[1], 1, 64);
    int it = bench_int(argv[2], 1, INT_MAX / th);
    int nc = bench_int(argv[3], 1, 32);
    int rf = argc > 4 ? bench_int(argv[4], 0, INT_MAX) : 8;
    uint64_t sink = 0;
    omp_set_dynamic(0); omp_set_num_threads(th);
    memset(ctr, 0, sizeof(ctr));
    bench_roi_begin();
    #pragma omp parallel reduction(+:sink)
    {
        for (int i = 0; i < it; ++i) {
            add(&ctr[i % nc]);
            if (rf && i % rf == 0)
                for (int k = 0; k < nc; ++k)
                    sink += (uint32_t)__atomic_load_n(&ctr[k], __ATOMIC_RELAXED);
        }
    }
    bench_roi_end();
    int ok = 1;
    for (int k = 0; k < nc; ++k) ok &= ctr[k] == th * (it / nc + (k < it % nc));
    printf("per-cell oracle: %s read_sink=%llu\n", ok ? "CORRECT" : "WRONG", (unsigned long long)sink);
    return ok ? 0 : 2;
}
