#pragma once

/**
 * geoset_rules.hpp - how a character geoset id is chosen when a model does not
 * have the one that was asked for.
 *
 * A geoset id is a group and a variant: group * 100 + variant. The members of a
 * group are alternatives for one part of a character - five kinds of boot, six
 * kinds of cloak - so when a model does not carry the exact variant asked for,
 * another member of the same group is usually the right answer.
 *
 * Usually. Variant 1 means NONE - bare feet, no cloak, no beard - and so does
 * variant 0 where a DBC stores the variant directly and uses zero for absent.
 * Every other member of the group is *something*, so substituting for none does
 * not approximate it, it contradicts it. That single mistake produced three
 * separate faults: a clean-shaven NPC with a beard, a character with no cloak
 * wearing an untextured one, and a player with no feet.
 *
 * It produced them three times because this rule was written five times, as a
 * local lambda in each place that needed it, and only some of them learned it.
 * It lives here now, with a test, and the call sites ask rather than decide.
 */

#include <array>
#include <cstdint>
#include <iterator>
#include <unordered_set>

namespace wowee {
namespace core {

// Default (bare) geoset IDs per equipment group.
// Each group's base is groupNumber * 100; variant 01 is typically bare/default.
constexpr uint16_t kGeosetDefaultConnector = 101;   // Group  1: default hair connector
constexpr uint16_t kGeosetBareForearms     = 401;   // Group  4: no gloves
constexpr uint16_t kGeosetBareShins        = 501;   // Group  5: no boots
constexpr uint16_t kGeosetDefaultEars      = 702;   // Group  7: ears
constexpr uint16_t kGeosetBareSleeves      = 801;   // Group  8: no chest armor sleeves
constexpr uint16_t kGeosetDefaultKneepads  = 901;   // Group  9: none. 902/903 are kneepad
                                                     // meshes a legs item selects with GeosetGroup[1];
                                                     // 902 by default drew boot tops on bare calves.
constexpr uint16_t kGeosetDefaultTabard    = 1201;  // Group 12: tabard base
constexpr uint16_t kGeosetBarePants        = 1301;  // Group 13: no leggings
constexpr uint16_t kGeosetNoCape           = 1501;  // Group 15: no cape
constexpr uint16_t kGeosetWithCape         = 1502;  // Group 15: with cape
constexpr uint16_t kGeosetBareFeet         = 2002;  // Group 20: bare feet
/// The other half of group 20. Models that split the feet out of the body do
/// not agree on which number to use - an HD human male carries 2002 and an HD
/// human female carries 2001 - so both are asked for and the model draws the
/// one it has. Stock models carry neither and are unaffected.
constexpr uint16_t kGeosetBareFeetAlt      = 2001;

/// Group 17: the eye-glow overlay - the mesh that makes a night elf's eyes
/// shine rather than sit there pale.
///
/// Off for everyone else, which is why the renderer strips group 17 from any
/// model drawn without a geoset filter: an NPC given the whole model drew
/// glowing eyes whatever it was.
constexpr uint16_t kGeosetEyeGlow          = 1701;


/// The body part a geoset id belongs to: 501 and 505 are both group 5, boots.
constexpr uint16_t geosetGroup(uint16_t id) { return static_cast<uint16_t>(id / 100); }

/// Which alternative within the group: 505 is variant 5.
constexpr uint16_t geosetVariant(uint16_t id) { return static_cast<uint16_t>(id % 100); }

/// Whether this id is the group's way of saying the character has none of it.
///
/// Both spellings appear. The geoset tables use variant 1 - 501 is bare feet,
/// 1501 is no cloak, 101 is no beard. The DBCs that store a variant directly,
/// CharFacialHairStyles among them, use 0 for absent, and 0 arrives here as
/// group*100 + 0.
constexpr bool geosetMeansNone(uint16_t id) {
    const uint16_t variant = geosetVariant(id);
    return variant == 0 || variant == 1;
}

/// The geoset a model should actually draw for `preferred`.
///
/// Returns `preferred` when the model has it; the group's lowest member when it
/// does not and a substitute is meaningful; and 0 - draw nothing - when what was
/// asked for was none and the model has no way to say so. A caller that gets 0
/// adds nothing for that group.
///
/// `available` is what the model carries. An empty set means "unknown", and then
/// `preferred` is returned unchanged rather than guessed at.
inline uint16_t resolveGeoset(uint16_t preferred,
                              const std::unordered_set<uint16_t>& available) {
    if (available.empty()) return preferred;
    if (available.count(preferred) > 0) return preferred;
    if (geosetMeansNone(preferred)) return 0;

    const uint16_t group = geosetGroup(preferred);
    uint16_t lowest = 0;
    for (uint16_t id : available) {
        if (geosetGroup(id) != group) continue;
        if (lowest == 0 || id < lowest) lowest = id;
    }
    return lowest;
}

/// Add a character's facial-hair geosets - beard, moustache, sideburns - to a set.
///
/// CharFacialHairStyles stores a variant per group and uses 0 for "this
/// character has none of that". Zero must not be turned into an id: group*100+0
/// is a geoset no model carries, and a caller that then substitutes within the
/// group hands a beard to someone who asked for none. That happened, on NPCs.
///
/// A caller that inserts raw ids was only accidentally safe - the invalid id
/// matched nothing - so both spellings of the mistake are removed by asking here
/// instead.
inline void addFacialHairGeosets(std::unordered_set<uint16_t>& out,
                                 uint16_t variant100, uint16_t variant200,
                                 uint16_t variant300, uint16_t variant1700 = 0) {
    if (variant100 != 0) out.insert(static_cast<uint16_t>(100 + variant100));
    if (variant200 != 0) out.insert(static_cast<uint16_t>(200 + variant200));
    if (variant300 != 0) out.insert(static_cast<uint16_t>(300 + variant300));
    // Group 1700 from the same row: the racial eye glow, 1702 on the stock
    // night elf, blood elf and some undead faces (none of them carries 1701).
    if (variant1700 != 0) out.insert(static_cast<uint16_t>(1700 + variant1700));
}

/// Key for the (race, sex, variation) maps built from CharHairGeosets.dbc and
/// CharFacialHairStyles.dbc.
///
/// It was packed by hand in ten places across three files. Every one of them had
/// to agree bit for bit with every other, or a lookup would miss and say
/// nothing - and a hair or beard lookup that misses does not report a fault, it
/// quietly draws the default.
constexpr uint32_t appearanceKey(uint8_t race, uint8_t sex, uint8_t variation) {
    return (static_cast<uint32_t>(race) << 16) |
           (static_cast<uint32_t>(sex) << 8) |
           static_cast<uint32_t>(variation);
}

/// Whether a race's eyes glow.
///
/// Night elves', and only theirs among the playable races - it is not an option
/// they choose, both sexes have it and always have. The NPC path had this rule
/// and the player path did not, so a night elf player looked out of pale eyes
/// while every night elf standing next to them glowed.
///
/// Death knights glow too, whatever their race, and are not handled here: that
/// depends on the class rather than the model and the paths that build these
/// sets do not all know it.
inline bool raceHasGlowingEyes(uint8_t raceId) {
    return raceId == 4;  // NightElf
}

/// The geosets a character shows with nothing equipped, before any armour is
/// known: the body, the chosen hair scalp, the chosen facial features, and the
/// bare variant of every equipment group.
///
/// Written twice before this - once for the player and once for the portrait -
/// and the two had drifted apart in exactly the ways that cost something. The
/// portrait named one of the two feet variants, so an HD model spelling its feet
/// the other way lost them there while the player kept his. The player named the
/// cloak mesh and the portrait named the no-cloak panel.
///
/// `hairScalp` and the three facial variants come from the DBC maps; a zero
/// facial variant means the character has none of that feature and adds nothing.
inline std::unordered_set<uint16_t> bareCharacterGeosets(uint16_t hairScalp,
                                                         uint16_t facial100,
                                                         uint16_t facial200,
                                                         uint16_t facial300,
                                                         uint8_t raceId = 0,
                                                         uint16_t facial1700 = 0) {
    std::unordered_set<uint16_t> geosets;
    geosets.insert(0);                      // the body
    if (hairScalp != 0) geosets.insert(hairScalp);
    addFacialHairGeosets(geosets, facial100, facial200, facial300, facial1700);
    if (raceHasGlowingEyes(raceId)) geosets.insert(kGeosetEyeGlow);

    geosets.insert(kGeosetBareForearms);    // no gloves
    geosets.insert(kGeosetBareShins);       // no boots
    geosets.insert(kGeosetDefaultEars);
    geosets.insert(kGeosetBareSleeves);     // no chest sleeves
    geosets.insert(kGeosetDefaultKneepads);
    geosets.insert(kGeosetBarePants);       // no leggings
    // The feet, both spellings. The models the game shipped have no group 20 at
    // all and are unaffected; the replacements split the feet out and do not
    // agree on the number, so naming one loses them on half of them.
    geosets.insert(kGeosetBareFeet);
    geosets.insert(kGeosetBareFeetAlt);
    // The no-cloak back panel. Without it a character no equipment pass ran
    // for (character create, a naked preview) had a hole in the upper back.
    // Models without the panel simply have no 1501 submesh to draw, and every
    // equipment pass erases group 15 before choosing the cape or the panel.
    geosets.insert(kGeosetNoCape);
    return geosets;
}

/// The geoset a piece of equipment selects within its group.
///
/// ItemDisplayInfo's GeosetGroup columns hold a small number G meaning "the Gth
/// variant after the bare one" - so a chest with G=2 wants group 8 variant 3,
/// which is the bare sleeves id plus 2. The arithmetic is a single addition and
/// was written out at a dozen call sites, half of them against the named bare
/// constant and half against the literal number it holds, with the convention
/// itself recorded nowhere.
///
/// G of zero means the item does not touch that group, and the caller keeps
/// whatever it had.
constexpr uint16_t equippedGeoset(uint16_t bareId, uint32_t geosetGroupValue) {
    return static_cast<uint16_t>(bareId + geosetGroupValue);
}

/// The cape a cloak selects: its own variant, or the long cape when the item names none.
constexpr uint16_t cloakGeoset(uint32_t geosetGroupValue) {
    return geosetGroupValue > 0 ? equippedGeoset(kGeosetNoCape, geosetGroupValue) : kGeosetWithCape;
}

/// Which geoset group each worn item drives, and the bare variant it replaces.
///
/// The two paths that read equipment come at it from different directions - a
/// player's inventory is numbered one way and an NPC's CreatureDisplayInfoExtra
/// array another - so the slot numbers cannot be shared. What they were both
/// restating is this: which part of the body a given piece of armour changes.
///
/// Kept as named constants rather than a table, because each call site reads one
/// of them and a table would only be looked up by an enum that says the same
/// thing. What matters is that "a chest changes the sleeves" is written once.
namespace equipment {
constexpr uint16_t kChestBare  = kGeosetBareSleeves;   ///< group 8
constexpr uint16_t kLegsBare   = kGeosetBarePants;     ///< group 13
constexpr uint16_t kBootsBare  = kGeosetBareShins;     ///< group 5
constexpr uint16_t kGlovesBare = kGeosetBareForearms;  ///< group 4
constexpr uint16_t kWristBare  = kGeosetBareSleeves;   ///< group 8, same as chest
constexpr uint16_t kBeltBase   = 1801;                 ///< group 18, the buckle
constexpr uint16_t kTabardBase = 1200;                 ///< group 12

/// A robe's chest piece also names the kilt over the legs, in its second geoset
/// column rather than its first. Reading only the first is why an NPC in a robe
/// wore trousers under it while a player in the same robe did not.
constexpr uint16_t kRobeKiltBare = kGeosetBarePants;   ///< group 13
}  // namespace equipment

/// What stock's equipment geoset pass reads (FUN_004ed900, run for players and
/// for CreatureDisplayInfoExtra NPCs alike).
///
/// `gg[kind][i]` is ItemDisplayInfo.GeosetGroup[i] of the item worn in that
/// kind, 0 when nothing is worn. The kinds are CreatureDisplayInfoExtra's
/// NPCItemDisplay order, which is also the order of the component's item array.
struct StockGeosetInput {
    enum Kind { Head, Shoulder, Shirt, Chest, Belt, Legs, Boots, Wrist, Gloves, Tabard, Cape, KindCount };
    uint32_t gg[KindCount][3] = {};
    /// The chest's ItemDisplayInfo flag that hides the legs (component flag
    /// 0x20): long robes whose texture covers the legs.
    bool chestHidesLegs = false;
    /// Something over the shirt paints the lower arm (ArmLower layers 1-6):
    /// then the shirt's sleeves (group 8 from the shirt) are not shown.
    bool armLowerCoveredAboveShirt = false;
    /// A guild tabard (component flag 0x40).
    bool guildTabard = false;
    /// A death knight (class 6, or a face whose CharSections row has flag 4):
    /// group 17 is 1703, the rune glow.
    bool deathKnight = false;
};

/// Stock's default value per group, 0..18 (0x4dfda0): variant 1 everywhere,
/// ears 702.
inline std::array<uint16_t, 19> stockDefaultGeosetValues() {
    std::array<uint16_t, 19> v{};
    for (uint16_t g = 0; g < 19; ++g) v[g] = static_cast<uint16_t>(g * 100 + 1);
    v[0] = 1;
    v[7] = kGeosetDefaultEars;
    return v;
}

/// The geoset ids stock enables for a character (FUN_004ed900), as a set of
/// exact ids. `base` is the value per group before equipment: hair (group 0),
/// facial hair (1-3, 16, 17) and whatever the helm hid (HelmetGeosetVisData),
/// the defaults otherwise. Id 0, the body, is always on.
///
/// Exact ids only: a model that lacks one draws nothing for it. Stock does
/// not substitute another member of the group.
inline std::unordered_set<uint16_t> stockEquipmentGeosets(const std::array<uint16_t, 19>& base,
                                                          const StockGeosetInput& in) {
    std::unordered_set<uint16_t> on;
    auto rangeOff = [&](uint16_t lo, uint16_t hi) {
        for (auto it = on.begin(); it != on.end();)
            it = (*it >= lo && *it <= hi) ? on.erase(it) : std::next(it);
    };
    auto set = [&](uint32_t id) { on.insert(static_cast<uint16_t>(id)); };
    using K = StockGeosetInput;
    auto gg = [&](int kind, int i) { return in.gg[kind][i]; };

    set(0);
    for (uint16_t g = 0; g < 19; ++g)
        set((g == 17 && in.deathKnight) ? 1703u : base[g]);

    // Hands: gloves own group 4 and hide the chest's sleeves.
    if (gg(K::Gloves, 0)) { rangeOff(401, 499); set(401 + gg(K::Gloves, 0)); }
    else if (gg(K::Chest, 0)) set(801 + gg(K::Chest, 0));
    if (!in.armLowerCoveredAboveShirt && gg(K::Shirt, 0)) set(801 + gg(K::Shirt, 0));

    bool robeChest = false, robeLegs = false, tabardOn = false;
    auto robeOff = [&] { rangeOff(501, 599); rangeOff(902, 999); rangeOff(1100, 1199); rangeOff(1300, 1399); };
    if (gg(K::Chest, 2)) {
        robeOff(); set(1301 + gg(K::Chest, 2)); robeChest = true;
    } else if (gg(K::Legs, 2) && !in.chestHidesLegs) {
        robeOff(); set(1301 + gg(K::Legs, 2)); robeLegs = true;
    } else {
        if (gg(K::Boots, 0)) { rangeOff(501, 599); set(901); set(501 + gg(K::Boots, 0)); }
        else if (gg(K::Legs, 1) && !in.chestHidesLegs) set(901 + gg(K::Legs, 1));
        else set(901);
        if (gg(K::Tabard, 0)) { set(1201 + gg(K::Tabard, 0)); tabardOn = true; }
    }
    if (in.guildTabard) { set(1201); if (!robeChest && !robeLegs) set(1202); }
    if (!robeChest) {
        if (!tabardOn && gg(K::Shirt, 1)) set(1001 + gg(K::Shirt, 1));
        if (!in.chestHidesLegs && gg(K::Legs, 0)) {
            if (gg(K::Legs, 0) >= 3) { rangeOff(1300, 1399); set(1101 + gg(K::Legs, 0)); }
            else if (!tabardOn) set(1101 + gg(K::Legs, 0));
        }
    }
    if (gg(K::Cape, 0)) { rangeOff(1500, 1599); set(1501 + gg(K::Cape, 0)); }
    if (gg(K::Belt, 0)) { rangeOff(1800, 1899); set(1801 + gg(K::Belt, 0)); }
    return on;
}

/// One row of HelmetGeosetVisData: seven race bitmasks, each tested with
/// 1 << race (0x4ef0d0). Column 0 hides the hair, 1-3 the facial hair, 4 the
/// ears, 5 group 16, 6 the eye glow.
struct HelmGeosetVis {
    uint32_t mask[7] = {};
};

/// Apply a helm's HelmetGeosetVisData row to the per-group values: a column
/// whose bit for this race is set puts its group to variant 1 (none).
inline void applyHelmGeosetVis(std::array<uint16_t, 19>& v, const HelmGeosetVis& row, uint8_t race) {
    static constexpr uint8_t kGroup[7] = {0, 1, 2, 3, 7, 16, 17};
    if (race >= 32) return;
    for (int c = 0; c < 7; ++c)
        if (row.mask[c] & (1u << race)) v[kGroup[c]] = static_cast<uint16_t>(kGroup[c] * 100 + 1);
}

/// CreatureDisplayInfo.CreatureGeosetData (0x4e7790): for groups 1-8, nibble
/// g-1 = v, when non-zero, turns the group off and puts on 100g + v.
inline void applyCreatureGeosetData(std::unordered_set<uint16_t>& on, uint32_t data) {
    for (uint16_t g = 1; g <= 8; ++g) {
        const uint32_t v = (data >> (4 * (g - 1))) & 0xF;
        if (v == 0) continue;
        for (auto it = on.begin(); it != on.end();)
            it = (*it >= g * 100 && *it <= g * 100 + 99) ? on.erase(it) : std::next(it);
        on.insert(static_cast<uint16_t>(g * 100 + v));
    }
}

}  // namespace core
}  // namespace wowee
