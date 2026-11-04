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

uint32_t tierFare(uint32_t price, int tier, bool perHop) {
    tier = std::clamp(tier, 0, 4);
    float fare = 0.0f;
    if (perHop) {
        float factor = 1.0f;
        for (int i = 0; i < tier; ++i) factor -= 0.05f;  // 0x9F1958
        fare = factor * static_cast<float>(price);
    } else {
        static constexpr float kDiscount[5] = {0.0f, 0.05f, 0.10f, 0.15f, 0.20f};
        fare = static_cast<float>((1.0L - static_cast<long double>(kDiscount[tier])) *
                                  static_cast<long double>(price));
    }
    const float r = std::nearbyint(fare);  // round to nearest, ties to even
    return r <= 0.0f ? 0u : static_cast<uint32_t>(std::min<double>(r, std::numeric_limits<uint32_t>::max()));
}

uint32_t routePrice(const std::vector<Edge>& edges, const std::vector<uint32_t>& route, int tier) {
    if (route.size() < 2) return 0;
    const auto edge = [&](uint32_t from, uint32_t to) -> const Edge* {
        for (const auto& e : edges)
            if (e.from == from && e.to == to) return &e;
        return nullptr;
    };
    if (const Edge* direct = edge(route.front(), route.back())) return tierFare(direct->price, tier, false);
    uint64_t total = 0;
    for (size_t i = 0; i + 1 < route.size(); ++i)
        if (const Edge* e = edge(route[i], route[i + 1])) total += tierFare(e->price, tier, true);
    return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

}  // namespace wowee::game::taxi
