#pragma once

// Duels as the stock 3.3.5a client (build 12340) knows them, kept apart from
// the handlers so the rules can be tested. docs/re/combat-targeting-movement.md
// section 1 ("Duels") has the addresses.
//
// A duel is two update fields on each player: PLAYER_DUEL_ARBITER (the flag's
// guid, set by the server on the challenge) and PLAYER_DUEL_TEAM (1 and 2,
// set when the countdown ends and cleared with the arbiter when it is over).
// The client keeps nothing else: whether the two may fight is read off those
// fields by UnitReaction (0x7251C0) and CanAttack (0x729740), for the players
// and for anything they control.

#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::game::stock_duel {

/// One side's duel fields.
struct DuelFields {
    uint64_t arbiter = 0;
    uint32_t team = 0;
};

/// 0x7251C0 between two player-controlled units whose controlling players
/// (0x718B70: the unit, else its charmer, else its creator) have these
/// fields: both teams set and the same arbiter answer 1 (hostile) for
/// different teams and 4 (neutral) for the same one. Nothing otherwise - the
/// ordinary rules decide.
[[nodiscard]] constexpr std::optional<int> duelReaction(DuelFields a, DuelFields b) {
    if (a.team == 0 || b.team == 0 || a.arbiter == 0 || a.arbiter != b.arbiter)
        return std::nullopt;
    return a.team != b.team ? 1 : 4;
}

/// 0x729740's duel test, after its reaction test (a reaction of 4 or better
/// refuses): the two duellists, and what they control, may attack each other
/// whatever their factions and PvP flags.
[[nodiscard]] constexpr bool duelAllowsAttack(DuelFields a, DuelFields b) {
    const auto r = duelReaction(a, b);
    return r.has_value() && *r < 4;
}

/// SMSG_DUEL_COUNTDOWN (0x5CFA50 -> 0x5CF870): the count is the time in whole
/// seconds; DUEL_COUNTDOWN ("Duel starting: %d") goes to chat at once, then
/// every 1000 ms with one less, until the count reaches zero.
/// SMSG_DUEL_COMPLETE unregisters it (0x5CFA90).
class Countdown {
public:
    /// The numbers to print now.
    std::vector<int> start(uint32_t ms) {
        count_ = static_cast<int>(ms / 1000);
        untilNextMs_ = 0;
        running_ = true;
        return tick(0);
    }
    /// The numbers due after `elapsedMs` more.
    std::vector<int> tick(uint32_t elapsedMs) {
        std::vector<int> out;
        if (!running_) return out;
        if (elapsedMs >= untilNextMs_) {
            elapsedMs -= untilNextMs_;
            untilNextMs_ = 0;
        } else {
            untilNextMs_ -= elapsedMs;
            return out;
        }
        for (;;) {
            out.push_back(count_);
            --count_;
            if (count_ <= 0) {
                running_ = false;
                break;
            }
            if (elapsedMs < 1000) {
                untilNextMs_ = 1000 - elapsedMs;
                break;
            }
            elapsedMs -= 1000;
        }
        return out;
    }
    void stop() { running_ = false; }
    [[nodiscard]] bool running() const { return running_; }

private:
    int count_ = 0;
    uint32_t untilNextMs_ = 0;
    bool running_ = false;
};

} // namespace wowee::game::stock_duel
