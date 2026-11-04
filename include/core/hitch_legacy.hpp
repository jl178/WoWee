#pragma once

/// WOWEE_HITCH_LEGACY=1: the main-thread loading paths as they were before the
/// hitch work (docs/performance.md "Hitches"), so both can be measured in one
/// build at the same load: beginFrame waits for every upload batch, players
/// spawn synchronously three a frame, equipment is prepared one change a frame
/// with only its composite's textures, game object M2s load on the main
/// thread, and item models are parsed and uploaded again for every attach.
/// A measuring switch, not a setting.

#include "core/env_flag.hpp"

namespace wowee {
namespace core {

inline bool hitchLegacy() {
    static const bool legacy = envFlagEnabled("WOWEE_HITCH_LEGACY", false);
    return legacy;
}

}  // namespace core
}  // namespace wowee
