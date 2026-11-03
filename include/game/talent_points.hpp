#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee {
namespace game {

/// Points spent in one talent group: the learned map holds rank counts
/// (1 = one point), so the sum is the spend.
inline uint32_t talentPointsSpent(const std::unordered_map<uint32_t, uint8_t>& learned) {
    uint32_t spent = 0;
    for (const auto& [id, rank] : learned) { (void)id; spent += rank; }
    return spent;
}

/// Unspent points of a talent group the server did not report.
///
/// SMSG_TALENTS_INFO carries one unspent count, the active group's
/// (AzerothCore Player::SendTalentsInfoData writes GetFreeTalentPoints()).
/// Both groups draw from the same total - Player::CalculateTalentsPoints,
/// level-based plus quest-granted - so the other group's unspent is that
/// total, which the active group's spent + unspent is, minus its own spend.
inline uint32_t otherGroupUnspent(uint32_t activeSpent, uint32_t activeUnspent,
                                  uint32_t otherSpent) {
    const uint32_t total = activeSpent + activeUnspent;
    return total > otherSpent ? total - otherSpent : 0;
}

/// GLYPH_ADDED / GLYPH_REMOVED / GLYPH_UPDATED between two socket states.
/// Each change is (event, 1-based socket), the argument glyphui reads.
enum class GlyphChange : uint8_t { Added, Removed, Updated };
template <size_t N>
std::vector<std::pair<GlyphChange, int>> glyphChanges(const std::array<uint16_t, N>& before,
                                                      const std::array<uint16_t, N>& after) {
    std::vector<std::pair<GlyphChange, int>> out;
    for (size_t i = 0; i < N; ++i) {
        if (before[i] == after[i]) continue;
        const GlyphChange c = before[i] == 0 ? GlyphChange::Added
                            : after[i] == 0  ? GlyphChange::Removed
                                             : GlyphChange::Updated;
        out.emplace_back(c, static_cast<int>(i) + 1);
    }
    return out;
}

}  // namespace game
}  // namespace wowee
