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
    void clear() { entries.clear(); }
    void print(std::ostream &out) const { out << "DStateMergeSet(n=" << entries.size() << ")"; }

  private:
    std::vector<std::pair<MachineID, std::uint64_t>> entries;
};
inline std::ostream &operator<<(std::ostream &out, const DStateMergeSet &set)
{
    set.print(out);
    return out;
}
} // namespace gem5::ruby
#endif
