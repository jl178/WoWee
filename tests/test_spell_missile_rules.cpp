// The stock rules for ranged and item missiles (Wow.exe 0x720F80, 0x7FFFB0,
// 0x732FF0, 0x732100, 0x4EAB50): which visual a ranged attack plays, whether
// a missile flies, what its model is and where the item model lives.
#include <catch_amalgamated.hpp>

#include "rendering/spell_missile_rules.hpp"

using namespace wowee::rendering::missilerules;

namespace {
VisualColumns wandVisual225() {
    // ItemDisplayInfo.SpellVisualID 225 (fire wands): precast 372 (HoldThrown),
    // cast 2970 (AttackThrown + the school's sound), the fire bolt missile
    // (effect 103), destination index 1, attachment -1.
    VisualColumns v{};
    v[0] = 225; v[1] = 372; v[2] = 2970; v[7] = 1; v[8] = 103; v[10] = 1; v[16] = -1;
    return v;
}
VisualColumns bowVisual5() {
    // Bows: LoadBow / AttackBow kits, CombatWound impact, MissileSound 4222,
    // no missile of its own - the arrow is the ammo.
    VisualColumns v{};
    v[0] = 5; v[1] = 7; v[2] = 164; v[3] = 1947; v[10] = 1; v[11] = 4222; v[16] = -1;
    return v;
}
}  // namespace

TEST_CASE("only ranged-slot spells borrow the weapon's visual", "[missile_rules]") {
    CHECK(usesRangedItemVisual(0x50012));   // Auto Shot
    CHECK(usesRangedItemVisual(0x12));      // Shoot (wand)
    CHECK(usesRangedItemVisual(0x10002));   // Arcane Shot
    CHECK_FALSE(usesRangedItemVisual(0x10000));  // Mind Flay
}

TEST_CASE("a spell with no visual takes the wand's whole presentation", "[missile_rules]") {
    const VisualColumns m = mergeRangedItemVisual(nullptr, wandVisual225());
    CHECK(m[0] == 0);
    CHECK(m[1] == 372);
    CHECK(m[2] == 2970);
    CHECK(m[7] == 1);
    CHECK(m[8] == 103);
    CHECK(m[10] == 1);
    CHECK(m[16] == -1);
    CHECK(m[13] == 0);  // flags are the spell's own: none
}

TEST_CASE("a spell's own kits and missile win over the weapon's", "[missile_rules]") {
    // Arcane Shot 3299: its own missile and impact; the bow's precast and cast.
    VisualColumns arcane{};
    arcane[0] = 3299; arcane[3] = 419; arcane[7] = 1; arcane[8] = 777; arcane[10] = 1;
    arcane[11] = 3013; arcane[16] = -1;
    const VisualColumns m = mergeRangedItemVisual(&arcane, bowVisual5());
    CHECK(m[1] == 7);       // LoadBow from the bow
    CHECK(m[2] == 164);     // AttackBow from the bow
    CHECK(m[3] == 419);     // the shot's own impact
    CHECK(m[8] == 777);     // its own missile
    CHECK(m[11] == 3013);   // its own sound
}

TEST_CASE("the weapon's missile comes only as a pair with HasMissile", "[missile_rules]") {
    const VisualColumns m = mergeRangedItemVisual(nullptr, bowVisual5());
    CHECK(m[7] == 0);
    CHECK(m[8] == 0);
    CHECK(m[3] == 1947);
    // The bow flies nothing of its own; the arrow comes from SPELL_GO.
    CHECK_FALSE(fliesMissile(m[7] != 0, 0));
    CHECK(fliesMissile(m[7] != 0, 5996));
}

TEST_CASE("the missile model kind follows MissileModel and the ammo", "[missile_rules]") {
    CHECK(missileKind(-1, 0) == MissileKind::MainHand);
    CHECK(missileKind(-2, 0) == MissileKind::OffHand);
    CHECK(missileKind(-3, 0) == MissileKind::Ranged);
    CHECK(missileKind(-4, 5996) == MissileKind::Ammo);
    CHECK(missileKind(-5, 5998) == MissileKind::Ammo);
    CHECK(missileKind(0, 5996) == MissileKind::AmmoModel);
    CHECK(missileKind(0, 0) == MissileKind::None);
    CHECK(missileKind(103, 5996) == MissileKind::Effect);  // a wand bolt ignores the ammo
    CHECK(missileItemSlot(MissileKind::Ranged) == 2);
    CHECK(missileItemSlot(MissileKind::Ammo) == -1);
}

TEST_CASE("item missile paths by inventory type (0x732100)", "[missile_rules]") {
    ItemDisplayNames dagger;
    dagger.model[0] = "Thrown_1H_Dagger_B_01.mdx";
    dagger.texture[0] = "Thrown_1H_Dagger_B_02Black";
    ItemMissilePaths p;
    REQUIRE(itemMissilePaths(dagger, 25, p));
    CHECK(p.model == "Item\\ObjectComponents\\Weapon\\Thrown_1H_Dagger_B_01.mdx");
    CHECK(p.texture == "Item\\ObjectComponents\\Weapon\\Thrown_1H_Dagger_B_02Black.blp");

    ItemDisplayNames arrow;
    arrow.model[1] = "ArrowFlight_01.mdx";
    arrow.texture[1] = "Arrow_A_01Brown";
    REQUIRE(itemMissilePaths(arrow, 24, p));  // the second name when the first is empty
    CHECK(p.model == "Item\\ObjectComponents\\Ammo\\ArrowFlight_01.mdx");
    CHECK(p.texture == "Item\\ObjectComponents\\Ammo\\Arrow_A_01Brown.blp");

    ItemDisplayNames shield;
    shield.model[0] = "Shield_A.mdx";
    REQUIRE(itemMissilePaths(shield, 14, p));
    CHECK(p.model == "Item\\ObjectComponents\\Shield\\Shield_A.mdx");
    CHECK(p.texture.empty());

    CHECK_FALSE(itemMissilePaths(ItemDisplayNames{}, 25, p));
    CHECK_FALSE(itemMissilePaths(shield, 1, p));  // helms carry race and sex
}

TEST_CASE("ammo flight models only from the second name (0x4EAB50)", "[missile_rules]") {
    ItemDisplayNames arrow;
    arrow.model[1] = "ArrowFlight_01.mdx";
    arrow.texture[1] = "Arrow_A_01Brown";
    ItemMissilePaths p;
    REQUIRE(ammoMissilePaths(arrow, 24, p));
    CHECK(p.model == "Item\\ObjectComponents\\Ammo\\ArrowFlight_01.mdx");

    ItemDisplayNames axe;
    axe.model[0] = axe.model[1] = "Thrown_1H_Axe_A_01.mdx";
    axe.texture[0] = axe.texture[1] = "Thrown_1H_Axe_A_01Black";
    REQUIRE(ammoMissilePaths(axe, 25, p));
    CHECK(p.model == "Item\\ObjectComponents\\Weapon\\Thrown_1H_Axe_A_01.mdx");

    ItemDisplayNames onlyFirst;
    onlyFirst.model[0] = "Thrown_1H_Dagger_B_01.mdx";
    onlyFirst.texture[0] = "Thrown_1H_Dagger_B_02Black";
    CHECK_FALSE(ammoMissilePaths(onlyFirst, 25, p));  // the stock's ErrorCube
    CHECK_FALSE(ammoMissilePaths(arrow, 26, p));      // a wand is no ammo
}
