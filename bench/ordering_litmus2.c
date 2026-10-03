/* Same-line store-order litmus for the TSO same-line batching rule.
 *
 * The rule lets a younger no-return add to the SAME line leave the store queue
 * while an older add is in flight; stores to any other line still wait for all
 * of them. TSO requires that no observer see a younger store before an older
 * one. Two variants, 2 hardware contexts (main thread writes):
 *
 *   1  writer: add X; add X2 (same line, different word); store FLAG (other line)
 *      reader: wait FLAG == round; X >= round and X2 >= round must both hold
 *   2  writer: add X; add X (same word, twice); store FLAG (other line)
 *      reader: wait FLAG == round; X >= 2*round must hold
 *
 * forbidden counts any observation of the flag with an add not yet visible.
 * Build with -DDSTATE_FENCED to put mfence around each add (control). */
#include "dstate_ops.h"
#include <pthread.h>
#include <string.h>
#define LINE 128
#define LINES 8
#ifndef LITMUS_ROUNDS
#define LITMUS_ROUNDS 20000u
#endif
static uint32_t cells[LINES][LINE / sizeof(uint32_t)] __attribute__((aligned(LINE)));
#define X    (&cells[1][0])
#define X2   (&cells[1][9])
#define FLAG (&cells[5][0])
#define ACK  (&cells[7][0])
static uint32_t sync_count, sync_sense;
static uint64_t forbidden, observed_rounds;

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

static void writer(int variant)
{
    uint32_t sense = 0;
    barrier(&sense);
    for (uint32_t round = 1; round <= LITMUS_ROUNDS; ++round) {
        dstate_add32(X, 1);
        if (variant == 1) dstate_add32(X2, 1); else dstate_add32(X, 1);
        __atomic_store_n(FLAG, round, __ATOMIC_RELEASE);   /* plain mov on x86 */
        while (__atomic_load_n(ACK, __ATOMIC_ACQUIRE) != round) {}
    }
    barrier(&sense);
}

static void *reader(void *arg)
{
    int variant = (int)(uintptr_t)arg;
    uint32_t sense = 0;
    barrier(&sense);
    for (uint32_t round = 1; round <= LITMUS_ROUNDS; ++round) {
        while (__atomic_load_n(FLAG, __ATOMIC_ACQUIRE) != round) {}
        uint32_t x = __atomic_load_n(X, __ATOMIC_RELAXED);
        uint32_t x2 = __atomic_load_n(X2, __ATOMIC_RELAXED);
        ++observed_rounds;
        if (variant == 1 ? (x < round || x2 < round) : (x < 2 * round)) ++forbidden;
        __atomic_store_n(ACK, round, __ATOMIC_RELEASE);
    }
    barrier(&sense);
    return NULL;
}

static int run(int variant)
{
    pthread_t r;
    forbidden = observed_rounds = 0;
    memset(cells, 0, sizeof cells);
    bench_roi_begin();
    bench_thread_check(pthread_create(&r, NULL, reader, (void *)(uintptr_t)variant));
    writer(variant);
    pthread_join(r, NULL);
    bench_roi_end();
    uint32_t want = variant == 1 ? LITMUS_ROUNDS : 2 * LITMUS_ROUNDS;
    printf("variant=%d rounds=%llu forbidden=%llu final_x=%u%s\n", variant,
           (unsigned long long)observed_rounds, (unsigned long long)forbidden,
           *X, forbidden ? " ORDERING-VIOLATION" : " ok");
    if (*X != want || (variant == 1 && *X2 != LITMUS_ROUNDS)) { puts("LOST-UPDATE"); return 1; }
    return forbidden != 0;
}

int main(void)
{
    int bad = run(1);
    bad |= run(2);
    puts(bad ? "FAIL" : "PASS: same-line adds never observed behind a younger flag store");
    return bad;
}
