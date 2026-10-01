/* Integer/pattern Matrix Market SpMV, a single-writer negative control.
 * No floating-point approximation, zero substitution, or general SpMV claim.
 * Arithmetic is explicitly modulo 2^32; every output is independently checked.
 */
#include "dstate_ops.h"
#include <string.h>
#include <omp.h>
#define LINE_ROWS 32

static void input_error(void) {
    fputs("Invalid/unsupported Matrix Market input: expected coordinate integer or pattern, general or symmetric\n", stderr);
    exit(1);
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s threads iters matrix.mtx\n", argv[0]); return 1; }
    int threads = bench_int(argv[1], 1, 64);
    int iters = bench_int(argv[2], 1, INT_MAX);
    FILE *f = fopen(argv[3], "r");
    if (!f) { perror("matrix"); return 1; }
    char line[512], object[32], format[32], field[32], symmetry[32];
    if (!fgets(line, sizeof line, f) ||
        sscanf(line, "%%%%MatrixMarket %31s %31s %31s %31s", object, format, field, symmetry) != 4 ||
        strcmp(object, "matrix") || strcmp(format, "coordinate") ||
        (strcmp(field, "integer") && strcmp(field, "pattern")) ||
        (strcmp(symmetry, "general") && strcmp(symmetry, "symmetric"))) input_error();
    int pattern = !strcmp(field, "pattern"), sym = !strcmp(symmetry, "symmetric");
    do { if (!fgets(line, sizeof line, f)) input_error(); } while (line[0] == '%');
    long long rows, cols, count;
    if (sscanf(line, "%lld %lld %lld", &rows, &cols, &count) != 3 ||
        rows < 1 || cols < 1 || rows > 10000000 || cols > 10000000 ||
        count < 0 || count > 50000000 || (sym && rows != cols)) input_error();
    int R = (int)rows, C = (int)cols;
    size_t cap = (size_t)count * (sym ? 2 : 1) + 1, nz = 0;
    int *rr = bench_calloc(cap, sizeof(*rr)), *cc = bench_calloc(cap, sizeof(*cc));
    uint32_t *vv = bench_calloc(cap, sizeof(*vv));
    uint32_t *oracle = bench_calloc((size_t)R, sizeof(*oracle));
    for (long long k = 0; k < count; ++k) {
        long long r, c, v = 1;
        do { if (!fgets(line, sizeof line, f)) input_error(); } while (line[0] == '%');
        int consumed = 0;
        int fields = pattern ? sscanf(line, "%lld %lld %n", &r, &c, &consumed) : sscanf(line, "%lld %lld %lld %n", &r, &c, &v, &consumed);
        if (fields != (pattern ? 2 : 3) || r < 1 || r > R || c < 1 || c > C || v < INT32_MIN || v > INT32_MAX) input_error();
        if (!consumed || line[consumed]) input_error();
        --r; --c;
        rr[nz] = (int)r; cc[nz] = (int)c; vv[nz++] = (uint32_t)v;
        oracle[r] += (uint32_t)v * (uint32_t)(c % 5 + 1);
        if (sym && r != c) {
            rr[nz] = (int)c; cc[nz] = (int)r; vv[nz++] = (uint32_t)v;
            oracle[c] += (uint32_t)v * (uint32_t)(r % 5 + 1);
        }
    }
    /* Reject unaccounted entries rather than silently ignoring a wrong nnz. */
    while (fgets(line, sizeof line, f))
        if (line[0] != '%' && strspn(line, " \t\r\n") != strlen(line)) input_error();
    fclose(f);
    size_t *row_ptr = bench_calloc((size_t)R + 1, sizeof(*row_ptr));
    for (size_t k = 0; k < nz; ++k) row_ptr[rr[k] + 1]++;
    for (int r = 0; r < R; ++r) row_ptr[r + 1] += row_ptr[r];
    size_t *cursor = bench_calloc((size_t)R, sizeof(*cursor));
    memcpy(cursor, row_ptr, (size_t)R * sizeof(*cursor));
    int *col_idx = bench_calloc(nz + 1, sizeof(*col_idx));
    uint32_t *val = bench_calloc(nz + 1, sizeof(*val));
    for (size_t k = 0; k < nz; ++k) {
        size_t p = cursor[rr[k]]++;
        col_idx[p] = cc[k]; val[p] = vv[k];
    }
    free(rr); free(cc); free(vv); free(cursor);
    uint32_t *x = bench_calloc((size_t)C, sizeof(*x));
    for (int c = 0; c < C; ++c) x[c] = (uint32_t)(c % 5 + 1);
    size_t padded = ((size_t)R + LINE_ROWS - 1) / LINE_ROWS * LINE_ROWS;
    uint32_t *y = aligned_alloc(128, padded * sizeof(*y));
    if (!y) { perror("aligned_alloc"); return 1; }
    memset(y, 0, padded * sizeof(*y));
    int rpt = ((R + threads - 1) / threads + LINE_ROWS - 1) / LINE_ROWS * LINE_ROWS;
    omp_set_dynamic(0); omp_set_num_threads(threads);
    bench_roi_begin();
    #pragma omp parallel
    {
        int lo = omp_get_thread_num() * rpt, hi = lo + rpt;
        if (hi > R) hi = R;
        for (int i = 0; i < iters; ++i) {
            for (int r = lo; r < hi; ++r) {
                uint32_t sum = 0;
                for (size_t p = row_ptr[r]; p < row_ptr[r + 1]; ++p) sum += val[p] * x[col_idx[p]];
#ifdef BASELINE
                y[r] += sum;
#else
                dstate_add32(&y[r], sum);
#endif
            }
            #pragma omp barrier
        }
    }
    bench_roi_end();
    int ok = 1;
    for (int r = 0; r < R; ++r) ok &= y[r] == oracle[r] * (uint32_t)iters;
    printf("integer SpMV per-row oracle: %s (%d rows)\n", ok ? "CORRECT" : "WRONG", R);
    free(y); free(x); free(oracle); free(row_ptr); free(col_idx); free(val);
    return ok ? 0 : 2;
}
