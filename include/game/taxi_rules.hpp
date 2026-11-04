#pragma once

// Stock 3.3.5a (build 12340) taxi rules, docs/re/mounts-taxi-vehicles.md
// section 4. Pure functions; tests/test_taxi_rules.cpp.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace wowee::game::taxi {

/// The GlobalStrings name of an SMSG_ACTIVATETAXIREPLY failure (0x6CEE90,
/// the table at 0xAD9C94); nullptr for 0 (success) or an unknown code.
inline const char* replyErrorKey(uint32_t result) {
    static const char* const kKeys[] = {
        nullptr,
        "ERR_TAXIUNSPECIFIEDSERVERERROR",
        "ERR_TAXINOSUCHPATH",
        "ERR_TAXINOTENOUGHMONEY",
        "ERR_TAXITOOFARAWAY",
        "ERR_TAXINOVENDORNEARBY",
        "ERR_TAXINOTVISITED",
        "ERR_TAXIPLAYERBUSY",
        "ERR_TAXIPLAYERALREADYMOUNTED",
        "ERR_TAXIPLAYERSHAPESHIFTED",
        "ERR_TAXIPLAYERMOVING",
        "ERR_TAXISAMENODE",
        "ERR_TAXINOTSTANDING",
    };
    return result < sizeof(kKeys) / sizeof(kKeys[0]) ? kKeys[result] : nullptr;
}

/// One TaxiPath edge: its nodes, price and flown length (the sum of the 3D
/// distances between its TaxiPathNode points, 0x592580).
struct Edge {
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t price = 0;
    float length = 0.0f;
};

/// The route TakeTaxiNode flies (0x592BB0 / 0x5926C0): the shortest total
/// flight distance over known nodes - not the fewest hops, not the cheapest.
/// Returns the node list from `start` to `dest` inclusive, empty when none.
std::vector<uint32_t> shortestRoute(const std::vector<Edge>& edges, uint32_t start, uint32_t dest,
                                    const std::function<bool(uint32_t)>& known);

/// The reputation tier of a fare (0x591350 / 0x7279A0): 0 at Neutral or
/// worse, 1 Friendly, 2 Honored, 3 Revered, 4 Exalted. `reaction` is the
/// flight master's reaction to the player numbered as UnitReaction numbers it
/// (1 hated .. 8 exalted); a flight master whose faction carries no
/// reputation is always tier 0.
inline int reputationTier(int reaction, bool factionHasReputation) {
    if (!factionHasReputation) return 0;
    const int rank = reaction - 1;  // stock's 0 hated .. 7 exalted
    return rank > 3 ? std::min(rank, 7) - 3 : 0;
}

/// One fare at a tier, as the client prices it: `(1 - 0.05 tier) x price`,
/// rounded to the nearest (x87 FISTP, ties to even). `perHop` is the
/// several-hop table's factor (0x5926C0: 1.0f less 0.05f per tier, in
/// float); without it the direct fare's (0x591350: 1 - the tier's discount
/// 0.05 / 0.10 / 0.15 / 0.20, 0x7279A0, in extended precision).
uint32_t tierFare(uint32_t price, int tier, bool perHop);

/// The price TaxiNodeCost shows (0x591350). A destination with a TaxiPath of
/// its own from `start` costs that path's fare, discounted by the tier,
/// whatever the route; otherwise the route's hops, each discounted and
/// rounded on its own (0x5926C0 keeps five running sums, one per tier).
uint32_t routePrice(const std::vector<Edge>& edges, const std::vector<uint32_t>& route, int tier = 0);

}  // namespace wowee::game::taxi
