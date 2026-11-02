// Stock 3.3.5a taxi rules (include/game/taxi_rules.hpp).
#include "game/taxi_rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace wowee::game::taxi {

std::vector<uint32_t> shortestRoute(const std::vector<Edge>& edges, uint32_t start, uint32_t dest,
                                    const std::function<bool(uint32_t)>& known) {
    if (start == 0 || dest == 0 || start == dest) return {};
    std::unordered_map<uint32_t, std::vector<const Edge*>> adj;
    for (const auto& e : edges) adj[e.from].push_back(&e);

    std::unordered_map<uint32_t, float> dist;
    std::unordered_map<uint32_t, uint32_t> prev;
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    dist[start] = 0.0f;
    open.push({0.0f, start});
    while (!open.empty()) {
        const auto [d, node] = open.top();
        open.pop();
        if (d > dist[node]) continue;
        if (node == dest) break;
        auto it = adj.find(node);
        if (it == adj.end()) continue;
        for (const Edge* e : it->second) {
            // Only through nodes the player knows (the map's slots are the
            // known nodes; 0x5926C0 walks those).
            if (known && !known(e->to)) continue;
            const float nd = d + std::max(0.0f, e->length);
            auto dIt = dist.find(e->to);
            if (dIt == dist.end() || nd < dIt->second) {
                dist[e->to] = nd;
                prev[e->to] = node;
                open.push({nd, e->to});
            }
        }
    }
    if (!dist.count(dest)) return {};
    std::vector<uint32_t> route{dest};
    uint32_t cur = dest;
    while (cur != start) {
        auto it = prev.find(cur);
        if (it == prev.end() || route.size() > prev.size() + 1) return {};
        cur = it->second;
        route.push_back(cur);
    }
    std::reverse(route.begin(), route.end());
    return route;
}

uint32_t routePrice(const std::vector<Edge>& edges, const std::vector<uint32_t>& route,
                    float multiHopFactor) {
    uint64_t total = 0;
    for (size_t i = 0; i + 1 < route.size(); ++i) {
        for (const auto& e : edges) {
            if (e.from == route[i] && e.to == route[i + 1]) {
                total += e.price;
                break;
            }
        }
    }
    if (route.size() > 2 && multiHopFactor != 1.0f)
        total = static_cast<uint64_t>(std::ceil(static_cast<double>(total) * multiHopFactor));
    return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

}  // namespace wowee::game::taxi
