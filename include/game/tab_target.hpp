// tab_target.hpp - which unit TargetNearestEnemy and its siblings pick, and in
// what order, as Wow.exe 12340 does it (docs/re/combat-targeting-movement.md
// section 1, "Tab targeting").
//
// The Lua functions (TargetNearest 0x525A90, TargetNearestEnemy 0x525AD0,
// ...EnemyPlayer 0x525B10, ...Friend 0x525B50, ...FriendPlayer 0x525B90,
// ...PartyMember 0x525BD0, ...RaidMember 0x525C00) all call 0x524FC0 with a
// mode (0-6) and the "reverse" argument. 0x524FC0 keeps one list of
// candidates (0xBD0C04, count 0xBD0C00) built by 0x524440 for every unit in
// the object manager and sorted by 0x5131D0, and an index into it (0xBD08C8):
//
//   - a unit qualifies when it is not the player (the vehicle when driving
//     one), passes the mode's test (0x518E40), its creature type is not one
//     CreatureType.dbc flags 1 (critter, non-combat pet, gas cloud), its model
//     was inside the camera's frustum on the last frame drawn (the map
//     entity's +0x25, 0x77F0B0), it is not a commentator, and it is near
//     enough: 41 yards (distance squared 1681, 0x9FE7F8) inside a 30 degree
//     half-angle (0xA02D20) cone around the player's facing, 10 yards
//     (0x9FE7FC) anywhere else. No line of sight test.
//   - sorted: those in the cone first, then by distance.
//   - the list is rebuilt when it is empty, when the mode changed, or 3 s
//     after the last press (0xBD08D0 + 3000); a press with the current target
//     still the list's current entry steps to the next one (reverse: the
//     previous one, wrapping); stepping off the end starts again at the top,
//     and rebuilds first when the list is a second old (0xBD08CC + 1000).
//   - the entry stepped to is selected if it still exists and still passes
//     the mode's test; otherwise the next one is tried. Nothing passing
//     clears the list and leaves the target alone.
//
// Nothing is said when there is nothing to select.
#pragma once

#include "game/protocol_constants.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace game {

/// The mode 0x524FC0 is called with.
enum class TargetNearestMode : uint8_t {
    Any = 0,           // TargetNearest
    Enemy = 1,         // TargetNearestEnemy
    EnemyPlayer = 2,   // TargetNearestEnemyPlayer
    Friend = 3,        // TargetNearestFriend
    FriendPlayer = 4,  // TargetNearestFriendPlayer
    Party = 5,         // TargetNearestPartyMember
    Raid = 6,          // TargetNearestRaidMember
};

namespace tab_target {

inline constexpr float kPi = 3.14159265358979f;
/// 0xA02D20: the half-angle of the cone in front of the player.
inline constexpr float kFrontHalfAngle = 0.5235988f;
/// 0x9FE7F8 / 0x9FE7FC: distance squared, in front / anywhere else.
inline constexpr float kFrontRangeSq = 1681.0f;
inline constexpr float kNearRangeSq = 100.0f;
/// The list is built again this long after the last press.
inline constexpr uint32_t kListExpiresMs = 3000;
/// Stepping off the end of a list this old builds it again.
inline constexpr uint32_t kWrapRebuildMs = 1000;

/// CreatureType.dbc rows with flags & 1 in 3.3.5a: Critter (8), Non-combat
/// Pet (12), Gas Cloud (13). Never a candidate, whatever the mode.
[[nodiscard]] constexpr bool creatureTypeIgnoredByTab(uint32_t creatureType) {
    return creatureType == 8 || creatureType == 12 || creatureType == 13;
}

/// Whether a unit at `angleTo` (the direction from the player to it, in the
/// same convention as `facing`) is inside the cone: 0x524440 takes
/// fmod(angleTo - facing + 4pi, 2pi) and asks for less than the half-angle or
/// more than 2pi less it.
[[nodiscard]] inline bool inFrontCone(float facing, float angleTo) {
    float d = std::fmod(angleTo - facing + 4.0f * kPi, 2.0f * kPi);
    if (d < 0.0f) d += 2.0f * kPi;
    return d < kFrontHalfAngle || d > 2.0f * kPi - kFrontHalfAngle;
}

[[nodiscard]] constexpr bool withinReach(float distSq, bool inFront) {
    return distSq <= (inFront ? kFrontRangeSq : kNearRangeSq);
}

/// UNIT_FIELD_FLAGS that make a unit one the player may not attack
/// (CanAttack 0x729740): NON_ATTACKABLE 0x2, 0x80, 0x10000, TAXI_FLIGHT
/// 0x100000, NOT_SELECTABLE 0x2000000, and IMMUNE_TO_PC 0x100 against a
/// player.
inline constexpr uint32_t kUnattackableFlags =
    0x2u | 0x80u | 0x10000u | 0x100000u | 0x2000000u | UNIT_FLAG_IMMUNE_TO_PC;

/// UNIT_FIELD_BYTES_2 byte 1 (the PvP byte) bits.
inline constexpr uint8_t kPvpFlagPvp = 0x01;
inline constexpr uint8_t kPvpFlagUnk1 = 0x02;
inline constexpr uint8_t kPvpFlagFfa = 0x04;
inline constexpr uint8_t kPvpFlagSanctuary = 0x08;

/// CanAttack (0x729740) between two players that are not friendly to each
/// other: either flagged for PvP, or both free-for-all, and neither in a
/// sanctuary. (A duel between the two answers yes before this; Wowee does
/// not track the arbiter, see the doc's gaps.)
[[nodiscard]] constexpr bool playersMayFight(uint8_t mine, uint8_t theirs) {
    if (theirs & kPvpFlagPvp) {
        return !(mine & kPvpFlagSanctuary) && !(theirs & kPvpFlagSanctuary);
    }
    if ((mine & kPvpFlagFfa) && (theirs & kPvpFlagFfa)) return true;
    if (!(mine & kPvpFlagUnk1) && !(theirs & kPvpFlagUnk1)) return false;
    return !(mine & kPvpFlagSanctuary) && !(theirs & kPvpFlagSanctuary);
}

/// What 0x518E40 asks of a unit for each mode, with the answers about the
/// unit worked out by the caller.
struct UnitFacts {
    bool isPlayer = false;
    bool canAttack = false;    // CanAttack (0x729740)
    bool canAssist = false;    // CanAssist (0x7293D0)
    bool dead = false;         // vtable +0x128: health below 1 (a player: or a ghost)
    bool dynFlagDead = false;  // UNIT_DYNAMIC_FLAGS 0x20 (feigning too)
    uint8_t standState = 0;    // UNIT_FIELD_BYTES_1 byte 0; 7 is dead
    uint32_t health = 0;
    bool partyMember = false;  // the player or one of party1..4 (0x52C680)
    bool raidMember = false;   // party, or anyone in the raid (0x512A00)
};

[[nodiscard]] constexpr bool passesMode(TargetNearestMode mode, const UnitFacts& u) {
    switch (mode) {
    case TargetNearestMode::EnemyPlayer:
        if (!u.isPlayer) return false;
        [[fallthrough]];
    case TargetNearestMode::Enemy:
        return u.canAttack && !u.dead && !u.dynFlagDead && u.standState != 7;
    case TargetNearestMode::FriendPlayer:
        if (!u.isPlayer) return false;
        [[fallthrough]];
    case TargetNearestMode::Friend:
        return u.canAssist && u.health >= 1;
    case TargetNearestMode::Party:
        return u.partyMember;
    case TargetNearestMode::Raid:
        return u.raidMember || u.partyMember;
    case TargetNearestMode::Any:
        break;
    }
    return true;
}

struct Candidate {
    uint64_t guid = 0;
    float distSq = 0.0f;
    bool inFront = false;
};

/// 0x5131D0: the cone first, then nearest first.
inline void sortCandidates(std::vector<Candidate>& list) {
    std::stable_sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        if (a.inFront != b.inFront) return a.inFront;
        return a.distSq < b.distSq;
    });
}

/// The list and the cursor 0x524FC0 keeps between presses.
class Cycle {
public:
    /// One press. `gather` builds the candidates (filtered, unsorted);
    /// `stillWanted` is the check made when stepping onto an entry - the unit
    /// still exists and passes the mode's test. Returns the unit to select,
    /// or 0 to leave the target as it is.
    template <typename Gather, typename StillWanted>
    uint64_t press(TargetNearestMode mode, bool reverse, uint32_t nowMs,
                   uint64_t currentTarget, Gather&& gather, StillWanted&& stillWanted) {
        // Times are kept non-zero: 0 means "never" (0x524FC0 stores 1 for a
        // clock that reads 0).
        if (nowMs == 0) nowMs = 1;
        bool rebuild = builtMs_ == 0 || lastPressMs_ == 0 ||
                       nowMs - lastPressMs_ >= kListExpiresMs || list_.empty() ||
                       mode != mode_;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (rebuild) {
                index_ = 0;
                builtMs_ = 0;
                lastPressMs_ = 0;
                mode_ = mode;
                list_.clear();
                for (const Candidate& c : gather()) list_.push_back(c);
                if (list_.empty()) return 0;
                sortCandidates(list_);
                builtMs_ = nowMs;
            } else if (list_[index_].guid == currentTarget) {
                if (!reverse) {
                    if (++index_ >= list_.size()) {
                        index_ = 0;
                        if (nowMs - builtMs_ >= kWrapRebuildMs) {
                            rebuild = true;
                            continue;
                        }
                    }
                } else {
                    index_ = index_ == 0 ? list_.size() - 1 : index_ - 1;
                }
            }
            lastPressMs_ = nowMs;
            size_t i = index_;
            while (true) {
                if (stillWanted(list_[i].guid)) {
                    index_ = i;
                    return list_[i].guid;
                }
                if (++i >= list_.size()) {
                    i = 0;
                    if (nowMs - builtMs_ >= kWrapRebuildMs && attempt == 0) {
                        rebuild = true;
                        break;
                    }
                }
                if (i == index_) {
                    list_.clear();
                    return 0;
                }
            }
        }
        return 0;
    }

    void reset() {
        list_.clear();
        index_ = 0;
        builtMs_ = 0;
        lastPressMs_ = 0;
    }

    [[nodiscard]] const std::vector<Candidate>& list() const { return list_; }
    [[nodiscard]] size_t index() const { return index_; }

private:
    std::vector<Candidate> list_;
    size_t index_ = 0;
    uint32_t builtMs_ = 0;      // 0xBD08CC
    uint32_t lastPressMs_ = 0;  // 0xBD08D0
    TargetNearestMode mode_ = TargetNearestMode::Any;  // 0xBD08D4
};

} // namespace tab_target
} // namespace game
} // namespace wowee
