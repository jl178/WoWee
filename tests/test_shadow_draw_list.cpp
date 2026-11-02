// The pieces the shadow passes keep their draws with: the light-box test that
// drops a character, the merge that turns a run of terrain chunks into one
// draw, the tile grouping kept up to date by appending, and the mega-buffer
// slot pool. Each of them may only ever leave out what draws nothing, merge
// what draws the same, or group exactly as a full regroup would.
#include <catch_amalgamated.hpp>

#include "rendering/shadow_draw_list.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace wowee::rendering;

namespace {

// A sun-style light: orthographic, depth 0..1, looking down at an angle on a
// point far from the origin, as the shadow pass's matrix does.
glm::mat4 lightMatrix(const glm::vec3& centre, float halfExtent) {
    const glm::vec3 dir = glm::normalize(glm::vec3(0.4f, -0.3f, -0.85f));
    const glm::mat4 view = glm::lookAtRH(centre - dir * 400.0f, centre, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 proj = glm::orthoRH_ZO(-halfExtent, halfExtent, -halfExtent, halfExtent, 1.0f, 800.0f);
    return proj * view;
}

bool clipInside(const glm::mat4& m, const glm::vec3& p) {
    const glm::vec4 c = m * glm::vec4(p, 1.0f);
    return std::abs(c.x) <= 1.0f && std::abs(c.y) <= 1.0f && c.z >= 0.0f && c.z <= 1.0f;
}

}  // namespace

TEST_CASE("a sphere on the map is never culled", "[shadowcull]") {
    const glm::vec3 centre(-1200.0f, 4300.0f, 25.0f);
    const glm::mat4 m = lightMatrix(centre, 200.0f);
    CHECK_FALSE(sphereOutsideOrthoClip(m, centre, 0.0f));
    CHECK_FALSE(sphereOutsideOrthoClip(m, centre + glm::vec3(50.0f, -30.0f, 5.0f), 4.0f));
}

TEST_CASE("a sphere far off the side or past a depth plane is culled", "[shadowcull]") {
    const glm::vec3 centre(-1200.0f, 4300.0f, 25.0f);
    const glm::mat4 m = lightMatrix(centre, 200.0f);
    CHECK(sphereOutsideOrthoClip(m, centre + glm::vec3(1000.0f, 0.0f, 0.0f), 6.0f));
    CHECK(sphereOutsideOrthoClip(m, centre + glm::vec3(0.0f, -1000.0f, 0.0f), 6.0f));
    // Toward and away from the sun, beyond the near and far planes.
    const glm::vec3 dir = glm::normalize(glm::vec3(0.4f, -0.3f, -0.85f));
    CHECK(sphereOutsideOrthoClip(m, centre - dir * 700.0f, 6.0f));
    CHECK(sphereOutsideOrthoClip(m, centre + dir * 700.0f, 6.0f));
}

TEST_CASE("a sphere reaching onto the map from outside it is kept", "[shadowcull]") {
    const glm::vec3 centre(-1200.0f, 4300.0f, 25.0f);
    const glm::mat4 m = lightMatrix(centre, 200.0f);
    // Walk a point straight out of the map along light-space x until its
    // centre is off it, and check a sphere big enough to reach back is kept.
    const glm::vec3 right = glm::normalize(glm::vec3(m[0][0], m[1][0], m[2][0]));
    glm::vec3 p = centre;
    while (clipInside(m, p)) p += right * 1.0f;
    p += right * 3.0f;  // 3-4 yards past the edge
    CHECK(sphereOutsideOrthoClip(m, p, 1.0f));
    CHECK_FALSE(sphereOutsideOrthoClip(m, p, 5.0f));
}

TEST_CASE("nothing inside a culled sphere lands on the map", "[shadowcull]") {
    const glm::vec3 centre(5000.0f, -2500.0f, 80.0f);
    const glm::mat4 m = lightMatrix(centre, 150.0f);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> offset(-700.0f, 700.0f);
    std::uniform_real_distribution<float> radius(0.0f, 60.0f);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int culled = 0;
    for (int i = 0; i < 4000; ++i) {
        const glm::vec3 c = centre + glm::vec3(offset(rng), offset(rng), offset(rng) * 0.5f);
        const float r = radius(rng);
        if (!sphereOutsideOrthoClip(m, c, r)) continue;
        ++culled;
        // The six extreme points along the light's axes and random points
        // inside: none of them may be on the map.
        for (int row = 0; row < 3; ++row) {
            const glm::vec3 axis = glm::normalize(glm::vec3(m[0][row], m[1][row], m[2][row]));
            REQUIRE_FALSE(clipInside(m, c + axis * r));
            REQUIRE_FALSE(clipInside(m, c - axis * r));
        }
        for (int k = 0; k < 32; ++k) {
            glm::vec3 d(unit(rng), unit(rng), unit(rng));
            if (glm::length(d) > 1.0f || glm::length(d) == 0.0f) continue;
            REQUIRE_FALSE(clipInside(m, c + d * r));
        }
    }
    CHECK(culled > 100);  // the test above was actually put to work
}

TEST_CASE("a projective matrix or a bad radius is never culled", "[shadowcull]") {
    const glm::mat4 persp = glm::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 100.0f);
    CHECK_FALSE(sphereOutsideOrthoClip(persp, glm::vec3(0.0f, 0.0f, 50.0f), 1.0f));
    const glm::mat4 m = lightMatrix(glm::vec3(0.0f), 100.0f);
    CHECK_FALSE(sphereOutsideOrthoClip(m, glm::vec3(5000.0f, 0.0f, 0.0f), -1.0f));
    CHECK_FALSE(sphereOutsideOrthoClip(m, glm::vec3(5000.0f, 0.0f, 0.0f),
                                       std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("index ranges that follow one another merge", "[shadowmerge]") {
    IndexRange a{.firstIndex = 768, .indexCount = 768, .vertexOffset = 0};
    CHECK(tryExtendIndexRange(a, {.firstIndex = 1536, .indexCount = 700, .vertexOffset = 0}));
    CHECK(a.firstIndex == 768);
    CHECK(a.indexCount == 1468);
    // A gap, an overlap and a different vertex offset do not.
    CHECK_FALSE(tryExtendIndexRange(a, {.firstIndex = 3000, .indexCount = 768, .vertexOffset = 0}));
    CHECK_FALSE(tryExtendIndexRange(a, {.firstIndex = 2000, .indexCount = 768, .vertexOffset = 0}));
    CHECK_FALSE(tryExtendIndexRange(a, {.firstIndex = 2236, .indexCount = 768, .vertexOffset = 145}));
    CHECK(a.indexCount == 1468);
}

TEST_CASE("an index range never wraps when merged", "[shadowmerge]") {
    IndexRange a{.firstIndex = 0, .indexCount = UINT32_MAX - 10, .vertexOffset = 0};
    CHECK_FALSE(tryExtendIndexRange(a, {.firstIndex = UINT32_MAX - 10, .indexCount = 20, .vertexOffset = 0}));
    CHECK(a.indexCount == UINT32_MAX - 10);
}

namespace {

struct FakeChunk {
    uint64_t tile;
    glm::vec3 centre;
    float radius;
};

TileChunkGrouping groupAll(const std::vector<FakeChunk>& chunks, size_t count) {
    TileChunkGrouping g;
    g.append(
        count, [&](size_t i) { return chunks[i].tile; },
        [&](uint32_t i, glm::vec3& c, float& r) {
            c = chunks[i].centre;
            r = chunks[i].radius;
        },
        nullptr);
    return g;
}

}  // namespace

TEST_CASE("appending chunks groups them as grouping them all at once does", "[tilegroup]") {
    // Tiles arriving 16 chunks at a time, two of them interleaved, and one
    // tile coming back after another has started.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-500.0f, 500.0f);
    std::vector<FakeChunk> chunks;
    const uint64_t order[] = {3, 3, 5, 3, 5, 9, 9, 3, 12, 5, 12, 12};
    for (uint64_t tile : order) {
        for (int k = 0; k < 16; ++k)
            chunks.push_back({tile, glm::vec3(pos(rng), pos(rng), pos(rng) * 0.1f), 30.0f + k});
    }

    TileChunkGrouping inc;
    std::vector<uint32_t> touched;
    for (size_t n = 16; n <= chunks.size(); n += 16) {
        inc.append(
            n, [&](size_t i) { return chunks[i].tile; },
            [&](uint32_t i, glm::vec3& c, float& r) {
                c = chunks[i].centre;
                r = chunks[i].radius;
            },
            &touched);
        REQUIRE(touched.size() == 1);
        CHECK(inc.tiles[touched[0]].chunkIndices.back() == n - 1);

        const TileChunkGrouping all = groupAll(chunks, n);
        REQUIRE(inc.tiles.size() == all.tiles.size());
        for (size_t s = 0; s < all.tiles.size(); ++s) {
            CHECK(inc.tiles[s].chunkIndices == all.tiles[s].chunkIndices);
            // The same arithmetic over the same list: equal to the bit.
            CHECK(inc.tiles[s].center == all.tiles[s].center);
            CHECK(inc.tiles[s].radius == all.tiles[s].radius);
        }
    }
    // Slots by first appearance: 3, 5, 9, 12.
    CHECK(inc.slotOf.at(3) == 0);
    CHECK(inc.slotOf.at(5) == 1);
    CHECK(inc.slotOf.at(9) == 2);
    CHECK(inc.slotOf.at(12) == 3);
}

TEST_CASE("a tile's sphere holds every chunk's sphere", "[tilegroup]") {
    std::vector<FakeChunk> chunks;
    for (int k = 0; k < 256; ++k)
        chunks.push_back({1, glm::vec3((k % 16) * 33.3f, (k / 16) * 33.3f, (k % 7) * 4.0f), 24.0f});
    const TileChunkGrouping g = groupAll(chunks, chunks.size());
    REQUIRE(g.tiles.size() == 1);
    for (const auto& c : chunks)
        CHECK(glm::length(c.centre - g.tiles[0].center) + c.radius <= g.tiles[0].radius + 1e-3f);
}

TEST_CASE("appending nothing touches nothing", "[tilegroup]") {
    std::vector<FakeChunk> chunks(20, FakeChunk{4, glm::vec3(1.0f), 2.0f});
    TileChunkGrouping g = groupAll(chunks, 20);
    std::vector<uint32_t> touched{99};
    g.append(
        20, [&](size_t i) { return chunks[i].tile; },
        [&](uint32_t i, glm::vec3& c, float& r) {
            c = chunks[i].centre;
            r = chunks[i].radius;
        },
        &touched);
    CHECK(touched.empty());
    CHECK(g.grouped == 20);
}

TEST_CASE("slots come out lowest first and go back to be reused", "[slots]") {
    LowestFreeSlots pool(4);
    CHECK(pool.capacity() == 4);
    CHECK(pool.acquire() == 0);
    CHECK(pool.acquire() == 1);
    CHECK(pool.acquire() == 2);
    CHECK(pool.acquire() == 3);
    CHECK(pool.acquire() == -1);
    CHECK(pool.freeCount() == 0);

    // A tile unloaded frees its slot; the next tile takes the lowest free.
    pool.release(2);
    pool.release(1);
    CHECK(pool.freeCount() == 2);
    CHECK(pool.acquire() == 1);
    CHECK(pool.acquire() == 2);
    CHECK(pool.acquire() == -1);

    // Freeing twice or out of range changes nothing.
    pool.release(3);
    pool.release(3);
    pool.release(17);
    CHECK(pool.freeCount() == 1);
    CHECK(pool.acquire() == 3);

    pool.releaseAll();
    CHECK(pool.freeCount() == 4);
    CHECK(pool.acquire() == 0);
}
