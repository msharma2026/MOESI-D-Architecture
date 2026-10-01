/* SPDX-License-Identifier: BSD-3-Clause
 * Directed CPU requests, NOT RubyTester: run unchanged under enabled/off,
 * persistence-off and forced-NACK settings. Native backend tests the oracle
 * and synchronization only; it does not validate the gem5 protocol.
 */
#include "dstate_ops.h"
#include <pthread.h>
#include <string.h>

enum { THREADS = 4, CELLS = 32, ITERS = 4096, ROUNDS = 128 };
static uint32_t values[CELLS] __attribute__((aligned(128)));
static uint32_t expected[THREADS][CELLS];
static uint64_t wide __attribute__((aligned(128)));
static uint32_t publication __attribute__((aligned(128)));
static uint32_t published_value __attribute__((aligned(128)));
static uint32_t sync_count, sync_sense;
static uint32_t observed[THREADS];

static void barrier(uint32_t *sense)
{
    *sense ^= 1;
    if (__atomic_add_fetch(&sync_count, 1, __ATOMIC_ACQ_REL) == THREADS) {
        __atomic_store_n(&sync_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&sync_sense, *sense, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&sync_sense, __ATOMIC_ACQUIRE) != *sense) {}
    }
}

static uint32_t rng(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

static void *worker(void *arg)
{
    unsigned tid = (unsigned)(uintptr_t)arg;
    uint32_t sense = 0, seed = 0x9e3779b9u ^ (tid + 1);
    barrier(&sense);
    for (int i = 0; i < ITERS; ++i) {
        unsigned slot = rng(&seed) % CELLS;
        uint32_t delta = rng(&seed); /* overflow and adjacent-word isolation */
        expected[tid][slot] += delta;
        dstate_add32(&values[slot], delta);
        dstate_add64(&wide, UINT64_C(0x100000001));
        if (!(i % 8))
            observed[tid] ^= __atomic_load_n(&values[(slot + 1) % CELLS], __ATOMIC_RELAXED);
    }
    barrier(&sense);
    if (tid == 0) {
        for (unsigned k = 0; k < CELLS; ++k) {
            uint32_t want = 0;
            for (unsigned t = 0; t < THREADS; ++t) want += expected[t][k];
            if (values[k] != want) abort();
        }
    }
    barrier(&sense);
    /* One writer, multiple readers, then a different updater. Each round
     * exercises ownership changes; scheduling/state coverage needs traces.
     */
    for (uint32_t round = 0; round < ROUNDS; ++round) {
        if (tid == 0) __atomic_store_n(&values[0], round, __ATOMIC_SEQ_CST);
        barrier(&sense);
        if (__atomic_load_n(&values[0], __ATOMIC_ACQUIRE) != round) abort();
        barrier(&sense);
        if (tid == 1) dstate_add32(&values[0], 7);
        barrier(&sense);
        if (__atomic_load_n(&values[0], __ATOMIC_ACQUIRE) != round + 7) abort();
        barrier(&sense);
    }
    /* A release flag cannot become visible before the prior update. */
    if (tid == 0) {
        for (uint32_t round = 1; round <= ROUNDS; ++round) {
            while (__atomic_load_n(&publication, __ATOMIC_ACQUIRE) != 0) {}
            dstate_add32(&published_value, 1);
            __atomic_store_n(&publication, round, __ATOMIC_RELEASE);
        }
    } else if (tid == 1) {
        for (uint32_t round = 1; round <= ROUNDS; ++round) {
            while (__atomic_load_n(&publication, __ATOMIC_ACQUIRE) != round) {}
            if (__atomic_load_n(&published_value, __ATOMIC_RELAXED) != round) abort();
            __atomic_store_n(&publication, 0, __ATOMIC_RELEASE);
        }
    }
    barrier(&sense);
    return NULL;
}

static void locked_add_compatibility(void)
{
#if defined(__x86_64__)
    struct { uint8_t byte, guard; uint16_t word, guard2; } v = {255, 0xa5, 65535, 0x5aa5};
    uint8_t z, c;
    __asm__ volatile("lock addb $1,%0; setz %1; setc %2"
                     : "+m"(v.byte), "=qm"(z), "=qm"(c) : : "memory", "cc");
    if (v.byte || !z || !c || v.guard != 0xa5 || v.word != 65535) abort();
    __asm__ volatile("lock addw $1,%0; setz %1; setc %2"
                     : "+m"(v.word), "=qm"(z), "=qm"(c) : : "memory", "cc");
    if (v.word || !z || !c || v.guard2 != 0x5aa5) abort();
#endif
}

int main(void)
{
    locked_add_compatibility();
    pthread_t threads[THREADS];
    for (unsigned t = 0; t < THREADS; ++t)
        bench_thread_check(pthread_create(&threads[t], NULL, worker, (void *)(uintptr_t)t));
    for (unsigned t = 0; t < THREADS; ++t) bench_thread_check(pthread_join(threads[t], NULL));
    /* Cell 0 was deliberately overwritten by the ownership test. */
    for (unsigned k = 1; k < CELLS; ++k) {
        uint32_t want = 0;
        for (unsigned t = 0; t < THREADS; ++t) want += expected[t][k];
        if (values[k] != want) { fprintf(stderr, "cell %u mismatch\n", k); return 2; }
    }
    if (values[0] != ROUNDS - 1 + 7 || published_value != ROUNDS ||
        wide != UINT64_C(0x100000001) * THREADS * ITERS) return 2;
    puts("CORRECT: widths, modular adds, multiple readers, ordinary stores, publication");
    return 0;
}
