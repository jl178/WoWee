#pragma once

#include <cstdint>
#include <vector>

#include "game/combat_handler.hpp"

namespace wowee {
namespace game {

/// A unit's standing on a mob's threat list: the status WoW reports, its raw
/// threat, and the leader's.
///
/// One place, because UnitThreatSituation, UnitDetailedThreatSituation and the
/// nameplate's threat glow must agree - the indicator's colour comes from the
/// first and its number from the second, and two copies of the rule would
/// eventually disagree about which unit is tanking.
struct ThreatStanding {
    int status = 0;          ///< 0 none, 1 threat, 2 insecurely, 3 securely tanking
    uint32_t threat = 0;
    uint32_t topThreat = 0;
    bool onList = false;
};

/// Answers false when no list has arrived for this mob, which is not the same
/// as being absent from one: the caller falls back to guessing from whom the
/// mob is attacking, and only a list that exists can say "not on it".
inline bool threatStandingOn(const std::vector<CombatHandler::ThreatEntry>* list,
                             uint64_t unitGuid, ThreatStanding& out) {
    if (!list || list->empty() || unitGuid == 0) return false;
    out.topThreat = (*list)[0].threat;
    for (size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].victimGuid != unitGuid) continue;
        out.onList = true;
        out.threat = (*list)[i].threat;
        if (i != 0) { out.status = 1; return true; }
        // Securely tanking only when the lead is clear. WoW's own rule is a
        // percentage margin over the runner-up; at the head with someone close
        // behind it is "insecurely", which is what turns the indicator amber
        // before it turns red.
        const uint32_t next = (list->size() > 1) ? (*list)[1].threat : 0;
        out.status = (list->size() > 1 && next * 11 >= out.topThreat * 10) ? 2 : 3;
        return true;
    }
    return true;  // the list exists and this unit is not on it
}

}  // namespace game
}  // namespace wowee
