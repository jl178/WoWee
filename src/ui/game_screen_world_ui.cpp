// The in-world interface the 3.3.5a engine draws itself, not FrameXML:
// nameplates (CGNamePlateFrame), names over heads and the raid mark over a
// head (CPlayerName, PlayerName.cpp). docs/re/world-ui.md has the stock rules
// and their addresses; ui/world_ui_rules.hpp has the numbers.
#include "ui/game_screen.hpp"
#include "ui/world_ui_rules.hpp"
#include "ui/stock_unit_color.hpp"
#include "ui/nameplate_registry.hpp"
#include "ui/nameplate_stacking.hpp"
#include "ui/sight_cache.hpp"
#include "ui/ui_raid_icons.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/interface_fonts.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "core/window.hpp"
#include "game/game_handler.hpp"
#include "game/threat_standing.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace ui {

namespace {

using stockui::Rgb;

ImU32 rgbA(const Rgb& c, int alpha) { return IM_COL32(c.r, c.g, c.b, std::clamp(alpha, 0, 255)); }

/// The stock plate art, uploaded once. Every piece is laid out against the
/// plate's own 128 x 32 (Nameplate-Border.blp at 1280 units to the DDC).
struct NameplateArt {
    VkDescriptorSet border = VK_NULL_HANDLE;      // Tooltips\Nameplate-Border
    VkDescriptorSet castShield = VK_NULL_HANDLE;  // Tooltips\Nameplate-CastBar-Shield
    VkDescriptorSet glow = VK_NULL_HANDLE;        // Tooltips\Nameplate-Glow (additive)
    VkDescriptorSet elite = VK_NULL_HANDLE;       // Tooltips\EliteNameplateIcon
    VkDescriptorSet barFill = VK_NULL_HANDLE;     // TargetingFrame\UI-TargetingFrame-BarFill
    VkDescriptorSet skull = VK_NULL_HANDLE;       // TargetingFrame\UI-TargetingFrame-Skull
    VkDescriptorSet flash = VK_NULL_HANDLE;       // TargetingFrame\UI-TargetingFrame-Flash
    int attempts = 0;
};

/// One plate texture. `luminanceAsAlpha` is for the art the client draws with
/// ADD blending (the glow has no alpha channel, only a bright shape on black):
/// the brightness becomes the coverage, so ordinary blending draws the shape
/// and not a black slab around it.
VkDescriptorSet loadPlateTexture(pipeline::AssetManager* am, core::Window* window,
                                 const char* path, bool luminanceAsAlpha) {
    if (!am || !window || !window->getVkContext()) return VK_NULL_HANDLE;
    auto bytes = am->readFile(path);
    if (bytes.empty()) {
        LOG_WARNING("Nameplates: missing texture ", path);
        return VK_NULL_HANDLE;
    }
    auto image = pipeline::BLPLoader::load(bytes);
    if (!image.isValid() || image.data.empty()) {
        LOG_WARNING("Nameplates: could not decode ", path);
        return VK_NULL_HANDLE;
    }
    if (luminanceAsAlpha) {
        for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
            const uint8_t m = std::max({image.data[i], image.data[i + 1], image.data[i + 2]});
            image.data[i] = image.data[i + 1] = image.data[i + 2] = 255;
            image.data[i + 3] = m;
        }
    }
    return window->getVkContext()->uploadImGuiTexture(image.data.data(), image.width,
                                                      image.height);
}

NameplateArt& nameplateArt(pipeline::AssetManager* am, core::Window* window) {
    static NameplateArt art;
    // Retried a few times only: before the Vulkan context is up nothing loads,
    // and a file that is not in the data will not appear later.
    if (!art.border && art.attempts < 8 && am && window && window->getVkContext()) {
        ++art.attempts;
        art.border     = loadPlateTexture(am, window, "Interface\\Tooltips\\Nameplate-Border.blp", false);
        art.castShield = loadPlateTexture(am, window, "Interface\\Tooltips\\Nameplate-CastBar-Shield.blp", false);
        art.glow       = loadPlateTexture(am, window, "Interface\\Tooltips\\Nameplate-Glow.blp", true);
        art.elite      = loadPlateTexture(am, window, "Interface\\Tooltips\\EliteNameplateIcon.blp", false);
        art.barFill    = loadPlateTexture(am, window, "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp", false);
        art.skull      = loadPlateTexture(am, window, "Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp", false);
        art.flash      = loadPlateTexture(am, window, "Interface\\TargetingFrame\\UI-TargetingFrame-Flash.blp", false);
    }
    return art;
}

/// A plate's FontStrings: no outline, a black shadow 0.001 DDC right and down
/// (0x98f790, SetShadow(0xff000000, {0.001, -0.001})).
void addShadowedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                     int alpha, float shadow, const char* text) {
    dl->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, alpha), text);
    dl->AddText(font, size, pos, col, text);
}

/// The names over heads: UNIT_NAME_FONT with THICKOUTLINE (font flag 4,
/// 0x7e64d0; the flag table at 0xa44050).
void addThickOutlinedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                          int alpha, const char* text) {
    const float t = std::max(1.0f, std::round(size / 14.0f));
    const ImU32 black = IM_COL32(0, 0, 0, alpha);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            dl->AddText(font, size, ImVec2(pos.x + dx * t, pos.y + dy * t), black, text);
        }
    }
    dl->AddText(font, size, pos, col, text);
}

/// The UnitName* CVars as the engine's bit mask (0x7e60e0), read once a frame.
uint32_t unitNameMask() {
    struct Row { const char* cvar; const char* def; uint32_t bit; };
    static constexpr Row kRows[] = {
        {"UnitNameOwn", "0", stockui::kNameOwn},
        {"UnitNameNPC", "0", stockui::kNameNpc},
        {"UnitNamePlayerGuild", "1", stockui::kNamePlayerGuild},
        {"UnitNamePlayerPVPTitle", "1", stockui::kNamePlayerPvpTitle},
        {"UnitNameEnemyPlayerName", "1", stockui::kNameEnemyPlayer},
        {"UnitNameEnemyPetName", "1", stockui::kNameEnemyPet},
        {"UnitNameEnemyGuardianName", "0", stockui::kNameEnemyGuardian},
        {"UnitNameEnemyTotemName", "0", stockui::kNameEnemyTotem},
        {"UnitNameFriendlyPlayerName", "1", stockui::kNameFriendlyPlayer},
        {"UnitNameFriendlyPetName", "1", stockui::kNameFriendlyPet},
        {"UnitNameFriendlyGuardianName", "0", stockui::kNameFriendlyGuardian},
        {"UnitNameFriendlyTotemName", "0", stockui::kNameFriendlyTotem},
        {"UnitNameNonCombatCreatureName", "0", stockui::kNameNonCombatCreature},
    };
    uint32_t mask = 0;
    for (const auto& r : kRows)
        if (addons::storedCVarValue(r.cvar, r.def) != "0") mask |= r.bit;
    return mask;
}

/// Pixels per yard at a point `clipW` in front of the camera.
float pixelsPerYard(const glm::mat4& proj, float clipW, float screenH) {
    return std::abs(proj[1][1]) * 0.5f * screenH / std::max(clipW, 0.01f);
}

/// UNIT_FIELD_BYTES_1 byte 2 bit 0x02: the unit is stealthed ("creep").
bool isStealthed(const game::Entity& e) {
    const uint32_t bytes1 = e.getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1));
    return ((bytes1 >> 16) & 0x02) != 0;
}

constexpr uint32_t kCreatureTypeBeast = 1;
constexpr uint32_t kCreatureTypeCritter = 8;
constexpr uint32_t kCreatureTypeTotem = 11;
constexpr uint32_t kCreatureTypeNonCombatPet = 12;
constexpr uint32_t kTypeFlagNoNamePlate = 0x00100000;  // CREATURE_TYPE_FLAG_NO_NAME_PLATE

/// When each unit last took damage, for the red plate name (0x98e5b0).
std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>& lastDamaged() {
    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m;
    return m;
}

void noteDamageFromCombatText(game::GameHandler& gh) {
    using T = game::CombatTextEntry;
    const auto now = std::chrono::steady_clock::now();
    auto& m = lastDamaged();
    for (const auto& e : gh.getCombatText()) {
        if (e.dstGuid == 0) continue;
        switch (e.type) {
            case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::CRIT_DAMAGE:
            case T::PERIODIC_DAMAGE: case T::ENVIRONMENTAL: case T::GLANCING:
            case T::CRUSHING: {
                const auto at = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<float>(e.age));
                auto it = m.find(e.dstGuid);
                if (it == m.end() || it->second < at) m[e.dstGuid] = at;
                break;
            }
            default: break;
        }
    }
    if (m.size() > 512) {
        for (auto it = m.begin(); it != m.end();) {
            if (now - it->second > std::chrono::seconds(10)) it = m.erase(it);
            else ++it;
        }
    }
}

bool damagedRecently(uint64_t guid) {
    auto& m = lastDamaged();
    auto it = m.find(guid);
    if (it == m.end()) return false;
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count() <
           stockui::kPlateNameDamageSeconds;
}

/// A plate that passed the stock visibility rule this frame.
struct Plate {
    std::shared_ptr<game::Entity> entity;
    game::Unit* unit = nullptr;
    uint64_t guid = 0;
    float px = 0.0f, py = 0.0f;  // top-left, pixels
    float depth = 0.0f;          // clip w: smaller is nearer
    bool isTarget = false;
    bool isPlayer = false;
    int reaction = 4;
    bool hostile = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// The name and selection colour (0x521bf0)
// ---------------------------------------------------------------------------

glm::vec3 stockUnitColor(game::GameHandler& gameHandler, const game::Unit& unit, bool attackRed) {
    auto toVec = [](const Rgb& c) { return glm::vec3(c.r, c.g, c.b) / 255.0f; };
    constexpr uint8_t kPvpFlag = 0x01;        // UNIT_FIELD_BYTES_2 byte 1
    constexpr uint8_t kSanctuaryFlag = 0x08;
    const uint64_t guid = unit.getGuid();
    const int react = gameHandler.unitReactionToPlayer(unit);
    const bool hostile = react <= 2 || gameHandler.isAggressiveTowardPlayer(guid);
    const Rgb reactionRgb = hostile ? Rgb{255, 0, 0} : stockui::reactionNameRgb(react);
    const bool dead = (unit.getHealth() == 0 && unit.getMaxHealth() > 0) ||
                      (unit.getDynamicFlags() & game::UNIT_DYNFLAG_DEAD) != 0;

    // Attacking it, while it can be fought (CGUnit_C +0xA30 bit 0x10).
    if (attackRed && !dead && (hostile || react <= 4)) {
        const auto* me = gameHandler.getUnitByGuid(gameHandler.getPlayerGuid());
        if (me && me->getAttackTarget() == guid) return glm::vec3(1.0f, 0.0f, 0.0f);
    }

    const bool isPlayerType = unit.getType() == game::ObjectType::PLAYER;
    const bool playerControlled =
        isPlayerType || (unit.getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED) != 0;
    if (!playerControlled) {
        return dead ? toVec(Rgb{127, 127, 127}) : toVec(reactionRgb);
    }
    auto pvpByte = [](const game::Entity& e) {
        return static_cast<uint8_t>(
            (e.getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2)) >> 8) & 0xFF);
    };
    const uint8_t targetPvp = pvpByte(unit);
    const auto self = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
    const bool selfSanctuary = self && (pvpByte(*self) & kSanctuaryFlag) != 0;
    const bool flagged = (targetPvp & kPvpFlag) != 0 && (targetPvp & kSanctuaryFlag) == 0 &&
                         !selfSanctuary;
    // Each may attack the other (0x729740 both ways): red. Flagged for PvP,
    // or a duel opponent and what it controls (stock_duel.hpp), whatever the
    // PvP flags.
    if (hostile && (flagged || gameHandler.duelReactionToPlayer(unit).has_value()))
        return glm::vec3(1.0f, 0.0f, 0.0f);
    if (isPlayerType) {
        // Your own party: blue, or green while flagged (0x52d310).
        const auto& party = gameHandler.getPartyData();
        if (!party.isEmpty()) {
            uint8_t mySub = 0;
            for (const auto& m : party.members)
                if (m.guid == gameHandler.getPlayerGuid()) mySub = m.subGroup;
            for (const auto& m : party.members) {
                if (m.guid != guid) continue;
                if (party.groupType == 1 && m.subGroup != mySub) break;
                return toVec(flagged ? Rgb{170, 255, 170} : Rgb{170, 170, 255});
            }
        }
        // A friend (the contact list, 0x6b3510).
        if (gameHandler.friendGuidsRef().count(guid)) return toVec(Rgb{83, 201, 255});
    }
    if (flagged) return toVec(reactionRgb);
    return toVec(Rgb{96, 96, 255});
}

// ---------------------------------------------------------------------------
// Nameplates
// ---------------------------------------------------------------------------

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;

    // Reset mouseover each frame; we'll set it below when the cursor is over a nameplate
    gameHandler.setMouseoverGuid(0);

    auto* appRenderer = services_.renderer;
    if (!appRenderer) return;
    rendering::Camera* camera = appRenderer->getCamera();
    if (!camera) return;
    auto* window = services_.window;
    if (!window) return;
    auto& app = core::Application::getInstance();

    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());
    const glm::mat4 proj = camera->getProjectionMatrix();
    const glm::mat4 viewProj = proj * camera->getViewMatrix();
    const glm::vec3 camPos = camera->getPosition();
    const uint64_t playerGuid = gameHandler.getPlayerGuid();
    const uint64_t targetGuid = gameHandler.getTargetGuid();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    noteDamageFromCombatText(gameHandler);

    static thread_local std::vector<std::shared_ptr<game::Entity>> nearby;
    glm::vec3 playerCanonical(0.0f);
    if (auto player = gameHandler.getEntityManager().getEntity(playerGuid))
        playerCanonical = glm::vec3(player->getX(), player->getY(), player->getZ());
    gameHandler.getEntityManager().getEntitiesNear(playerCanonical.x, playerCanonical.y, 150.0f,
                                                   nearby);

    // One pass over the CVars a frame: each read can reach the settings file.
    const bool allowOverlap = addons::storedCVarValue("nameplateAllowOverlap", "1") != "0";
    const bool classColours = addons::storedCVarValue("ShowClassColorInNameplate", "0") != "0";
    const bool vKeyCastBar = addons::storedCVarValue("showVKeyCastbar", "1") != "0";
    auto cvarOn = [](const char* name, const char* def) {
        return addons::storedCVarValue(name, def) != "0";
    };
    const bool showEnemyPets = cvarOn("nameplateShowEnemyPets", "1");
    const bool showEnemyGuardians = cvarOn("nameplateShowEnemyGuardians", "1");
    const bool showEnemyTotems = cvarOn("nameplateShowEnemyTotems", "1");
    const bool showFriendlyPets = cvarOn("nameplateShowFriendlyPets", "1");
    const bool showFriendlyGuardians = cvarOn("nameplateShowFriendlyGuardians", "1");
    const bool showFriendlyTotems = cvarOn("nameplateShowFriendlyTotems", "1");

    // Plate size: 0.1 x 0.025 DDC, a fraction of the screen diagonal (it is a
    // WorldFrame child, so the interface scale does not touch it).
    const float s = settingsPanel_.nameplateScale_ * stockui::plateUnitPixels(screenW, screenH);
    const float plateW = 128.0f * s;
    const float plateH = 32.0f * s;

    // ---- Which units carry a plate (0x72b060) ----
    std::vector<Plate> plates;
    for (const auto& entityPtr : nearby) {
        if (!entityPtr || !entityPtr->isUnit()) continue;
        const uint64_t guid = entityPtr->getGuid();
        if (guid == playerGuid) continue;
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        if (unit->getMaxHealth() == 0 || unit->getHealth() == 0) continue;
        if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;
        if ((unit->getDynamicFlags() & game::UNIT_DYNFLAG_DEAD) != 0) continue;

        const bool isPlayer = entityPtr->getType() == game::ObjectType::PLAYER;
        const bool isHostile = unit->isHostile();
        const int reaction = isPlayer ? (isHostile ? 2 : 5) : gameHandler.unitReactionToPlayer(*unit);
        // "Friendly" is the one Shift-V owns: a player you cannot fight, a
        // creature you cannot attack. Everything else is V's.
        const bool friendly = !isHostile && reaction >= 5;
        if (friendly && !settingsPanel_.showFriendlyNameplates_) continue;
        if (!friendly && !settingsPanel_.showEnemyNameplates_) continue;
        // A hostile unit in stealth carries none.
        if (!friendly && isStealthed(*entityPtr)) continue;

        if (!isPlayer) {
            const uint32_t ctype = gameHandler.getCreatureType(unit->getEntry());
            if (ctype == kCreatureTypeCritter || ctype == kCreatureTypeNonCombatPet) continue;
            if (gameHandler.getCreatureTypeFlags(unit->getEntry()) & kTypeFlagNoNamePlate) continue;
            // A player's totem, pet or guardian answers to its own setting.
            const uint64_t owner = game::unitSummonedByGuid(*entityPtr);
            auto ownerEnt = owner ? gameHandler.getEntityManager().getEntity(owner) : nullptr;
            if (ownerEnt && ownerEnt->getType() == game::ObjectType::PLAYER) {
                bool shown = true;
                if (ctype == kCreatureTypeTotem) {
                    shown = friendly ? showFriendlyTotems : showEnemyTotems;
                } else if (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED) {
                    shown = friendly ? showFriendlyPets : showEnemyPets;
                } else {
                    shown = friendly ? showFriendlyGuardians : showEnemyGuardians;
                }
                if (!shown) continue;
            }
        }

        // 41 yards from the character, not the camera.
        const glm::vec3 d(unit->getX() - playerCanonical.x, unit->getY() - playerCanonical.y,
                          unit->getZ() - playerCanonical.z);
        if (glm::dot(d, d) >= stockui::kPlateRange * stockui::kPlateRange) continue;

        // The plate's TOP hangs from the name position plus 2/3 yd; it is up
        // only while that point is on the screen (0x715720). No line of sight:
        // a plate shows through walls, as in the stock client.
        glm::vec3 namePos;
        if (!app.getNamePositionForGuid(guid, namePos)) {
            namePos = core::coords::canonicalToRender(glm::vec3(unit->getX(), unit->getY(),
                                                                unit->getZ() + 2.0f));
        }
        namePos.z += stockui::kPlateLiftYards;
        const glm::vec4 clip = viewProj * glm::vec4(namePos, 1.0f);
        if (clip.w <= 0.01f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) continue;
        const float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        const float sy = (ndc.y * 0.5f + 0.5f) * screenH;

        Plate p;
        p.entity = entityPtr;
        p.unit = unit;
        p.guid = guid;
        p.px = sx - plateW * 0.5f;
        p.py = sy;
        p.depth = clip.w;
        p.isTarget = guid == targetGuid;
        p.isPlayer = isPlayer;
        p.reaction = reaction;
        p.hostile = isHostile;
        plates.push_back(std::move(p));
    }

    auto& plated = platedUnits();
    plated.clear();
    for (const auto& p : plates) plated.insert(p.guid);

    // ---- Placement ----
    // Overlap off ("the (old) never overlapping version"): nearest first, each
    // lifted clear of those already placed.
    std::sort(plates.begin(), plates.end(),
              [](const Plate& a, const Plate& b) { return a.depth < b.depth; });
    if (!allowOverlap) {
        static thread_local std::vector<PlateBox> placed;
        placed.clear();
        for (auto& p : plates) {
            p.py = plateTopClearOf(placed, p.px, p.px + plateW, p.py, plateH, 0.0f);
            placed.push_back({.x0 = p.px, .y0 = p.py, .x1 = p.px + plateW, .y1 = p.py + plateH});
        }
    }

    // ---- Mouse: the nearest plate under the cursor ----
    // The mouseover is the nearest plate the cursor is on; the highlight goes
    // to the nearest that is not the target's (0x7271d0).
    const Plate* hoveredPlate = nullptr;
    const Plate* highlightPlate = nullptr;
    if (!ImGui::GetIO().WantCaptureMouse) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (const auto& p : plates) {  // nearest first
            const bool in = mouse.x >= p.px && mouse.x <= p.px + plateW && mouse.y >= p.py &&
                            mouse.y <= p.py + plateH;
            if (!in) continue;
            if (!hoveredPlate) hoveredPlate = &p;
            if (!highlightPlate && !p.isTarget) highlightPlate = &p;
        }
    }

    // ---- Draw: furthest first, the target last (frame level 20 over 10) ----
    std::vector<const Plate*> order;
    order.reserve(plates.size());
    for (const auto& p : plates) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const Plate* a, const Plate* b) {
        if (a->isTarget != b->isTarget) return !a->isTarget;
        return a->depth > b->depth;
    });

    const NameplateArt& art = nameplateArt(services_.assetManager, services_.window);
    ImFont* plateFont = interfaceFaceOrDefault("Fonts\\FRIZQT__.TTF");
    const int playerLevel = static_cast<int>(gameHandler.getPlayerLevel());
    const float shadow = 1.28f * s;  // 0.001 DDC

    for (const Plate* pp : order) {
        const Plate& p = *pp;
        game::Unit* unit = p.unit;
        const uint64_t guid = p.guid;
        auto P = [&](float tx, float ty) { return ImVec2(p.px + tx * s, p.py + ty * s); };
        const int alpha = stockui::nameplateAlpha(targetGuid != 0, p.isTarget);
        const ImU32 white = IM_COL32(255, 255, 255, alpha);
        const bool highlighted = highlightPlate == pp;

        // Threat glow: UI-TargetingFrame-Flash cut to the bar's outline
        // (0..0.555 across, 0.53..0.6 down), 0.11 x 0.029 DDC, TOP at the
        // plate's TOP +(-0.001, -0.0065), coloured by your threat status.
        if (art.flash && p.hostile) {
            game::ThreatStanding st;
            int status = 0;
            if (game::threatStandingOn(gameHandler.getThreatList(guid), playerGuid, st)) {
                status = st.onList ? st.status : 0;
            } else if (game::unitTargetGuid(*p.entity) == playerGuid) {
                status = 3;  // no list from the server: the same guess UnitThreatSituation makes
            }
            if (stockui::nameplateThreatShown(status)) {
                const ImVec2 f0 = P(64.0f - 1.28f - 70.4f, 8.32f);
                const ImVec2 f1 = P(64.0f - 1.28f + 70.4f, 8.32f + 37.12f);
                drawList->AddImage((ImTextureID)(uintptr_t)art.flash, f0, f1, ImVec2(0.0f, 0.53f),
                                   ImVec2(0.555f, 0.6f),
                                   rgbA(stockui::nameplateThreatRgb(status), alpha));
            }
        }

        // Health: UI-TargetingFrame-BarFill tinted by the stock colour, cut at
        // the health fraction. BOTTOMLEFT +(0.031W, 0.125H), 0.804W x 0.281H.
        {
            const float healthPct = std::clamp(
                static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()),
                0.0f, 1.0f);
            const stockui::PlateBar kind = stockui::nameplateBar(p.reaction, p.isPlayer, classColours);
            ImU32 barColor;
            const uint8_t classId = p.isPlayer ? helpers::entityClassId(p.entity.get()) : 0;
            if (kind == stockui::PlateBar::ClassColour && classId != 0) {
                barColor = helpers::classColorU32(classId, alpha);
            } else {
                barColor = rgbA(stockui::nameplateBarRgb(
                                    kind == stockui::PlateBar::ClassColour ? stockui::PlateBar::Red : kind),
                                alpha);
            }
            const ImVec2 b0 = P(3.968f, 19.008f);
            const ImVec2 b1 = P(106.88f, 28.0f);
            const float fillX = b0.x + (b1.x - b0.x) * healthPct;
            if (art.barFill) {
                drawList->AddImage((ImTextureID)(uintptr_t)art.barFill, b0, ImVec2(fillX, b1.y),
                                   ImVec2(0, 0), ImVec2(healthPct, 1), barColor);
            } else {
                drawList->AddRectFilled(b0, ImVec2(fillX, b1.y), barColor);
            }
        }

        // The border, with its level box.
        if (art.border) {
            drawList->AddImage((ImTextureID)(uintptr_t)art.border, P(0, 0), P(128, 32),
                               ImVec2(0, 0), ImVec2(1, 1), white);
        }

        // The mouseover highlight: Nameplate-Glow over the whole plate, ADD.
        if (highlighted && art.glow) {
            drawList->AddImage((ImTextureID)(uintptr_t)art.glow, P(0, 0), P(128, 32),
                               ImVec2(0, 0), ImVec2(1, 1), white);
        }

        // Level, or the skull: CENTER at the plate's BOTTOMRIGHT
        // +(-0.0092, +0.0071) DDC; level text 0.009 DDC, skull 0.01 square.
        const ImVec2 levelC = P(128.0f - 11.776f, 32.0f - 9.088f);
        const int creatureRank = p.isPlayer ? -1 : gameHandler.getCreatureRank(unit->getEntry());
        const int level = static_cast<int>(unit->getLevel());
        const bool skullLevel = creatureRank == 3 || level == 0 ||
                                (p.reaction <= 2 && playerLevel > 0 && level >= playerLevel + 10);
        if (skullLevel) {
            if (art.skull) {
                drawList->AddImage((ImTextureID)(uintptr_t)art.skull,
                                   ImVec2(levelC.x - 6.4f * s, levelC.y - 6.4f * s),
                                   ImVec2(levelC.x + 6.4f * s, levelC.y + 6.4f * s),
                                   ImVec2(0, 0), ImVec2(1, 1), white);
            }
        } else {
            char levelBuf[8];
            snprintf(levelBuf, sizeof(levelBuf), "%d", level);
            const float levelSize = 11.52f * s;
            const ImVec2 lsz = plateFont->CalcTextSizeA(levelSize, FLT_MAX, 0.0f, levelBuf);
            addShadowedText(drawList, plateFont, levelSize,
                            ImVec2(std::round(levelC.x - lsz.x * 0.5f), std::round(levelC.y - lsz.y * 0.5f)),
                            rgbA(stockui::nameplateLevelRgb(level, playerLevel), alpha), alpha,
                            shadow, levelBuf);
        }

        // The elite dragon (elite, rare elite and boss): the icon's 37 x 27
        // corner, 0.0294 x 0.0215 DDC, CENTER at the level's +(0.003, -0.001).
        if ((creatureRank == 1 || creatureRank == 2 || creatureRank == 3) && art.elite) {
            const ImVec2 c(levelC.x + 3.84f * s, levelC.y + 1.28f * s);
            drawList->AddImage((ImTextureID)(uintptr_t)art.elite,
                               ImVec2(c.x - 18.816f * s, c.y - 13.76f * s),
                               ImVec2(c.x + 18.816f * s, c.y + 13.76f * s),
                               ImVec2(0, 0), ImVec2(0.578125f, 0.84375f), white);
        }

        // Cast bar: the target's only, with showVKeyCastbar (0x720e50). The
        // plain border when it can be interrupted, the shielded one when it
        // cannot, the spell's icon in the box at the left. No text.
        if (p.isTarget && vKeyCastBar) {
            const auto* cs = gameHandler.getUnitCastState(guid);
            if (cs && cs->casting && cs->timeTotal > 0.0f) {
                const float elapsed = std::clamp((cs->timeTotal - cs->timeRemaining) / cs->timeTotal,
                                                 0.0f, 1.0f);
                // A channel drains; a cast fills (0x98f040).
                const float castPct = cs->isChannel ? 1.0f - elapsed : elapsed;
                const bool shielded = !cs->interruptible;
                const float barBottom = shielded ? 52.0f : 44.0f;
                const ImVec2 c0 = P(124.0f - 102.912f, barBottom - 8.992f);
                const ImVec2 c1 = P(124.0f, barBottom);
                const ImU32 castColor = IM_COL32(255, 179, 0, alpha);
                if (art.barFill) {
                    drawList->AddImage((ImTextureID)(uintptr_t)art.barFill, c0,
                                       ImVec2(c0.x + (c1.x - c0.x) * castPct, c1.y),
                                       ImVec2(0, 0), ImVec2(castPct, 1), castColor);
                } else {
                    drawList->AddRectFilled(c0, ImVec2(c0.x + (c1.x - c0.x) * castPct, c1.y), castColor);
                }
                if (shielded) {
                    if (art.castShield) {
                        drawList->AddImage((ImTextureID)(uintptr_t)art.castShield, P(0, 32), P(128, 64),
                                           ImVec2(0, 0), ImVec2(1, 1), white);
                    }
                } else if (art.border) {
                    drawList->AddImage((ImTextureID)(uintptr_t)art.border, P(0, 16), P(128, 48),
                                       ImVec2(1, 0), ImVec2(0, 1), white);
                }
                auto* castAm = services_.assetManager;
                const VkDescriptorSet castIcon = (cs->spellId && castAm)
                    ? getSpellIcon(cs->spellId, castAm) : VK_NULL_HANDLE;
                if (castIcon) {
                    const float iconCy = shielded ? 48.0f : 48.0f - 9.088f;
                    drawList->AddImage((ImTextureID)(uintptr_t)castIcon,
                                       P(11.776f - 6.4f, iconCy - 6.4f), P(11.776f + 6.4f, iconCy + 6.4f),
                                       ImVec2(0, 0), ImVec2(1, 1), white);
                }
            }
        }

        // Name: NAMEPLATE_FONT 0.01 DDC, BOTTOM at the plate's CENTER; white,
        // yellow while highlighted, red for 5 s after the unit is hurt.
        const std::string& unitName = unit->getName();
        if (p.isPlayer && unitName.empty()) gameHandler.queryPlayerName(guid);
        if (!unitName.empty()) {
            const float nameSize = 12.8f * s;
            const ImVec2 nsz = plateFont->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, unitName.c_str());
            const ImVec2 anchor = P(64.0f, 16.0f);
            addShadowedText(drawList, plateFont, nameSize,
                            ImVec2(std::round(anchor.x - nsz.x * 0.5f), std::round(anchor.y - nsz.y)),
                            rgbA(stockui::nameplateNameRgb(damagedRecently(guid), highlighted), alpha),
                            alpha, shadow, unitName.c_str());
        }

        // Raid mark: 0.02 DDC square, RIGHT at the border's LEFT.
        const uint8_t raidMark = gameHandler.getEntityRaidMark(guid);
        if (raidMark < game::GameHandler::kRaidMarkCount) {
            if (VkDescriptorSet markTex = ui::getRaidTargetIcon(raidMark, services_.assetManager)) {
                drawList->AddImage((ImTextureID)(uintptr_t)markTex, P(-25.6f, 3.2f), P(0.0f, 28.8f),
                                   ImVec2(0, 0), ImVec2(1, 1), white);
            }
        }
    }

    // Click to target, right-click for the unit menu; hover is mouseover.
    if (hoveredPlate) {
        gameHandler.setMouseoverGuid(hoveredPlate->guid);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gameHandler.setTarget(hoveredPlate->guid);
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            nameplateCtxGuid_ = hoveredPlate->guid;
            nameplateCtxPos_ = ImGui::GetIO().MousePos;
            ImGui::OpenPopup("##NameplateCtx");
        }
    }

    renderUnitNames(gameHandler, nearby, viewProj, proj, camPos, screenW, screenH);

    // Render nameplate context popup (uses a tiny overlay window as host)
    if (nameplateCtxGuid_ != 0) {
        ImGui::SetNextWindowPos(nameplateCtxPos_, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGuiWindowFlags ctxHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##NameplateCtxHost", nullptr, ctxHostFlags)) {
            if (ImGui::BeginPopup("##NameplateCtx")) {
                auto entityPtr = gameHandler.getEntityManager().getEntity(nameplateCtxGuid_);
                std::string ctxName = entityPtr ? game::entityDisplayName(entityPtr) : "";
                if (!ctxName.empty()) {
                    ImGui::TextDisabled("%s", ctxName.c_str());
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Target"))
                    gameHandler.setTarget(nameplateCtxGuid_);
                if (ImGui::MenuItem("Set Focus"))
                    gameHandler.setFocus(nameplateCtxGuid_);
                bool isPlayer = entityPtr && entityPtr->getType() == game::ObjectType::PLAYER;
                if (isPlayer && !ctxName.empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Whisper")) {
                        chatPanel_.setWhisperTarget(ctxName);
                    }
                    if (ImGui::MenuItem("Invite to Group"))
                        gameHandler.inviteToGroup(ctxName);
                    if (ImGui::MenuItem("Trade"))
                        gameHandler.initiateTrade(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Duel"))
                        gameHandler.proposeDuel(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Inspect")) {
                        gameHandler.setTarget(nameplateCtxGuid_);
                        gameHandler.inspectTarget();
                        openInspectWindow(gameHandler);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add Friend"))
                        gameHandler.addFriend(ctxName);
                    if (ImGui::MenuItem("Ignore"))
                        gameHandler.addIgnore(ctxName);
                }
                ImGui::EndPopup();
            } else {
                nameplateCtxGuid_ = 0;
            }
        }
        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// Names over heads and the raid mark over a head (PlayerName.cpp)
// ---------------------------------------------------------------------------

void GameScreen::renderUnitNames(game::GameHandler& gameHandler,
                                 const std::vector<std::shared_ptr<game::Entity>>& nearby,
                                 const glm::mat4& viewProj, const glm::mat4& proj,
                                 const glm::vec3& camPos, float screenW, float screenH) {
    auto& app = core::Application::getInstance();
    const uint64_t playerGuid = gameHandler.getPlayerGuid();
    const uint64_t targetGuid = gameHandler.getTargetGuid();
    const uint32_t mask = unitNameMask();
    const auto& plated = platedUnits();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImFont* nameFont = interfaceFaceOrDefault("Fonts\\FRIZQT__.TTF");

    // Names are 3D text drawn with the depth test (the PlayerNames string
    // batch, 0x6bf160(1,1)): a wall in front hides them. The collision query
    // stands in for the depth buffer here.
    static SightCache sight;
    auto* wmo = services_.renderer ? services_.renderer->getWMORenderer() : nullptr;

    auto consider = [&](const std::shared_ptr<game::Entity>& entityPtr) {
        if (!entityPtr || !entityPtr->isUnit()) return;
        const uint64_t guid = entityPtr->getGuid();
        if (plated.count(guid)) return;  // the plate carries the name and the mark
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        const bool isPlayer = entityPtr->getType() == game::ObjectType::PLAYER;
        const bool isSelf = guid == playerGuid;
        const bool isHostile = unit->isHostile();
        const int reaction = isPlayer ? (isHostile ? 2 : 5) : gameHandler.unitReactionToPlayer(*unit);
        const bool friendly = !isHostile && reaction >= 5;

        // What kind of unit it is, for the UnitName* bits (0x729c70).
        stockui::NameKind kind = stockui::NameKind::Npc;
        std::string ownerName;
        uint32_t ctype = 0;
        if (isSelf) {
            kind = stockui::NameKind::Self;
        } else if (isPlayer) {
            kind = stockui::NameKind::Player;
        } else {
            ctype = gameHandler.getCreatureType(unit->getEntry());
            const uint64_t owner = game::unitSummonedByGuid(*entityPtr);
            auto ownerEnt = owner ? gameHandler.getEntityManager().getEntity(owner) : nullptr;
            const bool ownedByPlayer = ownerEnt && ownerEnt->getType() == game::ObjectType::PLAYER;
            if (ctype == kCreatureTypeCritter || ctype == kCreatureTypeNonCombatPet) {
                kind = stockui::NameKind::NonCombatCreature;
            } else if (ownedByPlayer) {
                ownerName = static_cast<game::Unit*>(ownerEnt.get())->getName();
                if (ctype == kCreatureTypeTotem) kind = stockui::NameKind::PlayerTotem;
                else if (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED)
                    kind = stockui::NameKind::PlayerPet;
                else kind = stockui::NameKind::PlayerGuardian;
            }
        }
        const bool stealthedHostile = !friendly && isStealthed(*entityPtr);
        const bool nameShown = stockui::unitNameShown(kind, guid == targetGuid, friendly,
                                                      stealthedHostile, mask);
        const uint8_t raidMark = gameHandler.getEntityRaidMark(guid);
        const bool markShown = raidMark < game::GameHandler::kRaidMarkCount;
        if (!nameShown && !markShown) return;

        // A cheap look first: posing a model for its attachment is not free,
        // and most of the units in range are off to the side or behind.
        {
            const glm::vec3 feet = core::coords::canonicalToRender(
                glm::vec3(entityPtr->getX(), entityPtr->getY(), entityPtr->getZ() + 1.5f));
            const glm::vec4 c = viewProj * glm::vec4(feet, 1.0f);
            if (c.w <= 0.01f) return;
            const float ax = std::abs(c.x / c.w), ay = std::abs(c.y / c.w);
            if (ax > 1.5f || ay > 2.0f) return;
        }
        glm::vec3 namePos;
        float modelHeight = 0.0f;
        if (!app.getNamePositionForGuid(guid, namePos, &modelHeight)) return;
        const glm::vec4 clip = viewProj * glm::vec4(namePos, 1.0f);
        if (clip.w <= 0.01f) return;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.3f || ndc.x > 1.3f || ndc.y < -1.6f || ndc.y > 1.3f) return;
        const float ppy = pixelsPerYard(proj, clip.w, screenH);
        auto toScreen = [&](const glm::vec3& w, ImVec2& out) {
            const glm::vec4 c = viewProj * glm::vec4(w, 1.0f);
            if (c.w <= 0.01f) return false;
            out = ImVec2((c.x / c.w * 0.5f + 0.5f) * screenW, (c.y / c.w * 0.5f + 0.5f) * screenH);
            return true;
        };

        // The lines: name (with the title), then <Guild> for a player, the
        // creature's <Sub Name>, or its owner's "<Name's Pet>" (0x72d4f0).
        std::vector<std::string> lines;
        float lineYards = stockui::unitNameLineYards(modelHeight);
        if (nameShown) {
            std::string name = unit->getName();
            if (isPlayer && name.empty()) gameHandler.queryPlayerName(guid);
            if (isPlayer && (mask & stockui::kNamePlayerPvpTitle)) {
                const uint32_t titleBit = isSelf
                    ? static_cast<uint32_t>(std::max(0, gameHandler.getChosenTitleBit()))
                    : entityPtr->getField(game::fieldIndex(game::UF::PLAYER_CHOSEN_TITLE));
                if (titleBit != 0 && titleBit != 0xFFFFFFFFu && !name.empty()) {
                    const std::string titled = gameHandler.getFormattedTitle(titleBit);
                    if (!titled.empty() && titled.find(name) != std::string::npos) name = titled;
                }
            }
            if (!name.empty()) lines.push_back(name);
            if (isPlayer && (mask & stockui::kNamePlayerGuild)) {
                const uint32_t guildId = gameHandler.getEntityGuildId(guid);
                if (guildId != 0) {
                    const std::string& guild = gameHandler.lookupGuildName(guildId);
                    if (!guild.empty()) lines.push_back("<" + guild + ">");
                }
            } else if (!isPlayer) {
                const std::string sub = gameHandler.getCachedCreatureSubName(unit->getEntry());
                if (!sub.empty() && ownerName.empty()) lines.push_back("<" + sub + ">");
                if (!ownerName.empty()) {
                    lines.push_back("<" + ownerName +
                                    (ctype == kCreatureTypeBeast ? "'s Pet>" : "'s Minion>"));
                }
            }
        }

        // Hidden behind a wall, as the depth test would have it.
        const bool occluded = nameShown && !lines.empty() &&
                              sight.blocked(wmo, guid, camPos, namePos);

        const float linePx = lineYards * ppy;
        if (nameShown && !lines.empty() && !occluded && linePx >= 2.0f) {
            const glm::vec3 c = stockUnitColor(gameHandler, *unit, true);
            const ImU32 col = IM_COL32(static_cast<int>(c.r * 255.0f), static_cast<int>(c.g * 255.0f),
                                       static_cast<int>(c.b * 255.0f), 255);
            // The first line's bottom is the extra lines' height above the
            // name position; the rest run down from it.
            const size_t extra = lines.size() - 1;
            for (size_t i = 0; i < lines.size(); ++i) {
                const float bottomZ = static_cast<float>(extra - i) * lineYards;
                ImVec2 at;
                if (!toScreen(namePos + glm::vec3(0.0f, 0.0f, bottomZ), at)) continue;
                const ImVec2 sz = nameFont->CalcTextSizeA(linePx, FLT_MAX, 0.0f, lines[i].c_str());
                addThickOutlinedText(drawList, nameFont, linePx,
                                     ImVec2(std::round(at.x - sz.x * 0.5f), std::round(at.y - sz.y)),
                                     col, 255, lines[i].c_str());
            }
        }

        // The raid mark: a 1 yd square over the name (or over the head with
        // no name), no depth test, faded near the camera (0x7e5640).
        if (markShown) {
            if (VkDescriptorSet markTex = ui::getRaidTargetIcon(raidMark, services_.assetManager)) {
                float baseZ = 0.0f;
                if (nameShown && !lines.empty())
                    baseZ = static_cast<float>(lines.size()) * lineYards;
                const glm::vec3 bottom = namePos + glm::vec3(0.0f, 0.0f, baseZ);
                ImVec2 b;
                if (toScreen(bottom, b)) {
                    const float sizePx = stockui::kHeadMarkYards * ppy;
                    const uint8_t a = stockui::headMarkAlpha(glm::length(bottom - camPos));
                    drawList->AddImage((ImTextureID)(uintptr_t)markTex,
                                       ImVec2(b.x - sizePx * 0.5f, b.y - sizePx), ImVec2(b.x + sizePx * 0.5f, b.y),
                                       ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
                }
            }
        }
    };

    for (const auto& e : nearby) {
        if (e && e->getGuid() != playerGuid) consider(e);
    }
    consider(gameHandler.getEntityManager().getEntity(playerGuid));
}

}  // namespace ui
}  // namespace wowee
