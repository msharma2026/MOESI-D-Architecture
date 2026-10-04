/* Push-style PageRank on a real graph, fixed-point integer accumulation.
 *
 * Reads a SNAP edge list ("from<TAB>to" per line, '#' comments), builds CSR by
 * source, and runs ITER iterations of push PageRank: each thread owns a
 * contiguous range of sources (balanced by out-edges) and adds every source's
 * share  rank[u] / outdeg[u]  to next[v] for each out-edge (u, v) with a
 * no-return 32-bit add. Ranks are Q16 fixed point so the reduction is integer
 * addition, which commutes; the result is therefore deterministic and the
 * oracle is a serial pass over the same CSR. next[] is an unpadded array, the
 * layout a real implementation uses: 32 vertices share each 128-byte line, and
 * high in-degree vertices are hot words.
 *
 *     pagerank_push threads edgelist iterations                                */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>

static uint32_t *csr_off, *csr_dst, *outdeg;
static uint32_t *rank_q16, *next_q16;
static uint32_t nn;
static uint64_t ne;

static void load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    size_t cap = 1 << 20; uint32_t *src = malloc(cap * 4), *dst = malloc(cap * 4);
    char line[256]; uint32_t maxid = 0; ne = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned a, b; if (sscanf(line, "%u %u", &a, &b) != 2) continue;
        if (ne == cap) { cap *= 2; src = realloc(src, cap * 4); dst = realloc(dst, cap * 4); }
        src[ne] = a; dst[ne] = b; ++ne;
        if (a > maxid) maxid = a; if (b > maxid) maxid = b;
    }
    fclose(f);
    nn = maxid + 1;
    csr_off = bench_calloc((size_t)nn + 1, 4); csr_dst = bench_calloc(ne, 4); outdeg = bench_calloc(nn, 4);
    for (uint64_t e = 0; e < ne; ++e) outdeg[src[e]]++;
    for (uint32_t u = 0; u < nn; ++u) csr_off[u + 1] = csr_off[u] + outdeg[u];
    uint32_t *fill = bench_calloc(nn, 4);
    for (uint64_t e = 0; e < ne; ++e) csr_dst[csr_off[src[e]] + fill[src[e]]++] = dst[e];
    free(src); free(dst); free(fill);
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s threads edgelist iterations\n", argv[0]); return 1; }
    int th = bench_int(argv[1], 1, 64);
    int iters = bench_int(argv[3], 1, 64);
    load(argv[2]);
    rank_q16 = bench_calloc(nn, 4); next_q16 = bench_calloc(nn, 4);
    uint32_t *want = bench_calloc(nn, 4), *wnext = bench_calloc(nn, 4);
    const uint32_t base = (uint32_t)(0.15 * 65536.0);        /* (1-d) in Q16 */
    for (uint32_t u = 0; u < nn; ++u) rank_q16[u] = want[u] = 65536;
    /* thread ranges balanced by out-edges */
    uint32_t *lo = bench_calloc((size_t)th + 1, 4);
    for (int t = 1; t < th; ++t) {
        uint64_t target = ne * (uint64_t)t / th; uint32_t u = lo[t - 1];
        while (u < nn && csr_off[u] < target) ++u;
        lo[t] = u;
    }
    lo[th] = nn;
    omp_set_dynamic(0); omp_set_num_threads(th);
    bench_roi_begin();
    for (int it = 0; it < iters; ++it) {
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            for (uint32_t u = lo[tid]; u < lo[tid + 1]; ++u) {
                if (!outdeg[u]) continue;
                uint32_t share = rank_q16[u] / outdeg[u];
                for (uint32_t e = csr_off[u]; e < csr_off[u + 1]; ++e) dstate_add32(&next_q16[csr_dst[e]], share);
            }
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
        #pragma omp parallel for schedule(static)
        for (uint32_t u = 0; u < nn; ++u) {              /* rank = (1-d) + d * next, d = 0.85 */
            rank_q16[u] = base + (uint32_t)(((uint64_t)next_q16[u] * 55706u) >> 16);
            next_q16[u] = 0;
        }
    }
    bench_roi_end();
    /* serial oracle over the same CSR, same arithmetic */
    for (int it = 0; it < iters; ++it) {
        for (uint32_t u = 0; u < nn; ++u) {
            if (!outdeg[u]) continue;
            uint32_t share = want[u] / outdeg[u];
            for (uint32_t e = csr_off[u]; e < csr_off[u + 1]; ++e) wnext[csr_dst[e]] += share;
        }
        for (uint32_t u = 0; u < nn; ++u) { want[u] = base + (uint32_t)(((uint64_t)wnext[u] * 55706u) >> 16); wnext[u] = 0; }
    }
    long long bad = 0; uint64_t sum = 0, top = 0; uint32_t topv = 0;
    for (uint32_t u = 0; u < nn; ++u) { sum += rank_q16[u]; if (rank_q16[u] != want[u]) ++bad; if (rank_q16[u] > top) { top = rank_q16[u]; topv = u; } }
    printf("pagerank push oracle: %s (%u nodes, %llu edges, %d iterations, rank sum %llu, top vertex %u, %lld bad)\n",
           bad ? "WRONG" : "CORRECT", nn, (unsigned long long)ne, iters, (unsigned long long)sum, topv, bad);
    return bad ? 2 : 0;
}
