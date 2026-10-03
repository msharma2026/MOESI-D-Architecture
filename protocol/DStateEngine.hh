// SPDX-License-Identifier: BSD-3-Clause
// Finite-capacity bank executor reference model, not a synthesized datapath.
// Capacity, initiation interval, service latencies and the hot-word buffer are
// configuration inputs for sensitivity studies; the defaults selected in
// MOESI_D.py reproduce a single non-pipelined accepted operation per bank with a
// flat service time. Compiles standalone (tests/dstate_unit.cc); the merge set
// for combining lives in DStateMergeSet.hh because it needs MachineID.
#ifndef MOESI_D_ENGINE_HH
#define MOESI_D_ENGINE_HH

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <ostream>
#include <utility>
#include <unordered_map>
#include <vector>

namespace gem5::ruby
{
// Shared by DataBlock and the standalone datapath boundary tests.
inline void dstateAdd(std::uint8_t *data, const std::uint8_t *operand,
                      int blockSize, int offset, int size)
{
    assert(data && operand);
    assert((size == 4 || size == 8) && offset >= 0 && offset % size == 0);
    assert(size <= blockSize && offset <= blockSize - size);
    std::uint64_t value = 0, delta = 0;
    for (int byte = 0; byte < size; ++byte) {
        value |= std::uint64_t(data[offset + byte]) << (8 * byte);
        delta |= std::uint64_t(operand[offset + byte]) << (8 * byte);
    }
    value += delta;
    for (int byte = 0; byte < size; ++byte)
        data[offset + byte] = std::uint8_t(value >> (8 * byte));
}

// Word masks for multi-word (delta-line) operations: bit i of mask32 marks the
// 4-byte slot at offset 4i, bit k of mask64 the 8-byte slot at offset 8k. A
// request is well formed when no byte is claimed at both widths.
inline std::uint32_t dstateMask64As32(std::uint32_t m64)
{
    std::uint32_t out = 0;
    for (int k = 0; k < 16; ++k)
        if ((m64 >> k) & 1u) out |= 3u << (2 * k);
    return out;
}
inline bool dstateMasksDisjoint(std::uint32_t a32, std::uint32_t a64,
                                std::uint32_t b32, std::uint32_t b64)
{
    return (a32 & dstateMask64As32(b64)) == 0 && (b32 & dstateMask64As32(a64)) == 0;
}
inline void dstateAddMasked(std::uint8_t *data, const std::uint8_t *operand,
                            int blockSize, std::uint32_t m32, std::uint32_t m64)
{
    for (int i = 0; i < 32; ++i)
        if ((m32 >> i) & 1u) dstateAdd(data, operand, blockSize, 4 * i, 4);
    for (int k = 0; k < 16; ++k)
        if ((m64 >> k) & 1u) dstateAdd(data, operand, blockSize, 8 * k, 8);
}

class DStateEngine
{
  public:
    // Admission-gate bookkeeping: a bounded per-bank table, line -> {bitmap of
    // requester node ids, updates and would-be ownership changes in a decayed
    // window, last writer, last access cycle}. It is kept outside the cache entry
    // because a line the home does not hold has no entry, yet its rejected
    // writers must still be counted. When the table is full the oldest entry is
    // dropped (that line starts as single-writer again).
    //
    // A "change" is an access by a core other than the line's previous writer:
    // in conventional mode that is a GETX from a non-owner (a migration); in
    // delegated mode it is the update that would have migrated the line had it
    // not been delegated. Counting both ways keeps the signal alive while the
    // line is delegated, when there are no migrations to observe.
    struct WriterEntry {
        std::uint64_t bits = 0;
        int epochUpdates = 0;          // updates since the bitmap last restarted
        int updates = 0, changes = 0;  // decayed window (halved every epoch updates)
        int lastWriter = -1;
        std::uint64_t lastCycle = 0, age = 0;
    };
    static constexpr std::size_t kWriterTable = 1024;
    std::unordered_map<std::uint64_t, WriterEntry> writers;
    std::uint64_t writerClock = 0;
    enum AccessKind { Rejected = 0, Accepted = 1, Migration = 2 };

    // Per-bank adaptive threshold (gatePct): decisions in the current window and
    // the mean change ratio of the rejected ones.
    struct Adaptive {
        int pct = -1, configured = 0, accepts = 0, rejects = 0, cooloff = 0;
        long rejRatioSum = 0;
    } adaptive;
    static constexpr int kAdaptiveWindow = 1024, kAdaptiveStep = 10, kAdaptiveCooloff = 8, kAdaptiveFloor = 10;

    bool expired(const WriterEntry &e, std::uint64_t now, std::uint64_t idle) const
    {
        return idle > 0 && now > e.lastCycle && now - e.lastCycle > idle;
    }
    const WriterEntry *lookup(std::uint64_t line, std::uint64_t now, std::uint64_t idle) const
    {
        auto it = writers.find(line);
        return it == writers.end() || expired(it->second, now, idle) ? nullptr : &it->second;
    }
    static int popcount(std::uint64_t bits)
    {
        int n = 0;
        for (; bits; bits &= bits - 1) ++n;
        return n;
    }
    static int changePctOf(const WriterEntry &e, int node)
    {
        return (e.changes + (e.lastWriter != node ? 1 : 0)) * 100 / (e.updates + 1);
    }

    // Distinct writers of `line` once `node` is counted (no state change).
    int writersAfter(std::uint64_t line, int node, std::uint64_t now, std::uint64_t idle) const
    {
        const WriterEntry *e = lookup(line, now, idle);
        std::uint64_t bits = e ? e->bits : 0;
        if (node >= 0 && node < 64) bits |= std::uint64_t(1) << node;
        return popcount(bits);
    }
    // Would-be ownership changes in the window once `node`'s access is counted.
    int changesAfter(std::uint64_t line, int node, std::uint64_t now, std::uint64_t idle) const
    {
        const WriterEntry *e = lookup(line, now, idle);
        return (e ? e->changes : 0) + ((e ? e->lastWriter : -1) != node ? 1 : 0);
    }
    // Changes per hundred updates in the window once `node`'s access is counted.
    int changePct(std::uint64_t line, int node, std::uint64_t now, std::uint64_t idle) const
    {
        const WriterEntry *e = lookup(line, now, idle);
        return e ? changePctOf(*e, node) : 100;
    }
    // Record one access from `node`: a delegated update (accepted or rejected) or
    // a conventional GETX. The bitmap restarts every `epoch` updates (idle == 0)
    // or after `idle` quiet cycles (idle > 0); the window counters halve every
    // `epoch` updates and clear after an idle gap, so a steadily contested line
    // keeps a steady ratio and never bounces between modes.
    void noteAccess(std::uint64_t line, int node, std::uint64_t now, int epoch, std::uint64_t idle, int kind)
    {
        if (writers.size() >= kWriterTable && !writers.count(line)) {
            auto victim = writers.begin();
            for (auto it = writers.begin(); it != writers.end(); ++it)
                if (it->second.age < victim->second.age) victim = it;
            writers.erase(victim);
        }
        WriterEntry &e = writers[line];
        if (expired(e, now, idle)) e = WriterEntry();
        if (kind != Migration) noteDecision(kind == Accepted, changePctOf(e, node));
        if (e.lastWriter != node) ++e.changes;
        e.lastWriter = node;
        if (++e.updates >= epoch) { e.updates = (e.updates + 1) / 2; e.changes = (e.changes + 1) / 2; }
        if (idle == 0 && ++e.epochUpdates >= epoch) { e.bits = 0; e.epochUpdates = 0; }
        if (node >= 0 && node < 64) e.bits |= std::uint64_t(1) << node;
        e.lastCycle = now;
        e.age = ++writerClock;
    }
    void forgetWriters(std::uint64_t line) { writers.erase(line); }

    // Requester-side owner-tenure predictor (used by the L1): how many adds this
    // L1 performed on a line while owning it before the line was taken away. A
    // short last tenure means the line is contended enough that delegation beats
    // ownership; a long one means the owner would have hit locally. This is the
    // owner hit rate, which the home never observes. Bounded table, oldest dropped.
    struct TenureEntry { int current = 0, last = -1; std::uint64_t lastCycle = 0, age = 0; };
    static constexpr std::size_t kTenureTable = 1024;
    std::unordered_map<std::uint64_t, TenureEntry> tenures;
    std::uint64_t tenureClock = 0;

    TenureEntry &tenureSlot(std::uint64_t line)
    {
        if (tenures.size() >= kTenureTable && !tenures.count(line)) {
            auto victim = tenures.begin();
            for (auto it = tenures.begin(); it != tenures.end(); ++it)
                if (it->second.age < victim->second.age) victim = it;
            tenures.erase(victim);
        }
        TenureEntry &e = tenures[line];
        e.age = ++tenureClock;
        return e;
    }
    void noteOwnerAdd(std::uint64_t line) { ++tenureSlot(line).current; }
    void noteOwnershipLost(std::uint64_t line, std::uint64_t now)
    {
        TenureEntry &e = tenureSlot(line);
        e.last = e.current;
        e.current = 0;
        e.lastCycle = now;
    }
    // The last observed tenure of `line`, or -1 when unknown or expired
    // (idle > 0 and recorded more than idle cycles ago).
    int predictedTenure(std::uint64_t line, std::uint64_t now, std::uint64_t idle) const
    {
        auto it = tenures.find(line);
        if (it == tenures.end() || it->second.last < 0) return -1;
        if (idle > 0 && now > it->second.lastCycle && now - it->second.lastCycle > idle) return -1;
        return it->second.last;
    }

    // Adaptive threshold: the effective change-ratio threshold for this bank.
    // Every kAdaptiveWindow decisions, if rejections dominate (>= 4x accepts) and
    // the rejected requests were contested (mean ratio >= half the threshold),
    // the threshold steps down; when accepts dominate it steps back up toward the
    // configured value. A cooling-off period of kAdaptiveCooloff windows follows
    // every step so an isolated burst cannot walk the threshold away.
    int gatePct(int configured, bool enabled)
    {
        if (!enabled) return configured;
        adaptive.configured = configured;
        if (adaptive.pct < 0) adaptive.pct = configured;
        return adaptive.pct;
    }
    void noteDecision(bool accepted, int ratio)
    {
        if (adaptive.pct < 0) return;    // adaptation off or not yet queried
        if (accepted) ++adaptive.accepts; else { ++adaptive.rejects; adaptive.rejRatioSum += ratio; }
        if (adaptive.accepts + adaptive.rejects < kAdaptiveWindow) return;
        if (adaptive.cooloff > 0) {
            --adaptive.cooloff;
        } else if (adaptive.rejects >= 4 * adaptive.accepts && adaptive.rejects > 0 &&
                   adaptive.rejRatioSum / adaptive.rejects >= adaptive.pct / 2 &&
                   adaptive.pct - kAdaptiveStep >= kAdaptiveFloor) {
            adaptive.pct -= kAdaptiveStep; adaptive.cooloff = kAdaptiveCooloff;
        } else if (adaptive.accepts >= 4 * adaptive.rejects && adaptive.pct < adaptive.configured) {
            adaptive.pct = std::min(adaptive.configured, adaptive.pct + kAdaptiveStep);
            adaptive.cooloff = kAdaptiveCooloff;
        }
        adaptive.accepts = adaptive.rejects = 0; adaptive.rejRatioSum = 0;
    }
    // One request's own masks must not claim a byte at both widths.
    bool masksValid(int m32, int m64) const
    {
        return dstateMasksDisjoint(std::uint32_t(m32), 0, 0, std::uint32_t(m64)) &&
               (m32 != 0 || m64 != 0);
    }
    void hotWordTouchMask(std::uint64_t line, int m32, int m64, int capacity)
    {
        for (int i = 0; i < 32; ++i)
            if ((std::uint32_t(m32) >> i) & 1u) hotWordTouch(line, 4 * i, capacity);
        for (int k = 0; k < 16; ++k)
            if ((std::uint32_t(m64) >> k) & 1u) hotWordTouch(line, 8 * k, capacity);
    }
    // Admission: at most `capacity` accepted operations in flight per bank.
    bool hasCapacity(int capacity) const { return inflight < capacity; }
    int occupancy() const { return inflight; }
    void reserve() { ++inflight; }
    void release() { assert(inflight > 0); --inflight; }

    // Issue timing: operation k+1 may start no earlier than start_k + interval.
    // interval == service latency gives the non-pipelined model; a shorter
    // interval models a pipelined datapath whose latency stays at the service
    // time. Returns the start tick for the caller to add its latency to.
    std::uint64_t scheduleStart(std::uint64_t now, std::uint64_t interval)
    {
        const std::uint64_t start = std::max(now, nextStart);
        nextStart = start + interval;
        return start;
    }

    // Hot-word buffer: a small LRU set of (line, word offset) pairs whose latest
    // value is modelled as living in registers at the bank rather than in the
    // SRAM array, like a write-combining buffer. A hit charges the hit latency;
    // a miss charges the full service latency. Purely a timing model: the
    // DataBlock always holds the authoritative value. Capacity 0 disables it.
    bool hotWordHit(std::uint64_t line, int offset) const
    {
        for (const auto &w : hotWords)
            if (w.first == line && w.second == offset) return true;
        return false;
    }
    void hotWordTouch(std::uint64_t line, int offset, int capacity)
    {
        if (capacity <= 0) { hotWords.clear(); return; }
        const std::pair<std::uint64_t, int> key(line, offset);
        auto it = std::find(hotWords.begin(), hotWords.end(), key);
        if (it != hotWords.end()) hotWords.erase(it);
        hotWords.insert(hotWords.begin(), key);
        while (static_cast<int>(hotWords.size()) > capacity) hotWords.pop_back();
    }
    // The line left this bank (eviction, ownership transfer): its words can no
    // longer be register-resident here.
    void hotWordDrop(std::uint64_t line)
    {
        hotWords.erase(std::remove_if(hotWords.begin(), hotWords.end(),
                                      [line](const auto &w) { return w.first == line; }),
                       hotWords.end());
    }

    std::uint64_t nextId()
    {
        assert(sequence != std::numeric_limits<std::uint64_t>::max());
        return ++sequence;
    }
    void print(std::ostream &out) const
    {
        out << "DStateEngine(inflight=" << inflight
            << " nextStart=" << nextStart
            << " hotWords=" << hotWords.size() << ")";
    }

  private:
    int inflight = 0;
    std::uint64_t nextStart = 0;
    std::uint64_t sequence = 0;
    std::vector<std::pair<std::uint64_t, int>> hotWords;
};
inline std::ostream &operator<<(std::ostream &out, const DStateEngine &engine)
{
    engine.print(out);
    return out;
}
} // namespace gem5::ruby
#endif
