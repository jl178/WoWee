// A streamed tile's WMO placements and their liquids, made across several
// finalisation steps (wmo_liquid_stream.hpp).
//
// The failure this guards was scenery, not a crash: Stormwind's canal sheet
// around the Trade District was never made, because the step that had begun
// the city's placement took it for a duplicate when it came back for the rest
// of its liquids. The loop below is TerrainManager's WMO_INSTANCES phase with
// the renderers taken out.

#include <catch_amalgamated.hpp>

#include "rendering/wmo_liquid_stream.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace ls = wowee::rendering::wmo_liquid_stream;

namespace {

struct Placement {
    uint32_t uniqueId = 0;
    std::vector<bool> liquid;  // per group: has an MLIQ
};

// Stormwind: 286 groups, liquid in the Canals fountain (224) and the four
// canal sheets (281-284).
Placement stormwind() {
    Placement p;
    p.uniqueId = 10047;
    p.liquid.assign(286, false);
    for (size_t g : {224u, 281u, 282u, 283u, 284u}) p.liquid[g] = true;
    return p;
}

struct Cursor {
    size_t index = 0;
    size_t liquidGroup = 0;
    bool begun = false;
};

struct Loaded {
    uint32_t uniqueId;
    size_t group;
    bool operator<(const Loaded& o) const {
        return uniqueId != o.uniqueId ? uniqueId < o.uniqueId : group < o.group;
    }
};

constexpr size_t kInstancesPerStep = 4;
constexpr size_t kLiquidsPerStep = 4;

// One call of the phase. Returns true when the tile's placements are done.
bool runStep(const std::vector<Placement>& placements, Cursor& c, std::set<uint32_t>& placed,
             std::vector<Loaded>& loaded) {
    size_t created = 0;
    size_t used = 0;
    auto nextPlacement = [&c]() {
        c.index++;
        c.liquidGroup = 0;
        c.begun = false;
    };
    while (c.index < placements.size() && created < kInstancesPerStep) {
        const Placement& p = placements[c.index];
        const auto v = ls::visit(c.begun, p.uniqueId != 0 && placed.count(p.uniqueId) > 0, true);
        if (v == ls::Visit::Skip) {
            nextPlacement();
            continue;
        }
        if (v == ls::Visit::Create) {
            c.begun = true;
            if (p.uniqueId != 0) placed.insert(p.uniqueId);
        }
        const bool done = ls::step(
            p.liquid.size(), c.liquidGroup, used, kLiquidsPerStep,
            [&](size_t g) { return p.liquid[g]; },
            [&](size_t g) { loaded.push_back({p.uniqueId, g}); });
        if (!done) return false;
        nextPlacement();
        created++;
    }
    return c.index >= placements.size();
}

std::vector<Loaded> runTile(const std::vector<Placement>& placements, std::set<uint32_t>& placed,
                            int* steps = nullptr) {
    Cursor c;
    std::vector<Loaded> loaded;
    int n = 0;
    while (!runStep(placements, c, placed, loaded)) {
        REQUIRE(++n < 1000);
    }
    if (steps) *steps = n + 1;
    return loaded;
}

}  // namespace

TEST_CASE("a placement begun is resumed, never judged again") {
    // Its own uniqueId is in the placed set by then, and on a model that
    // unloaded meanwhile there is nothing new to decide either.
    REQUIRE(ls::visit(true, true, true) == ls::Visit::Resume);
    REQUIRE(ls::visit(true, true, false) == ls::Visit::Resume);
    REQUIRE(ls::visit(true, false, true) == ls::Visit::Resume);
}

TEST_CASE("a placement not begun is skipped as a duplicate or for its model") {
    REQUIRE(ls::visit(false, true, true) == ls::Visit::Skip);
    REQUIRE(ls::visit(false, false, false) == ls::Visit::Skip);
    REQUIRE(ls::visit(false, false, true) == ls::Visit::Create);
}

TEST_CASE("every Stormwind liquid group is loaded, once") {
    std::set<uint32_t> placed;
    int steps = 0;
    const auto loaded = runTile({stormwind()}, placed, &steps);
    std::vector<size_t> groups;
    for (const auto& l : loaded) groups.push_back(l.group);
    REQUIRE(groups == std::vector<size_t>{224, 281, 282, 283, 284});
    // Four in the first step, the fifth in the next.
    REQUIRE(steps == 2);
}

TEST_CASE("liquids carried over behind other buildings on the tile still arrive") {
    // A pool and a fountain earlier on the tile spend the first step's budget
    // before the city is reached; the city is begun in that step and carried.
    Placement pool;
    pool.uniqueId = 1;
    pool.liquid = {true, false, true};
    Placement shed;
    shed.uniqueId = 2;
    shed.liquid = {false, false};
    std::set<uint32_t> placed;
    const auto loaded = runTile({pool, shed, stormwind()}, placed);
    std::set<Loaded> unique(loaded.begin(), loaded.end());
    REQUIRE(unique.size() == loaded.size());
    REQUIRE(loaded.size() == 7);
    for (size_t g : {224u, 281u, 282u, 283u, 284u}) {
        REQUIRE(unique.count({10047, g}) == 1);
    }
}

TEST_CASE("a placement another tile made is still skipped") {
    std::set<uint32_t> placed = {10047};
    const auto loaded = runTile({stormwind()}, placed);
    REQUIRE(loaded.empty());
}

TEST_CASE("a step does not carry a model over for groups with no liquid") {
    // The budget is spent on the last liquid group; the 1 group after it has
    // none, so the model is finished in this step.
    std::vector<bool> liquid = {true, true, true, true, false};
    size_t next = 0;
    size_t used = 0;
    int loads = 0;
    const bool done = ls::step(liquid.size(), next, used, 4, [&](size_t g) { return liquid[g]; },
                               [&](size_t) { ++loads; });
    REQUIRE(done);
    REQUIRE(loads == 4);
    REQUIRE(next == liquid.size());
}

TEST_CASE("a step that starts with its budget spent loads nothing and keeps its place") {
    // Liquid in group 0 with nothing left to spend: the cursor must stay on
    // group 0, and the placement must still read as begun on the next step.
    std::vector<bool> liquid = {true, false};
    size_t next = 0;
    size_t used = 4;
    int loads = 0;
    const bool done = ls::step(liquid.size(), next, used, 4, [&](size_t g) { return liquid[g]; },
                               [&](size_t) { ++loads; });
    REQUIRE_FALSE(done);
    REQUIRE(loads == 0);
    REQUIRE(next == 0);
}
