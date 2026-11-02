#pragma once

/**
 * creature_skin_rules.hpp - what the stock client puts in each texture slot of
 * a creature display's model (3.3.5a build 12340, docs/re/characters.md §5).
 *
 * An M2 texture slot carries a type. Type 0 names its file in the model. Types
 * 11, 12 and 13 are the creature skins: CDI_SetSkins (0x4F20C0) copies the
 * CreatureModelData path, cuts it after the last backslash, appends
 * CreatureDisplayInfo.TextureVariation[i] and hands the result to
 * CreateTexture (0x4B9760), which replaces whatever follows the name's last dot
 * with ".blp" (0x4B51C0, 0x4B8BE0) or appends it. An empty variation is
 * skipped, so that slot keeps no texture at all. Every other type (1 body, 2
 * cape, 6 hair, 8 skin extra...) belongs to the character composite and is
 * none of this function's business.
 *
 * What the GPU then sees, which is where Wowee went wrong:
 *  - a named file that does not exist loads as the default texture, a solid
 *    0xFFFFFFFF image (0x4B5280, "Texture %s not loaded -- replaced with
 *    default.");
 *  - an empty variation leaves the slot null, and the batch binds a null
 *    texture stage (0x81F450 -> 0x685F50 -> 0x6A4900 SetTexture(stage, 0)).
 *    A sampler with nothing bound reads (0, 0, 0, 1): opaque black, and
 *    nothing at all under additive blending.
 *
 * There is no other fallback: stock never borrows another display's skins,
 * never looks for the name outside the model's directory and never puts a
 * skin into a slot of a different type.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace core {

/// What goes into one texture slot of a creature display's model.
enum class CreatureSlotSource : uint8_t {
    ModelFile,     ///< type 0: the model names the file; loaded with the model
    DisplaySkin,   ///< types 11-13: the display's skin, and the file exists
    MissingFile,   ///< types 11-13: a skin is named but its file is not there -> default white
    Unbound,       ///< types 11-13: the display names no skin -> nothing bound (black)
    NotCreature,   ///< any other type: the character composite's slot, left alone
};

struct CreatureSlotTexture {
    CreatureSlotSource source = CreatureSlotSource::NotCreature;
    std::string path;  ///< the file asked for (DisplaySkin, MissingFile, ModelFile)
};

/// The model's directory with its trailing separator, as CDI_SetSkins cuts it:
/// everything up to and including the last backslash (a forward slash is
/// accepted too, for paths Wowee has normalised). Empty when there is none.
inline std::string creatureModelDirectory(const std::string& modelPath) {
    const size_t slash = modelPath.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : modelPath.substr(0, slash + 1);
}

/// The file a TextureVariation names: the model directory, the name, and the
/// name's extension replaced by ".blp" (or ".blp" appended when it has none).
/// The name is not trimmed and not searched for anywhere else. Empty when the
/// variation is empty.
inline std::string creatureSkinFile(const std::string& modelDir, const std::string& variation) {
    if (variation.empty()) return {};
    std::string path = modelDir + variation;
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        path.resize(dot);
    }
    return path + ".blp";
}

/// One slot of a model as far as these rules care.
struct CreatureModelSlot {
    uint32_t type = 0;
    std::string filename;  ///< type 0 only
};

/// Every slot of the model, in order, with what stock binds to it for a
/// display whose TextureVariation fields are `skins`.
inline std::vector<CreatureSlotTexture> resolveCreatureSlotTextures(
        const std::vector<CreatureModelSlot>& slots, const std::string& modelPath,
        const std::string (&skins)[3], const std::function<bool(const std::string&)>& fileExists) {
    const std::string dir = creatureModelDirectory(modelPath);
    std::vector<CreatureSlotTexture> out(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        const uint32_t type = slots[i].type;
        auto& o = out[i];
        if (type == 0) {
            o.source = CreatureSlotSource::ModelFile;
            o.path = slots[i].filename;
        } else if (type >= 11 && type <= 13) {
            o.path = creatureSkinFile(dir, skins[type - 11]);
            if (o.path.empty()) {
                o.source = CreatureSlotSource::Unbound;
            } else {
                o.source = (fileExists && fileExists(o.path)) ? CreatureSlotSource::DisplaySkin
                                                             : CreatureSlotSource::MissingFile;
            }
        } else {
            o.source = CreatureSlotSource::NotCreature;
        }
    }
    return out;
}

}  // namespace core
}  // namespace wowee
