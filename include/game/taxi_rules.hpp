#pragma once

// Stock 3.3.5a (build 12340) taxi rules, docs/re/mounts-taxi-vehicles.md
// section 4. Pure functions; tests/test_taxi_rules.cpp.

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

/// The price of a route's hops: a direct edge costs its price; a route of
/// several hops is discounted by the reputation tier (TaxiNodeCost 0x591350;
/// the tiers are 1.0, 0.95, 0.90, 0.85, 0.80 - Wowee passes 1.0 when the
/// faction standing is not known).
uint32_t routePrice(const std::vector<Edge>& edges, const std::vector<uint32_t>& route,
                    float multiHopFactor = 1.0f);

}  // namespace wowee::game::taxi
