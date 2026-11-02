// The indoor minimap's arithmetic: which md5translate keys a WMO group's tiles
// have, and where each tile lands. Numbers are Ironforge's, from the 3.3.5
// data (World\wmo\KhazModan\Cities\Ironforge\Ironforge.wmo).
//
// How the layout was settled. For 26 of Ironforge's groups (every fourth from
// 0 to 100, with 44 and 88 among them) the group's triangles were drawn into a
// mask from the MOGP header box, 2 pixels a yard, and each tile's pixels were
// scored by how much brighter they are inside the mask than outside it, under
// five readings: columns along +x or -x, rows along +y or -y, transposed, and
// a cropped tile anchored at either end. One reading scores best on every one
// of them (group 44: 26.3 grey levels, the next 7.4; group 88: 49.2):
//
//   - column c of tile (x, y) is local x = min.x + 128 x + c / 2;
//   - row r of a tile h pixels high is local y = min.y + 128 y + (h - r) / 2,
//     so row 0 is the high-y edge and a short tile keeps its low-y side.
//
// Group 44 (51 x 201 yards) is the plainest case: two 128 x 256 tiles, _00_00
// and _00_01; _00_01 is black over its top 110 rows, which is the 55 yards
// past the box's max.y, and the corridor's bend is the right way round only
// with rows running toward -y.
#include <catch_amalgamated.hpp>
#include "rendering/wmo_minimap.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace mm = wowee::rendering::wmo_minimap;

namespace {

// MOGP header boxes, local space.
const glm::vec3 kG44Min(-583.5418701171875f, -100.51414489746094f, -3.4685158729553223f);
const glm::vec3 kG44Max(-532.2286987304688f, 100.50852966308594f, 29.5855655670166f);
const glm::vec3 kG1Min(-91.67835998535156f, 41.86946105957031f, -5.222140312194824f);
const glm::vec3 kG1Max(-71.03341674804688f, 76.82889556884766f, 55.19541549682617f);
const glm::vec3 kG88Min(-286.07525634765625f, -119.0390625f, -140.3274383544922f);
const glm::vec3 kG88Max(-157.678955078125f, -11.32699203491211f, -3.5730252265930176f);

bool near(const glm::vec2& a, const glm::vec2& b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f;
}

}  // namespace

TEST_CASE("tile root is the path less World and the extension, lower case", "[wmo_minimap]") {
    CHECK(mm::tileRoot("World\\wmo\\KhazModan\\Cities\\Ironforge\\Ironforge.wmo") ==
          "wmo\\khazmodan\\cities\\ironforge\\ironforge");
    CHECK(mm::tileRoot("World/wmo/Lorderaeron/Undercity/Undercity.WMO") ==
          "wmo\\lorderaeron\\undercity\\undercity");
    // Nothing to take off: left as it is, lowered.
    CHECK(mm::tileRoot("wmo\\Dungeon\\X") == "wmo\\dungeon\\x");
}

TEST_CASE("tile key: group in three digits, column and row in two", "[wmo_minimap]") {
    const std::string root = "wmo\\khazmodan\\cities\\ironforge\\ironforge";
    CHECK(mm::tileKey(root, 44, 0, 1) == root + "_044_00_01");
    CHECK(mm::tileKey(root, 1, 0, 0) == root + "_001_00_00");
    CHECK(mm::tileKey(root, 103, 12, 3) == root + "_103_12_03");
}

TEST_CASE("tile count: 128 yards a tile, at least one", "[wmo_minimap]") {
    // The extents md5translate has two tiles for and one for.
    CHECK(mm::tileCount(kG44Max.x - kG44Min.x) == 1);   // 51.3
    CHECK(mm::tileCount(kG44Max.y - kG44Min.y) == 2);   // 201.0
    CHECK(mm::tileCount(kG88Max.x - kG88Min.x) == 2);   // 128.4
    CHECK(mm::tileCount(kG88Max.y - kG88Min.y) == 1);   // 107.7
    CHECK(mm::tileCount(185.6f) == 2);
    CHECK(mm::tileCount(128.0f) == 1);
    CHECK(mm::tileCount(0.0f) == 1);
}

TEST_CASE("group 44: two tiles stacked along y, rows toward -y", "[wmo_minimap]") {
    const glm::vec2 mn(kG44Min);
    // _00_00, 128 x 256: 64 yards along x, the box's first 128 along y.
    const mm::TileCorners t0 = mm::tileCorners(mn, 0, 0, 128, 256);
    CHECK(near(t0.uv01, mn));                                   // bottom-left texel corner
    CHECK(near(t0.uv11, mn + glm::vec2(64.0f, 0.0f)));
    CHECK(near(t0.uv00, mn + glm::vec2(0.0f, 128.0f)));         // top row: high y
    CHECK(near(t0.uv10, mn + glm::vec2(64.0f, 128.0f)));
    // _00_01 continues upward: its bottom row meets _00_00's top row.
    const mm::TileCorners t1 = mm::tileCorners(mn, 0, 1, 128, 256);
    CHECK(near(t1.uv01, t0.uv00));
    CHECK(near(t1.uv00, mn + glm::vec2(0.0f, 256.0f)));
    // The box ends 201 yards up, 55 short of that tile's top: the empty rows
    // at the top of _00_01.
    CHECK(t1.uv00.y - kG44Max.y == Catch::Approx(55.0f).margin(0.1f));
}

TEST_CASE("a cropped tile keeps the box's low corner", "[wmo_minimap]") {
    // Group 1 is 20.6 x 35 yards in one 64 x 128 tile: 32 x 64 yards, from
    // the box minimum.
    const glm::vec2 mn(kG1Min);
    const mm::TileCorners t = mm::tileCorners(mn, 0, 0, 64, 128);
    CHECK(near(t.uv01, mn));
    CHECK(near(t.uv10, mn + glm::vec2(32.0f, 64.0f)));
    CHECK(t.uv10.x >= kG1Max.x);
    CHECK(t.uv10.y >= kG1Max.y);
}

TEST_CASE("group 88: the second column starts 128 yards along x", "[wmo_minimap]") {
    const glm::vec2 mn(kG88Min);
    const mm::TileCorners a = mm::tileCorners(mn, 0, 0, 256, 256);
    const mm::TileCorners b = mm::tileCorners(mn, 1, 0, 256, 256);
    CHECK(near(b.uv00, a.uv10));
    CHECK(near(b.uv01, a.uv11));
    CHECK(b.uv11.x == Catch::Approx(mn.x + 256.0f));
}

TEST_CASE("groups to draw: indoor, on the player's level, current last", "[wmo_minimap]") {
    using G = mm::Group;
    std::vector<G> groups = {
        {0, {0, 0, 0}, {10, 10, 10}, mm::kGroupIndoor},      // level, low floor
        {1, {0, 0, 20}, {10, 10, 30}, mm::kGroupIndoor},     // the floor above
        {2, {0, 0, -5}, {10, 10, 40}, mm::kGroupIndoor},     // a tall hall
        {3, {0, 0, 0}, {10, 10, 10}, 0x8},                   // outdoors
        {4, {0, 0, 2}, {10, 10, 8}, mm::kGroupIndoor},       // the player's
    };
    const std::vector<size_t> order = mm::groupsToDraw(groups, 4, 5.0f);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 2);  // lowest box first
    CHECK(order[1] == 0);
    CHECK(order[2] == 4);  // the current group on top

    // Up the stairs: the floor above and the hall, not the one below.
    const std::vector<size_t> up = mm::groupsToDraw(groups, 1, 25.0f);
    REQUIRE(up.size() == 2);
    CHECK(up[0] == 2);
    CHECK(up[1] == 1);

    // The player's group is drawn even outside its own height range.
    CHECK(mm::groupsToDraw(groups, 4, 100.0f) == std::vector<size_t>{4});
}

TEST_CASE("composite: centre at the middle, north up, east right", "[wmo_minimap]") {
    const glm::vec2 c(100.0f, 200.0f);
    const float span = 400.0f;
    CHECK(near(mm::renderToComposite(c, c, span), {0.5f, 0.5f}));
    // Render x is north: north is up the screen, which is -u (the display
    // reads u down the screen and v across it).
    CHECK(near(mm::renderToComposite(c + glm::vec2(40.0f, 0.0f), c, span), {0.4f, 0.5f}));
    // Render y is west: west is left, -v.
    CHECK(near(mm::renderToComposite(c + glm::vec2(0.0f, 40.0f), c, span), {0.5f, 0.4f}));
}

TEST_CASE("a yawed placement turns the tile with the building", "[wmo_minimap]") {
    // A quarter turn about z: local +x becomes render +y (west). The tile's
    // high-x edge then lies west of its low-x edge, so further left on the map.
    const glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    const mm::TileCorners t = mm::tileCorners(glm::vec2(0.0f), 0, 0, 256, 256);
    auto place = [&](const glm::vec2& local) {
        return mm::renderToComposite(glm::vec2(m * glm::vec4(local, 0.0f, 1.0f)), glm::vec2(0.0f), 512.0f);
    };
    const glm::vec2 lo = place(t.uv01);  // local (0, 0)
    const glm::vec2 hi = place(t.uv11);  // local (128, 0)
    CHECK(hi.y < lo.y);                                 // west of it
    CHECK(hi.x == Catch::Approx(lo.x).margin(1e-5f));   // same north-south
}
