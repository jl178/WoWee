// What the stock client binds to each texture slot of a creature display's
// model (core/creature_skin_rules.hpp, docs/re/characters.md §5).
//
// Wowee had four copies of this - the spawner, the portrait, the local mount
// and other players' mounts - and between them five fallbacks stock does not
// have: a sibling display's skins for an empty row, the name looked up at the
// root, skin1 forced onto an empty hardcoded slot, guessed gryphon file names,
// and (in the draw path) the body texture on any slot still white. The Saronite
// Animus, a water elemental whose row names no skin, came out blue instead of
// black.
#include <catch_amalgamated.hpp>
#include "core/creature_skin_rules.hpp"

#include <set>

using namespace wowee::core;

namespace {
std::function<bool(const std::string&)> having(std::set<std::string> files) {
    return [files = std::move(files)](const std::string& p) { return files.count(p) != 0; };
}
}  // namespace

TEST_CASE("the directory is cut after the model's last separator", "[creature-skin]") {
    CHECK(creatureModelDirectory("Creature\\FireElemental\\FireElemental.m2") ==
          "Creature\\FireElemental\\");
    CHECK(creatureModelDirectory("creature/wolf/wolf.m2") == "creature/wolf/");
    CHECK(creatureModelDirectory("SoundTest.m2").empty());
}

TEST_CASE("a variation is a file in the model's directory, made .blp", "[creature-skin]") {
    const std::string dir = "Creature\\FireElemental\\";
    CHECK(creatureSkinFile(dir, "FireElementalSkin") == "Creature\\FireElemental\\FireElementalSkin.blp");
    // CreateTexture (0x4B9760) replaces the extension, whatever it is.
    CHECK(creatureSkinFile(dir, "FireElementalSkin.blp") == "Creature\\FireElemental\\FireElementalSkin.blp");
    CHECK(creatureSkinFile(dir, "FireElementalSkin.tga") == "Creature\\FireElemental\\FireElementalSkin.blp");
    // Display 19092 names "DraeneiMaleKid_01." - the dot is the extension.
    CHECK(creatureSkinFile("Creature\\DraeneiMaleKid\\", "DraeneiMaleKid_01.") ==
          "Creature\\DraeneiMaleKid\\DraeneiMaleKid_01.blp");
    // A dot in the directory is not an extension.
    CHECK(creatureSkinFile("Creature\\A.B\\", "Skin") == "Creature\\A.B\\Skin.blp");
    // Empty means no texture, not a file called ".blp".
    CHECK(creatureSkinFile(dir, "").empty());
}

TEST_CASE("each slot gets what stock binds", "[creature-skin]") {
    const std::vector<CreatureModelSlot> slots = {
        {0, "Creature\\Quillboar\\Spear.blp"},
        {11, ""}, {12, ""}, {13, ""},
        {1, ""}, {6, ""},
    };
    const std::string skins[3] = {"QuilBoarSkin", "", "QuilBoarArmor"};
    const auto r = resolveCreatureSlotTextures(
        slots, "Creature\\Quillboar\\QuillBoar.m2", skins,
        having({"Creature\\Quillboar\\QuilBoarSkin.blp"}));
    REQUIRE(r.size() == slots.size());

    CHECK(r[0].source == CreatureSlotSource::ModelFile);
    CHECK(r[0].path == "Creature\\Quillboar\\Spear.blp");

    CHECK(r[1].source == CreatureSlotSource::DisplaySkin);
    CHECK(r[1].path == "Creature\\Quillboar\\QuilBoarSkin.blp");

    // Empty variation: nothing is bound (black), and skin1 is not put there.
    CHECK(r[2].source == CreatureSlotSource::Unbound);
    CHECK(r[2].path.empty());

    // Named but not shipped: the default texture, not a search elsewhere.
    CHECK(r[3].source == CreatureSlotSource::MissingFile);
    CHECK(r[3].path == "Creature\\Quillboar\\QuilBoarArmor.blp");

    // The composite's slots are not the creature skins' business.
    CHECK(r[4].source == CreatureSlotSource::NotCreature);
    CHECK(r[5].source == CreatureSlotSource::NotCreature);
}

TEST_CASE("a skin is never looked for outside the model's directory", "[creature-skin]") {
    const std::vector<CreatureModelSlot> slots = {{11, ""}};
    const std::string skins[3] = {"WolfSkin", "", ""};
    // The file exists at the root and in another creature's folder, not here.
    const auto r = resolveCreatureSlotTextures(
        slots, "Creature\\DireWolf\\DireWolf.m2", skins,
        having({"WolfSkin.blp", "Creature\\Wolf\\WolfSkin.blp"}));
    CHECK(r[0].source == CreatureSlotSource::MissingFile);
    CHECK(r[0].path == "Creature\\DireWolf\\WolfSkin.blp");
}

TEST_CASE("a display with no skins leaves every skin slot unbound", "[creature-skin]") {
    // Saronite Animus (28992): WaterElemental.m2, all three variations empty.
    const std::vector<CreatureModelSlot> slots = {{11, ""}, {0, "Creature\\WaterElemental\\Ripple.blp"}};
    const std::string skins[3] = {"", "", ""};
    const auto r = resolveCreatureSlotTextures(
        slots, "Creature\\WaterElemental\\WaterElemental.m2", skins,
        having({"Creature\\WaterElemental\\WaterElementalSkin.blp"}));
    CHECK(r[0].source == CreatureSlotSource::Unbound);
    CHECK(r[1].source == CreatureSlotSource::ModelFile);
}
