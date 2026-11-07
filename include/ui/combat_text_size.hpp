#pragma once

// The combat text size setting ("combattextscale", Combat Text page): one
// percentage over both kinds of combat text this client shows.
//
//  - the world text over a unit the player hits or heals (CombatUI, by the
//    stock WorldText heights), and this client's stand-in for the floating
//    text while Blizzard_CombatText is not loaded;
//  - Blizzard_CombatText's floating text over the character, which sizes each
//    line from three globals it reads every time (COMBAT_TEXT_HEIGHT 25 and
//    the crit pop's COMBAT_TEXT_CRIT_MINHEIGHT 30 / _MAXHEIGHT 60, set once
//    when the addon's file runs).
//
// 100 is the stock size, and the size before the setting existed.

#include <algorithm>

namespace wowee::ui {

inline constexpr int kCombatTextScaleMinPct = 50;
inline constexpr int kCombatTextScaleMaxPct = 200;
inline constexpr int kCombatTextScaleDefaultPct = 100;

/// The percentage as a factor, clamped to the setting's range.
[[nodiscard]] constexpr float combatTextScale(int pct) {
    return static_cast<float>(std::clamp(pct, kCombatTextScaleMinPct, kCombatTextScaleMaxPct)) / 100.0f;
}

/// Blizzard_CombatText's three heights at a size.
struct CombatTextHeights {
    float normal = 25.0f;
    float critMin = 30.0f;
    float critMax = 60.0f;
};

[[nodiscard]] constexpr CombatTextHeights blizzardCombatTextHeights(int pct) {
    const float s = combatTextScale(pct);
    return {25.0f * s, 30.0f * s, 60.0f * s};
}

} // namespace wowee::ui
