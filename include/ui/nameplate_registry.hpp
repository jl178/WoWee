#pragma once

#include <cstdint>
#include <unordered_set>

namespace wowee {
namespace ui {

/// The units carrying a nameplate, as of the last frame the plates were
/// drawn. Stock asks the unit itself (CGUnit_C +0xC38, the plate frame): a
/// unit with a plate gets no name over its head and no chat bubble.
inline std::unordered_set<uint64_t>& platedUnits() {
    static std::unordered_set<uint64_t> units;
    return units;
}

}  // namespace ui
}  // namespace wowee
