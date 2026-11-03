// The stock client's terrain arithmetic (rendering/stock_terrain.hpp), held
// to what Wow.exe 3.3.5a and its terrain programs do at the addresses the
// header names.
#include <catch_amalgamated.hpp>

#include "rendering/stock_terrain.hpp"

#include <cstring>
#include <vector>

namespace st = wowee::rendering::stock_terrain;
using Catch::Approx;

TEST_CASE("without big alpha the layers are a chain of lerps", "[terrain][stock]") {
    // terrain1.bls variant 6: lerp(lerp(lerp(t0, t1, a1), t2, a2), t3, a3).
    const float a1 = 0.6f, a2 = 0.5f, a3 = 0.25f;
    const float t[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float chain = t[0];
    chain += (t[1] - chain) * a1;
    chain += (t[2] - chain) * a2;
    chain += (t[3] - chain) * a3;
    const auto w = st::layerWeights(a1, a2, a3, false);
    CHECK(w[0] * t[0] + w[1] * t[1] + w[2] * t[2] + w[3] * t[3] == Approx(chain));
    CHECK(w[0] + w[1] + w[2] + w[3] == Approx(1.0f));
    // A full top layer hides everything under it.
    const auto top = st::layerWeights(0.7f, 0.3f, 1.0f, false);
    CHECK(top[0] == Approx(0.0f));
    CHECK(top[3] == Approx(1.0f));
}

TEST_CASE("with big alpha the layers are weights and the base has the rest", "[terrain][stock]") {
    // terrain1.bls variant 14: t0 * (1 - sat(a1 + a2 + a3)) + t1 a1 + t2 a2 + t3 a3.
    const auto w = st::layerWeights(0.2f, 0.3f, 0.1f, true);
    CHECK(w[0] == Approx(0.4f));
    CHECK(w[1] == Approx(0.2f));
    CHECK(w[2] == Approx(0.3f));
    CHECK(w[3] == Approx(0.1f));
    // Past one the base is gone and the layers keep their own weights.
    const auto over = st::layerWeights(0.6f, 0.6f, 0.0f, true);
    CHECK(over[0] == Approx(0.0f));
    CHECK(over[1] == Approx(0.6f));
}

TEST_CASE("a scrolling layer moves by the stock direction and speed tables", "[terrain][stock]") {
    // Not animated: no offset.
    CHECK(st::animOffset(0x07, 10.0f) == glm::vec2(0.0f));
    // Direction 4 (+1, 0), speed 7 (divisor 1): the accumulator is 10 after
    // 10 s; c13.x = -10 * (1 / 0.24) * 0.125, and v takes c13.x.
    const glm::vec2 o = st::animOffset(0x40 | (7 << 3) | 4, 10.0f);
    CHECK(o.x == Approx(0.0f));
    CHECK(o.y == Approx(-10.0f / 0.24f * 0.125f));
    // Speed 0 divides by 64.
    const glm::vec2 slow = st::animOffset(0x40 | 4, 10.0f);
    CHECK(slow.y == Approx(o.y / 64.0f));
    // Only the upper end wraps: +1 goes back to 0 at 64 s, -1 keeps going.
    CHECK(st::animAccumulator(1.0f, 70.0f) == Approx(6.0f));
    CHECK(st::animAccumulator(-1.0f, 70.0f) == Approx(-70.0f));
    // Direction 2 (0, +1) moves u.
    const glm::vec2 u = st::animOffset(0x40 | (7 << 3) | 2, 1.0f);
    CHECK(u.x == Approx(-1.0f / 0.24f * 0.125f));
    CHECK(u.y == Approx(0.0f));
}

TEST_CASE("the highlight is the stock per-vertex Blinn term at power 20", "[terrain][stock]") {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    // Eye and light both straight up: N.H = 1, the full colour.
    CHECK(st::specular(up, up, up, glm::vec3(0.5f)).x == Approx(0.5f));
    // 60 degrees apart about the normal: H is the normal again.
    const glm::vec3 l = glm::normalize(glm::vec3(0.5f, 0.0f, 0.866f));
    const glm::vec3 e = glm::normalize(glm::vec3(-0.5f, 0.0f, 0.866f));
    CHECK(st::specular(up, l, e, glm::vec3(1.0f)).x == Approx(1.0f));
    // Off the mirror direction it falls as cos^20.
    const glm::vec3 h = glm::normalize(l + up);
    CHECK(st::specular(up, l, up, glm::vec3(1.0f)).x == Approx(std::pow(h.z, 20.0f)));
}

TEST_CASE("the specular tileset is the name with _s before the extension", "[terrain][stock]") {
    CHECK(st::specularTexturePath("Tileset\\Elwynn\\ElwynnGrassBase.blp") ==
          "Tileset\\Elwynn\\ElwynnGrassBase_s.blp");
    CHECK(st::specularTexturePath("noext").empty());
    CHECK(st::specularTexturePath("dir.v2\\noext").empty());
}

TEST_CASE("the horizon draws from the far clip less 50 to four far clips", "[terrain][stock]") {
    const glm::vec2 r = st::horizonDepthRange(1277.0f);
    CHECK(r.x == Approx(1227.0f));
    CHECK(r.y == Approx(5108.0f));
}

namespace {
void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(&d[at], &v, 4); }
void putTag(std::vector<uint8_t>& d, size_t at, const char* tag) {
    // Stored reversed: "MAOF" is the bytes F O A M.
    for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(tag[3 - i]);
}
}  // namespace

TEST_CASE("a WDL tile is read from its MAOF slot with heights and holes", "[terrain][stock]") {
    // MVER, then MAOF (64x64 offsets), then one MARE + MAHO for tile (5, 7).
    const size_t maofAt = 12;
    const size_t mareAt = maofAt + 8 + 64 * 64 * 4;
    const size_t mareBytes = 545 * 2;
    std::vector<uint8_t> d(mareAt + 8 + mareBytes + 8 + 32, 0);
    putTag(d, 0, "MVER");
    put32(d, 4, 4);
    put32(d, 8, 18);
    putTag(d, maofAt, "MAOF");
    put32(d, maofAt + 4, 64 * 64 * 4);
    put32(d, maofAt + 8 + (7 * 64 + 5) * 4, static_cast<uint32_t>(mareAt));
    putTag(d, mareAt, "MARE");
    put32(d, mareAt + 4, static_cast<uint32_t>(mareBytes));
    const int16_t h0 = 236, h17 = 168, inner = -12;
    std::memcpy(&d[mareAt + 8], &h0, 2);
    std::memcpy(&d[mareAt + 8 + 17 * 2], &h17, 2);
    std::memcpy(&d[mareAt + 8 + 289 * 2], &inner, 2);
    const size_t mahoAt = mareAt + 8 + mareBytes;
    putTag(d, mahoAt, "MAHO");
    put32(d, mahoAt + 4, 32);
    const uint16_t row2 = 0x0009;
    std::memcpy(&d[mahoAt + 8 + 2 * 2], &row2, 2);

    const auto tiles = st::parseWdl(d);
    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].tileX == 5);
    CHECK(tiles[0].tileY == 7);
    CHECK(tiles[0].heights[0] == 236);
    CHECK(tiles[0].heights[17] == 168);
    CHECK(tiles[0].heights[289] == -12);
    CHECK(tiles[0].holes[2] == 0x0009);

    std::vector<glm::vec3> verts;
    st::wdlTileVertices(tiles[0], 1600.0f / 3.0f, verts);
    REQUIRE(verts.size() == 545);
    // The tile's first corner sits where the ADT mesh puts it (terrain_mesh.cpp).
    CHECK(verts[0].x == Approx((32.0f - 7.0f) * 1600.0f / 3.0f));
    CHECK(verts[0].y == Approx((32.0f - 5.0f) * 1600.0f / 3.0f));
    CHECK(verts[0].z == Approx(236.0f));
    // Row 1 is one chunk along x, the inner point half a chunk along both.
    CHECK(verts[17].x == Approx(verts[0].x - 100.0f / 3.0f));
    CHECK(verts[289].y == Approx(verts[0].y - 50.0f / 3.0f));

    std::vector<uint16_t> idx;
    st::wdlTileIndices(tiles[0], idx);
    // Two holes (row 2, columns 0 and 3): 254 cells of four triangles.
    CHECK(idx.size() == 254u * 12u);
}

TEST_CASE("a WDL without MAOF has no tiles", "[terrain][stock]") {
    std::vector<uint8_t> d(12, 0);
    putTag(d, 0, "MVER");
    put32(d, 4, 4);
    CHECK(st::parseWdl(d).empty());
    CHECK(st::parseWdl({}).empty());
}

TEST_CASE("the horizon's far level is two triangles per two-chunk cell", "[terrain][stock]") {
    st::WdlTile t;
    std::vector<uint16_t> idx;
    st::wdlTileIndicesCoarse(t, idx);
    CHECK(idx.size() == 8u * 8u * 6u);
    // Every index is an outer point at an even row and column.
    for (uint16_t i : idx) {
        REQUIRE(i < 289);
        CHECK((i / 17) % 2 == 0);
        CHECK((i % 17) % 2 == 0);
    }
    // A hole in any of a cell's four drops the cell.
    t.holes[3] = 1u << 5;
    idx.clear();
    st::wdlTileIndicesCoarse(t, idx);
    CHECK(idx.size() == 63u * 6u);
}
