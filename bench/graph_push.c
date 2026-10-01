/* Push-style graph kernel: a weighted in-degree / contribution scatter.
 * Edges are generated once from a seed with a power-law destination skew (the
 * square of a uniform variate), each thread processes a contiguous slice of the
 * edge list and adds the edge's contribution to the destination's accumulator,
 * one 128-byte line per node. The oracle is a serial pass over the same edges.
 * This is the wide, skewed key space that push-style PageRank / BFS frontiers /
 * histogram building present to the coherence protocol.
 *
 *     graph_push threads edges nodes seed */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
#define MAXN 65536
typedef struct { uint32_t v; char pad[124]; } line_t;
static line_t acc[MAXN] __attribute__((aligned(128)));

static inline uint32_t rng(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s threads edges nodes seed\n", argv[0]); return 1; }
    int th = bench_int(argv[1], 1, 64);
    int ne = bench_int(argv[2], 1, 1 << 26);
    int nn = bench_int(argv[3], 1, MAXN);
    uint32_t seed = (uint32_t)bench_int(argv[4], 1, INT_MAX);
    uint32_t *dst = bench_calloc((size_t)ne, sizeof(uint32_t));
    uint8_t *wgt = bench_calloc((size_t)ne, sizeof(uint8_t));
    uint32_t s = seed * 2654435761u + 12345u;
    for (int e = 0; e < ne; ++e) {
        uint32_t r = rng(&s);
        uint64_t u2 = ((uint64_t)r * (uint64_t)r) >> 32;          /* skew toward low node ids */
        dst[e] = (uint32_t)((u2 * (uint64_t)nn) >> 32);
        wgt[e] = (uint8_t)(1 + (rng(&s) & 7));
    }
    memset(acc, 0, (size_t)nn * sizeof(line_t));
    omp_set_dynamic(0); omp_set_num_threads(th);
    bench_roi_begin();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int lo = (int)((long long)ne * tid / th), hi = (int)((long long)ne * (tid + 1) / th);
        for (int e = lo; e < hi; ++e) dstate_add32(&acc[dst[e]].v, wgt[e]);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    bench_roi_end();
    uint32_t *want = bench_calloc((size_t)nn, sizeof(uint32_t));
    for (int e = 0; e < ne; ++e) want[dst[e]] += wgt[e];
    int ok = 1; long long bad = 0, touched = 0;
    for (int n = 0; n < nn; ++n) { touched += want[n] != 0; if (acc[n].v != want[n]) { ok = 0; ++bad; } }
    printf("graph push oracle: %s (%d edges -> %d nodes, %lld touched, %lld bad)\n", ok ? "CORRECT" : "WRONG", ne, nn, touched, bad);
    return ok ? 0 : 2;
}
