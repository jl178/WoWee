// ============================================================
// WindowManager - extracted from GameScreen
// Owns all NPC interaction windows, popup dialogs, etc.
// ============================================================
#include "core/local_time.hpp"
#include "ui/window_manager.hpp"
#include "ui/ui_texture_load.hpp"
#include "game/item_text.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/ui_upload_budget.hpp"
#include "game/inventory_slots.hpp"
#include "ui/chat_panel.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/settings_panel.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/ui_colors.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/vk_context.hpp"
#include "core/window.hpp"
#include "game/game_handler.hpp"
#include "game/auction_filters.hpp"
#include "game/packed_time.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "pipeline/spell_icon_paths.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <fstream>

namespace wowee {
namespace ui {

WindowManager::~WindowManager() = default;









// ---------------------------------------------------------------------------
// Barber shop state
//
// Built once each time the chair opens, and kept out of the render function so
// that the answers survive the panel being handed to FrameXML.
// ---------------------------------------------------------------------------
int WindowManager::barberFindAppearance(const std::vector<BarberStyleOption>& options,
                                        uint8_t id) {
    const auto it = std::find_if(options.begin(), options.end(),
                                 [id](const auto& option) { return option.appearanceId == id; });
    return it == options.end() ? -1 : static_cast<int>(std::distance(options.begin(), it));
}

uint8_t WindowManager::barberSelectedAppearance(const std::vector<BarberStyleOption>& options,
                                                int index, uint8_t fallback) {
    return index >= 0 && index < static_cast<int>(options.size())
        ? options[static_cast<size_t>(index)].appearanceId : fallback;
}

void WindowManager::rebuildBarberHairColors(uint8_t hairStyle, uint8_t preferredColor,
                                            uint32_t raceId, uint32_t sexId) {
    barberHairColors_.clear();
    if (services_.assetManager) {
        auto dbc = services_.assetManager->loadDBC("CharSections.dbc");
        if (dbc && dbc->isLoaded()) {
            const auto* layout = pipeline::getActiveDBCLayout()
                ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
            const auto fields = pipeline::detectCharSectionsFields(dbc.get(), layout);
            for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
                if (dbc->getUInt32(row, fields.raceId) != raceId ||
                    dbc->getUInt32(row, fields.sexId) != sexId ||
                    dbc->getUInt32(row, fields.baseSection) != 3 ||
                    dbc->getUInt32(row, fields.variationIndex) != hairStyle) {
                    continue;
                }
                const uint32_t color = dbc->getUInt32(row, fields.colorIndex);
                if (color <= 255) barberHairColors_.push_back(static_cast<uint8_t>(color));
            }
        }
    }
    std::sort(barberHairColors_.begin(), barberHairColors_.end());
    barberHairColors_.erase(std::unique(barberHairColors_.begin(), barberHairColors_.end()),
                            barberHairColors_.end());
    const auto preferred = std::find(barberHairColors_.begin(), barberHairColors_.end(),
                                     preferredColor);
    barberHairColor_ = preferred == barberHairColors_.end()
        ? (barberHairColors_.empty() ? -1 : 0)
        : static_cast<int>(std::distance(barberHairColors_.begin(), preferred));
    barberColorsForHairStyle_ = hairStyle;
}

void WindowManager::ensureBarberState(game::GameHandler& gameHandler) {
    // Leaving the chair clears the state so the next visit rebuilds it against
    // whatever the character wears by then. The render function used to do this
    // on its early return, which stops happening the moment the panel is handed
    // over - and stale originals would price a change against the wrong hair.
    if (!gameHandler.isBarberShopOpen()) {
        // Taking the unbought preview off is GameHandler::endBarberShop's job,
        // on every way out. It was done here, which only ran if some barber
        // Lua call came after the close - Escape and standing up made none, so
        // the preview stayed on - and after a purchase it put the old look
        // back over the one just paid for.
        barberInitialized_ = false;
        return;
    }
    if (barberInitialized_) return;
    const auto* ch = gameHandler.getActiveCharacter();
    if (!ch) return;

    const uint32_t raceId = static_cast<uint32_t>(ch->race);
    const uint32_t sexId = (ch->gender == game::Gender::FEMALE || ch->useFemaleModel) ? 1u : 0u;

    // BarberShopStyle IDs, rather than the visible appearance numbers, are the
    // wire values expected by a WotLK server. Build the race/sex-specific lists
    // once each time the chair opens.
    barberOrigAppearanceBytes_ = ch->appearanceBytes;
    barberOrigSkinColor_ = static_cast<uint8_t>(ch->appearanceBytes & 0xFF);
    barberOrigHairStyle_ = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
    barberOrigHairColor_ = static_cast<uint8_t>((ch->appearanceBytes >> 24) & 0xFF);
    barberOrigFacialHair_ = ch->facialFeatures;
    barberHairStyles_.clear();
    barberFacialStyles_.clear();
    barberSkinStyles_.clear();
    // Entry zero tells the server to retain the existing skin.
    barberSkinStyles_.push_back({.entryId = 0, .appearanceId = barberOrigSkinColor_, .name = "Current"});

    if (services_.assetManager) {
        auto dbc = services_.assetManager->loadDBC("BarberShopStyle.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 40) {
            for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
                if (dbc->getUInt32(row, 37) != raceId || dbc->getUInt32(row, 38) != sexId)
                    continue;
                const uint32_t appearance = dbc->getUInt32(row, 39);
                if (appearance > 255) continue;
                std::string name;
                for (uint32_t field = 2; field <= 17 && name.empty(); ++field)
                    name = dbc->getString(row, field);
                if (name.empty()) name = "Style " + std::to_string(appearance);
                BarberStyleOption option{.entryId = dbc->getUInt32(row, 0),
                                         .appearanceId = static_cast<uint8_t>(appearance), .name = std::move(name)};
                switch (dbc->getUInt32(row, 1)) {
                    case 0: barberHairStyles_.push_back(std::move(option)); break;
                    case 2: barberFacialStyles_.push_back(std::move(option)); break;
                    case 3: barberSkinStyles_.push_back(std::move(option)); break;
                    default: break;
                }
            }
        } else {
            LOG_WARNING("Barber Shop: WotLK BarberShopStyle.dbc is unavailable or malformed");
        }

        auto baseCost = services_.assetManager->loadDBC("gtBarberShopCostBase.dbc");
        if (baseCost && baseCost->isLoaded() && baseCost->getRecordCount() > 0) {
            const uint32_t level = std::max<uint32_t>(1, ch->level);
            const uint32_t row = std::min(level, baseCost->getRecordCount()) - 1;
            barberBaseCost_ = baseCost->getFloat(row, 0);
        } else {
            barberBaseCost_ = 0.0f;
        }
    }

    auto normalizeOptions = [](std::vector<BarberStyleOption>& options) {
        std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) {
            return a.appearanceId != b.appearanceId
                ? a.appearanceId < b.appearanceId : a.entryId < b.entryId;
        });
        options.erase(std::unique(options.begin(), options.end(), [](const auto& a, const auto& b) {
            return a.appearanceId == b.appearanceId;
        }), options.end());
    };
    normalizeOptions(barberHairStyles_);
    normalizeOptions(barberFacialStyles_);
    if (barberSkinStyles_.size() > 1) {
        std::sort(barberSkinStyles_.begin() + 1, barberSkinStyles_.end(),
                  [](const auto& a, const auto& b) { return a.appearanceId < b.appearanceId; });
        barberSkinStyles_.erase(std::unique(barberSkinStyles_.begin() + 1,
                                            barberSkinStyles_.end(),
                                            [](const auto& a, const auto& b) {
                                                return a.appearanceId == b.appearanceId;
                                            }), barberSkinStyles_.end());
    }

    barberHairStyle_ = barberFindAppearance(barberHairStyles_, barberOrigHairStyle_);
    barberFacialHair_ = barberFindAppearance(barberFacialStyles_, barberOrigFacialHair_);
    barberSkinColor_ = 0;
    rebuildBarberHairColors(barberOrigHairStyle_, barberOrigHairColor_, raceId, sexId);
    barberInitialized_ = true;
}

WindowManager::BarberSelection WindowManager::barberSelection(game::GameHandler& gameHandler) {
    BarberSelection out;
    ensureBarberState(gameHandler);
    const auto* ch = gameHandler.getActiveCharacter();
    if (!ch) return out;
    const uint32_t raceId = static_cast<uint32_t>(ch->race);
    const uint32_t sexId = (ch->gender == game::Gender::FEMALE || ch->useFemaleModel) ? 1u : 0u;

    out.hairStyle = barberSelectedAppearance(barberHairStyles_, barberHairStyle_,
                                             barberOrigHairStyle_);
    // The colours on offer depend on the hair style, so a style change has to
    // rebuild them before the colour selection can be read back.
    if (out.hairStyle != barberColorsForHairStyle_) {
        const uint8_t previousColor =
            barberHairColor_ >= 0 && barberHairColor_ < static_cast<int>(barberHairColors_.size())
                ? barberHairColors_[static_cast<size_t>(barberHairColor_)] : barberOrigHairColor_;
        rebuildBarberHairColors(out.hairStyle, previousColor, raceId, sexId);
    }
    out.hairColor = barberHairColor_ >= 0 &&
                    barberHairColor_ < static_cast<int>(barberHairColors_.size())
        ? barberHairColors_[static_cast<size_t>(barberHairColor_)] : barberOrigHairColor_;
    out.facialHair = barberSelectedAppearance(barberFacialStyles_, barberFacialHair_,
                                              barberOrigFacialHair_);
    out.skin = barberSelectedAppearance(barberSkinStyles_, barberSkinColor_,
                                        barberOrigSkinColor_);
    return out;
}

uint32_t WindowManager::barberTotalCostCopper(game::GameHandler& gameHandler) {
    const BarberSelection sel = barberSelection(gameHandler);
    float cost = 0.0f;
    if (sel.hairStyle != barberOrigHairStyle_)      cost += barberBaseCost_;
    else if (sel.hairColor != barberOrigHairColor_) cost += barberBaseCost_ * 0.5f;
    if (sel.facialHair != barberOrigFacialHair_)    cost += barberBaseCost_ * 0.75f;
    if (sel.skin != barberOrigSkinColor_)           cost += barberBaseCost_ * 0.75f;
    return static_cast<uint32_t>(cost);
}

void WindowManager::barberResetSelections(game::GameHandler& gameHandler) {
    ensureBarberState(gameHandler);
    const auto* ch = gameHandler.getActiveCharacter();
    if (!ch) return;
    const uint32_t raceId = static_cast<uint32_t>(ch->race);
    const uint32_t sexId = (ch->gender == game::Gender::FEMALE || ch->useFemaleModel) ? 1u : 0u;
    barberHairStyle_ = barberFindAppearance(barberHairStyles_, barberOrigHairStyle_);
    barberFacialHair_ = barberFindAppearance(barberFacialStyles_, barberOrigFacialHair_);
    barberSkinColor_ = 0;
    rebuildBarberHairColors(barberOrigHairStyle_, barberOrigHairColor_, raceId, sexId);
    // And put the character back the way they walked in, since the preview has
    // been changing them as the selectors moved.
    barberPreview(gameHandler);
}

void WindowManager::barberApplySelection(game::GameHandler& gameHandler) {
    ensureBarberState(gameHandler);
    const BarberSelection sel = barberSelection(gameHandler);
    // The server is sent BarberShopStyle.dbc entry ids, not the appearance
    // numbers the preview uses - the two are different numbering spaces and
    // only the entry id means anything on the wire.
    auto entryOf = [](const std::vector<BarberStyleOption>& options, int index) {
        return index >= 0 && index < static_cast<int>(options.size())
            ? options[static_cast<size_t>(index)].entryId : 0u;
    };
    gameHandler.sendAlterAppearance(entryOf(barberHairStyles_, barberHairStyle_),
                                    sel.hairColor,
                                    entryOf(barberFacialStyles_, barberFacialHair_),
                                    entryOf(barberSkinStyles_, barberSkinColor_));
}

bool WindowManager::barberStyleInfo(game::GameHandler& gameHandler, int selector,
                                    std::string& name, bool& isCurrent) {
    ensureBarberState(gameHandler);
    const BarberSelection sel = barberSelection(gameHandler);
    switch (selector) {
        case 1:
            if (barberHairStyle_ < 0 ||
                barberHairStyle_ >= static_cast<int>(barberHairStyles_.size())) return false;
            name = barberHairStyles_[static_cast<size_t>(barberHairStyle_)].name;
            isCurrent = sel.hairStyle == barberOrigHairStyle_;
            return true;
        case 2: {
            if (barberHairColor_ < 0 ||
                barberHairColor_ >= static_cast<int>(barberHairColors_.size())) return false;
            // The colours have no names of their own in the data, so they are
            // numbered the way the character creation screen numbers them.
            name = "Color " + std::to_string(barberHairColor_ + 1);
            isCurrent = sel.hairColor == barberOrigHairColor_;
            return true;
        }
        case 3:
            if (barberFacialHair_ < 0 ||
                barberFacialHair_ >= static_cast<int>(barberFacialStyles_.size())) return false;
            name = barberFacialStyles_[static_cast<size_t>(barberFacialHair_)].name;
            isCurrent = sel.facialHair == barberOrigFacialHair_;
            return true;
        case 4:
            if (barberSkinColor_ < 0 ||
                barberSkinColor_ >= static_cast<int>(barberSkinStyles_.size())) return false;
            name = barberSkinStyles_[static_cast<size_t>(barberSkinColor_)].name;
            isCurrent = sel.skin == barberOrigSkinColor_;
            return true;
        default:
            return false;
    }
}

void WindowManager::barberCycleStyle(game::GameHandler& gameHandler, int selector,
                                     int direction) {
    ensureBarberState(gameHandler);
    const int step = direction >= 0 ? 1 : -1;
    auto advance = [step](int& index, size_t count) {
        if (count == 0) { index = -1; return; }
        const int n = static_cast<int>(count);
        index = ((index < 0 ? 0 : index) + step % n + n) % n;
    };
    switch (selector) {
        case 1: advance(barberHairStyle_, barberHairStyles_.size()); break;
        case 2: advance(barberHairColor_, barberHairColors_.size()); break;
        case 3: advance(barberFacialHair_, barberFacialStyles_.size()); break;
        case 4: advance(barberSkinColor_, barberSkinStyles_.size()); break;
        default: break;
    }
    // Re-resolve so a hair style change rebuilds the colours behind it.
    barberSelection(gameHandler);
    barberPreview(gameHandler);
}

/// Put the current selection on the character standing in the shop.
///
/// The whole of the barber shop is a preview - WoW shows the change on the
/// character itself and charges for it when Accept is pressed - and this was
/// the missing half: the selectors named a style and cycling them changed a
/// number, with nothing to see. Nothing is sent by this; barberApplySelection
/// is still the only thing that talks to the server.
void WindowManager::barberPreview(game::GameHandler& gameHandler) {
    // Out of the chair there is nothing to preview, and the selections left
    // behind are stale: written to the record now, they would be a look the
    // character never bought.
    if (!gameHandler.isBarberShopOpen()) return;
    const auto* ch = gameHandler.getActiveCharacter();
    if (!ch) return;
    const BarberSelection sel = barberSelection(gameHandler);
    // Skin, face, hair style and hair colour, one byte each, in the order
    // PLAYER_BYTES packs them. The face is not the barber's to change, so it is
    // kept from what is already there.
    const uint32_t bytes = static_cast<uint32_t>(sel.skin)
                         | (ch->appearanceBytes & 0x0000FF00u)
                         | (static_cast<uint32_t>(sel.hairStyle) << 16)
                         | (static_cast<uint32_t>(sel.hairColor) << 24);
    if (!gameHandler.previewPlayerAppearance(bytes, sel.facialHair)) {
        // Said once. A preview that reaches no record looks exactly like a
        // preview that is not wired up at all, which is the shape this whole
        // panel arrived in.
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARNING("Barber preview: no character record to show it on - "
                        "the styles will cycle and the character will not change");
        }
    }
}

} // namespace ui
} // namespace wowee
