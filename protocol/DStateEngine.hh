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

class DStateEngine
{
  public:
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
