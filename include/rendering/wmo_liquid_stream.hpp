#pragma once

/// A streamed tile's WMO placements, and their liquids, made across several
/// finalisation steps.
///
/// The WMO_INSTANCES phase creates a few instances per step and loads at most a
/// few liquid groups per step, so a WMO with many liquid groups is carried over
/// from one step to the next part-way through its liquids. Stormwind has five:
/// the Canals fountain (group 224) and four canal sheets (281-284).
///
/// The step used to begin every placement, the carried-over one included, with
/// the duplicate test. By then the carried-over one had been placed - by this
/// same tile, one step earlier - so it was taken for a duplicate and skipped,
/// and every liquid group after the step budget went with it. With the
/// fountain counted (interior water, drawn since stock pass 3) the budget ran
/// out at group 283, and the canal sheet of group 284 - the canals around the
/// Trade District - was never made: no water drawn, and nothing to swim in,
/// while the server had the player under water.
///
/// The rule this keeps: a placement already begun is resumed, never judged
/// again. The pure parts live here for the test (test_wmo_liquid_stream).

#include <cstddef>

namespace wowee {
namespace rendering {
namespace wmo_liquid_stream {

/// What a step does with the placement under its cursor.
enum class Visit {
    Skip,    ///< placed already (by another tile) or its model is not loaded
    Create,  ///< make its instance, then start on its liquids
    Resume,  ///< made by an earlier step of this tile: carry on with its liquids
};

/// `begun` is whether this tile already created the instance for the placement
/// under the cursor. Only a placement not begun is tested as a duplicate.
constexpr Visit visit(bool begun, bool alreadyPlaced, bool modelLoaded) {
    if (begun) return Visit::Resume;
    if (alreadyPlaced || !modelLoaded) return Visit::Skip;
    return Visit::Create;
}

/// Loads the liquid groups of one placement from `next` on, spending at most
/// `budget - used` loads. A group without liquid costs nothing and is passed
/// over, so a model whose last liquid group falls on the budget is finished in
/// this step instead of being carried over for groups that have none.
/// Returns true once every group has been visited.
template <typename HasLiquid, typename Load>
bool step(std::size_t groupCount, std::size_t& next, std::size_t& used, std::size_t budget,
          HasLiquid&& hasLiquid, Load&& load) {
    while (next < groupCount) {
        if (!hasLiquid(next)) {
            ++next;
            continue;
        }
        if (used >= budget) return false;
        load(next);
        ++next;
        ++used;
    }
    return true;
}

}  // namespace wmo_liquid_stream
}  // namespace rendering
}  // namespace wowee
