// How the stock 3.3.5a client (build 12340) dresses and lights a unit, for the
// parts that are pure rules. See docs/re/characters.md for the addresses.
#include <catch_amalgamated.hpp>
#include "pipeline/dbc_layout.hpp"
#include <unordered_set>

using wowee::pipeline::chooseWeaponItemVisual;

TEST_CASE("a weapon's own glow wins over any enchant (0x759050)", "[item-visual]") {
    // Display visual present: enchants are not consulted.
    CHECK(chooseWeaponItemVisual(178, 103, 25) == 178);
    CHECK(chooseWeaponItemVisual(178, 0, 0) == 178);
}

TEST_CASE("the permanent enchant's glow wins over the temporary one", "[item-visual]") {
    // Crusader (visual 103) with a sharpening stone's or oil's visual on top.
    CHECK(chooseWeaponItemVisual(0, 103, 32) == 103);
    // Only a temporary one (a shaman imbue on a plain weapon).
    CHECK(chooseWeaponItemVisual(0, 0, 32) == 32);
    CHECK(chooseWeaponItemVisual(0, 0, 0) == 0);
}

TEST_CASE("the chooser is usable at compile time", "[item-visual]") {
    static_assert(chooseWeaponItemVisual(0, 155, 0) == 155);
}

// ---------------------------------------------------------------------------
// Geosets: FUN_004ed900 and HelmetGeosetVisData (0x4ef0d0).
// ---------------------------------------------------------------------------
#include "core/geoset_rules.hpp"

namespace {
using wowee::core::StockGeosetInput;
using K = StockGeosetInput;

std::unordered_set<uint16_t> dress(const StockGeosetInput& in) {
    return wowee::core::stockEquipmentGeosets(wowee::core::stockDefaultGeosetValues(), in);
}
bool groupHasOnly(const std::unordered_set<uint16_t>& s, uint16_t group, std::initializer_list<uint16_t> ids) {
    size_t n = 0;
    for (uint16_t id : s) if (id / 100 == group && id >= 100) ++n;
    if (group == 0) { n = 0; for (uint16_t id : s) if (id > 0 && id < 100) ++n; }
    if (n != ids.size()) return false;
    for (uint16_t id : ids) if (!s.count(id)) return false;
    return true;
}
}  // namespace

TEST_CASE("bare character: the stock defaults", "[geosets]") {
    const auto s = dress({});
    CHECK(s.count(0));
    CHECK(groupHasOnly(s, 4, {401}));
    CHECK(groupHasOnly(s, 5, {501}));
    CHECK(groupHasOnly(s, 7, {702}));
    CHECK(groupHasOnly(s, 9, {901}));
    CHECK(groupHasOnly(s, 13, {1301}));
    CHECK(groupHasOnly(s, 15, {1501}));
    CHECK(groupHasOnly(s, 18, {1801}));
}

TEST_CASE("gloves hide the chest's sleeves", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Chest][0] = 1;
    in.gg[K::Gloves][0] = 2;
    const auto s = dress(in);
    CHECK(groupHasOnly(s, 4, {403}));
    CHECK_FALSE(s.count(802));
    CHECK(s.count(801));
}

TEST_CASE("chest sleeves without gloves", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Chest][0] = 1;
    const auto s = dress(in);
    CHECK(s.count(802));
}

TEST_CASE("the shirt's sleeves only when nothing over it paints the lower arm", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Shirt][0] = 1;
    CHECK(dress(in).count(802));
    in.armLowerCoveredAboveShirt = true;
    CHECK_FALSE(dress(in).count(802));
}

TEST_CASE("a robe drops boots, kneepads, flaps and trousers", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Chest][2] = 1;
    in.gg[K::Boots][0] = 2;
    in.gg[K::Legs][0] = 1;
    const auto s = dress(in);
    CHECK(groupHasOnly(s, 13, {1302}));
    CHECK(groupHasOnly(s, 5, {}));
    CHECK_FALSE(s.count(902));
    CHECK(groupHasOnly(s, 11, {}));
}

TEST_CASE("legs GeosetGroup[0] of 3 or more replaces the trousers", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Legs][0] = 3;
    const auto s = dress(in);
    CHECK(s.count(1104));
    CHECK(groupHasOnly(s, 13, {}));
}

TEST_CASE("boots keep 901; kneepads only without boots", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Boots][0] = 1;
    in.gg[K::Legs][1] = 2;
    auto s = dress(in);
    CHECK(groupHasOnly(s, 5, {502}));
    CHECK(groupHasOnly(s, 9, {901}));
    in.gg[K::Boots][0] = 0;
    s = dress(in);
    CHECK(s.count(903));
}

TEST_CASE("tabard, cape, belt and the death knight's eyes", "[geosets]") {
    StockGeosetInput in;
    in.gg[K::Tabard][0] = 1;
    in.gg[K::Belt][0] = 1;
    in.gg[K::Cape][0] = 2;
    in.deathKnight = true;
    const auto s = dress(in);
    CHECK(s.count(1202));
    CHECK(groupHasOnly(s, 15, {1503}));
    CHECK(groupHasOnly(s, 18, {1802}));
    CHECK(groupHasOnly(s, 17, {1703}));
}

TEST_CASE("a cloak naming no geoset keeps 1501", "[geosets]") {
    StockGeosetInput in;
    CHECK(groupHasOnly(dress(in), 15, {1501}));
}

TEST_CASE("HelmetGeosetVisData masks are per race", "[geosets]") {
    // Row 246: hair hidden for every race but tauren (bit 6 clear).
    wowee::core::HelmGeosetVis row246;
    row246.mask[0] = 0xffffffbfu;
    auto v = wowee::core::stockDefaultGeosetValues();
    v[0] = 5;  // a hair style's scalp
    auto human = v; wowee::core::applyHelmGeosetVis(human, row246, 1);
    auto tauren = v; wowee::core::applyHelmGeosetVis(tauren, row246, 6);
    CHECK(human[0] == 1);
    CHECK(tauren[0] == 5);
    // Row 249: ears hidden on tauren and draenei (0x840).
    wowee::core::HelmGeosetVis row249;
    row249.mask[4] = 0x840u;
    auto draenei = v; wowee::core::applyHelmGeosetVis(draenei, row249, 11);
    auto human2 = v; wowee::core::applyHelmGeosetVis(human2, row249, 1);
    CHECK(draenei[7] == 701);
    CHECK(human2[7] == 702);
}

TEST_CASE("CreatureGeosetData nibbles pick groups 1-8", "[geosets]") {
    std::unordered_set<uint16_t> on{0, 101, 102, 401, 501};
    wowee::core::applyCreatureGeosetData(on, 0x1102u);
    CHECK(on.count(102));          // group 1 nibble 2
    CHECK_FALSE(on.count(101));
    CHECK(on.count(301));          // group 3 nibble 1
    CHECK(on.count(401));          // group 4 nibble 1
    CHECK(on.count(501));          // group 5 untouched (nibble 0)
}

// ---------------------------------------------------------------------------
// Unit light (CM2Lighting and the entity colour callback).
// ---------------------------------------------------------------------------
#include "rendering/unit_light_rules.hpp"

namespace ul = wowee::rendering::unit_light;

TEST_CASE("unit ambient multiplier from AreaTable", "[unit-light]") {
    CHECK(ul::ambientMultiplier(0.3f) == Catch::Approx(1.6f));
    CHECK(ul::ambientMultiplier(0.6f) == Catch::Approx(2.2f));
    CHECK(ul::ambientMultiplier(0.0f) == Catch::Approx(1.0f));
    const glm::vec3 a = ul::boostedAmbient(glm::vec3(0.4f, 0.5f, 0.6f), 2.2f);
    CHECK(a.r == Catch::Approx(0.88f));
    CHECK(a.g == Catch::Approx(1.0f));
    CHECK(a.b == Catch::Approx(1.0f));
    CHECK(ul::subareaUsesParent(12, 0x40));
    CHECK_FALSE(ul::subareaUsesParent(12, 0x2000));
    CHECK_FALSE(ul::subareaUsesParent(0, 0));
}

TEST_CASE("interior colour split: diffuse up to 168, ambient capped at 96", "[unit-light]") {
    // Dim: diffuse is raised so its brightest channel is 168/255.
    auto l = ul::interiorSplit(glm::vec3(0.2f, 0.1f, 0.05f));
    CHECK(l.diffuse.r == Catch::Approx(168.0f / 255.0f));
    CHECK(l.diffuse.g == Catch::Approx(84.0f / 255.0f));
    CHECK(l.ambient.r == Catch::Approx(0.2f));
    // Bright: ambient is capped at 96/255, diffuse kept.
    l = ul::interiorSplit(glm::vec3(0.9f, 0.45f, 0.3f));
    CHECK(l.diffuse.r == Catch::Approx(0.9f));
    CHECK(l.ambient.r == Catch::Approx(96.0f / 255.0f));
    CHECK(l.ambient.g == Catch::Approx(48.0f / 255.0f));
    // Black stays black.
    l = ul::interiorSplit(glm::vec3(0.0f));
    CHECK(l.diffuse.r == 0.0f);
}

TEST_CASE("point light falloff 1/(0.7d + 0.03d^2)", "[unit-light]") {
    CHECK(ul::pointAttenuation(1.0f) == Catch::Approx(1.0f / 0.73f));
    CHECK(ul::pointAttenuation(10.0f) == Catch::Approx(0.1f));
}

TEST_CASE("highlight adds the ambient after the saturate", "[unit-light]") {
    const glm::vec3 c = ul::finalLight(glm::vec3(1.4f, 0.3f, -0.2f), glm::vec3(0.25f));
    CHECK(c.r == Catch::Approx(1.0f));
    CHECK(c.g == Catch::Approx(0.55f));
    CHECK(c.b == Catch::Approx(0.25f));
}

// ---------------------------------------------------------------------------
// Skin composite: the 2x averaging upscale (0x4E89F0).
// ---------------------------------------------------------------------------
#include "rendering/character_composite_rules.hpp"

TEST_CASE("item textures grow 2x with neighbour averaging", "[composite]") {
    // One channel shown; every channel (alpha too) is filtered alike.
    const uint8_t src[2 * 2 * 4] = {0, 0, 0, 255, 100, 100, 100, 255,
                                    200, 200, 200, 255, 50, 50, 50, 255};
    const auto out = wowee::rendering::composite::stockUpscale2x(src, 2, 2);
    auto px = [&](int x, int y) { return out[(static_cast<size_t>(y) * 4 + x) * 4]; };
    CHECK(px(0, 0) == 0);   CHECK(px(1, 0) == 50);  CHECK(px(2, 0) == 100); CHECK(px(3, 0) == 100);
    CHECK(px(0, 1) == 100); CHECK(px(1, 1) == 87);  CHECK(px(2, 1) == 75);  CHECK(px(3, 1) == 75);
    CHECK(px(0, 2) == 200); CHECK(px(1, 2) == 125); CHECK(px(2, 2) == 50);  CHECK(px(3, 2) == 50);
    CHECK(out[3] == 255);
}
