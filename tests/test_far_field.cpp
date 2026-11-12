// "Far field (enhanced)" (rendering/far_field.hpp): the colour atlas's
// layout over the map's WDL tiles, the box filter and the BC1 encoder that
// pack the minimap into it, and the limits the far field is drawn between.
#include <catch_amalgamated.hpp>

#include "rendering/far_field.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace ff = wowee::rendering::far_field;
namespace st = wowee::rendering::stock_terrain;

namespace {
std::array<int, 3> decodeTexel(const uint8_t block[8], int i) {
    const uint16_t c0 = static_cast<uint16_t>(block[0] | block[1] << 8);
    const uint16_t c1 = static_cast<uint16_t>(block[2] | block[3] << 8);
    uint32_t bits;
    std::memcpy(&bits, block + 4, 4);
    const auto e0 = ff::from565(c0), e1 = ff::from565(c1);
    const int k = (bits >> (2 * i)) & 3;
    std::array<int, 3> out{};
    for (int c = 0; c < 3; ++c) {
        if (k == 0) out[c] = e0[c];
        else if (k == 1) out[c] = e1[c];
        else if (k == 2) out[c] = (2 * e0[c] + e1[c]) / 3;
        else out[c] = (e0[c] + 2 * e1[c]) / 3;
    }
    return out;
}
}  // namespace

TEST_CASE("Far field: the atlas spans the WDL tiles' box", "[farfield]") {
    std::vector<st::WdlTile> tiles(3);
    tiles[0].tileX = 30; tiles[0].tileY = 40;
    tiles[1].tileX = 35; tiles[1].tileY = 38;
    tiles[2].tileX = 32; tiles[2].tileY = 49;
    const ff::AtlasLayout l = ff::layoutFor(tiles);
    CHECK(l.minX == 30);
    CHECK(l.minY == 38);
    CHECK(l.tilesW == 6);
    CHECK(l.tilesH == 12);
    CHECK(l.width() == 6 * ff::kTexelsPerTile);
    CHECK(ff::layoutFor({}).empty());
}

TEST_CASE("Far field: a box filter keeps a flat colour and averages a split one", "[farfield]") {
    std::vector<uint8_t> src(256 * 256 * 4);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            uint8_t* p = &src[(static_cast<size_t>(y) * 256 + x) * 4];
            p[0] = 200; p[1] = static_cast<uint8_t>(x % 2 ? 100 : 0); p[2] = 10; p[3] = 255;
        }
    std::vector<uint8_t> dst(64 * 64 * 4);
    ff::boxDownsample(src.data(), 256, 256, 64, dst.data(), 64, 0, 0);
    for (size_t i = 0; i < dst.size(); i += 4) {
        REQUIRE(dst[i] == 200);
        REQUIRE(dst[i + 1] == 50);
        REQUIRE(dst[i + 2] == 10);
    }
}

TEST_CASE("Far field: BC1 keeps a flat block and the ends of a ramp", "[farfield]") {
    uint8_t texels[16][4];
    for (auto& t : texels) { t[0] = 120; t[1] = 160; t[2] = 40; t[3] = 255; }
    uint8_t block[8];
    ff::encodeBC1Block(texels, block);
    for (int i = 0; i < 16; ++i) {
        const auto c = decodeTexel(block, i);
        CHECK(std::abs(c[0] - 120) <= 8);
        CHECK(std::abs(c[1] - 160) <= 4);
        CHECK(std::abs(c[2] - 40) <= 8);
    }
    // A ramp from dark to light: every texel within one palette step.
    for (int i = 0; i < 16; ++i) {
        const auto v = static_cast<uint8_t>(i * 16);
        texels[i][0] = texels[i][1] = texels[i][2] = v;
    }
    ff::encodeBC1Block(texels, block);
    for (int i = 0; i < 16; ++i) {
        const auto c = decodeTexel(block, i);
        CHECK(std::abs(c[1] - i * 16) <= 48);
    }
}

TEST_CASE("Far field: every mip level is encoded down to four texels a tile", "[farfield]") {
    ff::AtlasLayout l;
    l.tilesW = 2;
    l.tilesH = 3;
    std::vector<uint8_t> rgba(static_cast<size_t>(l.width()) * l.height() * 4, 128);
    const ff::Atlas a = ff::encodeAtlas(l, rgba);
    REQUIRE(a.levels.size() == static_cast<size_t>(ff::kMipLevels));
    int w = l.width(), h = l.height();
    for (const auto& level : a.levels) {
        CHECK(level.size() == static_cast<size_t>(w / 4) * (h / 4) * 8);
        w /= 2;
        h /= 2;
    }
}

TEST_CASE("Far field: it starts inside the chunks' reach and ends at the reference client's clip", "[farfield]") {
    // A chunk is kept while its centre is within the far clip, so the ground
    // reaches at least the far clip less half its diagonal.
    const float halfDiagonal = 33.333333f * std::sqrt(2.0f) * 0.5f;
    CHECK(ff::kNearInset >= halfDiagonal);
    CHECK(ff::kNearInset < 50.0f);   // the stock horizon's inset
    CHECK(ff::kReach == 7000.0f);
}
