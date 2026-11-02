#include "pipeline/char_sections.hpp"

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

namespace wowee {
namespace pipeline {

namespace {
// CharSections' BaseSection column: which part of a character a row describes.
constexpr uint32_t kSectionSkin = 0;
constexpr uint32_t kSectionFace = 1;
constexpr uint32_t kSectionFacialHair = 2;
constexpr uint32_t kSectionHair = 3;
constexpr uint32_t kSectionUnderwear = 4;
}  // namespace

CharacterSectionTextures resolveCharacterSections(
    const DBCFile* charSections,
    const CharSectionsFields& f,
    const CharacterAppearance& who,
    bool (*keepUnderwear)(const std::string&, void*),
    void* keepUnderwearContext) {

    CharacterSectionTextures out;
    if (!charSections) return out;

    // The nearest usable face, kept in case the exact pair is absent.
    //
    // Character creation falls back to a synthetic 0..9 range whenever its own
    // scan of this table comes up empty, so a character can be created carrying
    // a face number that has no row at all - and would then have no face for
    // good, because the lookup simply did not match and said nothing.
    std::string altFaceLower, altFaceUpper;
    bool haveAltFace = false;

    bool foundSkin = false, foundUnderwear = false, foundFacialHair = false;
    // Hair style 1 in the character's colour: a bald male's hair texture
    // (0x4EA2F0 loads it before style 0's, which names none for most races).
    std::string style1Hair;

    for (uint32_t r = 0; r < charSections->getRecordCount(); r++) {
        if (charSections->getUInt32(r, f.raceId) != who.raceId) continue;
        if (charSections->getUInt32(r, f.sexId) != who.sexId) continue;

        const uint32_t section = charSections->getUInt32(r, f.baseSection);
        const uint32_t variation = charSections->getUInt32(r, f.variationIndex);
        const uint32_t colour = charSections->getUInt32(r, f.colorIndex);

        if (section == kSectionSkin && !foundSkin && colour == who.skinId) {
            std::string tex1 = charSections->getString(r, f.texture1);
            if (!tex1.empty()) {
                out.bodySkin = tex1;
                foundSkin = true;
            }
            out.skinExtra = charSections->getString(r, f.texture2);
            if (f.flags < charSections->getFieldCount())
                out.specialSkin = (charSections->getUInt32(r, f.flags) & 0x8u) != 0;
        } else if (section == kSectionHair && !out.haveHair &&
                   variation == who.hairStyleId && colour == who.hairColorId) {
            out.hair = charSections->getString(r, f.texture1);
            out.haveHair = !out.hair.empty();
            out.scalpLower = charSections->getString(r, f.texture2);
            out.scalpUpper = charSections->getString(r, f.texture3);
        } else if (section == kSectionHair && style1Hair.empty() &&
                   variation == 1 && colour == who.hairColorId) {
            style1Hair = charSections->getString(r, f.texture1);
        } else if (section == kSectionFace && !out.exactFace &&
                   variation == who.faceId && colour == who.skinId) {
            out.faceLower = charSections->getString(r, f.texture1);
            out.faceUpper = charSections->getString(r, f.texture2);
            out.exactFace = true;
            out.haveFace = !out.faceLower.empty();
        } else if (section == kSectionFace && !out.exactFace && !haveAltFace &&
                   (variation == who.faceId || colour == who.skinId)) {
            // The same face in another skin colour, or failing that any face in
            // the right colour. Either beats a blank head.
            std::string tex1 = charSections->getString(r, f.texture1);
            if (!tex1.empty()) {
                altFaceLower = tex1;
                altFaceUpper = charSections->getString(r, f.texture2);
                haveAltFace = true;
            }
        } else if (section == kSectionFacialHair && !foundFacialHair &&
                   variation == who.facialHairId && colour == who.hairColorId) {
            // FacialLowerHairVV_CC and FacialUpperHairVV_CC: the style, in the
            // character's hair colour.
            out.facialLower = charSections->getString(r, f.texture1);
            out.facialUpper = charSections->getString(r, f.texture2);
            foundFacialHair = true;
        } else if (section == kSectionUnderwear && !foundUnderwear &&
                   colour == who.skinId) {
            for (uint32_t col = f.texture1; col <= f.texture1 + 2; col++) {
                std::string tex = charSections->getString(r, col);
                if (tex.empty()) continue;
                if (keepUnderwear && !keepUnderwear(tex, keepUnderwearContext)) continue;
                out.underwear.push_back(tex);
            }
            foundUnderwear = !out.underwear.empty();
        }

        if (foundSkin && out.haveHair && out.exactFace && foundUnderwear && foundFacialHair) break;
    }

    if (!out.exactFace && haveAltFace) {
        out.faceLower = altFaceLower;
        out.faceUpper = altFaceUpper;
        out.haveFace = true;
    }
    // A bald male (style 0) takes style 1's hair texture in his colour, so a
    // beard on a bald head is the hair colour (0x4EA2F0).
    if (out.hair.empty() && who.sexId == 0 && who.hairStyleId == 0 && !style1Hair.empty()) {
        out.hair = style1Hair;
        out.haveHair = true;
    }
    // Special skins carry their own face and wear no underwear.
    if (out.specialSkin) {
        out.faceLower.clear();
        out.faceUpper.clear();
        out.haveFace = false;
        out.underwear.clear();
    }
    return out;
}

void applyCharacterTextures(M2Model& model,
                            const CharacterSectionTextures& textures,
                            const std::string& raceFolderName) {
    for (auto& tex : model.textures) {
        switch (tex.type) {
            case 1:  // the body, composited from skin, face and underwear
                if (!textures.bodySkin.empty()) tex.filename = textures.bodySkin;
                break;
            case 6:  // hair and scalp
                if (!textures.hair.empty()) {
                    tex.filename = textures.hair;
                } else if (tex.filename.empty()) {
                    tex.filename = "Character\\" + raceFolderName + "\\Hair00_00.blp";
                }
                break;
            case 8:
                // Skin Extra. What this art is depends entirely on the race - a
                // head-detail sheet for a human or an orc, the ears for a night
                // elf, the horns for a draenei male, the tail for a draenei
                // female - and seven of the twenty race and sex pairs name none
                // at all. Those fall back to art that exists rather than keeping
                // a name that does not.
                if (!textures.skinExtra.empty()) {
                    tex.filename = textures.skinExtra;
                } else if (!textures.underwear.empty()) {
                    tex.filename = textures.underwear.front();
                } else if (!textures.bodySkin.empty()) {
                    tex.filename = textures.bodySkin;
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace pipeline
}  // namespace wowee
