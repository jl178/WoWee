// Which terrain tile goes into or out of the mega buffers' slots. The tiles
// nearest the camera are to draw from the slots; the choice must reach that
// after any trip, one move at a time, and must not keep swapping a tile on
// the edge of the ring in and out.
#include <catch_amalgamated.hpp>

#include "rendering/terrain_mega_slots.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace wowee::rendering;
using Kind = TerrainSlotMove::Kind;

namespace {

TerrainSlotTile tile(float distance, bool hasSlot, bool hasFallback, bool promotable = true) {
    return {.distance = distance, .hasSlot = hasSlot, .hasFallback = hasFallback, .promotable = promotable};
}

// A map of tiles 533 yards across, streamed round a camera the way the
// terrain manager does: loaded nearest first within the load radius, kept
// until past the unload radius. A tile takes a free slot when it loads,
// as uploadChunk does, and an evicted or unloaded tile's slot comes back
// only `kFenceFrames` frames later, as deferAfterAllFrameFences gives it.
struct Sim {
    static constexpr float kTile = 533.333f;
    static constexpr float kTileRadius = 377.2f;  // half the diagonal
    static constexpr int kFenceFrames = 3;
    struct Resident {
        int x = 0, y = 0;
        bool slotted = false;
    };
    uint32_t capacity;
    uint32_t freeSlots;
    bool migrate;
    int loadRadius = 6, unloadRadius = 9;
    std::vector<Resident> tiles;
    std::vector<int> releasing;  // frames left for each slot on its way back
    int moves = 0;

    Sim(uint32_t cap, bool mig) : capacity(cap), freeSlots(cap), migrate(mig) {}

    static float distance(const Resident& t, float cx, float cy) {
        const float dx = (static_cast<float>(t.x) + 0.5f) * kTile - cx;
        const float dy = (static_cast<float>(t.y) + 0.5f) * kTile - cy;
        return std::max(0.0f, std::sqrt(dx * dx + dy * dy) - kTileRadius);
    }

    void releaseSlot() { releasing.push_back(kFenceFrames); }

    void frame(float cx, float cy, float need, float margin) {
        // Slots whose frames have finished come back.
        for (auto it = releasing.begin(); it != releasing.end();) {
            if (--*it <= 0) {
                ++freeSlots;
                it = releasing.erase(it);
            } else {
                ++it;
            }
        }
        const int ctx = static_cast<int>(std::floor(cx / kTile));
        const int cty = static_cast<int>(std::floor(cy / kTile));
        // Unload past the unload radius.
        for (auto it = tiles.begin(); it != tiles.end();) {
            const int dx = it->x - ctx, dy = it->y - cty;
            if (dx * dx + dy * dy > unloadRadius * unloadRadius) {
                if (it->slotted) releaseSlot();
                it = tiles.erase(it);
            } else {
                ++it;
            }
        }
        // Load the nearest missing tile within the load radius (one a frame).
        int bestX = 0, bestY = 0, bestD = 1 << 30;
        for (int dy = -loadRadius; dy <= loadRadius; ++dy) {
            for (int dx = -loadRadius; dx <= loadRadius; ++dx) {
                if (dx * dx + dy * dy > loadRadius * loadRadius) continue;
                const int x = ctx + dx, y = cty + dy;
                const bool have = std::any_of(tiles.begin(), tiles.end(),
                                              [&](const Resident& t) { return t.x == x && t.y == y; });
                if (!have && dx * dx + dy * dy < bestD) {
                    bestD = dx * dx + dy * dy;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        if (bestD != (1 << 30)) {
            Resident r{.x = bestX, .y = bestY};
            if (freeSlots > 0) {
                --freeSlots;
                r.slotted = true;
            }
            tiles.push_back(r);
        }
        if (!migrate) return;
        std::vector<TerrainSlotTile> view;
        for (const Resident& t : tiles) view.push_back(tile(distance(t, cx, cy), t.slotted, !t.slotted));
        const TerrainSlotMove move = chooseTerrainSlotMove(view, freeSlots, !releasing.empty(), need, margin);
        if (move.kind == Kind::Promote) {
            REQUIRE(freeSlots > 0);
            REQUIRE_FALSE(tiles[move.tile].slotted);
            --freeSlots;
            tiles[move.tile].slotted = true;
            ++moves;
        } else if (move.kind == Kind::Evict) {
            REQUIRE(tiles[move.tile].slotted);
            tiles[move.tile].slotted = false;
            releaseSlot();
            ++moves;
        }
    }

    // Chunks (tiles, here) within `need` of the camera left on their own buffers.
    int nearFallback(float cx, float cy, float need) const {
        int n = 0;
        for (const Resident& t : tiles)
            if (!t.slotted && distance(t, cx, cy) <= need) ++n;
        return n;
    }

    uint32_t slotsHeld() const {
        return static_cast<uint32_t>(std::count_if(tiles.begin(), tiles.end(),
                                                   [](const Resident& t) { return t.slotted; }));
    }
};

// A long trip east across the map, then standing still.
void travel(Sim& sim, float need, float margin) {
    const float y = 32.5f * Sim::kTile;
    for (float x = 10.5f * Sim::kTile; x < 40.5f * Sim::kTile; x += 40.0f) sim.frame(x, y, need, margin);
    for (int i = 0; i < 600; ++i) sim.frame(40.5f * Sim::kTile, y, need, margin);
}

}  // namespace

TEST_CASE("nothing moves when every near tile is in a slot", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {tile(0.0f, true, false), tile(900.0f, true, false),
                                                tile(4000.0f, false, true)};  // far, outside the ring
    CHECK(chooseTerrainSlotMove(tiles, 3, false, 1200.0f, 266.0f).kind == Kind::None);
    CHECK(chooseTerrainSlotMove(tiles, 0, false, 1200.0f, 266.0f).kind == Kind::None);
}

TEST_CASE("a free slot goes to the nearest near tile on its own buffers", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {
        tile(800.0f, false, true),          // near, wanted
        tile(300.0f, false, true, false),   // nearer, but its mesh is not at hand
        tile(200.0f, false, true),          // nearest wanted
        tile(1500.0f, false, true),         // beyond the ring
        tile(0.0f, true, false),            // already in a slot
    };
    const TerrainSlotMove move = chooseTerrainSlotMove(tiles, 1, false, 1200.0f, 266.0f);
    CHECK(move.kind == Kind::Promote);
    CHECK(move.tile == 2);
}

TEST_CASE("a tile only partly in a slot is wanted like one with none", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {tile(400.0f, true, true), tile(600.0f, false, true)};
    const TerrainSlotMove move = chooseTerrainSlotMove(tiles, 1, false, 1200.0f, 266.0f);
    CHECK(move.kind == Kind::Promote);
    CHECK(move.tile == 0);
}

TEST_CASE("with no slot free the farthest slotted tile past the margin is evicted", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {
        tile(100.0f, false, true),   // wanted
        tile(2000.0f, true, false),
        tile(3500.0f, true, false),  // farthest
        tile(0.0f, true, false),
    };
    const TerrainSlotMove move = chooseTerrainSlotMove(tiles, 0, false, 1200.0f, 266.0f);
    CHECK(move.kind == Kind::Evict);
    CHECK(move.tile == 2);
}

TEST_CASE("nothing is evicted while a slot is on its way back", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {tile(100.0f, false, true), tile(3500.0f, true, false)};
    CHECK(chooseTerrainSlotMove(tiles, 0, true, 1200.0f, 266.0f).kind == Kind::None);
    // A free slot is still used.
    CHECK(chooseTerrainSlotMove(tiles, 1, true, 1200.0f, 266.0f).kind == Kind::Promote);
}

TEST_CASE("a slotted tile inside the ring or the margin is never evicted", "[megaslots]") {
    const std::vector<TerrainSlotTile> tiles = {
        tile(100.0f, false, true),
        tile(1100.0f, true, false),  // inside the ring
        tile(1400.0f, true, false),  // past the ring, inside the margin
        tile(1466.0f, true, false),  // exactly on need + margin
    };
    CHECK(chooseTerrainSlotMove(tiles, 0, false, 1200.0f, 266.0f).kind == Kind::None);
}

TEST_CASE("ties go to the lower index", "[megaslots]") {
    const std::vector<TerrainSlotTile> want = {tile(500.0f, false, true), tile(500.0f, false, true)};
    CHECK(chooseTerrainSlotMove(want, 1, false, 1200.0f, 266.0f).tile == 0);
    const std::vector<TerrainSlotTile> evict = {tile(100.0f, false, true), tile(3000.0f, true, false),
                                                tile(3000.0f, true, false)};
    CHECK(chooseTerrainSlotMove(evict, 0, false, 1200.0f, 266.0f).tile == 1);
}

TEST_CASE("a NaN distance is neither wanted nor evicted", "[megaslots]") {
    const float nan = std::nanf("");
    const std::vector<TerrainSlotTile> tiles = {tile(nan, false, true), tile(nan, true, false)};
    CHECK(chooseTerrainSlotMove(tiles, 1, false, 1200.0f, 266.0f).kind == Kind::None);
    CHECK(chooseTerrainSlotMove(tiles, 0, false, 1200.0f, 266.0f).kind == Kind::None);
}

TEST_CASE("after a long trip the near tiles are left on their own buffers without moves",
          "[megaslots]") {
    // What the placement is for: with 42 slots and nothing moved, tiles that
    // loaded while the slots were full end up next to the camera.
    Sim sim(42, false);
    travel(sim, 1200.0f, 266.0f);
    CHECK(sim.nearFallback(40.5f * Sim::kTile, 32.5f * Sim::kTile, 1200.0f) > 0);
}

TEST_CASE("after a long trip every near tile is in a slot", "[megaslots]") {
    for (uint32_t capacity : {42u, 128u}) {
        for (float need : {1200.0f, 2400.0f}) {
            if (capacity == 42 && need > 1500.0f) continue;  // the ring holds more tiles than 42
            CAPTURE(capacity, need);
            Sim sim(capacity, true);
            travel(sim, need, 266.0f);
            CHECK(sim.nearFallback(40.5f * Sim::kTile, 32.5f * Sim::kTile, need) == 0);
            CHECK(sim.slotsHeld() + sim.freeSlots + sim.releasing.size() == capacity);
        }
    }
}

TEST_CASE("pacing back and forth inside the margin stops moving once settled", "[megaslots]") {
    // The first swings bring in the tiles the ring reaches at either end;
    // after that a tile that came in stays within the margin and nothing
    // that went out comes back into the ring.
    Sim sim(42, true);
    travel(sim, 1200.0f, 266.0f);
    const float x0 = 40.5f * Sim::kTile, y = 32.5f * Sim::kTile;
    int i = 0;
    const auto pace = [&](int frames) {
        for (int end = i + frames; i < end; ++i)
            sim.frame(x0 + 120.0f * std::sin(static_cast<float>(i) * 0.05f), y, 1200.0f, 266.0f);
    };
    pace(4 * 126);  // four swings
    const int settled = sim.moves;
    pace(20 * 126);
    CHECK(sim.moves == settled);
    CHECK(sim.nearFallback(x0 + 120.0f * std::sin(static_cast<float>(i - 1) * 0.05f), y, 1200.0f) == 0);
}
