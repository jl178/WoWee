#pragma once

/**
 * hd_characters.hpp - the "HD characters" setting: the reference client's player models.
 *
 * tools/hd_characters/convert.py turns Enhanced's HD models for the ten classic
 * races into 3.3.5 M2s plus a small table per model (<model>.hdlook) that maps
 * the five 3.3.5 appearance bytes onto Enhanced's customization choices. They
 * live in their own folder (AssetManager's HD characters folder) and are read
 * under the virtual prefix "HD\": "HD\Character\Human\Male\HumanMale.m2". With
 * the setting off, or a model not converted, nothing here changes a path, so
 * the original models are drawn and none of this is read.
 *
 * Only players use them. An NPC drawn with a player model wears a texture baked
 * for the 3.3.5 layout, which an HD body cannot take.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "pipeline/char_sections.hpp"

namespace wowee {
namespace pipeline {

class AssetManager;

/// Set by the renderer from Enhancements::hdCharacters.
void setHdCharactersEnabled(bool on);
bool hdCharactersEnabled();

/// The virtual folder the converted models are read from.
inline constexpr const char* kHdCharacterPrefix = "HD\\";

/// "HD\" + m2Path when the setting is on and that model was converted;
/// m2Path itself otherwise.
std::string hdCharacterModelPath(const AssetManager& assets, const std::string& m2Path);

/// Whether a model path names a converted HD model.
bool isHdCharacterModelPath(const std::string& m2Path);

/// One converted model's appearance table (<model>.hdlook, written by
/// tools/hd_characters/convert.py, which documents the format): which of
/// Enhanced's customization choices each 3.3.5 appearance index stands for,
/// and what each choice draws.
struct HdCharacterLook {
    /// The 3.3.5 options, as ChrCustomizationConversion numbers them less one.
    enum Option : int { kSkin = 0, kFace, kHairStyle, kHairColor, kFacial, kOptionCount };
    struct Choice {
        int option = 0;          ///< Option
        int index = 0;           ///< the 3.3.5 index
        uint32_t choice = 0;     ///< ChrCustomizationChoice
        uint32_t enhanced = 0;    ///< its ChrCustomizationOption
        int dependsOn = -1;      ///< Option, or -1
        int dependsIndex = 0;
    };
    struct Default {
        uint32_t choice = 0;
        uint32_t enhanced = 0;
    };
    struct Texture {
        uint32_t choice = 0;
        uint32_t related = 0;    ///< 0, or a choice that must also be picked
        int target = 0;          ///< ChrModelTextureTarget
        std::string path;
    };
    struct Geoset {
        uint32_t choice = 0;     ///< 0: always, unless a picked choice names its group
        uint16_t id = 0;
    };
    std::vector<int> bodyOrder;  ///< body layers by target, the first is the base
    int hairTarget = 10;
    std::vector<Choice> choices;
    std::vector<Default> defaults;
    std::vector<Texture> textures;
    std::vector<Geoset> geosets;
};

/// The choices a character's five appearance bytes pick, with the defaults
/// for every Enhanced option none of them reached. An index the table does not
/// know falls back to that option's index 0.
std::vector<uint32_t> selectHdCharacterChoices(const HdCharacterLook& look,
                                               const CharacterAppearance& who);

/// Parse a .hdlook file's text. Unknown lines are skipped.
HdCharacterLook parseHdCharacterLook(const std::string& text);

/// The look table beside a converted model ("HD\...\HumanMale.m2" ->
/// "HD\...\HumanMale.hdlook"), parsed once and kept. Null when absent.
std::shared_ptr<const HdCharacterLook> loadHdCharacterLook(AssetManager& assets,
                                                           const std::string& hdM2Path);

/// The character's textures in the shape CharSections gives them, so every
/// caller composites and binds them as before: the base skin, the face, the
/// scalp, the brows and the underwear, in Enhanced's layer order, and the hair.
/// Body layers after the base are all returned in `underwear`, in order; the
/// face and facial fields stay empty.
CharacterSectionTextures resolveHdCharacterTextures(const HdCharacterLook& look,
                                                    const CharacterAppearance& who);

/// The geosets the appearance picks on the HD model, in the shape the 3.3.5
/// tables give them so the stock rules (helms hiding hair, robes, boots) apply
/// unchanged: the hair style's scalp as CharHairGeosets would have it, the
/// facial hair as CharacterFacialHairStyles variants (Enhanced's beards are
/// meshes where 3.3.5 painted them), and the geosets in groups the stock rules
/// do not know (above 20: face shape, eyes, torso), which are always on.
/// Enhanced's bald is "no hair geoset", where 3.3.5 draws geoset 1 for a bald
/// head - and an HD model's geoset 1 is a short crop. A group-0 id no model
/// carries says "none" through every 3.3.5 path that would otherwise put the
/// 1 back (the stock defaults, the paper doll reading the set back).
inline constexpr uint16_t kHdNoHairGeoset = 99;

struct HdCharacterGeosets {
    uint16_t hairScalp = 0;  ///< kHdNoHairGeoset for bald; 0 when the table has no hair row
    uint16_t facial100 = 0, facial200 = 0, facial300 = 0;  ///< variants, 0 = none
    std::vector<uint16_t> extra;
};
HdCharacterGeosets resolveHdCharacterGeosets(const HdCharacterLook& look,
                                             const CharacterAppearance& who);

}  // namespace pipeline
}  // namespace wowee
