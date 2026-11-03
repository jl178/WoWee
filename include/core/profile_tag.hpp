#pragma once

/// A name for what the frame profile is measuring right now.
///
/// WOWEE_ENHANCE_AB (and the "enhanceab" setting a test script sets) flips one
/// enhancement row between its stock and recommended values on a period. Each
/// flip names the new state here; under WOWEE_FRAME_PROFILE the application
/// then reports one window per state instead of one every ten seconds - the
/// window starts a moment after the flip, so the frame that made a pipeline or
/// an image is not counted, and ends at the next flip - which is what makes the
/// difference between two windows the cost of the row.
///
/// Main thread only.

#include <cstdint>
#include <string>
#include <utility>

namespace wowee {
namespace core {

struct ProfileTag {
    std::string label;
    uint32_t generation = 0;   ///< 0: no tag has been set; the ten second report runs
};

inline ProfileTag& profileTag() {
    static ProfileTag tag;
    return tag;
}

inline void setProfileTag(std::string label) {
    auto& t = profileTag();
    t.label = std::move(label);
    ++t.generation;
}

}  // namespace core
}  // namespace wowee
