/* Randomized stress program for the delegated path. Every thread performs a
 * seeded random mix of 32-bit adds (bytes 0..63 of a line), 64-bit adds (bytes
 * 64..127), loads of random words, full fences, and plain stores to a private
 * line, over `lines` lines (large values exceed the L2 and force evictions of
 * retained lines). Widths never share bytes, so the final value of every word is
 * the exact sum of the deltas recorded per thread; the check runs after the
 * parallel region. Different seeds give different interleavings.
 *
 *     random_stress threads iters lines seed */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
#define MAXL 16384
typedef struct { uint32_t w32[16]; uint64_t w64[8]; } line_t;   /* 128 bytes */
static line_t lines_[MAXL] __attribute__((aligned(128)));
static line_t priv_[64] __attribute__((aligned(128)));

static inline uint32_t rng(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s threads iters lines seed\n", argv[0]); return 1; }
    int th = bench_int(argv[1], 1, 64);
    int it = bench_int(argv[2], 1, 1 << 24);
    int nl = bench_int(argv[3], 1, MAXL);
    uint32_t seed = (uint32_t)bench_int(argv[4], 1, INT_MAX);
    memset(lines_, 0, (size_t)nl * sizeof(line_t));
    memset(priv_, 0, sizeof(priv_));
    /* per-thread expectation tables */
    uint32_t *e32 = bench_calloc((size_t)th * nl * 16, sizeof(uint32_t));
    uint64_t *e64 = bench_calloc((size_t)th * nl * 8, sizeof(uint64_t));
    long long sink = 0; int priv_ok = 1;
    omp_set_dynamic(0); omp_set_num_threads(th);
    bench_roi_begin();
    #pragma omp parallel reduction(+:sink) reduction(&:priv_ok)
    {
        int tid = omp_get_thread_num();
        uint32_t s = seed * 2654435761u ^ (uint32_t)(tid + 1) * 0x9e3779b9u;
        uint32_t *m32 = e32 + (size_t)tid * nl * 16;
        uint64_t *m64 = e64 + (size_t)tid * nl * 8;
        uint32_t pv = 0;
        for (int i = 0; i < it; ++i) {
            uint32_t r = rng(&s);
            int line = (int)((uint64_t)rng(&s) * (uint64_t)nl >> 32);
            switch (r & 15) {
            case 0: case 1: case 2: case 3: case 4: case 5: {          /* 32-bit add */
                int w = (r >> 4) & 15; uint32_t d = rng(&s);
                dstate_add32(&lines_[line].w32[w], d); m32[(size_t)line * 16 + w] += d; break; }
            case 6: case 7: case 8: case 9: {                           /* 64-bit add */
                int w = (r >> 4) & 7; uint64_t d = ((uint64_t)rng(&s) << 32) | rng(&s);
                dstate_add64(&lines_[line].w64[w], d); m64[(size_t)line * 8 + w] += d; break; }
            case 10: case 11: case 12:                                  /* load a random word */
                sink += __atomic_load_n(&lines_[line].w32[(r >> 4) & 15], __ATOMIC_RELAXED); break;
            case 13:                                                    /* full fence */
                __atomic_thread_fence(__ATOMIC_SEQ_CST); break;
            default: {                                                  /* private store/load */
                pv += r; priv_[tid].w32[0] = pv;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                priv_ok &= (priv_[tid].w32[0] == pv); break; }
            }
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* relaxed-ordering contract: drain before publishing */
    }
    bench_roi_end();
    int ok = priv_ok;
    long long bad = 0;
    for (int l = 0; l < nl; ++l) {
        for (int w = 0; w < 16; ++w) {
            uint32_t want = 0;
            for (int t = 0; t < th; ++t) want += e32[((size_t)t * nl + l) * 16 + w];
            if (lines_[l].w32[w] != want) { ok = 0; ++bad; }
        }
        for (int w = 0; w < 8; ++w) {
            uint64_t want = 0;
            for (int t = 0; t < th; ++t) want += e64[((size_t)t * nl + l) * 8 + w];
            if (lines_[l].w64[w] != want) { ok = 0; ++bad; }
        }
    }
    printf("random stress oracle: %s (%d lines x 24 words, %d threads x %d ops, seed %u, %lld bad words) read_sink=%lld\n",
           ok ? "CORRECT" : "WRONG", nl, th, it, seed, bad, sink);
    return ok ? 0 : 2;
}
