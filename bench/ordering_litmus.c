/* SPDX-License-Identifier: BSD-3-Clause
 * Store-ordering litmus for no-return adds. Directed CPU test, not RubyTester.
 *
 * Writer:  add x, 1 ; store flag = round          (x, flag, ack on different lines)
 * Reader:  wait flag == round ; load x ; require x >= round ; store ack = round
 * Writer:  wait ack == round
 *
 * Under TSO a younger store (flag) may not become visible before an older
 * store-class operation (the add) has taken effect. The early-completion
 * implementation this repository replaced returned "done" to the CPU before
 * the add was applied, so the reader could observe flag == round with a stale
 * x. Variant 2 does the same with two adds to two lines before the flag.
 *
 * The flag line is written only by the writer and the ack line only by the
 * reader, so each stays owned by its writer's L1 and the flag store becomes
 * visible after a single forwarded read, while the delegated add still has to
 * cross the network and invalidate the reader's copy of x. That widens the
 * window in which the historical implementation can be caught; a deterministic
 * simulator that never exposes the window is not proof that the window is
 * closed, so only a FAIL here is conclusive.
 *
 * Two hardware contexts: the main thread is the writer, so run with
 * --num-cpus=2 in gem5 SE mode (each pthread needs its own context).
 *
 * Build variants (see bench/Makefile):
 *   ordering_litmus          explicit AADD encoding via dstate_add32
 *   ordering_litmus_fenced   the same, with -DDSTATE_FENCED (mfence around AADD)
 *   ordering_litmus_lockadd  stock `lock addl` (conventional control; on the
 *                            historical tree this is the repointed early path)
 *
 * Exit status is nonzero on any forbidden observation or lost update. Counts
 * are printed either way so a passing run is distinguishable from one that
 * never raced.
 */
#include "dstate_ops.h"
#include <pthread.h>
#include <string.h>

#ifndef LITMUS_ROUNDS
#define LITMUS_ROUNDS 20000
#endif
#define LINE 128
#define LINES 8

/* One line per slot so x, y, flag and ack sit on different lines and, with
 * interleaved L2 banks, on different home banks. */
static uint32_t cells[LINES][LINE / sizeof(uint32_t)] __attribute__((aligned(LINE)));
#define X    (&cells[0][0])
#define Y    (&cells[2][0])
#define FLAG (&cells[5][0])
#define ACK  (&cells[7][0])

static uint32_t sync_count, sync_sense;
static uint64_t forbidden, observed_rounds;

static void add_one(uint32_t *p)
{
#if defined(LITMUS_LOCK_ADD)
    __asm__ volatile("lock addl $1, %0" : "+m"(*p) : : "memory", "cc");
#else
    dstate_add32(p, 1);
#endif
}

static void barrier(uint32_t *sense)
{
    *sense ^= 1;
    if (__atomic_add_fetch(&sync_count, 1, __ATOMIC_ACQ_REL) == 2) {
        __atomic_store_n(&sync_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&sync_sense, *sense, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&sync_sense, __ATOMIC_ACQUIRE) != *sense) {}
    }
}

/* Variant 1: single add then flag. Variant 2: two adds to two lines then flag. */
static void writer(int two)
{
    uint32_t sense = 0;
    barrier(&sense);
    for (uint32_t round = 1; round <= LITMUS_ROUNDS; ++round) {
        add_one(X);
        if (two) add_one(Y);
        /* RELEASE is a plain mov on x86; no fence instruction is emitted. The
         * "memory" clobber on the add is a compiler barrier only. */
        __atomic_store_n(FLAG, round, __ATOMIC_RELEASE);
        while (__atomic_load_n(ACK, __ATOMIC_ACQUIRE) != round) {}
    }
    barrier(&sense);
}

static void *reader(void *arg)
{
    int two = (int)(uintptr_t)arg;
    uint32_t sense = 0;
    barrier(&sense);
    for (uint32_t round = 1; round <= LITMUS_ROUNDS; ++round) {
        while (__atomic_load_n(FLAG, __ATOMIC_ACQUIRE) != round) {}
        uint32_t x = __atomic_load_n(X, __ATOMIC_RELAXED);
        uint32_t y = two ? __atomic_load_n(Y, __ATOMIC_RELAXED) : round;
        ++observed_rounds;
        if (x < round || y < round) ++forbidden;
        __atomic_store_n(ACK, round, __ATOMIC_RELEASE);
    }
    barrier(&sense);
    return NULL;
}

static int run(int two)
{
    pthread_t r;
    forbidden = observed_rounds = 0;
    memset(cells, 0, sizeof cells);
    bench_roi_begin();
    bench_thread_check(pthread_create(&r, NULL, reader, (void *)(uintptr_t)two));
    writer(two); /* main thread is the writer */
    pthread_join(r, NULL);
    bench_roi_end();
    printf("variant=%d rounds=%llu forbidden=%llu final_x=%u%s\n", two ? 2 : 1,
           (unsigned long long)observed_rounds, (unsigned long long)forbidden,
           *X, forbidden ? " ORDERING-VIOLATION" : " ok");
    if (*X != LITMUS_ROUNDS) { puts("LOST-UPDATE: final x != rounds"); return 1; }
    return forbidden != 0;
}

int main(void)
{
    int bad = run(0);
    bad |= run(1);
    puts(bad ? "FAIL" : "PASS: no younger store observed before an older add");
    return bad;
}
