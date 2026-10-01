/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef DSTATE_OPS_H
#define DSTATE_OPS_H
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef BENCH_GEM5_ROI
#include <gem5/m5ops.h>
#endif
static inline void bench_roi_begin(void)
{
#ifdef BENCH_GEM5_ROI
    puts("ROI_BEGIN: updates plus synchronization and merge; oracle excluded");
    fflush(stdout);
    m5_work_begin(0, 0);
    m5_reset_stats(0, 0);
#endif
}
static inline void bench_roi_end(void)
{
#ifdef BENCH_GEM5_ROI
    m5_dump_stats(0, 0);
    m5_work_end(0, 0);
    puts("ROI_END");
#endif
}

/* Opt-in research encoding; never run the default build on an ordinary host.
 * gem5 implements a timing-CPU-only ADD subset, not complete RAO-INT.
 *
 * Ordering contract: the AADD instruction is unfenced and ordered as a store
 * under TSO -- older loads and stores complete before it is sent, and younger
 * stores are not sent until its acknowledgement returns. That already forbids a
 * later flag store from becoming visible before the add (see ordering_litmus).
 * TSO does let a younger *load* pass it, exactly as for an ordinary store; code
 * needing load-after-add ordering adds a fence, as it would after a store.
 * -DDSTATE_FENCED wraps every AADD in mfence, reproducing the earlier
 * always-fenced macro-op as an ablation column on the same simulator build.
 * DSTATE_NATIVE is an oracle/host-test backend, NOT a delegated implementation.
 */
#if defined(DSTATE_FENCED)
#define DSTATE_FENCE_PRE "mfence\n\t"
#define DSTATE_FENCE_POST "\n\tmfence"
#else
#define DSTATE_FENCE_PRE ""
#define DSTATE_FENCE_POST ""
#endif

static inline void dstate_add32(void *address, uint32_t delta)
{
    uint32_t *p = address;
#if defined(DSTATE_NATIVE)
    (void)__atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
#elif defined(__x86_64__)
    __asm__ volatile(DSTATE_FENCE_PRE ".byte 0x0f,0x38,0xfc,0x08" DSTATE_FENCE_POST
                     : "+m"(*p) : "a"(p), "c"(delta) : "memory");
#else
#error "Use x86-64 for gem5 benchmarks, or DSTATE_NATIVE for host oracle tests"
#endif
}

static inline void dstate_add64(void *address, uint64_t delta)
{
    uint64_t *p = address;
#if defined(DSTATE_NATIVE)
    (void)__atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
#elif defined(__x86_64__)
    __asm__ volatile(DSTATE_FENCE_PRE ".byte 0x48,0x0f,0x38,0xfc,0x08" DSTATE_FENCE_POST
                     : "+m"(*p) : "a"(p), "c"(delta) : "memory");
#else
#error "Use x86-64 or DSTATE_NATIVE"
#endif
}

static inline int32_t conventional_add32(int32_t *p, int32_t delta)
{
#if defined(__x86_64__) && !defined(DSTATE_NATIVE)
    __asm__ volatile("lock xaddl %0,%1"
                     : "+r"(delta), "+m"(*p) : : "memory", "cc");
    return delta;
#else
    return __atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
#endif
}

static inline int bench_int(const char *text, int lo, int hi)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < lo || value > hi) {
        fprintf(stderr, "invalid integer '%s': expected %d..%d\n", text, lo, hi);
        exit(1);
    }
    return (int)value;
}

static inline void *bench_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p) { perror("calloc"); exit(1); }
    return p;
}

static inline void bench_thread_check(int rc)
{
    if (rc) { fprintf(stderr, "pthread failure: %d\n", rc); exit(1); }
}
#endif
