// The world map Lua API's arithmetic (addons/world_map_api.hpp, detail), held
// to Wow.exe 3.3.5a (12340). Header-only: no client data, no game.
#include <catch_amalgamated.hpp>
#include "addons/world_map_api.hpp"

#include <limits>

using namespace wowee::addons::worldmap::detail;

TEST_CASE("Lua numbers become ints the way the map handlers do", "[world_map_lua]") {
    // FISTP, default rounding: nearest, ties to even.
    CHECK(luaRound(2.5) == 2);
    CHECK(luaRound(3.5) == 4);
    CHECK(luaRound(-0.5) == 0);
    CHECK(luaRound(1.4999) == 1);
    // _ftol on the double itself: 2.9999999999 is 2, not the 3 it becomes
    // once squeezed through a float.
    CHECK(luaTruncate(2.9999999999) == 2);
    CHECK(luaTruncate(-1.5) == -1);
    // Integer indefinite: the low half is zero.
    CHECK(luaRound(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(luaTruncate(1e30) == 0);
    // 64 bits, low 32 kept.
    CHECK(luaRound(4294967297.0) == 1);
}

TEST_CASE("WorldMap rectangles are closed on every side", "[world_map_lua]") {
    // 0x00544420 / 0x005468F0 / 0x00488D40 / 0x00543920
    CHECK(closedRectContains(0.1f, 0.2f, 0.5f, 0.6f, 0.1f, 0.2f));
    CHECK(closedRectContains(0.1f, 0.2f, 0.5f, 0.6f, 0.5f, 0.6f));
    CHECK_FALSE(closedRectContains(0.1f, 0.2f, 0.5f, 0.6f, 0.50001f, 0.4f));
    CHECK_FALSE(closedRectContains(0.1f, 0.2f, 0.5f, 0.6f, 0.3f, 0.19999f));
    CHECK_FALSE(closedRectContains(0.1f, 0.2f, 0.5f, 0.6f,
                                   std::numeric_limits<float>::quiet_NaN(), 0.4f));
}

TEST_CASE("A point off the map comes back as zero", "[world_map_lua]") {
    float u = 1.0f, v = 1.0f;
    clampToMap(u, v);
    CHECK(u == 1.0f);
    CHECK(v == 1.0f);
    u = 1.0001f; v = 0.5f;
    clampToMap(u, v);
    CHECK(u == 0.0f);
    CHECK(v == 0.0f);
    u = 0.5f; v = std::numeric_limits<float>::quiet_NaN();
    clampToMap(u, v);
    CHECK(u == 0.0f);
    CHECK(v == 0.0f);
    u = std::numeric_limits<float>::quiet_NaN(); v = 0.5f;
    clampToMap(u, v);
    CHECK(u == 0.0f);
    CHECK(v == 0.0f);
}

TEST_CASE("The .zmp grid cell under a point", "[world_map_lua]") {
    CHECK(zmpCell(0.0f, 0.0f) == 0);
    CHECK(zmpCell(1.0f / 128.0f, 0.0f) == 128);
    // Column 128 is the next row's first cell, as the client reads it.
    CHECK(zmpCell(0.0f, 1.0f) == 128);
    // Ties to even.
    CHECK(zmpCell(0.0f, 0.5f / 128.0f) == 0);
    CHECK(zmpCell(0.0f, 1.5f / 128.0f) == 2);
    // The last row and column stay inside the grid.
    CHECK(zmpCell(1.0f, 1.0f) == 128 * 128 - 1);
}

TEST_CASE("A landmark's icon slot follows its world state", "[world_map_lua]") {
    CHECK(landmarkIconSlot(false, 5) == 0);
    CHECK(landmarkIconSlot(true, 0) == 0);
    CHECK(landmarkIconSlot(true, 1) == 0);
    CHECK(landmarkIconSlot(true, 2) == 1);
    CHECK(landmarkIconSlot(true, 9) == 8);
    CHECK(landmarkIconSlot(true, 10) == 0);
    CHECK(landmarkIconSlot(true, -3) == 0);
}

TEST_CASE("GetMapInfo and UpdateMapHighlight texture sizes", "[world_map_lua]") {
    // 0x00544CA0: low byte up to a power of two.
    CHECK(lowBytePow2(668) == 768);
    CHECK(lowBytePow2(0x100) == 0x100);
    CHECK(lowBytePow2(0x80) == 0x80);
    // 0x00545110
    CHECK(highlightTextureSize(0x80) == 0x80);
    CHECK(highlightTextureSize(100) == 128);
    CHECK(highlightTextureSize(0x100) == 0x100);
    CHECK(highlightTextureSize(300) == 64 + 0x100 + 0x100);
}

TEST_CASE("The gossip point's lifetime", "[world_map_lua]") {
    CHECK(kGossipPoiSeconds == 480);
    CHECK(kGossipPoiReachedSq == 100.0f);
}
