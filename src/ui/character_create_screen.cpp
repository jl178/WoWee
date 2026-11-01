#include "ui/character_create_screen.hpp"
#include "core/logger.hpp"
#include "ui/glue_ui.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/renderer.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <random>

namespace wowee {
namespace ui {

// Full WotLK race/class lists (used as defaults when no expansion constraints set)
static constexpr game::Race kAllRaces[] = {
    // Alliance
    game::Race::HUMAN, game::Race::DWARF, game::Race::NIGHT_ELF,
    game::Race::GNOME, game::Race::DRAENEI,
    // Horde
    game::Race::ORC, game::Race::UNDEAD, game::Race::TAUREN,
    game::Race::TROLL, game::Race::BLOOD_ELF,
};
static constexpr int kAllRaceCount = 10;
static constexpr int kAllianceCount = 5;

static constexpr game::Class kAllClasses[] = {
    game::Class::WARRIOR, game::Class::PALADIN, game::Class::HUNTER,
    game::Class::ROGUE, game::Class::PRIEST, game::Class::DEATH_KNIGHT,
    game::Class::SHAMAN, game::Class::MAGE, game::Class::WARLOCK,
    game::Class::DRUID,
};

namespace {

uint8_t selectedAppearanceId(const std::vector<uint8_t>& ids, int index) {
    if (!ids.empty() && index >= 0 && index < static_cast<int>(ids.size())) {
        return ids[static_cast<size_t>(index)];
    }
    return static_cast<uint8_t>(std::max(index, 0));
}

void sortUnique(std::vector<uint8_t>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

// Last resort when the DBC scan found nothing for this race and sex. The
// numbers it invents are not backed by any CharSections row, so a character
// created from them has an appearance the client cannot draw - a face picked
// this way never resolves to a texture and the head renders bare. Keeping the
// fallback (an empty slider is worse than a wrong one) but saying so, because
// silently offering unrenderable choices is how a character ends up broken for
// good at the one moment it cannot be changed.
void useFallbackRange(const char* what, std::vector<uint8_t>& ids, int maxId) {
    if (!ids.empty()) return;
    core::Logger::getInstance().warning(
        "Character creation: no DBC entries for ", what,
        " - offering an unverified 0..", maxId,
        " range; choices may not render");
    ids.reserve(static_cast<size_t>(maxId + 1));
    for (int id = 0; id <= maxId; ++id) {
        ids.push_back(static_cast<uint8_t>(id));
    }
}

} // namespace


CharacterCreateScreen::CharacterCreateScreen() {
    reset();
}

CharacterCreateScreen::~CharacterCreateScreen() = default;

void CharacterCreateScreen::setExpansionConstraints(
        const std::vector<uint32_t>& races, const std::vector<uint32_t>& classes) {
    // Build filtered race list: alliance first, then horde
    availableRaces_.clear();
    expansionClasses_.clear();

    if (!races.empty()) {
        // Alliance races in display order
        for (auto r : std::initializer_list<game::Race>{
                game::Race::HUMAN, game::Race::DWARF, game::Race::NIGHT_ELF,
                game::Race::GNOME, game::Race::DRAENEI}) {
            if (std::find(races.begin(), races.end(), static_cast<uint32_t>(r)) != races.end()) {
                availableRaces_.push_back(r);
            }
        }
        allianceRaceCount_ = static_cast<int>(availableRaces_.size());

        // Horde races in display order
        for (auto r : std::initializer_list<game::Race>{
                game::Race::ORC, game::Race::UNDEAD, game::Race::TAUREN,
                game::Race::TROLL, game::Race::BLOOD_ELF}) {
            if (std::find(races.begin(), races.end(), static_cast<uint32_t>(r)) != races.end()) {
                availableRaces_.push_back(r);
            }
        }
    }

    if (!classes.empty()) {
        for (auto cls : kAllClasses) {
            if (std::find(classes.begin(), classes.end(), static_cast<uint32_t>(cls)) != classes.end()) {
                expansionClasses_.push_back(cls);
            }
        }
    }

    // If no constraints provided, fall back to WotLK defaults
    if (availableRaces_.empty()) {
        availableRaces_.assign(kAllRaces, kAllRaces + kAllRaceCount);
        allianceRaceCount_ = kAllianceCount;
    }

    raceIndex = 0;
    classIndex = 0;
    updateAvailableClasses();
}

void CharacterCreateScreen::reset() {
    name_.clear();
    raceIndex = 0;
    classIndex = 0;
    genderIndex = 0;
    bodyTypeIndex = 0;
    skin = 0;
    face = 0;
    hairStyle = 0;
    hairColor = 0;
    facialHair = 0;
    // The same limits game::getMax* answers, asked rather than repeated: this
    // is the fallback before the DBC scan has run, and the two have to agree
    // or the sliders offer a face the scan will not produce.
    const game::Race race = availableRaces_.empty()
        ? game::Race::HUMAN : availableRaces_[0];
    const game::Gender gender = game::Gender::MALE;
    maxSkin = game::getMaxSkin(race, gender);
    maxFace = game::getMaxFace(race, gender);
    maxHairStyle = game::getMaxHairStyle(race, gender);
    maxHairColor = game::getMaxHairColor(race, gender);
    maxFacialHair = game::getMaxFacialFeature(race, gender);
    statusMessage.clear();
    statusIsError = false;
    createTimer_ = -1.0f;
    skinIds_.clear();
    faceIds_.clear();
    hairStyleIds_.clear();
    hairColorIds_.clear();
    facialHairIds_.clear();

    // Populate default races if not yet set by setExpansionConstraints
    if (availableRaces_.empty()) {
        availableRaces_.assign(kAllRaces, kAllRaces + kAllRaceCount);
        allianceRaceCount_ = kAllianceCount;
    }

    updateAvailableClasses();

    // Reset preview tracking to force model reload on next render
    prevRaceIndex_ = -1;
    prevGenderIndex_ = -1;
    prevBodyTypeIndex_ = -1;
    prevSkin_ = -1;
    prevFace_ = -1;
    prevHairStyle_ = -1;
    prevHairColor_ = -1;
    prevFacialHair_ = -1;
    prevRangeRace_ = -1;
    prevRangeGender_ = -1;
    prevRangeBodyType_ = -1;
    prevRangeSkin_ = -1;
    prevRangeHairStyle_ = -1;
    prevRangeDK_ = -1;

    sceneShown_ = false;
    prevSceneDK_ = -1;
    nameFocused_ = false;
    rotatingLeft_ = rotatingRight_ = false;
    raceScroll_ = classScroll_ = 0.0f;
    infoKey_ = -1;
}

void CharacterCreateScreen::initializePreview(pipeline::AssetManager* am) {
    // The view itself is built on the first frame drawn, at the window's
    // size: it is the whole screen, as the stock CharacterCreate ModelFFX is.
    assetManager_ = am;
    if (preview_) preview_->resetView();
    // Force model reload
    prevRaceIndex_ = -1;
}

void CharacterCreateScreen::update(float deltaTime) {
    if (preview_) {
        preview_->update(deltaTime);
    }
    // Timeout waiting for server response
    if (createTimer_ >= 0.0f) {
        createTimer_ += deltaTime;
        if (createTimer_ > 10.0f) {
            createTimer_ = -1.0f;
            setStatus("Server did not respond. Try again.", true);
        }
    }
}

void CharacterCreateScreen::setStatus(const std::string& msg, bool isError) {
    statusMessage = msg;
    statusIsError = isError;
    if (isError || msg.empty()) {
        createTimer_ = -1.0f;  // Stop waiting on error/clear
    }
}

void CharacterCreateScreen::updateAvailableClasses() {
    availableClasses.clear();
    if (availableRaces_.empty() || raceIndex >= static_cast<int>(availableRaces_.size())) return;
    game::Race race = availableRaces_[raceIndex];
    for (auto cls : kAllClasses) {
        if (!game::isValidRaceClassCombo(race, cls)) continue;
        // If expansion constraints set, only allow listed classes
        if (!expansionClasses_.empty()) {
            if (std::find(expansionClasses_.begin(), expansionClasses_.end(), cls) == expansionClasses_.end())
                continue;
        }
        availableClasses.push_back(cls);
    }
    // Clamp class index
    if (classIndex >= static_cast<int>(availableClasses.size())) {
        classIndex = 0;
    }
}

void CharacterCreateScreen::updatePreviewIfNeeded() {
    if (!preview_) return;

    bool changed = (raceIndex != prevRaceIndex_ ||
                    classIndex != prevClassIndex_ ||
                    genderIndex != prevGenderIndex_ ||
                    bodyTypeIndex != prevBodyTypeIndex_ ||
                    skin != prevSkin_ ||
                    face != prevFace_ ||
                    hairStyle != prevHairStyle_ ||
                    hairColor != prevHairColor_ ||
                    facialHair != prevFacialHair_);

    if (changed) {
        bool useFemaleModel = (genderIndex == 2 && bodyTypeIndex == 1);  // Nonbinary + Feminine
        preview_->loadCharacter(
            availableRaces_[raceIndex],
            static_cast<game::Gender>(genderIndex),
            selectedAppearanceId(skinIds_, skin),
            selectedAppearanceId(faceIds_, face),
            selectedAppearanceId(hairStyleIds_, hairStyle),
            selectedAppearanceId(hairColorIds_, hairColor),
            selectedAppearanceId(facialHairIds_, facialHair),
            useFemaleModel);
        applyStartOutfit();

        prevRaceIndex_ = raceIndex;
        prevClassIndex_ = classIndex;
        prevGenderIndex_ = genderIndex;
        prevBodyTypeIndex_ = bodyTypeIndex;
        prevSkin_ = skin;
        prevFace_ = face;
        prevHairStyle_ = hairStyle;
        prevHairColor_ = hairColor;
        prevFacialHair_ = facialHair;
    }
}

void CharacterCreateScreen::applyStartOutfit() {
    // The stock create screen dresses the model in the class's starter gear
    // (CharStartOutfit.dbc: id, race/class/sex bytes, 24 item ids, 24 display
    // ids, 24 inventory types). It showed a naked character instead.
    if (!preview_ || !assetManager_ || availableClasses.empty() ||
        classIndex >= static_cast<int>(availableClasses.size()) ||
        raceIndex >= static_cast<int>(availableRaces_.size())) return;
    auto dbc = assetManager_->loadDBC("CharStartOutfit.dbc");
    if (!dbc || dbc->getFieldCount() < 74) return;
    const uint32_t race = static_cast<uint32_t>(availableRaces_[raceIndex]);
    const uint32_t cls = static_cast<uint32_t>(availableClasses[classIndex]);
    // Nonbinary picks a body; the outfit follows the model used.
    const uint32_t sex = (genderIndex == 1 || (genderIndex == 2 && bodyTypeIndex == 1)) ? 1u : 0u;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t packed = dbc->getUInt32(r, 1);
        if ((packed & 0xFF) != race || ((packed >> 8) & 0xFF) != cls ||
            ((packed >> 16) & 0xFF) != sex) continue;
        std::vector<game::EquipmentItem> equipment;
        for (uint32_t i = 0; i < 24; ++i) {
            const uint32_t display = dbc->getUInt32(r, 26 + i);
            const uint32_t invType = dbc->getUInt32(r, 50 + i);
            if (display == 0 || display == 0xFFFFFFFFu || invType == 0 || invType == 0xFFFFFFFFu) continue;
            game::EquipmentItem item{};
            item.displayModel = display;
            item.inventoryType = static_cast<uint8_t>(invType);
            equipment.push_back(item);
        }
        preview_->applyEquipment(equipment);
        return;
    }
}

void CharacterCreateScreen::updateAppearanceRanges() {
    // CharSections.Flags: 0x1 is offered at character creation, 0x4 is death
    // knight art (the pale skins, the scarred faces, the DK hair colours). Rows
    // without 0x1 (NPC skins, barbershop-only styles) are never offered, and
    // the 0x4 rows only to a death knight - they sit at ordinary skin colours
    // too, so without the check every class was offered DK faces and
    // Randomize picked them.
    const bool dk = classIndex >= 0 &&
                    classIndex < static_cast<int>(availableClasses.size()) &&
                    availableClasses[static_cast<size_t>(classIndex)] == game::Class::DEATH_KNIGHT;
    if (raceIndex == prevRangeRace_ &&
        genderIndex == prevRangeGender_ &&
        bodyTypeIndex == prevRangeBodyType_ &&
        skin == prevRangeSkin_ &&
        hairStyle == prevRangeHairStyle_ &&
        static_cast<int>(dk) == prevRangeDK_) {
        return;
    }

    // The fallback ranges, before the DBC scan narrows them to what this race
    // and sex actually have art for.
    const game::Race race = raceIndex < static_cast<int>(availableRaces_.size())
        ? availableRaces_[raceIndex] : game::Race::HUMAN;
    const game::Gender gender =
        genderIndex == 0 ? game::Gender::MALE : game::Gender::FEMALE;
    maxSkin = game::getMaxSkin(race, gender);
    maxFace = game::getMaxFace(race, gender);
    maxHairStyle = game::getMaxHairStyle(race, gender);
    maxHairColor = game::getMaxHairColor(race, gender);
    maxFacialHair = game::getMaxFacialFeature(race, gender);
    skinIds_.clear();
    faceIds_.clear();
    hairStyleIds_.clear();
    hairColorIds_.clear();
    facialHairIds_.clear();

    if (!assetManager_ || availableRaces_.empty()) return;
    auto dbc = assetManager_->loadDBC("CharSections.dbc");
    if (!dbc) return;

    uint32_t targetRaceId = static_cast<uint32_t>(availableRaces_[raceIndex]);
    const bool useFemaleModel = genderIndex == 1 || (genderIndex == 2 && bodyTypeIndex == 1);
    uint32_t targetSexId = useFemaleModel ? 1u : 0u;

    const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
    auto csF = pipeline::detectCharSectionsFields(dbc.get(), csL);
    const bool haveFlags = csF.flags < dbc->getFieldCount();
    auto offered = [&](uint32_t r) {
        if (!haveFlags) return true;
        const uint32_t flags = dbc->getUInt32(r, csF.flags);
        if ((flags & 0x1u) == 0) return false;
        return dk || (flags & 0x4u) == 0;
    };
    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t raceId = dbc->getUInt32(r, csF.raceId);
        uint32_t sexId = dbc->getUInt32(r, csF.sexId);
        if (raceId != targetRaceId || sexId != targetSexId) continue;
        if (!offered(r)) continue;

        uint32_t baseSection = dbc->getUInt32(r, csF.baseSection);
        uint32_t variationIndex = dbc->getUInt32(r, csF.variationIndex);
        uint32_t colorIndex = dbc->getUInt32(r, csF.colorIndex);

        if (baseSection == 0 && variationIndex == 0 && colorIndex <= 255) {
            skinIds_.push_back(static_cast<uint8_t>(colorIndex));
        } else if (baseSection == 3 && variationIndex <= 255) {
            hairStyleIds_.push_back(static_cast<uint8_t>(variationIndex));
        }
    }

    sortUnique(skinIds_);
    sortUnique(hairStyleIds_);
    useFallbackRange("skin", skinIds_, maxSkin);
    useFallbackRange("hair style", hairStyleIds_, maxHairStyle);
    maxSkin = static_cast<int>(skinIds_.size()) - 1;
    maxHairStyle = static_cast<int>(hairStyleIds_.size()) - 1;
    skin = std::clamp(skin, 0, maxSkin);
    hairStyle = std::clamp(hairStyle, 0, maxHairStyle);

    const uint8_t skinId = selectedAppearanceId(skinIds_, skin);
    const uint8_t hairStyleId = selectedAppearanceId(hairStyleIds_, hairStyle);

    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t raceId = dbc->getUInt32(r, csF.raceId);
        uint32_t sexId = dbc->getUInt32(r, csF.sexId);
        if (raceId != targetRaceId || sexId != targetSexId) continue;
        if (!offered(r)) continue;

        uint32_t baseSection = dbc->getUInt32(r, csF.baseSection);
        uint32_t variationIndex = dbc->getUInt32(r, csF.variationIndex);
        uint32_t colorIndex = dbc->getUInt32(r, csF.colorIndex);

        if (baseSection == 1 && colorIndex == skinId && variationIndex <= 255) {
            faceIds_.push_back(static_cast<uint8_t>(variationIndex));
        } else if (baseSection == 3 && variationIndex == hairStyleId) {
            if (colorIndex <= 255) {
                hairColorIds_.push_back(static_cast<uint8_t>(colorIndex));
            }
        }
    }

    sortUnique(faceIds_);
    sortUnique(hairColorIds_);
    useFallbackRange("face", faceIds_, maxFace);
    useFallbackRange("hair colour", hairColorIds_, maxHairColor);
    maxFace = static_cast<int>(faceIds_.size()) - 1;
    maxHairColor = static_cast<int>(hairColorIds_.size()) - 1;
    face = std::clamp(face, 0, maxFace);
    hairColor = std::clamp(hairColor, 0, maxHairColor);

    auto facialDbc = assetManager_->loadDBC("CharacterFacialHairStyles.dbc");
    const auto* fhL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
    if (facialDbc) {
        for (uint32_t r = 0; r < facialDbc->getRecordCount(); r++) {
            uint32_t raceId = facialDbc->getUInt32(r, fhL ? (*fhL)["RaceID"] : 0);
            uint32_t sexId = facialDbc->getUInt32(r, fhL ? (*fhL)["SexID"] : 1);
            if (raceId != targetRaceId || sexId != targetSexId) continue;
            uint32_t variation = facialDbc->getUInt32(r, fhL ? (*fhL)["Variation"] : 2);
            if (variation <= 255) {
                facialHairIds_.push_back(static_cast<uint8_t>(variation));
            }
        }
    }
    sortUnique(facialHairIds_);
    useFallbackRange("facial hair", facialHairIds_, targetSexId == 1 ? 0 : maxFacialHair);
    maxFacialHair = static_cast<int>(facialHairIds_.size()) - 1;
    facialHair = std::clamp(facialHair, 0, maxFacialHair);

    prevRangeRace_ = raceIndex;
    prevRangeGender_ = genderIndex;
    prevRangeBodyType_ = bodyTypeIndex;
    prevRangeSkin_ = skin;
    prevRangeHairStyle_ = hairStyle;
    prevRangeDK_ = dk ? 1 : 0;
}

namespace {

// The screen is CharacterCreate.xml's, drawn natively: every number below is
// that file's, in glue units (see glue_ui.hpp).

/// A TexCoords rectangle: left, right, top, bottom.
struct TexRect {
    float l, r, t, b;
};

/// GetNameForRace's file string, which the glue's tables and strings key on.
const char* raceFile(game::Race race) {
    switch (race) {
        case game::Race::HUMAN:     return "HUMAN";
        case game::Race::ORC:       return "ORC";
        case game::Race::DWARF:     return "DWARF";
        case game::Race::NIGHT_ELF: return "NIGHTELF";
        case game::Race::UNDEAD:    return "SCOURGE";
        case game::Race::TAUREN:    return "TAUREN";
        case game::Race::GNOME:     return "GNOME";
        case game::Race::TROLL:     return "TROLL";
        case game::Race::BLOOD_ELF: return "BLOODELF";
        case game::Race::DRAENEI:   return "DRAENEI";
        default:                    return "HUMAN";
    }
}

/// The class's file string, as CLASS_ICON_TCOORDS and GlueStrings name it.
const char* classFile(game::Class cls) {
    switch (cls) {
        case game::Class::WARRIOR:      return "WARRIOR";
        case game::Class::PALADIN:      return "PALADIN";
        case game::Class::HUNTER:       return "HUNTER";
        case game::Class::ROGUE:        return "ROGUE";
        case game::Class::PRIEST:       return "PRIEST";
        case game::Class::DEATH_KNIGHT: return "DEATHKNIGHT";
        case game::Class::SHAMAN:       return "SHAMAN";
        case game::Class::MAGE:         return "MAGE";
        case game::Class::WARLOCK:      return "WARLOCK";
        case game::Class::DRUID:        return "DRUID";
        default:                        return "WARRIOR";
    }
}

/// RACE_ICON_TCOORDS: an 8x4 grid, males in the top two rows, Alliance
/// then Horde, the same again below for females.
TexRect raceIcon(game::Race race, bool female) {
    int col = 0, row = 0;
    switch (race) {
        case game::Race::HUMAN:     col = 0; row = 0; break;
        case game::Race::DWARF:     col = 1; row = 0; break;
        case game::Race::GNOME:     col = 2; row = 0; break;
        case game::Race::NIGHT_ELF: col = 3; row = 0; break;
        case game::Race::DRAENEI:   col = 4; row = 0; break;
        case game::Race::TAUREN:    col = 0; row = 1; break;
        case game::Race::UNDEAD:    col = 1; row = 1; break;
        case game::Race::TROLL:     col = 2; row = 1; break;
        case game::Race::ORC:       col = 3; row = 1; break;
        case game::Race::BLOOD_ELF: col = 4; row = 1; break;
        default: break;
    }
    if (female) row += 2;
    return {col * 0.125f, (col + 1) * 0.125f, row * 0.25f, (row + 1) * 0.25f};
}

/// CLASS_ICON_TCOORDS, for both UI-CharacterCreate-Classes and
/// UI-Classes-Circles.
TexRect classIcon(game::Class cls) {
    constexpr float c[] = {0.0f, 0.25f, 0.49609375f, 0.7421875f, 0.98828125f};
    int col = 0, row = 0;
    switch (cls) {
        case game::Class::WARRIOR:      col = 0; row = 0; break;
        case game::Class::MAGE:         col = 1; row = 0; break;
        case game::Class::ROGUE:        col = 2; row = 0; break;
        case game::Class::DRUID:        col = 3; row = 0; break;
        case game::Class::HUNTER:       col = 0; row = 1; break;
        case game::Class::SHAMAN:       col = 1; row = 1; break;
        case game::Class::PRIEST:       col = 2; row = 1; break;
        case game::Class::WARLOCK:      col = 3; row = 1; break;
        case game::Class::PALADIN:      col = 0; row = 2; break;
        case game::Class::DEATH_KNIGHT: col = 1; row = 2; break;
        default: break;
    }
    return {c[col], c[col + 1], row * 0.25f, (row + 1) * 0.25f};
}

/// ChrRaces' hairCustomization: what the third and fourth customization
/// rows are called (HAIR_<x>_STYLE / HAIR_<x>_COLOR).
const char* hairCustomization(game::Race race) {
    return race == game::Race::TAUREN ? "HORNS" : "NORMAL";
}

/// ChrRaces' facialHairCustomization for each sex: what the fifth row is
/// called (FACIAL_HAIR_<x>).
const char* facialHairCustomization(game::Race race, bool female) {
    if (!female) {
        if (race == game::Race::UNDEAD) return "FEATURES";
        if (race == game::Race::TROLL) return "TUSKS";
        return "NORMAL";
    }
    switch (race) {
        case game::Race::NIGHT_ELF: return "MARKINGS";
        case game::Race::UNDEAD:    return "FEATURES";
        case game::Race::TAUREN:    return "HAIR";
        case game::Race::GNOME:     return "EARRINGS";
        case game::Race::TROLL:     return "TUSKS";
        case game::Race::BLOOD_ELF: return "EARRINGS";
        case game::Race::DRAENEI:   return "HORNS";
        default:                    return "PIERCINGS";
    }
}

/// GetFlavorText: the sex's own text, or the other's when it has none.
std::string flavorText(const std::string& tag, bool female) {
    const std::string primary = female ? tag + "_FEMALE" : tag;
    const std::string secondary = female ? tag : tag + "_FEMALE";
    std::string text = glue::str(primary.c_str());
    if (text.empty()) text = glue::str(secondary.c_str());
    return text;
}

/// The numbered lines a race's or class's description starts with:
/// ABILITY_INFO_HUMAN1.. or CLASS_INFO_WARRIOR0.., each followed by a blank.
std::string numberedLines(const std::string& stem, int first) {
    std::string out;
    for (int i = first;; ++i) {
        const std::string key = stem + std::to_string(i);
        if (!glue::hasStr(key)) break;
        out += glue::str(key.c_str()) + "\n\n";
    }
    return out;
}

std::mt19937& rng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

int randomUpTo(int maxValue) {
    if (maxValue <= 0) return 0;
    return std::uniform_int_distribution<int>(0, maxValue)(rng());
}

} // namespace

void CharacterCreateScreen::drawScene(float screenW, float screenH) {
    if (!assetManager_) assetManager_ = core::Application::getInstance().getAssetManager();
    if (!assetManager_ || !assetManager_->isInitialized() || availableRaces_.empty()) return;
    if (!preview_) {
        auto preview = std::make_unique<rendering::CharacterPreview>();
        const int w = std::max(64, static_cast<int>(screenW));
        const int h = std::max(64, static_cast<int>(screenH));
        if (!preview->initialize(assetManager_, w, h)) {
            LOG_WARNING("CharacterCreateScreen: failed to init CharacterPreview");
            return;
        }
        preview_ = std::move(preview);
        preview_->setSceneCameraFraming(true);
        if (auto* renderer = core::Application::getInstance().getRenderer())
            renderer->registerPreview(preview_.get());
        prevRaceIndex_ = -1;
    }

    // GetCreateBackgroundModel answers by class for a death knight: Acherus.
    const bool dk = !availableClasses.empty() &&
                    classIndex < static_cast<int>(availableClasses.size()) &&
                    availableClasses[static_cast<size_t>(classIndex)] == game::Class::DEATH_KNIGHT;
    if (static_cast<int>(dk) != prevSceneDK_) {
        preview_->setGlueSceneOverride(dk ? "UI_DeathKnight" : "");
        prevSceneDK_ = dk ? 1 : 0;
        prevRaceIndex_ = -1;  // the scene is loaded with the character
    }

    updatePreviewIfNeeded();
    preview_->render();
    preview_->requestComposite();
    if (VkDescriptorSet tex = preview_->getTextureId()) {
        ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(0, 0),
                                                 ImVec2(screenW, screenH));
        sceneShown_ = true;
    }
}

void CharacterCreateScreen::randomizeAppearance() {
    // Skin and hair style first: the faces and hair colours on offer depend
    // on them, and are asked for again before they are chosen.
    skin = randomUpTo(maxSkin);
    hairStyle = randomUpTo(maxHairStyle);
    facialHair = randomUpTo(maxFacialHair);
    updateAppearanceRanges();
    face = randomUpTo(maxFace);
    hairColor = randomUpTo(maxHairColor);
}

std::string CharacterCreateScreen::randomName() const {
    if (!assetManager_ || availableRaces_.empty()) return {};
    auto dbc = assetManager_->loadDBC("NameGen.dbc");
    if (!dbc) return {};
    // NameGen.dbc: ID, Name, RaceID, Sex.
    const uint32_t race = static_cast<uint32_t>(availableRaces_[static_cast<size_t>(raceIndex)]);
    const uint32_t sex = genderIndex == 0 ? 0u : 1u;
    std::vector<uint32_t> rows;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        if (dbc->getUInt32(r, 2) == race && dbc->getUInt32(r, 3) == sex) rows.push_back(r);
    }
    if (rows.empty()) return {};
    const uint32_t row =
        rows[std::uniform_int_distribution<size_t>(0, rows.size() - 1)(rng())];
    return dbc->getString(row, 1);
}

void CharacterCreateScreen::render(game::GameHandler& /*gameHandler*/) {
    // Resolve valid DBC option IDs before loading the preview. Doing this after
    // loadCharacter left race/style changes using the previous option mapping.
    updateAppearanceRanges();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);
    sceneShown_ = false;

    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 white = IM_COL32_WHITE;

    // ---- the scene ----------------------------------------------------------
    drawScene(screen.x, screen.y);

    const bool waiting = (createTimer_ >= 0.0f);
    const bool errorUp = statusIsError && !statusMessage.empty();
    const bool modalUp = waiting || errorUp;
    if (modalUp) ui_.pushInert();

    const int raceCount = static_cast<int>(availableRaces_.size());
    raceIndex = std::clamp(raceIndex, 0, std::max(0, raceCount - 1));
    const game::Race race = raceCount > 0 ? availableRaces_[static_cast<size_t>(raceIndex)]
                                          : game::Race::HUMAN;
    const bool female = (genderIndex == 1);
    const bool alliance = raceIndex < allianceRaceCount_;
    const bool haveClass = !availableClasses.empty() &&
                           classIndex < static_cast<int>(availableClasses.size());
    const game::Class cls = haveClass ? availableClasses[static_cast<size_t>(classIndex)]
                                      : game::Class::WARRIOR;
    const glue::ButtonStyle buttonStyle =  // CharacterCreate_DeathKnightSwap
        (haveClass && cls == game::Class::DEATH_KNIGHT) ? glue::ButtonStyle::Blue
                                                         : glue::ButtonStyle::Red;

    // FACTION_BACKDROP_COLOR_TABLE: the border, then the background.
    const ImU32 edgeTint = alliance ? IM_COL32(128, 128, 128, 255) : IM_COL32(128, 51, 51, 255);
    const ImU32 bgTint = alliance ? IM_COL32(23, 23, 48, 255) : IM_COL32(48, 13, 13, 255);

    // A tooltip asked for by whatever is under the pointer, drawn last.
    std::string tip;
    ImVec2 tipAt(0, 0);

    const auto pickRace = [&](int i) {
        if (raceIndex == i) return;
        raceIndex = i;
        classIndex = 0;
        skin = face = hairStyle = hairColor = facialHair = 0;
        updateAvailableClasses();
        // The class stays chosen when the new race can be it, as it does in
        // the stock screen.
        if (haveClass) {
            const auto it = std::find(availableClasses.begin(), availableClasses.end(), cls);
            if (it != availableClasses.end())
                classIndex = static_cast<int>(it - availableClasses.begin());
        }
    };

    // CharacterCreateIconButtonTemplate: the shadow, the icon, its bevel, the
    // black of a disabled one, and the ADD highlight and checked art.
    const auto iconButton = [&](const char* id, float cx, float top, const char* file,
                                TexRect tc, bool checked, bool enabled,
                                bool disableOverlay) -> PaperUI::PressState {
        const ImVec2 a(cx - 19.0f * u, top);
        const ImVec2 b(cx + 19.0f * u, top + 38.0f * u);
        const PaperUI::PressState st = ui_.press(id, a, b, enabled);
        const ImVec2 c(cx, top + 19.0f * u);
        const float shadow = (st.held ? 26.0f : 29.0f) * u;
        glue::image(dl, "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-IconShadow",
                    ImVec2(c.x - shadow, c.y - shadow), ImVec2(c.x + shadow, c.y + shadow),
                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 217));
        const float push = st.held ? 2.0f * u : 0.0f;
        const ImVec2 pa(a.x + push, a.y + push), pb(b.x + push, b.y + push);
        // SetButtonDesaturated: half grey where the shader cannot desaturate.
        glue::image(dl, file, pa, pb, ImVec2(tc.l, tc.t), ImVec2(tc.r, tc.b),
                    enabled ? white : IM_COL32(128, 128, 128, 255));
        const char* hl = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Highlights";
        glue::image(dl, hl, pa, pb, ImVec2(0.0f, 0.5f), ImVec2(0.5f, 1.0f));
        if (disableOverlay) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 191));
        if (enabled && st.hovered) {
            glue::image(dl, hl, a, b, ImVec2(0, 0), ImVec2(0.5f, 0.5f), white, true);
        }
        if (checked) glue::image(dl, hl, a, b, ImVec2(0.5f, 0), ImVec2(1.0f, 0.5f), white, true);
        // motionScriptsWhileDisabled: a greyed button still says why.
        PaperUI::PressState out = st;
        out.hovered = st.hovered || (!enabled && ui_.hover(a, b));
        if (out.hovered) tipAt = ImVec2(b.x + 3.0f * u, a.y);
        return out;
    };

    // ---- CharacterCreateConfigurationFrame: the left column -----------------
    const float fx = 9.0f * u, fy = 5.0f * u;
    const float fw = 256.0f * u, fh = 758.0f * u;
    const float fcx = fx + fw * 0.5f;
    ui_.claimMouse(ImVec2(fx, fy), ImVec2(fx + fw, fy + fh));
    {
        const char* border = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-OuterBorder";
        glue::image(dl, "Interface\\Tooltips\\UI-Tooltip-Background",
                    ImVec2(fx + 10.0f * u, fy + 8.0f * u),
                    ImVec2(fx + fw - 10.0f * u, fy + fh - 8.0f * u), ImVec2(0, 0), ImVec2(1, 1),
                    bgTint);
        glue::image(dl, "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Banners",
                    ImVec2(fx, fy + 5.0f * u), ImVec2(fx + fw, fy + 517.0f * u));
        glue::image(dl, border, ImVec2(fx, fy), ImVec2(fx + fw, fy + 256.0f * u), ImVec2(0, 1),
                    ImVec2(1, 0));
        glue::image(dl, border, ImVec2(fx, fy + 256.0f * u), ImVec2(fx + fw, fy + 502.0f * u),
                    ImVec2(0, 0), ImVec2(1, 0.9609375f));
        glue::image(dl, border, ImVec2(fx, fy + 502.0f * u), ImVec2(fx + fw, fy + 758.0f * u));

        // FactionName_Shadow_Huge over each column.
        ImFont* title = glue::titleFace();
        const float huge = 18.0f * u;
        const std::string allianceLabel = glue::str("ALLIANCE", "Alliance");
        const std::string hordeLabel = glue::str("HORDE", "Horde");
        if (allianceRaceCount_ > 0) {
            glue::text(dl, title,
                       ImVec2(fcx - 50.0f * u -
                                  glue::textWidth(title, allianceLabel.c_str(), huge) * 0.5f,
                              fy + 20.0f * u),
                       allianceLabel.c_str(), huge, white);
        }
        if (allianceRaceCount_ < raceCount) {
            glue::text(dl, title,
                       ImVec2(fcx + 50.0f * u - glue::textWidth(title, hordeLabel.c_str(), huge) * 0.5f,
                              fy + 20.0f * u),
                       hordeLabel.c_str(), huge, white);
        }
    }

    // CharacterCreateRaceButton1..10: Alliance down the left at TOP(-50,-61),
    // Horde down the right at TOP(50,-61), each 21 under the last.
    {
        const char* racesFile = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Races";
        const float small = glue::kFontSmall * u;
        for (int i = 0; i < raceCount; ++i) {
            const bool horde = i >= allianceRaceCount_;
            const int slot = horde ? i - allianceRaceCount_ : i;
            const float cx = fcx + (horde ? 50.0f : -50.0f) * u;
            const float top = fy + (61.0f + 59.0f * static_cast<float>(slot)) * u;
            const game::Race r = availableRaces_[static_cast<size_t>(i)];
            char id[24];
            std::snprintf(id, sizeof(id), "cc.race%d", i);
            const auto st = iconButton(id, cx, top, racesFile, raceIcon(r, female), raceIndex == i,
                                       true, false);
            if (st.clicked) pickRace(i);
            if (st.hovered) tip = game::getRaceName(r);
            // The selected race's name over its button.
            if (raceIndex == i) {
                glue::textCentered(dl, cx, top - 2.0f * u - small, game::getRaceName(r), small,
                                   glue::kNormal);
            }
        }
    }

    // CharacterCreateGender over the two gender buttons at TOP(-+20,-392).
    {
        const float medium = 15.0f * u;
        const std::string maleLabel = glue::str("MALE", "Male");
        const std::string femaleLabel = glue::str("FEMALE", "Female");
        glue::textCentered(dl, fcx, fy + 388.0f * u - medium,
                           (female ? femaleLabel : maleLabel).c_str(), medium, glue::kNormal);
        const char* genderFile = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Gender";
        const auto male = iconButton("cc.male", fcx - 20.0f * u, fy + 392.0f * u, genderFile,
                                     {0.0f, 0.5f, 0.0f, 1.0f}, genderIndex == 0, true, false);
        if (male.hovered) tip = maleLabel;
        if (male.clicked) genderIndex = 0;
        const auto fem = iconButton("cc.female", fcx + 20.0f * u, fy + 392.0f * u, genderFile,
                                    {0.5f, 1.0f, 0.0f, 1.0f}, genderIndex == 1, true, false);
        if (fem.hovered) tip = femaleLabel;
        if (fem.clicked) genderIndex = 1;
    }

    // CharacterCreateClassName, then CharacterCreateClassButton1..10 in two
    // rows of five from TOP(-90,-463), six apart.
    {
        const float medium = 15.0f * u;
        if (haveClass) {
            glue::textCentered(dl, fcx, fy + 456.0f * u - medium, game::getClassName(cls), medium,
                               glue::kNormal);
        }
        std::vector<game::Class> shown = expansionClasses_;
        if (shown.empty()) shown.assign(std::begin(kAllClasses), std::end(kAllClasses));
        const char* classesFile = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes";
        for (size_t k = 0; k < shown.size() && k < 10; ++k) {
            const game::Class c = shown[k];
            const auto it = std::find(availableClasses.begin(), availableClasses.end(), c);
            const bool enabled = it != availableClasses.end();
            const int index = enabled ? static_cast<int>(it - availableClasses.begin()) : -1;
            const float cx = fcx + (-90.0f + 44.0f * static_cast<float>(k % 5)) * u;
            const float top = fy + (463.0f + 44.0f * static_cast<float>(k / 5)) * u;
            char id[24];
            std::snprintf(id, sizeof(id), "cc.class%zu", k);
            const auto st = iconButton(id, cx, top, classesFile, classIcon(c),
                                       haveClass && c == cls, enabled, !enabled);
            if (st.hovered) {
                // A class the race cannot be says why, as <CLASS>_DISABLED does.
                tip = enabled ? std::string(game::getClassName(c))
                              : glue::str((std::string(classFile(c)) + "_DISABLED").c_str(),
                                          game::getClassName(c));
            }
            if (st.clicked && enabled) classIndex = index;
        }
    }

    // Race, sex or class may have changed above; refresh the option lists
    // before the rows that cycle through them.
    updateAppearanceRanges();

    // CharCreateRandomizeButton at the frame's BOTTOM(0,10), and the five
    // CharacterCustomizationButtonFrames stacked on it.
    {
        const std::string label = glue::str("RANDOMIZE", "Randomize");
        const float small = glue::kFontSmall * u;
        const float rw = glue::textWidth(label.c_str(), small) + 50.0f * u;
        const float rBottom = fy + fh - 10.0f * u;
        const float rTop = rBottom - 30.0f * u;
        if (glue::button(ui_, dl, "cc.randomize", ImVec2(fcx - rw * 0.5f, rTop),
                         ImVec2(fcx + rw * 0.5f, rBottom), label.c_str(), buttonStyle, true,
                         glue::kFontSmall)) {
            randomizeAppearance();
        }

        const std::string hair = hairCustomization(race);
        const std::string facial = facialHairCustomization(race, female);
        const std::string labels[5] = {
            glue::str("CHAR_CUSTOMIZATION1_DESC", "Skin Color"),
            glue::str("CHAR_CUSTOMIZATION2_DESC", "Face"),
            glue::str(("HAIR_" + hair + "_STYLE").c_str(), "Hair Style"),
            glue::str(("HAIR_" + hair + "_COLOR").c_str(), "Hair Color"),
            glue::str(("FACIAL_HAIR_" + facial).c_str(), "Facial Hair"),
        };
        int* values[5] = {&skin, &face, &hairStyle, &hairColor, &facialHair};
        const int maxima[5] = {maxSkin, maxFace, maxHairStyle, maxHairColor, maxFacialHair};
        const char* labelFrame = "Interface\\Glues\\CharacterCreate\\CharacterCreate-LabelFrame";
        for (int k = 0; k < 5; ++k) {
            const float bottom = rTop - 3.0f * u - static_cast<float>(4 - k) * 32.0f * u;
            const float top = bottom - 32.0f * u;
            const float x0 = fcx - 6.0f * u - 115.0f * u;
            const float midY = top + 16.0f * u;
            glue::image(dl, labelFrame, ImVec2(x0 - 5.0f * u, midY - 32.0f * u),
                        ImVec2(x0 + 20.0f * u, midY + 32.0f * u), ImVec2(0, 0),
                        ImVec2(0.1953125f, 1));
            glue::image(dl, labelFrame, ImVec2(x0 + 20.0f * u, midY - 32.0f * u),
                        ImVec2(x0 + 161.0f * u, midY + 32.0f * u), ImVec2(0.1953125f, 0),
                        ImVec2(0.8046875f, 1));
            glue::image(dl, labelFrame, ImVec2(x0 + 161.0f * u, midY - 32.0f * u),
                        ImVec2(x0 + 186.0f * u, midY + 32.0f * u), ImVec2(0.8046875f, 0),
                        ImVec2(1, 1));
            glue::textCentered(dl, x0 + 90.5f * u, midY - 2.0f * u - small * 0.5f,
                               labels[k].c_str(), small, white);

            // CycleCharCustomization wraps round at either end.
            const int count = maxima[k] + 1;
            char id[24];
            std::snprintf(id, sizeof(id), "cc.cust%dl", k);
            const auto left = glue::textureButton(
                ui_, dl, id, ImVec2(x0 + 169.0f * u, top), ImVec2(x0 + 201.0f * u, top + 32.0f * u),
                "Interface\\Glues\\Common\\Glue-LeftArrow-Button-Up",
                "Interface\\Glues\\Common\\Glue-LeftArrow-Button-Down",
                "Interface\\Glues\\Common\\Glue-LeftArrow-Button-Highlight");
            std::snprintf(id, sizeof(id), "cc.cust%dr", k);
            const auto right = glue::textureButton(
                ui_, dl, id, ImVec2(x0 + 198.0f * u, top), ImVec2(x0 + 230.0f * u, top + 32.0f * u),
                "Interface\\Glues\\Common\\Glue-RightArrow-Button-Up",
                "Interface\\Glues\\Common\\Glue-RightArrow-Button-Down",
                "Interface\\Glues\\Common\\Glue-RightArrow-Button-Highlight");
            if (count > 1 && (left.clicked || right.clicked)) {
                *values[k] = (*values[k] + (left.clicked ? count - 1 : 1)) % count;
                // Skin and hair style choose the faces and hair colours on
                // offer; ask again before the next row reads them.
                updateAppearanceRanges();
            }
        }
    }

    // ---- the right: logo, race and class panels, Accept and Back ------------
    const float logoX1 = screen.x - 9.0f * u;
    const float logoCX = logoX1 - 128.0f * u;
    glue::image(dl, "Interface\\Glues\\Common\\Glues-WoW-WotLKLogo",
                ImVec2(logoX1 - 256.0f * u, 0.0f), ImVec2(logoX1, 128.0f * u));

    {
        const int key = raceIndex * 1000 + (haveClass ? static_cast<int>(cls) : 0) * 10 + genderIndex;
        if (key != infoKey_) {
            infoKey_ = key;
            raceScroll_ = classScroll_ = 0.0f;
        }
    }

    // CharacterCreateCharacterRace / ...Class: 267x287, the icon, the info
    // box art, the name in FactionName_Shadow_Huge and a 220x220 scroll of
    // the numbered lines in gold over the flavour text in white.
    const auto infoBox = [&](const char* id, ImVec2 a, const char* iconFile, TexRect tc,
                             const char* label, const std::string& gold,
                             const std::string& flavour, float& scroll) {
        const ImVec2 b(a.x + 267.0f * u, a.y + 287.0f * u);
        ui_.claimMouse(a, b);
        glue::backdrop(dl, a, b, "Interface\\Tooltips\\UI-Tooltip-Background", "", 16.0f * u,
                       16.0f * u, 12.0f * u, 3.0f * u, 20.0f * u, 6.0f * u, bgTint);
        glue::image(dl, iconFile, ImVec2(a.x + 4.0f * u, a.y + 4.0f * u),
                    ImVec2(a.x + 52.0f * u, a.y + 52.0f * u), ImVec2(tc.l, tc.t), ImVec2(tc.r, tc.b));
        glue::image(dl, "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-InfoBox", a, b,
                    ImVec2(0.005859375f, 0.005859375f), ImVec2(0.52734375f, 0.56640625f));
        glue::text(dl, glue::titleFace(), ImVec2(a.x + 63.5f * u, a.y + 10.0f * u), label,
                   18.0f * u, white);

        const ImVec2 sa(a.x + 20.0f * u, a.y + 55.0f * u);
        const ImVec2 sb(sa.x + 220.0f * u, sa.y + 220.0f * u);
        const float small = glue::kFontSmall * u;
        const std::vector<std::string> goldLines = glue::wrap(glue::plain(gold), 220.0f * u, small);
        const std::vector<std::string> flavourLines =
            glue::wrap(glue::plain(flavour), 220.0f * u, small);
        const float goldStep = small + 1.0f * u;
        const float flavourStep = small + 2.5f * u;  // GlueFontCharacterCreate's spacing
        const float total = goldStep * static_cast<float>(goldLines.size()) + 2.0f * u +
                            flavourStep * static_cast<float>(flavourLines.size());
        const float range = std::max(0.0f, total / u - 220.0f);
        if (!modalUp && ui_.hover(sa, sb) && ImGui::GetIO().MouseWheel != 0.0f) {
            scroll -= ImGui::GetIO().MouseWheel * 3.0f * (goldStep / u);
        }
        scroll = std::clamp(scroll, 0.0f, range);
        (void)id;

        dl->PushClipRect(sa, sb, true);
        float y = sa.y - scroll * u;
        for (const auto& line : goldLines) {
            if (y + goldStep >= sa.y && y <= sb.y)
                glue::text(dl, ImVec2(sa.x, y), line.c_str(), small, glue::kNormal);
            y += goldStep;
        }
        y += 2.0f * u;
        for (const auto& line : flavourLines) {
            if (y + flavourStep >= sa.y && y <= sb.y)
                glue::text(dl, ImVec2(sa.x, y), line.c_str(), small, white);
            y += flavourStep;
        }
        dl->PopClipRect();

        // Where there is more than fits, a thin bar beside it says how much.
        if (range > 0.0f) {
            const float trackX = sb.x + 9.0f * u;
            const float knobH = std::max(16.0f * u, (sb.y - sa.y) * 220.0f / (220.0f + range));
            const float knobY = sa.y + (sb.y - sa.y - knobH) * (scroll / range);
            dl->AddRectFilled(ImVec2(trackX, sa.y), ImVec2(trackX + 4.0f * u, sb.y),
                              IM_COL32(0, 0, 0, 120), 2.0f * u);
            dl->AddRectFilled(ImVec2(trackX, knobY), ImVec2(trackX + 4.0f * u, knobY + knobH),
                              IM_COL32(255, 199, 0, 200), 2.0f * u);
        }
    };

    const float raceCX = logoCX - 5.0f * u;
    const float raceTop = 123.0f * u;
    {
        const std::string file = raceFile(race);
        const TexRect tc = raceIcon(race, female);
        infoBox("cc.raceinfo", ImVec2(raceCX - 133.5f * u, raceTop),
                "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-RacesRound", tc,
                game::getRaceName(race), numberedLines("ABILITY_INFO_" + file, 1),
                flavorText("RACE_INFO_" + file, female), raceScroll_);
    }
    const float classCX = raceCX - 5.0f * u;
    const float classTop = raceTop + 287.0f * u + 5.0f * u;
    if (haveClass) {
        const std::string file = classFile(cls);
        infoBox("cc.classinfo", ImVec2(classCX - 133.5f * u, classTop),
                "Interface\\TargetingFrame\\UI-Classes-Circles", classIcon(cls),
                game::getClassName(cls), numberedLines("CLASS_INFO_" + file, 0),
                flavorText("CLASS_" + file, female), classScroll_);
    }

    // The create itself: the name, checked here as the client checks it
    // before asking the server, then the packet.
    const auto submit = [&]() {
        std::string name = name_.text();
        const size_t start = name.find_first_not_of(" \t\r\n");
        const size_t end = name.find_last_not_of(" \t\r\n");
        name = (start == std::string::npos) ? std::string() : name.substr(start, end - start + 1);
        size_t letters = 0;
        bool onlyLetters = true;
        for (unsigned char c : name) {
            if ((c & 0xC0) != 0x80) ++letters;
            if (c < 0x80 && !std::isalpha(c)) onlyLetters = false;
        }
        if (name.empty()) {
            setStatus(glue::str("CHAR_NAME_NO_NAME", "Enter a name for your character"), true);
            ui_.focus("cc.name");
            return;
        }
        if (letters < 2) {
            setStatus(glue::str("CHAR_NAME_TOO_SHORT", "Names must be at least 2 characters"), true);
            return;
        }
        if (!onlyLetters) {
            setStatus(glue::str("CHAR_NAME_INVALID_CHARACTER", "Names can only contain letters"),
                      true);
            return;
        }
        if (availableClasses.empty()) {
            setStatus("No valid class for this race.", true);
            return;
        }
        setStatus(glue::str("CHAR_CREATE_IN_PROGRESS", "Creating character"), false);
        createTimer_ = 0.0f;
        game::CharCreateData data;
        data.name = name;
        data.race = availableRaces_[static_cast<size_t>(raceIndex)];
        data.characterClass = availableClasses[static_cast<size_t>(classIndex)];
        data.gender = static_cast<game::Gender>(genderIndex);
        data.useFemaleModel = (genderIndex == 2 && bodyTypeIndex == 1);  // Nonbinary + Feminine
        data.skin = selectedAppearanceId(skinIds_, skin);
        data.face = selectedAppearanceId(faceIds_, face);
        data.hairStyle = selectedAppearanceId(hairStyleIds_, hairStyle);
        data.hairColor = selectedAppearanceId(hairColorIds_, hairColor);
        data.facialHair = selectedAppearanceId(facialHairIds_, facialHair);
        if (onCreate) onCreate(data);
    };

    // CharCreateOkayButton (160x35) under the class panel, and
    // CharCreateBackButton (120x30) under that.
    {
        const float okTop = classTop + 287.0f * u + 3.0f * u;
        const float okCX = classCX + 4.0f * u;
        const std::string accept = glue::str("CHARACTER_CREATE_ACCEPT", "Accept");
        if (glue::button(ui_, dl, "cc.accept", ImVec2(okCX - 80.0f * u, okTop),
                         ImVec2(okCX + 80.0f * u, okTop + 35.0f * u), accept.c_str(), buttonStyle,
                         !waiting, glue::kFontNormal)) {
            submit();
        }
        const float backTop = okTop + 35.0f * u - 3.0f * u;
        const float backCX = okCX + 4.0f * u;
        const std::string back = glue::str("BACK", "Back");
        if (glue::button(ui_, dl, "cc.back", ImVec2(backCX - 60.0f * u, backTop),
                         ImVec2(backCX + 60.0f * u, backTop + 30.0f * u), back.c_str(),
                         buttonStyle, true, glue::kFontSmall)) {
            if (onCancel) onCancel();
        }
    }

    // ---- the bottom: name, random name, rotation ------------------------------
    const float cx = screen.x * 0.5f;
    {
        // CharacterCreateNameEdit: 156x40 at BOTTOM(0,55), "Name" over it.
        const ImVec2 a(cx - 78.0f * u, screen.y - 95.0f * u);
        const ImVec2 b(cx + 78.0f * u, screen.y - 55.0f * u);
        glue::backdrop(dl, a, b, "Interface\\Tooltips\\UI-Tooltip-Background",
                       "Interface\\Glues\\Common\\Glue-Tooltip-Border", 16.0f * u, 16.0f * u,
                       10.0f * u, 5.0f * u, 4.0f * u, 9.0f * u, bgTint, edgeTint);
        const float large = glue::kFontLarge * u;
        const std::string nameLabel = glue::str("NAME", "Name");
        glue::textCentered(dl, cx, a.y - 9.0f * u - large * 0.5f, nameLabel.c_str(), large,
                           glue::kNormal);
        if (!nameFocused_ && !modalUp) {
            ui_.focus("cc.name");
            nameFocused_ = true;
        }
        PaperUI::FieldOpts opts;
        opts.glue = true;
        const PaperUI::FieldResult r = ui_.field("cc.name", ImVec2(a.x + 15.0f * u, a.y + 2.0f * u),
                                                 ImVec2(b.x - 8.0f * u, a.y + 34.0f * u), name_,
                                                 opts);
        (void)r;

        // CharacterCreateRandomName: under the box, 7 up into it.
        const std::string label = glue::str("RANDOMIZE", "Randomize");
        const float small = glue::kFontSmall * u;
        const float rw = glue::textWidth(label.c_str(), small) + 50.0f * u;
        if (glue::button(ui_, dl, "cc.randomname", ImVec2(cx - rw * 0.5f, b.y - 7.0f * u),
                         ImVec2(cx + rw * 0.5f, b.y + 23.0f * u), label.c_str(), buttonStyle, true,
                         glue::kFontSmall)) {
            const std::string picked = randomName();
            if (!picked.empty()) name_.setText(picked);
        }
    }

    // CharacterCreateRotateLeft/Right: 50x50 at BOTTOM(-+16,-5), turning
    // while held, with a hit rect inset 13/13/10/13.
    if (preview_) {
        const float bs = 50.0f * u;
        const auto rotateButton = [&](const char* id, float bcx, bool flip, bool& held) {
            const ImVec2 a(bcx - bs * 0.5f, screen.y + 5.0f * u - bs);
            const ImVec2 b(a.x + bs, a.y + bs);
            const auto st = ui_.press(id, ImVec2(a.x + 13.0f * u, a.y + 10.0f * u),
                                      ImVec2(b.x - 13.0f * u, b.y - 13.0f * u));
            held = st.held;
            const char* tex = st.held ? "Interface\\Glues\\CharacterCreate\\UI-RotationRight-Big-Down"
                                      : "Interface\\Glues\\CharacterCreate\\UI-RotationRight-Big-Up";
            glue::image(dl, tex, a, b, ImVec2(flip ? 1.0f : 0.0f, 0.0f), ImVec2(flip ? 0.0f : 1.0f, 1.0f));
            if (st.hovered) {
                const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                glue::image(dl, "Interface\\Buttons\\UI-Common-MouseHilight",
                            ImVec2(c.x - 15.0f * u, c.y - 15.0f * u),
                            ImVec2(c.x + 15.0f * u, c.y + 15.0f * u), ImVec2(0, 0), ImVec2(1, 1),
                            white, true);
            }
        };
        rotateButton("cc.rotl", cx - 16.0f * u, true, rotatingLeft_);
        rotateButton("cc.rotr", cx + 16.0f * u, false, rotatingRight_);
        const float dt = ImGui::GetIO().DeltaTime;
        if (rotatingLeft_) preview_->rotate(-120.0f * dt);
        if (rotatingRight_) preview_->rotate(120.0f * dt);

        // CharacterCreateFrame_OnMouseDown: a drag that starts on the scene
        // itself, between the two columns, turns the character.
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 from = io.MouseClickedPos[0];
        const float rightX = classCX - 133.5f * u - 5.0f * u;
        if (!modalUp && !rotatingLeft_ && !rotatingRight_ &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left) && from.x > fx + fw && from.x < rightX &&
            from.y < screen.y - 100.0f * u) {
            // CharacterCreateFrame_OnUpdate: cursor x in 768-tall glue units
            // times CHARACTER_ROTATION_CONSTANT (0.6), in degrees.
            preview_->rotate(io.MouseDelta.x / u * 0.6f);
        }
    }

    // Enter is Accept and Escape is Back, as CharacterCreate_OnKeyDown has it.
    if (!modalUp) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            submit();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (onCancel) onCancel();
        }
    }

    if (modalUp) ui_.popInert();

    if (!tip.empty() && !modalUp) glue::tooltip(ImGui::GetForegroundDrawList(), tipAt, tip);

    // ---- dialogs ----------------------------------------------------------------
    // The server's answer or the name check's, in a GlueDialog with Okay.
    if (errorUp) {
        if (glue::dialog(ui_, "cc.error", statusMessage, glue::str("OKAY", "Okay").c_str()) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            setStatus("", false);
            ui_.focus("cc.name");
        }
    } else if (waiting) {
        glue::dialog(ui_, "cc.waiting", statusMessage.empty()
                                            ? glue::str("CHAR_CREATE_IN_PROGRESS", "Creating character")
                                            : statusMessage,
                     nullptr);
    }

    ui_.end();
}

} // namespace ui
} // namespace wowee
