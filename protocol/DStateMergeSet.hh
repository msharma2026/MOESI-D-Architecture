// SPDX-License-Identifier: BSD-3-Clause
// Requesters whose same-word update was folded into an already-accepted
// operation (combining). Each still receives exactly one terminal ACK carrying
// its own transaction ID; the merged operand is applied once. Bounded by the
// controller's merge limit; the L2 TBE owns one of these. Kept separate from
// DStateEngine.hh because it needs MachineID, which the standalone unit test
// does not have.
#ifndef MOESI_D_MERGE_SET_HH
#define MOESI_D_MERGE_SET_HH

#include <cassert>
#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/structures/DStateEngine.hh"

namespace gem5::ruby
{
class DStateMergeSet
{
  public:
    void add(MachineID requester, std::uint64_t id) { entries.emplace_back(requester, id); }
    int count() const { return static_cast<int>(entries.size()); }
    MachineID requester(int i) const { assert(i >= 0 && i < count()); return entries[i].first; }
    std::uint64_t id(int i) const { assert(i >= 0 && i < count()); return entries[i].second; }
    void popBack() { assert(!entries.empty()); entries.pop_back(); }
    void clear() { entries.clear(); m32 = 0; m64 = 0; }
    void print(std::ostream &out) const
    { out << "DStateMergeSet(n=" << entries.size() << " m32=" << m32 << " m64=" << m64 << ")"; }

    // Words the accepted operation touches (OR of every merged request). A
    // scalar request that carries no masks is described by its offset/width.
    void setMasks(int mask32, int mask64, int offset, int size)
    {
        m32 = std::uint32_t(mask32); m64 = std::uint32_t(mask64);
        if (m32 == 0 && m64 == 0) {
            if (size == 8) m64 = 1u << (offset / 8); else m32 = 1u << (offset / 4);
        }
    }
    // Another request can fold into this operation when no byte is claimed at
    // two different widths; same-word adds (same width) always qualify.
    bool masksCompatible(int mask32, int mask64) const
    { return dstateMasksDisjoint(m32, m64, std::uint32_t(mask32), std::uint32_t(mask64)); }
    void orMasks(int mask32, int mask64) { m32 |= std::uint32_t(mask32); m64 |= std::uint32_t(mask64); }
    int mask32() const { return int(m32); }
    int mask64() const { return int(m64); }

  private:
    std::vector<std::pair<MachineID, std::uint64_t>> entries;
    std::uint32_t m32 = 0, m64 = 0;
};
inline std::ostream &operator<<(std::ostream &out, const DStateMergeSet &set)
{
    set.print(out);
    return out;
}
} // namespace gem5::ruby
#endif
