/* Mixed per-line-regime microbenchmark: two kinds of shared line in one program.
 *
 *   Region A (mapped at 512 MB): `la` lines every thread adds to in rotation
 *            and nobody reads until the end -- the many-writer regime home
 *            execution wins.
 *   Region B (mapped at 768 MB): `lb` lines every thread READS every
 *            iteration, and one thread adds to once every `wr` iterations --
 *            the read-mostly regime where cached copies win and a line kept
 *            at the home (D, far reads) can lose.
 *
 * The fixed mappings let DSTATE_RANGE_LO_MB/HI_MB give each region its best
 * static treatment (delegate A only: 512..768), which bounds what a dynamic
 * placement policy could earn. B's reads race with its adds, so only the final
 * values are checked exactly; the read sink is bounded. */
#include "dstate_ops.h"
#include <string.h>
#include <sys/mman.h>
#include <omp.h>
typedef struct { int32_t v; char pad[124]; } line_t;
#define REGION_A ((void *)0x20000000UL)   /* 512 MB */
#define REGION_B ((void *)0x30000000UL)   /* 768 MB */
#define MAXL 4096

static line_t *map_fixed(void *at, size_t lines) {
    size_t bytes = (lines * sizeof(line_t) + 4095) & ~(size_t)4095;
    void *p = mmap(at, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != at) { fprintf(stderr, "mmap(MAP_FIXED) at %p failed\n", at); exit(1); }
    memset(p, 0, bytes);
    return (line_t *)p;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s threads iters linesA linesB write_every\n", argv[0]);
        return 1;
    }
    int th = bench_int(argv[1], 1, 64);
    int it = bench_int(argv[2], 1, 1 << 20);
    int la = bench_int(argv[3], 1, MAXL);
    int lb = bench_int(argv[4], 1, MAXL);
    int wr = bench_int(argv[5], 1, 1 << 20);
    line_t *A = map_fixed(REGION_A, (size_t)la);
    line_t *B = map_fixed(REGION_B, (size_t)lb);
    omp_set_dynamic(0); omp_set_num_threads(th);
    long long sink = 0;
    bench_roi_begin();
    #pragma omp parallel reduction(+:sink)
    {
        int tid = omp_get_thread_num();
        for (int i = 0; i < it; ++i) {
            dstate_add32(&A[(i + tid) % la].v, 1);               /* shared, write-only */
            for (int j = 0; j < lb; ++j)
                sink += __atomic_load_n(&B[j].v, __ATOMIC_RELAXED); /* shared, read-mostly */
            if (i % wr == 0) dstate_add32(&B[(i / wr + tid) % lb].v, 1); /* rare write */
        }
    }
    bench_roi_end();
    int ok = 1;
    for (int k = 0; k < la; ++k) {
        long long want = 0;
        for (int t = 0; t < th; ++t) for (int i = 0; i < it; ++i) want += ((i + t) % la == k);
        ok &= A[k].v == want;
    }
    long long bwrites = 0;
    for (int j = 0; j < lb; ++j) {
        long long want = 0;
        for (int t = 0; t < th; ++t) for (int i = 0; i < it; i += wr) want += ((i / wr + t) % lb == j);
        ok &= B[j].v == want;
        bwrites += want;
    }
    /* every B read returns a value between 0 and that line's final count */
    long long upper = 0;
    for (int j = 0; j < lb; ++j) upper += (long long)th * it * B[j].v;
    ok &= sink >= 0 && sink <= upper;
    printf("per-cell oracle: %s (%d write-only + %d read-mostly lines, %lld rare writes) read_sink=%lld (<= %lld)\n",
           ok ? "CORRECT" : "WRONG", la, lb, bwrites, sink, upper);
    return ok ? 0 : 2;
}
