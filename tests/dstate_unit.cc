// Tests the ACTUAL byte-level helper and executor model called by gem5, not a
// replica. Compiles standalone (no gem5 headers): the MachineID-based merge set
// is guarded out of the header in that case and is exercised in simulation by
// the coherence_regression / scatter runs with DSTATE_MERGE_LIMIT set.
#include "DStateEngine.hh"
#include <array>
#include <iostream>

int main()
{
    using namespace gem5::ruby;
    DStateEngine engine;

    // Bounded admission: capacity is a call argument (the controller parameter).
    assert(engine.hasCapacity(1) && engine.occupancy() == 0);
    engine.reserve(); assert(!engine.hasCapacity(1) && engine.hasCapacity(2) && engine.occupancy() == 1);
    engine.reserve(); assert(!engine.hasCapacity(2) && engine.occupancy() == 2);
    engine.release(); engine.release(); assert(engine.hasCapacity(1) && engine.occupancy() == 0);

    // Initiation interval: back-to-back starts are spaced by `interval`, never
    // earlier than `now`; interval == latency reproduces the non-pipelined model.
    assert(engine.scheduleStart(100, 42) == 100);
    assert(engine.scheduleStart(100, 42) == 142);
    assert(engine.scheduleStart(100, 42) == 184);
    assert(engine.scheduleStart(1000, 42) == 1000);   // idle gap resets to now
    assert(engine.scheduleStart(1000, 4) == 1042);    // interval of the previous start
    assert(engine.scheduleStart(1000, 4) == 1046);

    // Hot-word buffer: LRU over (line, offset), capacity from the parameter,
    // 0 disables, dropping a line removes all of its words.
    assert(!engine.hotWordHit(0x1000, 0));
    engine.hotWordTouch(0x1000, 0, 2);
    assert(engine.hotWordHit(0x1000, 0) && !engine.hotWordHit(0x1000, 4) && !engine.hotWordHit(0x2000, 0));
    engine.hotWordTouch(0x1000, 4, 2);
    engine.hotWordTouch(0x2000, 0, 2);                 // evicts (0x1000,0), the LRU
    assert(!engine.hotWordHit(0x1000, 0) && engine.hotWordHit(0x1000, 4) && engine.hotWordHit(0x2000, 0));
    engine.hotWordTouch(0x1000, 4, 2);                 // touch moves to MRU
    engine.hotWordTouch(0x3000, 0, 2);                 // evicts (0x2000,0)
    assert(engine.hotWordHit(0x1000, 4) && !engine.hotWordHit(0x2000, 0) && engine.hotWordHit(0x3000, 0));
    engine.hotWordDrop(0x1000);
    assert(!engine.hotWordHit(0x1000, 4) && engine.hotWordHit(0x3000, 0));
    engine.hotWordTouch(0x3000, 0, 0);                 // capacity 0 disables and clears
    assert(!engine.hotWordHit(0x3000, 0));

    assert(engine.nextId() == 1 && engine.nextId() == 2);

    for (int block : {64, 128, 256}) {
        for (int width : {4, 8}) {
            for (int off = 0; off <= block - width; off += width) {
                std::array<uint8_t, 256> data, operand;
                data.fill(0xa5); operand.fill(0);
                for (int i = 0; i < width; ++i) data[off + i] = 0xff;
                operand[off] = 1;
                dstateAdd(data.data(), operand.data(), block, off, width);
                for (int i = 0; i < 256; ++i)
                    assert(data[i] == (i >= off && i < off + width ? 0 : 0xa5));
                operand[off] = 0;
                operand[off + width - 1] = 0x80;
                dstateAdd(data.data(), operand.data(), block, off, width);
                assert(data[off + width - 1] == 0x80);
                dstateAdd(data.data(), operand.data(), block, off, width);
                assert(data[off + width - 1] == 0);
            }
        }
    }
    // Combining folds a delta into a saved operand with the same helper; two
    // merged +1s and the original +1 must apply as exactly +3, modulo the width.
    {
        std::array<uint8_t, 128> operand{}, delta{}, line{};
        operand[8] = 1; delta[8] = 1;
        dstateAdd(operand.data(), delta.data(), 128, 8, 4);
        dstateAdd(operand.data(), delta.data(), 128, 8, 4);
        line[8] = 0xfe; line[9] = 0xff; line[10] = 0xff; line[11] = 0xff;   // 0xfffffffe + 3 wraps to 1
        dstateAdd(line.data(), operand.data(), 128, 8, 4);
        assert(line[8] == 1 && line[9] == 0 && line[10] == 0 && line[11] == 0 && line[12] == 0);
    }
    {   // change-rate gate: round-robin writers read 100 %, same-core runs read low,
        // GETX migrations count like delegated updates, idle resets, halving keeps a
        // steady ratio steady, and the adaptive threshold steps only after cooling off.
        DStateEngine g;
        const std::uint64_t L = 0x1000, M = 0x2000;
        for (int i = 0; i < 64; ++i) g.noteAccess(L, i % 4, 1000 + i, 64, 0, DStateEngine::Accepted);
        assert(g.changePct(L, 0, 1100, 0) >= 95);
        assert(g.changesAfter(L, 0, 1100, 0) >= 2);
        for (int i = 0; i < 64; ++i) g.noteAccess(M, i / 16, 1000 + i, 64, 0, DStateEngine::Rejected);
        assert(g.changePct(M, 3, 1100, 0) <= 15);          // 4 runs of 16: ~4 changes per 64 updates
        assert(g.changesAfter(M, 3, 1100, 0) < 6);
        // migrations observed in conventional mode are the same signal
        DStateEngine h;
        for (int i = 0; i < 32; ++i) h.noteAccess(L, i % 2, 10 + i, 64, 0, DStateEngine::Migration);
        assert(h.changePct(L, 0, 50, 0) >= 95 && h.writersAfter(L, 0, 50, 0) == 2);
        // a fresh line reads 100 % but has only one change: the floor rejects it
        assert(h.changePct(M, 5, 50, 0) == 100 && h.changesAfter(M, 5, 50, 0) == 1);
        // idle decay: after the idle gap the line restarts as single-writer
        assert(h.writersAfter(L, 0, 50, 1000) == 2);
        assert(h.writersAfter(L, 0, 5000, 1000) == 1 && h.changesAfter(L, 0, 5000, 1000) == 1);
        // halving every epoch keeps the ratio of a steady line steady
        for (int i = 0; i < 1000; ++i) g.noteAccess(L, i % 4, 2000 + i, 16, 0, DStateEngine::Accepted);
        assert(g.changePct(L, 0, 3100, 0) >= 90);
        // adaptive threshold: off -> configured; on -> steps down by 10 after a window of
        // contested rejections, then holds through the cooling-off period
        assert(g.gatePct(50, false) == 50);
        DStateEngine a;
        assert(a.gatePct(50, true) == 50);
        for (int i = 0; i < 1024; ++i) a.noteAccess(L + 128 * (i % 8), (i / 8) % 4, 100 + i, 64, 0, DStateEngine::Rejected);
        assert(a.gatePct(50, true) == 40);
        for (int w = 0; w < 8; ++w)
            for (int i = 0; i < 1024; ++i) a.noteAccess(L + 128 * (i % 8), (i / 8) % 4, 5000 + i, 64, 0, DStateEngine::Rejected);
        assert(a.gatePct(50, true) == 40);                  // cooling off: no further step
        for (int i = 0; i < 1024; ++i) a.noteAccess(L + 128 * (i % 8), (i / 8) % 4, 20000 + i, 64, 0, DStateEngine::Rejected);
        assert(a.gatePct(50, true) == 30);
        for (int w = 0; w < 9; ++w)
            for (int i = 0; i < 1024; ++i) a.noteAccess(L + 128 * (i % 8), (i / 8) % 4, 30000 + i, 64, 0, DStateEngine::Accepted);
        assert(a.gatePct(50, true) == 40);                  // steps back toward configured, never above
    }
    {   // owner-tenure predictor: unknown -> -1 (delegate); 5 adds then taken -> 5;
        // one add then taken -> 1; a recorded tenure expires after the idle window
        DStateEngine t;
        const std::uint64_t L = 0x3000;
        assert(t.predictedTenure(L, 10, 0) == -1);
        for (int i = 0; i < 5; ++i) t.noteOwnerAdd(L);
        assert(t.predictedTenure(L, 10, 0) == -1);          // still owned: nothing recorded yet
        t.noteOwnershipLost(L, 100);
        assert(t.predictedTenure(L, 150, 0) == 5);
        t.noteOwnerAdd(L);
        t.noteOwnershipLost(L, 200);
        assert(t.predictedTenure(L, 250, 0) == 1);
        assert(t.predictedTenure(L, 250, 1000) == 1 && t.predictedTenure(L, 5000, 1000) == -1);
    }
    std::cout << "PASS: admission, initiation interval, hot-word LRU/drop, IDs, ADD32/64 wrap, "
                 "every aligned offset and neighboring bytes, merged-operand arithmetic, "
                 "change-rate gate (ratio, migrations, floor, idle decay, halving, adaptive cooling-off), "
                 "owner-tenure predictor\n";
}
