// The order equipped items paint the character skin in.
//
// The stock client keeps seven layer slots per skin region and gives every item
// kind a fixed layer in each region it may paint (Wow.exe 3.3.5a, table at
// 0x009f6a00, adjustments in the region setters around 0x004effa0). Wowee used
// to paint in equipment slot order, which put trousers (slot 6) over the belt
// (slot 5) and hid every belt that has no buckle model.
#include <catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

#include "pipeline/item_textures.hpp"

using namespace wowee;
using pipeline::CharComponentSlot;
using pipeline::ItemRegionLayer;
using pipeline::ItemRegionSource;

namespace {

constexpr int kArmUpper = 0, kArmLower = 1, kHand = 2, kTorsoUpper = 3,
              kTorsoLower = 4, kLegUpper = 5, kLegLower = 6, kFoot = 7;

ItemRegionSource item(CharComponentSlot slot,
                      std::vector<std::pair<int, std::string>> textures,
                      uint32_t geosetGroup1 = 0, uint32_t geosetGroup3 = 0,
                      uint32_t flags = 0) {
    ItemRegionSource src;
    src.slot = slot;
    for (auto& [region, name] : textures) src.textures[region] = name;
    src.geosetGroup1 = geosetGroup1;
    src.geosetGroup3 = geosetGroup3;
    src.flags = flags;
    return src;
}

/// The textures painted into one region, bottom first.
std::vector<std::string> region(const std::vector<ItemRegionLayer>& layers, int r) {
    std::vector<std::string> out;
    for (const auto& l : layers) {
        if (l.region == r) out.push_back(l.texture);
    }
    return out;
}

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("belt paints over trousers", "[item_texture_layers]") {
    // Ragged Leather Belt (display 14335): no buckle model, one LegUpper
    // texture. Squire's Pants: LegUpper and LegLower. Given in equipment slot
    // order, waist before legs, which is how the old loop painted them.
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Belt, {{kLegUpper, "Leather_A_03Beige_Belt_LU"}}),
        item(CharComponentSlot::Legs, {{kLegUpper, "Pants_LU"}, {kLegLower, "Pants_LL"}}),
    });
    CHECK(region(layers, kLegUpper) == Names{"Pants_LU", "Leather_A_03Beige_Belt_LU"});
    CHECK(region(layers, kLegLower) == Names{"Pants_LL"});
}

TEST_CASE("torso: shirt, chest, tabard, belt", "[item_texture_layers]") {
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Tabard, {{kTorsoUpper, "Tabard_TU"}, {kTorsoLower, "Tabard_TL"}}),
        item(CharComponentSlot::Belt, {{kTorsoLower, "Belt_TL"}}),
        item(CharComponentSlot::Chest, {{kTorsoUpper, "Chest_TU"}, {kTorsoLower, "Chest_TL"},
                                        {kArmUpper, "Chest_AU"}}),
        item(CharComponentSlot::Shirt, {{kTorsoUpper, "Shirt_TU"}, {kTorsoLower, "Shirt_TL"},
                                        {kArmUpper, "Shirt_AU"}}),
    });
    CHECK(region(layers, kArmUpper) == Names{"Shirt_AU", "Chest_AU"});
    CHECK(region(layers, kTorsoUpper) == Names{"Shirt_TU", "Chest_TU", "Tabard_TU"});
    // The belt's buckle sits over the tabard.
    CHECK(region(layers, kTorsoLower) == Names{"Shirt_TL", "Chest_TL", "Tabard_TL", "Belt_TL"});
}

TEST_CASE("forearm: shirt, chest, bracers, gloves", "[item_texture_layers]") {
    const auto plain = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Hands, {{kArmLower, "Glove_AL"}, {kHand, "Glove_HA"}}),
        item(CharComponentSlot::Wrist, {{kArmLower, "Bracer_AL"}}),
        item(CharComponentSlot::Chest, {{kArmLower, "Chest_AL"}}),
        item(CharComponentSlot::Shirt, {{kArmLower, "Shirt_AL"}}),
    });
    CHECK(region(plain, kArmLower) == Names{"Shirt_AL", "Chest_AL", "Bracer_AL", "Glove_AL"});
    CHECK(region(plain, kHand) == Names{"Glove_HA"});

    // Sleeves with a geoset (GeosetGroup1) cover bracers and plain gloves.
    const auto sleeves = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Hands, {{kArmLower, "Glove_AL"}}),
        item(CharComponentSlot::Wrist, {{kArmLower, "Bracer_AL"}}),
        item(CharComponentSlot::Chest, {{kArmLower, "Sleeve_AL"}}, /*gg1*/ 1),
    });
    CHECK(region(sleeves, kArmLower) == Names{"Bracer_AL", "Glove_AL", "Sleeve_AL"});

    // Gauntlet cuffs (gloves with GeosetGroup1) cover even those sleeves.
    const auto cuffs = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Hands, {{kArmLower, "Gauntlet_AL"}}, /*gg1*/ 1),
        item(CharComponentSlot::Chest, {{kArmLower, "Sleeve_AL"}}, /*gg1*/ 1),
    });
    CHECK(region(cuffs, kArmLower) == Names{"Sleeve_AL", "Gauntlet_AL"});
}

TEST_CASE("legs: trousers, chest, boots", "[item_texture_layers]") {
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Boots, {{kLegLower, "Boot_LL"}, {kFoot, "Boot_FO"}}),
        item(CharComponentSlot::Chest, {{kLegUpper, "Chest_LU"}, {kLegLower, "Chest_LL"}}),
        item(CharComponentSlot::Legs, {{kLegUpper, "Pants_LU"}, {kLegLower, "Pants_LL"}}),
    });
    CHECK(region(layers, kLegUpper) == Names{"Pants_LU", "Chest_LU"});
    CHECK(region(layers, kLegLower) == Names{"Pants_LL", "Chest_LL", "Boot_LL"});
    CHECK(region(layers, kFoot) == Names{"Boot_FO"});
}

TEST_CASE("robe: skirt over boots, belt over the robe", "[item_texture_layers]") {
    // A robe is a chest with GeosetGroup3. Its LegLower moves above the boots,
    // even high ones, but its LegUpper keeps the chest layer and the belt
    // paints over it.
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Belt, {{kLegUpper, "Belt_LU"}}),
        item(CharComponentSlot::Boots, {{kLegLower, "Boot_LL"}}, /*gg1*/ 1),
        item(CharComponentSlot::Legs, {{kLegUpper, "Pants_LU"}, {kLegLower, "Pants_LL"}}),
        item(CharComponentSlot::Chest, {{kLegUpper, "Robe_LU"}, {kLegLower, "Robe_LL"}}, 0, /*gg3*/ 1),
    });
    CHECK(region(layers, kLegUpper) == Names{"Pants_LU", "Robe_LU", "Belt_LU"});
    CHECK(region(layers, kLegLower) == Names{"Pants_LL", "Boot_LL", "Robe_LL"});
}

TEST_CASE("long trousers and high boots", "[item_texture_layers]") {
    // Trousers with GeosetGroup3 go over the boots; high boots (GeosetGroup1)
    // go over ordinary trousers.
    const auto kilt = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Boots, {{kLegLower, "Boot_LL"}}),
        item(CharComponentSlot::Legs, {{kLegLower, "Kilt_LL"}}, 0, /*gg3*/ 1),
    });
    CHECK(region(kilt, kLegLower) == Names{"Boot_LL", "Kilt_LL"});

    const auto high = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Boots, {{kLegLower, "HighBoot_LL"}}, /*gg1*/ 1),
        item(CharComponentSlot::Legs, {{kLegLower, "Pants_LL"}}),
    });
    CHECK(region(high, kLegLower) == Names{"Pants_LL", "HighBoot_LL"});

    // Under a robe the long trousers drop to layer 3, which high boots also
    // take; the boots are the later slot and replace them.
    const auto robe = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Boots, {{kLegLower, "HighBoot_LL"}}, /*gg1*/ 1),
        item(CharComponentSlot::Legs, {{kLegLower, "Kilt_LL"}}, 0, /*gg3*/ 1),
        item(CharComponentSlot::Chest, {{kLegLower, "Robe_LL"}}, 0, /*gg3*/ 1),
    });
    CHECK(region(robe, kLegLower) == Names{"HighBoot_LL", "Robe_LL"});
}

TEST_CASE("chest flag 4 hides trousers and the belt's leg part", "[item_texture_layers]") {
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Belt, {{kLegUpper, "Belt_LU"}, {kTorsoLower, "Belt_TL"}}),
        item(CharComponentSlot::Legs, {{kLegUpper, "Pants_LU"}, {kLegLower, "Pants_LL"}}),
        item(CharComponentSlot::Boots, {{kLegLower, "Boot_LL"}}),
        item(CharComponentSlot::Chest, {{kLegUpper, "Dress_LU"}, {kLegLower, "Dress_LL"}},
             0, 0, /*flags*/ 4),
    });
    CHECK(region(layers, kLegUpper) == Names{"Dress_LU"});
    CHECK(region(layers, kLegLower) == Names{"Dress_LL", "Boot_LL"});
    CHECK(region(layers, kTorsoLower) == Names{"Belt_TL"});
}

TEST_CASE("an item only paints the regions its kind owns", "[item_texture_layers]") {
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Shirt, {{kLegUpper, "Shirt_LU"}, {kTorsoUpper, "Shirt_TU"}}),
        item(CharComponentSlot::Belt, {{kArmUpper, "Belt_AU"}}),
        item(CharComponentSlot::Head, {{kTorsoUpper, "Helm_TU"}}),
        item(CharComponentSlot::Shoulder, {{kArmUpper, "Shoulder_AU"}}),
        item(CharComponentSlot::Hands, {{kArmUpper, "Glove_AU"}}),
        item(CharComponentSlot::None, {{kTorsoUpper, "Cape_TU"}}),
    });
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].region == kTorsoUpper);
    CHECK(layers[0].texture == "Shirt_TU");
}

TEST_CASE("output runs region by region, lowest layer first", "[item_texture_layers]") {
    const auto layers = pipeline::orderItemRegionLayers({
        item(CharComponentSlot::Boots, {{kFoot, "Boot_FO"}}),
        item(CharComponentSlot::Shirt, {{kArmUpper, "Shirt_AU"}}),
    });
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].region == kArmUpper);
    CHECK(layers[1].region == kFoot);
}

TEST_CASE("item kinds from equipment slot and inventory type", "[item_texture_layers]") {
    using pipeline::componentSlotForEquipSlot;
    using pipeline::componentSlotForInventoryType;
    CHECK(componentSlotForEquipSlot(3) == CharComponentSlot::Shirt);
    CHECK(componentSlotForEquipSlot(4) == CharComponentSlot::Chest);
    CHECK(componentSlotForEquipSlot(5) == CharComponentSlot::Belt);
    CHECK(componentSlotForEquipSlot(6) == CharComponentSlot::Legs);
    CHECK(componentSlotForEquipSlot(7) == CharComponentSlot::Boots);
    CHECK(componentSlotForEquipSlot(8) == CharComponentSlot::Wrist);
    CHECK(componentSlotForEquipSlot(9) == CharComponentSlot::Hands);
    CHECK(componentSlotForEquipSlot(14) == CharComponentSlot::None);   // back
    CHECK(componentSlotForEquipSlot(18) == CharComponentSlot::Tabard);

    CHECK(componentSlotForInventoryType(5) == CharComponentSlot::Chest);
    CHECK(componentSlotForInventoryType(20) == CharComponentSlot::Chest);  // robe
    CHECK(componentSlotForInventoryType(6) == CharComponentSlot::Belt);
    CHECK(componentSlotForInventoryType(16) == CharComponentSlot::None);   // cloak
    CHECK(componentSlotForInventoryType(19) == CharComponentSlot::Tabard);
}
