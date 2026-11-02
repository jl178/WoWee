#include "game/group_defines.hpp"

#include <set>
#include "ui/game_screen.hpp"
#include "ui/settings_schema.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/ui_raid_icons.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/minimap_projection.hpp"
#include "game/quest_giver_status.hpp"
#include "game/update_field_table.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/weather.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"

#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>

#include <unordered_set>
#include "core/local_time.hpp"

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;

    /// How close the cursor has to be to a minimap blip to be pointing at it.
    ///
    /// The blips are two to four pixels across, so the target is deliberately
    /// larger than what is drawn - otherwise a marker is nearly impossible to
    /// hover on a map this small.
    constexpr float kBlipHoverRadius = 8.0f;
    constexpr float kSmallBlipHoverRadius = 7.0f;
    /// The off-map direction arrows are bigger than a blip, so their target is too.
    constexpr float kArrowHoverRadius = 10.0f;

    /// Whether the cursor is inside the minimap disc itself.
    ///
    /// Asked three times - for the wheel, for a ctrl+click ping, and for the
    /// hover readout - and each spelled the circle test out again.
    inline bool cursorOverMinimap(float centerX, float centerY, float mapRadius) {
        const ImVec2 cursor = ImGui::GetMousePos();
        const float dx = cursor.x - centerX;
        const float dy = cursor.y - centerY;
        return dx * dx + dy * dy <= mapRadius * mapRadius;
    }

    /// Whether the cursor is within `radius` pixels of a blip at (sx, sy).
    ///
    /// Written out fourteen times as a squared distance against a literal: 64
    /// is eight pixels and 49 is seven, and neither number said so.
    inline bool cursorNearBlip(float sx, float sy, float radius = kBlipHoverRadius) {
        const ImVec2 cursor = ImGui::GetMousePos();
        const float dx = cursor.x - sx;
        const float dy = cursor.y - sy;
        return dx * dx + dy * dy <= radius * radius;
    }

    // ---- The stock minimap art ---------------------------------------------
    //
    // What the 3.3.5a C client draws on the minimap, from the files it draws
    // them from, instead of the ImGui circles and triangles that stood in for
    // them. Sizes are interface units on the stock 140-unit Minimap frame and
    // scale with the ring, so they keep their proportion to the map at any UI
    // scale and on this client's own 200-pixel ring alike.
    constexpr const char* kObjectIconsPath    = "Interface\\Minimap\\ObjectIcons.blp";
    constexpr const char* kPartyRaidBlipsPath = "Interface\\Minimap\\PartyRaidBlips.blp";
    constexpr const char* kPlayerArrowPath    = "Interface\\Minimap\\MinimapArrow.blp";
    constexpr const char* kGroupArrowPath     = "Interface\\Minimap\\Rotating-MinimapGroupArrow.blp";
    constexpr const char* kCorpseArrowPath    = "Interface\\Minimap\\Rotating-MinimapCorpseArrow.blp";
    constexpr const char* kPoiIconsPath       = "Interface\\Minimap\\POIIcons.blp";
    constexpr const char* kFlightMasterPath   = "Interface\\Minimap\\Tracking\\FlightMaster.blp";

    constexpr float kStockMinimapSize = 140.0f;  // Minimap.xml: <AbsDimension x="140" y="140"/>
    constexpr float kBlipSize = 16.0f;           // one ObjectIcons / PartyRaidBlips cell
    constexpr float kPlayerArrowSize = 40.0f;    // Minimap.lua: SetPlayerTextureWidth/Height(40)
    constexpr float kEdgeArrowSize = 32.0f;      // the rotating rim arrows
    constexpr float kPoiIconSize = 16.0f;        // WorldMap_GetPOITextureCoords' worldMapIconDimension

    /// Cells of ObjectIcons.blp: 256x64, eight 32x32 cells to a row.
    ///
    /// Row one is the round blips: silver-rimmed party (blue), raid (lilac),
    /// hostile (red), neutral / tracked resource (gold) and friendly (green),
    /// then gold-rimmed red, gold and green. Row two is the quest marks -
    /// yellow ! and ?, blue ! and ? for repeatable quests, a green ! - and a
    /// last lilac blip.
    enum ObjectIcon : int {
        kIconHostile = 2,
        kIconNeutral = 3,       // also the tracked herb, ore or treasure blip
        kIconFriendly = 4,
        // The gold-rimmed cells carry this client's own optional markers, which
        // have no stock blip of their own: rares and quest objectives.
        kIconRareElite = 5,
        kIconRare = 6,
        kIconQuestTarget = 8,
        kIconQuestAvailable = 9,
        kIconQuestComplete = 10,
        kIconRepeatableAvailable = 11,
        kIconRepeatableComplete = 12,
    };
    constexpr int kObjectIconCols = 8, kObjectIconRows = 2;
    /// PartyRaidBlips.blp: 256x128, 8x4 cells, cell = classId - 1 in the top
    /// two rows (filled - your own party), and the same plus 16 in the bottom
    /// two (ringed - the rest of the raid).
    constexpr int kPartyBlipCols = 8, kPartyBlipRows = 4;

    /// A minimap texture, uploaded once. A file that is not there is not asked
    /// for again every frame; a failed upload (no context yet) is.
    VkDescriptorSet minimapArt(const char* path, wowee::pipeline::AssetManager* am,
                               wowee::core::Window* window) {
        struct Slot { VkDescriptorSet ds = VK_NULL_HANDLE; bool missing = false; };
        static std::unordered_map<std::string, Slot> cache;
        Slot& slot = cache[path];
        if (slot.ds || slot.missing || !am) return slot.ds;
        wowee::ui::UiTextureLoad why = wowee::ui::UiTextureLoad::Ok;
        slot.ds = wowee::ui::uploadUiTextureFromBlp(am, path, window, &why);
        if (!slot.ds && (why == wowee::ui::UiTextureLoad::NotFound ||
                         why == wowee::ui::UiTextureLoad::DecodeFailed)) {
            slot.missing = true;
            LOG_WARNING("Minimap art missing: ", path);
        }
        return slot.ds;
    }

    inline ImTextureID texId(VkDescriptorSet ds) {
        return (ImTextureID)(uintptr_t)ds;
    }

    /// One cell of an atlas, centred on (cx, cy).
    void drawAtlasCell(ImDrawList* dl, VkDescriptorSet tex, int cell, int cols, int rows,
                       float cx, float cy, float size, ImU32 tint = IM_COL32_WHITE) {
        if (!tex) return;
        const float du = 1.0f / static_cast<float>(cols);
        const float dv = 1.0f / static_cast<float>(rows);
        const float u0 = static_cast<float>(cell % cols) * du;
        const float v0 = static_cast<float>(cell / cols) * dv;
        const float h = size * 0.5f;
        dl->AddImage(texId(tex), ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h),
                     ImVec2(u0, v0), ImVec2(u0 + du, v0 + dv), tint);
    }

    /// A whole texture whose art points up, turned so up lands on (dirX, dirY)
    /// in screen space (a unit vector, +y down).
    void drawRotated(ImDrawList* dl, VkDescriptorSet tex, float cx, float cy, float size,
                     float dirX, float dirY) {
        if (!tex) return;
        const float c = -dirY;
        const float s = dirX;
        const float h = size * 0.5f;
        auto corner = [&](float lx, float ly) {
            return ImVec2(cx + lx * c - ly * s, cy + lx * s + ly * c);
        };
        dl->AddImageQuad(texId(tex), corner(-h, -h), corner(h, -h), corner(h, h), corner(-h, h),
                         ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f),
                         ImVec2(0.0f, 1.0f));
    }

    /// Where a direction from the centre meets the rim - the circle, or the
    /// box when the map is square.
    ImVec2 minimapRimPoint(float cx, float cy, float radius, float nx, float ny, bool square) {
        float r = radius;
        if (square) {
            const float m = std::max(std::fabs(nx), std::fabs(ny));
            if (m > 1e-4f) r = radius / m;
        }
        return ImVec2(cx + nx * r, cy + ny * r);
    }

    /// Lock.dbc id -> the LockType bits (1 << (type - 1)) that open it through
    /// a skill: 2 Herbalism, 3 Mining, 1 Lockpicking, 6/... treasure and so on.
    /// This is what PLAYER_TRACK_RESOURCES is matched against - Find Herbs sets
    /// bit 1, Find Minerals bit 2, Find Treasure its own.
    uint32_t lockTrackMask(wowee::pipeline::AssetManager* am, uint32_t lockId) {
        static std::unordered_map<uint32_t, uint32_t> masks;
        static bool built = false;
        if (!built && am) {
            built = true;
            auto lockDbc = am->loadDBC("Lock.dbc");
            if (lockDbc && lockDbc->isLoaded() && lockDbc->getFieldCount() >= 17) {
                for (uint32_t row = 0; row < lockDbc->getRecordCount(); ++row) {
                    uint32_t mask = 0;
                    for (uint32_t slot = 0; slot < 8; ++slot) {
                        constexpr uint32_t kLockKeySkill = 2;
                        if (lockDbc->getUInt32(row, 1 + slot) != kLockKeySkill) continue;
                        const uint32_t type = lockDbc->getUInt32(row, 9 + slot);
                        if (type >= 1 && type <= 32) mask |= 1u << (type - 1);
                    }
                    if (mask) masks[lockDbc->getUInt32(row, 0)] = mask;
                }
            }
        }
        auto it = masks.find(lockId);
        return it == masks.end() ? 0u : it->second;
    }

}

namespace wowee { namespace ui {

void GameScreen::refreshQuestObjectiveCache(game::GameHandler& gameHandler) {
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&](uint64_t value) {
        signature ^= value;
        signature *= 1099511628211ull;
    };
    const auto& tracked = gameHandler.getTrackedQuestIds();
    mix(tracked.size());
    for (const auto& quest : gameHandler.getQuestLog()) {
        if (quest.complete || quest.questId == 0 ||
            !tracked.count(quest.questId)) continue;
        mix(quest.questId);
        for (const auto& objective : quest.killObjectives) {
            if (objective.required == 0) continue;
            const uint32_t entry = static_cast<uint32_t>(objective.npcOrGoId > 0
                ? objective.npcOrGoId : -objective.npcOrGoId);
            auto count = quest.killCounts.find(entry);
            mix(static_cast<uint32_t>(objective.npcOrGoId));
            mix(objective.required);
            mix(count == quest.killCounts.end() ? 0 : count->second.first);
        }
    }
    if (signature == minimapQuestCacheSignature_) return;

    minimapQuestCacheSignature_ = signature;
    minimapQuestCreatureEntries_.clear();
    minimapQuestGameObjectEntries_.clear();
    for (const auto& quest : gameHandler.getQuestLog()) {
        if (quest.complete || quest.questId == 0 ||
            !tracked.count(quest.questId)) continue;
        for (const auto& objective : quest.killObjectives) {
            if (objective.required == 0 || objective.npcOrGoId == 0) continue;
            const uint32_t entry = static_cast<uint32_t>(objective.npcOrGoId > 0
                ? objective.npcOrGoId : -objective.npcOrGoId);
            auto count = quest.killCounts.find(entry);
            if (count != quest.killCounts.end() && count->second.first >= count->second.second) continue;
            if (objective.npcOrGoId > 0) minimapQuestCreatureEntries_.insert(entry);
            else minimapQuestGameObjectEntries_.insert(entry);
        }
    }
}

// The furniture around the minimap, as opposed to the marks on it.
//
// The mute button, the friends button, the zoom buttons, the clock, and the
// stack of indicators below - new mail, unspent talent points, a battleground
// queue, the Dungeon Finder, calendar invites, a taxi flight, latency and low
// durability.
//
// Split from the marker pass because the two answer the ownership question
// differently. When FrameXML draws the minimap the cluster brings its own zoom
// buttons, clock and mail icons, and this client's would sit on top of them -
// so none of this runs. The blips are the opposite case and the reason the
// marker pass runs at all when the element is owned: minimap.xml declares no
// frame for a party member, a flight master or a corpse, because in WoW those
// come from the C client. Gating the whole thing left the ring drawn and
// nothing on it.
bool GameScreen::MinimapFrame::project(const glm::vec3& worldRenderPos,
                                       float& sx, float& sy) const {
    const float dx = worldRenderPos.x - playerRender.x;
    const float dy = worldRenderPos.y - playerRender.y;
    const glm::vec2 off = renderDeltaToMinimapOffset(dx, dy, view);
    if (std::sqrt(off.x * off.x + off.y * off.y) > mapRadius - 3.0f) return false;
    sx = centerX + off.x;
    sy = centerY + off.y;
    return true;
}

bool GameScreen::MinimapFrame::projectEntity(const game::Entity& entity,
                                             float& sx, float& sy) const {
    return project(core::coords::canonicalToRender(
                       glm::vec3(entity.getX(), entity.getY(), entity.getZ())),
                   sx, sy);
}

bool GameScreen::MinimapFrame::projectCanonical(float wowX, float wowY,
                                                float& sx, float& sy) const {
    return project(core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f)), sx, sy);
}

// The three buttons around the ring: mute at the top right, friends at the top
// left, zoom at the bottom.
//
// All three are the client's own. FrameXML's cluster brings its own and this
// whole pass is skipped when it owns the minimap.
void GameScreen::renderMinimapButtons(game::GameHandler& gameHandler, float centerX, float centerY, float mapRadius) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    // The same work the settings panel does when a volume changes. It used to be
    // a second copy of applyAudioVolumes with every manager listed again, which
    // was two places to remember when a manager was added.
    auto applyMuteState = [this]() {
        settingsPanel_.applyAudioVolumes(services_.audioCoordinator);
    };
    // Speaker mute button at the minimap top-right corner
    ImGui::SetNextWindowPos(ImVec2(centerX + mapRadius - 26.0f, centerY - mapRadius + 4.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(22.0f, 22.0f), ImGuiCond_Always);
    ImGuiWindowFlags muteFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##MinimapMute", nullptr, muteFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 size(20.0f, 20.0f);
        if (ImGui::InvisibleButton("##MinimapMuteButton", size)) {
            settingsPanel_.soundMuted_ = !settingsPanel_.soundMuted_;
            if (settingsPanel_.soundMuted_) {
                settingsPanel_.preMuteVolume_ = audio::AudioEngine::instance().getMasterVolume();
            }
            applyMuteState();
            saveSettings();
        }
        bool hovered = ImGui::IsItemHovered();
        ImU32 bg = settingsPanel_.soundMuted_ ? IM_COL32(135, 42, 42, 230) : IM_COL32(38, 38, 38, 210);
        if (hovered) bg = settingsPanel_.soundMuted_ ? IM_COL32(160, 58, 58, 230) : IM_COL32(65, 65, 65, 220);
        ImU32 fg = IM_COL32(255, 255, 255, 245);
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 4.0f);
        draw->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(p.x + size.x - 0.5f, p.y + size.y - 0.5f),
                      IM_COL32(255, 255, 255, 42), 4.0f);
        draw->AddRectFilled(ImVec2(p.x + 4.0f, p.y + 8.0f), ImVec2(p.x + 7.0f, p.y + 12.0f), fg, 1.0f);
        draw->AddTriangleFilled(ImVec2(p.x + 7.0f, p.y + 7.0f),
                                ImVec2(p.x + 7.0f, p.y + 13.0f),
                                ImVec2(p.x + 11.8f, p.y + 10.0f), fg);
        if (settingsPanel_.soundMuted_) {
            draw->AddLine(ImVec2(p.x + 13.5f, p.y + 6.2f), ImVec2(p.x + 17.2f, p.y + 13.8f), fg, 1.8f);
            draw->AddLine(ImVec2(p.x + 17.2f, p.y + 6.2f), ImVec2(p.x + 13.5f, p.y + 13.8f), fg, 1.8f);
        } else {
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 3.6f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.4f);
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 5.5f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.2f);
        }
        if (hovered) ImGui::SetTooltip(settingsPanel_.soundMuted_ ? "Unmute" : "Mute");
    }
    ImGui::End();

    // Friends button at top-left of minimap
    {
        const auto& contacts = gameHandler.getContacts();
        int onlineCount = 0;
        for (const auto& c : contacts)
            if (c.isFriend() && c.isOnline()) ++onlineCount;

        ImGui::SetNextWindowPos(ImVec2(centerX - mapRadius + 4.0f, centerY - mapRadius + 4.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(22.0f, 22.0f), ImGuiCond_Always);
        ImGuiWindowFlags friendsBtnFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##MinimapFriendsBtn", nullptr, friendsBtnFlags)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 sz(20.0f, 20.0f);
            if (ImGui::InvisibleButton("##FriendsBtnInv", sz)) {
                gameHandler.runInterfaceCommand("ToggleFriendsFrame(1)");
            }
            // No lit state any more: FriendsFrame is FrameXML's and nothing
            // here can ask whether it is up. The button still opens it, and
            // still answers the pointer.
            bool hovered = ImGui::IsItemHovered();
            ImU32 bg = hovered ? IM_COL32(65, 65, 65, 220) : IM_COL32(38, 38, 38, 210);
            draw->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 4.0f);
            draw->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f),
                          ImVec2(p.x + sz.x - 0.5f, p.y + sz.y - 0.5f),
                          IM_COL32(255, 255, 255, 42), 4.0f);
            // Simple smiley-face dots as "social" icon
            ImU32 fg = IM_COL32(255, 255, 255, 245);
            draw->AddCircle(ImVec2(p.x + 10.0f, p.y + 10.0f), 6.5f, fg, 16, 1.2f);
            draw->AddCircleFilled(ImVec2(p.x + 7.5f, p.y + 8.0f), 1.2f, fg);
            draw->AddCircleFilled(ImVec2(p.x + 12.5f, p.y + 8.0f), 1.2f, fg);
            draw->PathArcTo(ImVec2(p.x + 10.0f, p.y + 11.5f), 3.0f, 0.2f, 2.9f, 8);
            draw->PathStroke(fg, 0, 1.2f);
            // Small green dot if friends online
            if (onlineCount > 0) {
                draw->AddCircleFilled(ImVec2(p.x + sz.x - 3.5f, p.y + 3.5f),
                                      3.5f, IM_COL32(50, 220, 50, 255));
            }
            if (hovered) {
                if (onlineCount > 0)
                    ImGui::SetTooltip("Friends (%d online)", onlineCount);
                else
                    ImGui::SetTooltip("Friends");
            }
        }
        ImGui::End();
    }

    // Zoom buttons at the bottom edge of the minimap
    ImGui::SetNextWindowPos(ImVec2(centerX - 22, centerY + mapRadius - 30), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(44, 24), ImGuiCond_Always);
    ImGuiWindowFlags zoomFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##MinimapZoom", nullptr, zoomFlags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
        if (ImGui::SmallButton("-")) {
            if (minimap) minimap->zoomOut();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            if (minimap) minimap->zoomIn();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::End();


}

// The clock at the bottom right of the ring, when it is wanted. Local time,
// which is what the setting means by it.
void GameScreen::renderMinimapClock(float centerX, float centerY, float mapRadius) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    // Optional clock display at bottom-right of minimap (local time).
    if (settingsPanel_.showMinimapClock_) {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
        tmBuf = core::localTime(tt);
        char clockText[16];
        std::snprintf(clockText, sizeof(clockText), "%d:%02d %s",
                      (tmBuf.tm_hour % 12 == 0) ? 12 : tmBuf.tm_hour % 12,
                      tmBuf.tm_min,
                      tmBuf.tm_hour >= 12 ? "PM" : "AM");
        ImVec2 clockSz = ImGui::CalcTextSize(clockText);
        float clockW = clockSz.x + 10.0f;
        float clockH = clockSz.y + 6.0f;
        ImGui::SetNextWindowPos(ImVec2(centerX + mapRadius - clockW - 2.0f,
                                       centerY + mapRadius - clockH - 2.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(clockW, clockH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGuiWindowFlags clockFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##MinimapClock", nullptr, clockFlags)) {
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.8f, 0.85f), "%s", clockText);
        }
        ImGui::End();
    }

}

// The stack under the minimap: new mail, unspent talent points, a battleground
// queue, the Dungeon Finder, calendar invites, a flight, latency and low
// durability.
//
// They share one running Y so that whichever of them apply stack without gaps -
// that shared cursor is why they are one function rather than eight.
void GameScreen::renderMinimapIndicators(game::GameHandler& gameHandler, float centerX, float centerY, float mapRadius) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    // Indicators below the minimap (stacked: new mail, then BG queue, then latency)

    float indicatorX = centerX - mapRadius;
    float nextIndicatorY = centerY + mapRadius + 4.0f;
    const float indicatorW = mapRadius * 2.0f;
    constexpr float kIndicatorH = 22.0f;
    ImGuiWindowFlags indicatorFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;

    // "New Mail" indicator
    if (gameHandler.hasNewMail()) {
        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##NewMailIndicator", nullptr, indicatorFlags)) {
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, pulse), "New Mail!");
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
    }

    // Unspent talent points indicator
    {
        uint8_t unspent = gameHandler.getUnspentTalentPoints();
        if (unspent > 0) {
            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##TalentIndicator", nullptr, indicatorFlags)) {
                float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.5f);
                char talentBuf[40];
                snprintf(talentBuf, sizeof(talentBuf), "! %u Talent Point%s Available",
                         static_cast<unsigned>(unspent), unspent == 1 ? "" : "s");
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f * pulse, pulse), "%s", talentBuf);
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

    // BG queue status indicator (when in queue but not yet invited)
    for (const auto& slot : gameHandler.getBgQueues()) {
        if (slot.statusId != 1) continue;  // STATUS_WAIT_QUEUE only

        std::string bgName;
        if (slot.arenaType > 0) {
            bgName = std::to_string(slot.arenaType) + "v" + std::to_string(slot.arenaType) + " Arena";
        } else {
            switch (slot.bgTypeId) {
                case 1: bgName = "AV"; break;
                case 2: bgName = "WSG"; break;
                case 3: bgName = "AB"; break;
                case 7: bgName = "EotS"; break;
                case 9: bgName = "SotA"; break;
                case 11: bgName = "IoC"; break;
                default: bgName = "BG"; break;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##BgQueueIndicator", nullptr, indicatorFlags)) {
            float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.5f);
            if (slot.avgWaitTimeSec > 0) {
                int avgMin = static_cast<int>(slot.avgWaitTimeSec) / 60;
                int avgSec = static_cast<int>(slot.avgWaitTimeSec) % 60;
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, pulse),
                    "Queue: %s (~%d:%02d)", bgName.c_str(), avgMin, avgSec);
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, pulse),
                    "In Queue: %s", bgName.c_str());
            }
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
        break;  // Show at most one queue slot indicator
    }

    // LFG queue indicator - shown when Dungeon Finder queue is active (Queued or RoleCheck)
    {
        using LfgState = game::GameHandler::LfgState;
        LfgState lfgSt = gameHandler.getLfgState();
        if (lfgSt == LfgState::Queued || lfgSt == LfgState::RoleCheck) {
            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##LfgQueueIndicator", nullptr, indicatorFlags)) {
                if (lfgSt == LfgState::RoleCheck) {
                    float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, pulse), "LFG: Role Check...");
                } else {
                    const uint32_t queued = gameHandler.getLfgQueuedSeconds();
                    const int qMin = static_cast<int>(queued / 60);
                    const int qSec = static_cast<int>(queued % 60);
                    float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.2f);
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, pulse),
                        "LFG: %d:%02d", qMin, qSec);
                }
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

    // Calendar pending invites indicator (WotLK only)
    {
        auto* expReg = services_.expansionRegistry;
        bool isWotLK = expReg && expReg->getActive() && expReg->getActive()->id == "wotlk";
        if (isWotLK) {
            uint32_t calPending = gameHandler.getCalendarPendingInvites();
            if (calPending > 0) {
                ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
                if (ImGui::Begin("##CalendarIndicator", nullptr, indicatorFlags)) {
                    float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f);
                    char calBuf[48];
                    snprintf(calBuf, sizeof(calBuf), "Calendar: %u Invite%s",
                             calPending, calPending == 1 ? "" : "s");
                    ImGui::TextColored(ImVec4(0.6f, 0.5f, 1.0f, pulse), "%s", calBuf);
                }
                ImGui::End();
                nextIndicatorY += kIndicatorH;
            }
        }
    }

    // Taxi flight indicator - shown while on a flight path
    if (gameHandler.isOnTaxiFlight()) {
        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##TaxiIndicator", nullptr, indicatorFlags)) {
            const std::string& dest = gameHandler.getTaxiDestName();
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.0f);
            if (dest.empty()) {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, pulse), "\xe2\x9c\x88 In Flight");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "\xe2\x9c\x88 \xe2\x86\x92 %s", dest.c_str());
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, pulse), "%s", buf);
            }
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
    }

    // Latency + FPS indicator - centered at top of screen
    uint32_t latMs = gameHandler.getLatencyMs();
    if (settingsPanel_.showLatencyMeter_ && gameHandler.getState() == game::WorldState::IN_WORLD) {
        float currentFps = ImGui::GetIO().Framerate;
        ImVec4 latColor;
        if      (latMs < 100) latColor = ImVec4(0.3f, 1.0f, 0.3f, 0.9f);
        else if (latMs < 250) latColor = ImVec4(1.0f, 1.0f, 0.3f, 0.9f);
        else if (latMs < 500) latColor = ImVec4(1.0f, 0.6f, 0.1f, 0.9f);
        else                  latColor = ImVec4(1.0f, 0.2f, 0.2f, 0.9f);

        ImVec4 fpsColor;
        if      (currentFps >= 60.0f) fpsColor = ImVec4(0.3f, 1.0f, 0.3f, 0.9f);
        else if (currentFps >= 30.0f) fpsColor = ImVec4(1.0f, 1.0f, 0.3f, 0.9f);
        else                          fpsColor = ImVec4(1.0f, 0.3f, 0.3f, 0.9f);

        char infoText[64];
        if (latMs > 0)
            snprintf(infoText, sizeof(infoText), "%.0f fps  |  %u ms", currentFps, latMs);
        else
            snprintf(infoText, sizeof(infoText), "%.0f fps", currentFps);

        ImVec2 textSize = ImGui::CalcTextSize(infoText);
        float latW = textSize.x + 16.0f;
        float latH = textSize.y + 8.0f;
        ImGuiIO& lio = ImGui::GetIO();
        float latX = (lio.DisplaySize.x - latW) * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(latX, 4.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(latW, latH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##LatencyIndicator", nullptr, indicatorFlags)) {
            // Color the FPS and latency portions differently
            ImGui::TextColored(fpsColor, "%.0f fps", currentFps);
            if (latMs > 0) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.7f), "|");
                ImGui::SameLine(0, 4);
                ImGui::TextColored(latColor, "%u ms", latMs);
            }
        }
        ImGui::End();
    }

    // Low durability warning - shown when any equipped item has < 20% durability
    if (gameHandler.getState() == game::WorldState::IN_WORLD) {
        const auto& inv = gameHandler.getInventory();
        float lowestDurPct = 1.0f;
        for (int i = 0; i < game::Inventory::NUM_EQUIP_SLOTS; ++i) {
            const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(i));
            if (slot.empty()) continue;
            const auto& it = slot.item;
            if (it.maxDurability > 0) {
                float pct = static_cast<float>(it.curDurability) / static_cast<float>(it.maxDurability);
                if (pct < lowestDurPct) lowestDurPct = pct;
            }
        }
        if (lowestDurPct < 0.20f) {
            bool critical = (lowestDurPct < 0.05f);
            float pulse = critical
                ? (0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f))
                : 1.0f;
            ImVec4 durWarnColor = critical
                ? ImVec4(1.0f, 0.2f, 0.2f, pulse)
                : ImVec4(1.0f, 0.65f, 0.1f, 0.9f);
            const char* durWarnText = critical ? "Item breaking!" : "Low durability";

            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##DurabilityIndicator", nullptr, indicatorFlags)) {
                ImGui::TextColored(durWarnColor, "%s", durWarnText);
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

}

void GameScreen::renderMinimapChrome(game::GameHandler& gameHandler, float centerX,
                                     float centerY, float mapRadius) {
    // The caller already has both; taking the handler by reference rather than
    // dereferencing services_ keeps this from being the one place that assumes
    // it is there.
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    renderMinimapButtons(gameHandler, centerX, centerY, mapRadius);
    renderMinimapClock(centerX, centerY, mapRadius);
    renderMinimapIndicators(gameHandler, centerX, centerY, mapRadius);
}

namespace {

/// What the player is tracking, as the server publishes it.
///
/// PLAYER_TRACK_CREATURES and PLAYER_TRACK_RESOURCES are the two fields just
/// before PLAYER_BLOCK_PERCENTAGE in every layout from 1.12 to 3.3.5a (1022
/// and 1023 in WotLK), so they are found from that mapped field rather than
/// added to every expansion's table. The server writes 1 << (misc - 1) from
/// the running SPELL_AURA_TRACK_CREATURES / TRACK_RESOURCES aura: a
/// CreatureType bit (Beasts 1, Humanoids 7, ...) and a LockType bit (Herbalism
/// 2, Mining 3, ...).
struct MinimapTracking {
    uint32_t creatures = 0;
    uint32_t resources = 0;
};

MinimapTracking playerTracking(game::GameHandler& gameHandler) {
    MinimapTracking t;
    const uint16_t block = game::fieldIndex(game::UF::PLAYER_BLOCK_PERCENTAGE);
    if (block == 0xFFFF || block < 2) return t;
    auto self = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
    if (!self) return t;
    t.creatures = self->getField(static_cast<uint16_t>(block - 2));
    t.resources = self->getField(static_cast<uint16_t>(block - 1));
    return t;
}

/// Whether a CreatureType (1-based, as creature_template has it) is tracked.
bool creatureTypeTracked(uint32_t mask, uint32_t creatureType) {
    return creatureType >= 1 && creatureType <= 32 && (mask & (1u << (creatureType - 1))) != 0;
}

constexpr uint32_t kCreatureTypeHumanoid = 7;

/// The blip for a unit by how it regards the player: red hostile, gold
/// neutral, green friendly - the three silver-rimmed ObjectIcons cells.
int reactionBlip(game::GameHandler& gameHandler, const game::Unit& unit) {
    if (unit.isHostile()) return kIconHostile;
    const int reaction = gameHandler.unitReactionToPlayer(unit);
    if (reaction <= 3) return kIconHostile;
    if (reaction == 4) return kIconNeutral;
    return kIconFriendly;
}

/// Interface units to pixels for this ring.
inline float minimapArtScale(float mapRadius) {
    return (mapRadius * 2.0f) / kStockMinimapSize;
}

}  // namespace

// Tracked creatures - what Track Beasts, Track Humanoids, Sense Undead and the
// rest of SPELL_AURA_TRACK_CREATURES put on the map - as the stock
// ObjectIcons blips coloured by reaction. With this client's optional "nearby
// creature dots" on, every live unit gets one. Quest objectives are drawn by
// renderMinimapQuestKills.
void GameScreen::renderMinimapNpcDots(const MinimapFrame& frame, const EntityList& minimapUnits,
                                      const EntrySet& minimapQuestEntries) {
    auto* gh = services_.gameHandler;
    if (!gh) return;
    const MinimapTracking tracking = playerTracking(*gh);
    const bool allDots = settingsPanel_.minimapNpcDots_;
    if (!allDots && tracking.creatures == 0) return;
    VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
    if (!icons) return;
    const float size = kBlipSize * minimapArtScale(frame.mapRadius);

    for (const auto& entity : minimapUnits) {
        auto unit = std::static_pointer_cast<game::Unit>(entity);
        if (!unit || unit->getHealth() == 0) continue;
        if (allDots && minimapQuestEntries.count(unit->getEntry()) != 0) continue;
        if (!allDots &&
            !creatureTypeTracked(tracking.creatures, gh->getCreatureType(unit->getEntry()))) {
            continue;
        }

        float sx = 0.0f, sy = 0.0f;
        if (!frame.projectEntity(*entity, sx, sy)) continue;
        drawAtlasCell(frame.drawList, icons, reactionBlip(*gh, *unit),
                      kObjectIconCols, kObjectIconRows, sx, sy, size);
        if (cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
            const std::string& nm = unit->getName();
            if (!nm.empty()) ImGui::SetTooltip("%s", nm.c_str());
        }
    }
}

// A standard service marker, independent of the optional NPC dots - and read
// from the live NPC flags, so an undiscovered flight master shows before the
// taxi window has ever been opened.
void GameScreen::renderMinimapFlightMasters(const MinimapFrame& frame, const EntityList& minimapUnits) {
    // Flight masters are a standard minimap service marker, independent of
    // the optional generic NPC-dot overlay. Use the live UNIT_NPC_FLAGS value
    // so an undiscovered flight master is visible before the taxi window has
    // ever been opened (and therefore before the known-node mask is available).
    //
    // Drawn with the stock Flight Master tracking icon - what a 3.3.5a client
    // puts on the map for a tracked town service. Stock shows it only while
    // "Flight Master" is the chosen tracking; this client has no town tracking
    // entries in GetTrackingInfo yet, so it is shown always, as it was before.
    VkDescriptorSet tex = minimapArt(kFlightMasterPath, services_.assetManager, services_.window);
    if (!tex) return;
    const float size = kBlipSize * minimapArtScale(frame.mapRadius);
    {
        for (const auto& entity : minimapUnits) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit || unit->getHealth() == 0 ||
                (unit->getNpcFlags() & game::NPC_FLAG_FLIGHT_MASTER) == 0) {
                continue;
            }

            float sx = 0.0f, sy = 0.0f;
            if (!frame.projectEntity(*entity, sx, sy)) continue;

            frame.drawList->AddImage(texId(tex), ImVec2(sx - size * 0.5f, sy - size * 0.5f),
                                     ImVec2(sx + size * 0.5f, sy + size * 0.5f));

            if (cursorNearBlip(sx, sy)) {
                const std::string& name = unit->getName();
                ImGui::SetTooltip("%s\nFlight Master",
                                  name.empty() ? "Flight Master" : name.c_str());
            }
        }
    }

}

// The same live creature-rank classification the world map uses.
void GameScreen::renderMinimapRares(const MinimapFrame& frame, const EntityList& minimapUnits,
                                     game::GameHandler& gameHandler) {
    // Rare tracker: use the same live creature-rank classification as the world map.
    // This is independent of generic NPC dots so enabling rare tracking consistently
    // shows spawned rares on both maps without adding every nearby creature.
    if (settingsPanel_.showRareTracker_) {
        VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
        if (!icons) return;
        const float size = kBlipSize * minimapArtScale(frame.mapRadius);
        for (const auto& entity : minimapUnits) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit || unit->getHealth() == 0) continue;

            const int rank = gameHandler.getCreatureRank(unit->getEntry());
            if (rank != 2 && rank != 4) continue; // 2 = Rare Elite, 4 = Rare

            float sx = 0.0f, sy = 0.0f;
            if (!frame.projectEntity(*entity, sx, sy)) continue;

            // A gold-rimmed ObjectIcons blip: there is no stock rare blip, so
            // this optional tracker borrows the stock art rather than a shape.
            const bool isElite = rank == 2;
            drawAtlasCell(frame.drawList, icons, isElite ? kIconRareElite : kIconRare,
                          kObjectIconCols, kObjectIconRows, sx, sy, size);

            if (cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
                const std::string& name = unit->getName();
                ImGui::SetTooltip("%s\n%s",
                                  name.empty() ? "Unknown creature" : name.c_str(),
                                  isElite ? "Rare Elite" : "Rare");
            }
        }
    }

}

// Other players nearby. Party and raid members have their own class-coloured
// blips, drawn elsewhere; everyone else shows while Track Humanoids is running
// (players are humanoids to the tracking aura, as in the stock client), or
// always with this client's optional creature dots on.
void GameScreen::renderMinimapPlayerDots(const MinimapFrame& frame, const EntityList& minimapPlayers,
                                          game::GameHandler& gameHandler) {
    const bool show = settingsPanel_.minimapNpcDots_ ||
        creatureTypeTracked(playerTracking(gameHandler).creatures, kCreatureTypeHumanoid);
    VkDescriptorSet icons = show
        ? minimapArt(kObjectIconsPath, services_.assetManager, services_.window) : VK_NULL_HANDLE;
    if (icons) {
        const float size = kBlipSize * minimapArtScale(frame.mapRadius);
        const uint64_t selfGuid = gameHandler.getPlayerGuid();
        const auto& partyData = gameHandler.getPartyData();
        for (const auto& entity : minimapPlayers) {
            const uint64_t guid = entity->getGuid();
            if (entity->getGuid() == selfGuid) continue;  // skip self (already drawn as arrow)

            // Skip party members (renderMinimapPartyDots draws their class blips)
            bool isPartyMember = false;
            for (const auto& m : partyData.members) {
                if (m.guid == guid) { isPartyMember = true; break; }
            }
            if (isPartyMember) continue;

            float sx = 0.0f, sy = 0.0f;
            if (!frame.projectEntity(*entity, sx, sy)) continue;

            auto player = std::static_pointer_cast<game::Unit>(entity);
            const int cell = player ? reactionBlip(gameHandler, *player) : kIconFriendly;
            drawAtlasCell(frame.drawList, icons, cell, kObjectIconCols, kObjectIconRows,
                          sx, sy, size);
            if (player && cursorNearBlip(sx, sy, kSmallBlipHoverRadius) &&
                !player->getName().empty()) {
                ImGui::SetTooltip("%s", player->getName().c_str());
            }
        }
    }

}

// Dead and lootable. Not a stock blip - the 3.3.5a minimap marks no corpses but
// your own - so it is part of this client's optional creature dots, and drawn
// with the stock art: a gold blip, dimmed.
void GameScreen::renderMinimapLootCorpses(const MinimapFrame& frame, const EntityList& minimapUnits) {
    if (!settingsPanel_.minimapNpcDots_) return;
    VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
    if (!icons) return;
    const float size = kBlipSize * minimapArtScale(frame.mapRadius);
    {
        for (const auto& entity : minimapUnits) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit) continue;
            // Must be dead (health == 0) and marked lootable
            if (unit->getHealth() != 0) continue;
            if (!(unit->getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE)) continue;

            float sx = 0.0f, sy = 0.0f;
            if (!frame.projectEntity(*entity, sx, sy)) continue;

            drawAtlasCell(frame.drawList, icons, kIconNeutral, kObjectIconCols, kObjectIconRows,
                          sx, sy, size, IM_COL32(170, 170, 170, 255));

            // Tooltip on hover
            if (cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
                const std::string& nm = unit->getName();
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.3f, 1.0f), "%s",
                                   nm.empty() ? "Lootable corpse" : nm.c_str());
                ImGui::EndTooltip();
            }
        }
    }

}

// Tracked resources - Find Herbs, Find Minerals, Find Treasure - as the stock
// gold blip. A node shows when a skill lock on it (Lock.dbc, through the
// object's lockId) is of a LockType the player's PLAYER_TRACK_RESOURCES bit
// says is tracked, which is the test the stock client makes. With this
// client's optional dots on, every interactable object gets a blip, and a
// tracked quest's objective objects the gold-rimmed one.
void GameScreen::renderMinimapObjectDots(const MinimapFrame& frame, const EntityList& minimapGameObjects,
                                          const EntrySet& minimapQuestGoEntries,
                                          game::GameHandler& gameHandler) {
    const MinimapTracking tracking = playerTracking(gameHandler);
    const bool allDots = settingsPanel_.minimapNpcDots_;
    if (!allDots && tracking.resources == 0) return;
    VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
    if (!icons) return;
    const float size = kBlipSize * minimapArtScale(frame.mapRadius);

    for (const auto& entity : minimapGameObjects) {
        auto go = std::static_pointer_cast<game::GameObject>(entity);
        if (!go) continue;

        // Only with query data: the lock is in it, and the name for the tooltip.
        const auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
        if (!goInfo || !goInfo->isValid()) continue;
        // Skip transport objects (boats/zeppelins): type 15 = MO_TRANSPORT, 11 = TRANSPORT
        if (goInfo->type == 11 || goInfo->type == 15) continue;

        // GAMEOBJECT_TYPE_CHEST (3) is every herb, vein and treasure; its lock
        // is data[0].
        constexpr uint32_t kGoTypeChest = 3;
        const bool tracked = tracking.resources != 0 && goInfo->type == kGoTypeChest &&
            goInfo->hasData &&
            (lockTrackMask(services_.assetManager, goInfo->data[0]) & tracking.resources) != 0;
        const bool isQuestGO = allDots && minimapQuestGoEntries.count(go->getEntry()) != 0;
        if (!tracked && !allDots) continue;

        float sx = 0.0f, sy = 0.0f;
        if (!frame.projectEntity(*entity, sx, sy)) continue;

        drawAtlasCell(frame.drawList, icons, isQuestGO ? kIconQuestTarget : kIconNeutral,
                      kObjectIconCols, kObjectIconRows, sx, sy, size);

        if (cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
            if (isQuestGO)
                ImGui::SetTooltip("%s (quest)", goInfo->name.c_str());
            else
                ImGui::SetTooltip("%s", goInfo->name.c_str());
        }
    }
}

// GAMEOBJECT_TYPE_CHEST covers gathering nodes on WoW servers, so the mining
// and herbalism classification is excluded explicitly rather than by luck.
void GameScreen::renderMinimapChests(const MinimapFrame& frame, const EntityList& minimapGameObjects,
                                      game::GameHandler& gameHandler) {
    // Chest tracker: GAMEOBJECT_TYPE_CHEST also covers gathering nodes on WoW
    // servers, so explicitly exclude the existing mining/herbalism classification.
    if (settingsPanel_.showChestTracker_) {
        // The gold blip, as Find Treasure draws a chest in the stock client.
        VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
        if (!icons) return;
        const float size = kBlipSize * minimapArtScale(frame.mapRadius);
        for (const auto& entity : minimapGameObjects) {
            auto chest = std::static_pointer_cast<game::GameObject>(entity);
            if (!chest) continue;
            const auto* info = gameHandler.getCachedGameObjectInfo(chest->getEntry());
            if (!info || !info->isValid() || info->type != 3) continue;
            if (gameHandler.isGatherGameObject(chest->getGuid())) continue;

            float sx = 0.0f, sy = 0.0f;
            if (!frame.projectEntity(*entity, sx, sy)) continue;

            drawAtlasCell(frame.drawList, icons, kIconNeutral, kObjectIconCols, kObjectIconRows,
                          sx, sy, size);

            if (cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
                const std::string& name = chest->getName().empty() ? info->name : chest->getName();
                ImGui::SetTooltip("%s\nChest", name.empty() ? "Chest" : name.c_str());
            }
        }
    }

}

// The ! and ? over a quest giver, from the status the server sends per NPC.
void GameScreen::renderMinimapQuestGivers(const MinimapFrame& frame, const QuestStatusMap& statuses,
                                         game::GameHandler& gameHandler) {
    for (const auto& [guid, status] : statuses) {
        // Status 9 is a turn-in the server deliberately keeps off the map,
        // which is the only thing separating it from 10.
        const auto mark = game::questGiverMarker(status);
        // Said once per status value, because a mark that appears over an
        // NPC's head and not on the minimap looks like a lost blip and is in
        // fact this rule. Which of 9 and 10 a core sends for a completed
        // quest differs between them, and only the log can say which arrives.
        if (mark.symbol && !mark.onMinimap) {
            static std::set<int> saidOffMap;
            if (saidOffMap.insert(static_cast<int>(status)).second) {
                LOG_INFO("Quest giver status ", static_cast<int>(status),
                            " draws its ", mark.symbol,
                            " over the NPC but not on the minimap - that is what"
                            " the status means. Anything missing from the minimap"
                            " with a mark over its head is arriving as this.");
            }
        }
        if (!mark.symbol || !mark.onMinimap) continue;
        // The grey marks - a low-level quest, a quest still in progress - are
        // not on the stock minimap: low-level ones only appear under the "Low
        // Level Quests" tracking, which this client does not offer, and an
        // incomplete turn-in never does.
        if (mark.dim) continue;

        auto entity = gameHandler.getEntityManager().getEntity(guid);
        if (!entity) continue;

        float sx = 0.0f, sy = 0.0f;
        if (!frame.projectEntity(*entity, sx, sy)) continue;

        // The ObjectIcons quest marks: yellow ! and ?, blue for repeatable.
        const bool available = mark.symbol[0] == '!';
        const int cell = available
            ? (mark.repeatable ? kIconRepeatableAvailable : kIconQuestAvailable)
            : (mark.repeatable ? kIconRepeatableComplete : kIconQuestComplete);
        drawAtlasCell(frame.drawList,
                      minimapArt(kObjectIconsPath, services_.assetManager, services_.window),
                      cell, kObjectIconCols, kObjectIconRows, sx, sy,
                      kBlipSize * minimapArtScale(frame.mapRadius));

        // Show NPC name and quest status on hover
        {
            if (cursorNearBlip(sx, sy)) {
                std::string npcName;
                if (entity->getType() == game::ObjectType::UNIT) {
                    auto npcUnit = std::static_pointer_cast<game::Unit>(entity);
                    npcName = npcUnit->getName();
                }
                if (!npcName.empty()) {
                    const auto mark2 = game::questGiverMarker(status);
                    const bool hasQuest = mark2.symbol && mark2.symbol[0] == '!';
                    ImGui::SetTooltip("%s\n%s", npcName.c_str(),
                                      hasQuest ? "Has a quest for you" : "Quest ready to turn in");
                }
            }
        }
    }

}

// A gold-rimmed blip on an NPC a tracked quest still needs killed.
//
// Not a stock blip: the 3.3.5a minimap shows a quest's objective as its POI
// area (renderMinimapGossipPois), not per creature. So it belongs to this
// client's optional creature dots, and is drawn with the stock art.
//
// Skips any NPC that already has a ! or a ?: a quest giver marker is more
// specific than an objective marker at the same NPC.
void GameScreen::renderMinimapQuestKills(const MinimapFrame& frame, const EntityList& minimapUnits,
                                        const QuestStatusMap& statuses,
                                        game::GameHandler& gameHandler) {
    if (!settingsPanel_.minimapNpcDots_) return;
    VkDescriptorSet icons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
    if (!icons) return;
    const float blipSize = kBlipSize * minimapArtScale(frame.mapRadius);
    // Quest kill objective markers - highlight live NPCs matching active quest kill objectives
    {
        // Build map of NPC entry → (quest title, current, required) for tooltips
        struct KillInfo { std::string questTitle; uint32_t current = 0; uint32_t required = 0; };
        std::unordered_map<uint32_t, KillInfo> killInfoMap;
        // Tracked, not every quest in the log: this highlights live NPCs in
        // the world, and a player with twenty quests wants the ones they are
        // watching lit up rather than all of them.
        const auto& trackedIds = gameHandler.getTrackedQuestIds();
        for (const auto& quest : gameHandler.getQuestLog()) {
            if (quest.complete) continue;
            if (!trackedIds.count(quest.questId)) continue;
            for (const auto& obj : quest.killObjectives) {
                if (obj.npcOrGoId <= 0 || obj.required == 0) continue;
                uint32_t npcEntry = static_cast<uint32_t>(obj.npcOrGoId);
                auto it = quest.killCounts.find(npcEntry);
                uint32_t current = (it != quest.killCounts.end()) ? it->second.first : 0;
                if (current < obj.required) {
                    killInfoMap[npcEntry] = { .questTitle = quest.title, .current = current, .required = obj.required };
                }
            }
        }

        if (!killInfoMap.empty()) {
            for (const auto& entity : minimapUnits) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                if (!unit || unit->getHealth() == 0) continue;
                // A quest giver/turn-in marker is more specific than an objective
                // marker at the same NPC. Do not paint the objective X over ! or ?.
                if (statuses.find(entity->getGuid()) != statuses.end()) continue;
                auto infoIt = killInfoMap.find(unit->getEntry());
                if (infoIt == killInfoMap.end()) continue;

                float sx = 0.0f, sy = 0.0f;
                if (!frame.projectEntity(*entity, sx, sy)) continue;

                drawAtlasCell(frame.drawList, icons, kIconQuestTarget,
                              kObjectIconCols, kObjectIconRows, sx, sy, blipSize);

                // Tooltip on hover
                if (cursorNearBlip(sx, sy)) {
                    const auto& ki = infoIt->second;
                    const std::string& npcName = unit->getName();
                    if (!npcName.empty()) {
                        ImGui::SetTooltip("%s\n%s: %u/%u",
                            npcName.c_str(),
                            ki.questTitle.empty() ? "Quest" : ki.questTitle.c_str(),
                            ki.current, ki.required);
                    } else {
                        ImGui::SetTooltip("%s: %u/%u",
                            ki.questTitle.empty() ? "Quest" : ki.questTitle.c_str(),
                            ki.current, ki.required);
                    }
                }
            }
        }
    }

}

// The points a gossip window has pointed at, and a tracked quest's POIs.
//
// A gossip point (SMSG_GOSSIP_POI) carries an icon number into POIIcons.blp,
// laid out the way WorldMap_GetPOITextureCoords reads it: 14 to a row on an
// 18-pixel pitch, 16 pixels of icon. A quest's objective (SMSG_QUEST_POI) is
// an area, which 3.3 shades on the minimap as it does on the world map; its
// end point is the ? once the quest is complete.
void GameScreen::renderMinimapGossipPois(const MinimapFrame& frame, game::GameHandler& gameHandler) {
    const float scale = minimapArtScale(frame.mapRadius);
    VkDescriptorSet poiIcons = minimapArt(kPoiIconsPath, services_.assetManager, services_.window);
    VkDescriptorSet objIcons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);

    auto questComplete = [&gameHandler](uint32_t questId) {
        for (const auto& q : gameHandler.getQuestLog()) {
            if (q.questId == questId) return q.complete;
        }
        return false;
    };

    for (const auto& poi : gameHandler.getGossipPois()) {
        // A gossip target always, a quest's points while it is tracked.
        if (poi.questObjectiveIndex != -2 && !gameHandler.isQuestTracked(poi.data)) {
            continue;
        }

        // An objective area: a translucent fill, its points pulled in to the
        // rim where they fall outside it. Only while the quest is unfinished.
        if (poi.questObjectiveIndex >= 0) {
            if (poi.area.size() < 3 || questComplete(poi.data)) continue;
            std::vector<ImVec2> pts;
            pts.reserve(poi.area.size());
            ImVec2 centroid(0.0f, 0.0f);
            const float rim = frame.mapRadius - 1.0f;
            for (const auto& [ax, ay] : poi.area) {
                const glm::vec3 r = core::coords::canonicalToRender(glm::vec3(ax, ay, 0.0f));
                glm::vec2 off = renderDeltaToMinimapOffset(r.x - frame.playerRender.x,
                                                           r.y - frame.playerRender.y, frame.view);
                const float len = std::sqrt(off.x * off.x + off.y * off.y);
                if (len > rim && len > 0.0f) off *= rim / len;
                pts.emplace_back(frame.centerX + off.x, frame.centerY + off.y);
                centroid.x += pts.back().x;
                centroid.y += pts.back().y;
            }
            centroid.x /= static_cast<float>(pts.size());
            centroid.y /= static_cast<float>(pts.size());
            // A fan from the centre: the server's outlines are hulls.
            const ImU32 fill = IM_COL32(110, 170, 255, 60);
            for (size_t i = 0; i < pts.size(); ++i) {
                frame.drawList->AddTriangleFilled(centroid, pts[i], pts[(i + 1) % pts.size()], fill);
            }
            frame.drawList->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                                        IM_COL32(255, 220, 120, 110), ImDrawFlags_Closed, 1.0f);
            continue;
        }

        float sx = 0.0f, sy = 0.0f;
        if (!frame.projectCanonical(poi.x, poi.y, sx, sy)) continue;

        if (poi.questObjectiveIndex == -1) {
            // The quest's end point: the stock turn-in ?, once there is
            // something to turn in.
            if (!questComplete(poi.data)) continue;
            drawAtlasCell(frame.drawList, objIcons, kIconQuestComplete,
                          kObjectIconCols, kObjectIconRows, sx, sy, kBlipSize * scale);
        } else if (poiIcons) {
            constexpr int kPoiCols = 14;
            constexpr float kTex = 256.0f, kPitch = 18.0f, kIcon = 16.0f;
            const float u0 = (static_cast<float>(poi.icon % kPoiCols) * kPitch + 1.0f) / kTex;
            const float v0 = (static_cast<float>(poi.icon / kPoiCols) * kPitch + 1.0f) / kTex;
            const float h = kPoiIconSize * scale * 0.5f;
            frame.drawList->AddImage(texId(poiIcons), ImVec2(sx - h, sy - h), ImVec2(sx + h, sy + h),
                                     ImVec2(u0, v0), ImVec2(u0 + kIcon / kTex, v0 + kIcon / kTex));
        }

        if (!poi.name.empty() && cursorNearBlip(sx, sy)) {
            ImGui::SetTooltip("%s", poi.name.c_str());
        }
    }
}

// What a party member pinged, for as long as the ping lasts.
void GameScreen::renderMinimapPings(const MinimapFrame& frame, game::GameHandler& gameHandler) {
    // Minimap pings from party members
    for (const auto& ping : gameHandler.getMinimapPings()) {
        float sx = 0.0f, sy = 0.0f;
        if (!frame.projectCanonical(ping.wowX, ping.wowY, sx, sy)) continue;

        float t = ping.age / game::GameHandler::MinimapPing::LIFETIME;
        float alpha = 1.0f - t;
        float pulse = 1.0f + 1.5f * t;  // expands outward as it fades

        ImU32 col  = IM_COL32(255, 220, 0, static_cast<int>(alpha * 200));
        ImU32 col2 = IM_COL32(255, 150, 0, static_cast<int>(alpha * 100));
        float r1 = 4.0f * pulse;
        float r2 = 8.0f * pulse;
        frame.drawList->AddCircle(ImVec2(sx, sy), r1, col, 16, 2.0f);
        frame.drawList->AddCircle(ImVec2(sx, sy), r2, col2, 16, 1.0f);
        frame.drawList->AddCircleFilled(ImVec2(sx, sy), 2.5f, col);
    }

}

// One blip per party and raid member, from PartyRaidBlips.blp in the member's
// class colour: the filled blip for your own party (or subgroup), the ringed
// one for the rest of the raid. A member beyond the edge of the map is the
// rotating group arrow on the rim, pointing at them.
//
// The position is the live entity's while the member is in sight, and the
// last SMSG_PARTY_MEMBER_STATS position otherwise - that one only moves every
// few seconds, so it is the fallback rather than the source.
void GameScreen::renderMinimapPartyDots(const MinimapFrame& frame, game::GameHandler& gameHandler) {
    VkDescriptorSet blips = minimapArt(kPartyRaidBlipsPath, services_.assetManager, services_.window);
    VkDescriptorSet objIcons = minimapArt(kObjectIconsPath, services_.assetManager, services_.window);
    VkDescriptorSet groupArrow = minimapArt(kGroupArrowPath, services_.assetManager, services_.window);
    auto* minimap = services_.renderer ? services_.renderer->getMinimap() : nullptr;
    const bool square = minimap && minimap->isSquareShape();
    const float scale = minimapArtScale(frame.mapRadius);
    const float blipSize = kBlipSize * scale;
    const float arrowSize = kEdgeArrowSize * scale;

    const auto& partyData = gameHandler.getPartyData();
    const bool inRaid = partyData.groupType == 1;
    const uint64_t selfGuid = gameHandler.getPlayerGuid();
    for (const auto& member : partyData.members) {
        if (!member.isOnline || member.guid == selfGuid) continue;

        auto mEnt = gameHandler.getEntityManager().getEntity(member.guid);
        glm::vec3 render;
        if (mEnt) {
            render = core::coords::canonicalToRender(
                glm::vec3(mEnt->getX(), mEnt->getY(), mEnt->getZ()));
        } else {
            if (!member.hasPartyStats || (member.posX == 0 && member.posY == 0)) continue;
            // posX/posY follow same server axis convention as minimap pings:
            // server posX = east/west axis → canonical Y (west)
            // server posY = north/south axis → canonical X (north)
            render = core::coords::canonicalToRender(glm::vec3(
                static_cast<float>(member.posY), static_cast<float>(member.posX), 0.0f));
        }
        const glm::vec2 off = renderDeltaToMinimapOffset(render.x - frame.playerRender.x,
                                                         render.y - frame.playerRender.y,
                                                         frame.view);
        const float edge = frame.mapRadius - 3.0f;
        const bool onMap = square
            ? (std::fabs(off.x) <= edge && std::fabs(off.y) <= edge)
            : (off.x * off.x + off.y * off.y <= edge * edge);

        float sx = frame.centerX + off.x;
        float sy = frame.centerY + off.y;
        if (onMap) {
            uint8_t cid = entityClassId(mEnt.get());
            if (cid == 0) cid = gameHandler.lookupPlayerClass(member.guid);
            const bool otherSubgroup = inRaid && member.subGroup != partyData.subGroup;
            // A corpse or a ghost greyed, as the old pass did.
            const bool deadOrGhost = (member.onlineStatus & 0x0030) != 0;
            const ImU32 tint = deadOrGhost ? IM_COL32(140, 140, 140, 255) : IM_COL32_WHITE;
            if (cid >= 1 && cid <= 16 && blips) {
                const int cell = (cid - 1) + (otherSubgroup ? 16 : 0);
                drawAtlasCell(frame.drawList, blips, cell, kPartyBlipCols, kPartyBlipRows,
                              sx, sy, blipSize, tint);
            } else {
                // Class not known yet: the plain ObjectIcons party / raid blip.
                drawAtlasCell(frame.drawList, objIcons, otherSubgroup ? 1 : 0,
                              kObjectIconCols, kObjectIconRows, sx, sy, blipSize, tint);
            }
        } else {
            const float len = std::sqrt(off.x * off.x + off.y * off.y);
            if (len < 1e-3f) continue;
            const float nx = off.x / len, ny = off.y / len;
            const ImVec2 rim = minimapRimPoint(frame.centerX, frame.centerY,
                                               frame.mapRadius, nx, ny, square);
            sx = rim.x;
            sy = rim.y;
            drawRotated(frame.drawList, groupArrow, sx, sy, arrowSize, nx, ny);
        }

        if (!member.name.empty() && cursorNearBlip(sx, sy, kSmallBlipHoverRadius)) {
            uint8_t pmk2 = gameHandler.getEntityRaidMark(member.guid);
            if (const char* markName = game::raidMarkName(pmk2)) {
                ImGui::SetTooltip("%s {%s}", member.name.c_str(), markName);
            } else {
                ImGui::SetTooltip("%s", member.name.c_str());
            }
        }
    }
}

// Flag carriers and the other positions a battleground reports.
void GameScreen::renderMinimapBattlegroundPositions(const MinimapFrame& frame, game::GameHandler& gameHandler) {
    // BG flag carrier / important player positions (MSG_BATTLEGROUND_PLAYER_POSITIONS)
    {
        const auto& bgPositions = gameHandler.getBgPlayerPositions();
        if (!bgPositions.empty()) {
            for (const auto& bp : bgPositions) {
                // Packet coords: wowX=canonical X (north), wowY=canonical Y (west)
                float sx = 0.0f, sy = 0.0f;
                if (!frame.projectCanonical(bp.wowX, bp.wowY, sx, sy)) continue;

                ImU32 col = bgGroupColor(bp.group);

                // Draw a flag-like diamond icon
                const float r = 5.0f;
                ImVec2 top  (sx,       sy - r);
                ImVec2 right(sx + r,   sy    );
                ImVec2 bot  (sx,       sy + r);
                ImVec2 left (sx - r,   sy    );
                frame.drawList->AddQuadFilled(top, right, bot, left, col);
                frame.drawList->AddQuad(top, right, bot, left, IM_COL32(255, 255, 255, 180), 1.0f);

                if (cursorNearBlip(sx, sy)) {
                    // Show entity name if available, otherwise guid
                    auto ent = gameHandler.getEntityManager().getEntity(bp.guid);
                    if (ent) {
                        std::string nm;
                        if (ent->getType() == game::ObjectType::PLAYER) {
                            auto pl = std::static_pointer_cast<game::Unit>(ent);
                            nm = pl ? pl->getName() : "";
                        }
                        if (!nm.empty())
                            ImGui::SetTooltip("Flag carrier: %s", nm.c_str());
                        else
                            ImGui::SetTooltip("Flag carrier");
                    } else {
                        ImGui::SetTooltip("Flag carrier");
                    }
                }
            }
        }
    }

}

// Where the corpse is, while the player is a ghost.
//
// Unlike every marker above it, a corpse off the edge of the map is not
// skipped - it becomes an arrow at the rim pointing at it, which is why this
// one keeps the render position rather than only the projected point.
void GameScreen::renderMinimapCorpseMarker(const MinimapFrame& frame, game::GameHandler& gameHandler) {
    // Corpse direction indicator - shown when player is a ghost
    if (gameHandler.isPlayerGhost()) {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        if (gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY)) {
            // The render position is kept: unlike the loops above, a corpse off
            // the edge is not skipped - it gets a direction arrow at the rim,
            // and that needs the direction.
            const glm::vec3 corpseRender =
                core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f));
            float csx = 0.0f, csy = 0.0f;
            const bool onMap = frame.project(corpseRender, csx, csy);

            const float scale = minimapArtScale(frame.mapRadius);
            if (onMap) {
                // The tombstone from POIIcons.blp, at the coordinates
                // WorldMapCorpseTemplate gives it (worldmapframetemplates.xml).
                if (VkDescriptorSet poi = minimapArt(kPoiIconsPath, services_.assetManager,
                                                     services_.window)) {
                    const float h = kPoiIconSize * scale * 0.5f;
                    frame.drawList->AddImage(texId(poi), ImVec2(csx - h, csy - h),
                                             ImVec2(csx + h, csy + h),
                                             ImVec2(0.56640625f, 0.00390625f),
                                             ImVec2(0.6328125f, 0.0703125f));
                }
                // Tooltip on hover
                if (cursorNearBlip(csx, csy)) {
                    float dist = gameHandler.getCorpseDistance();
                    if (dist >= 0.0f)
                        ImGui::SetTooltip("Your corpse (%.0f yd)", dist);
                    else
                        ImGui::SetTooltip("Your corpse");
                }
            } else {
                // Corpse is outside minimap - draw an edge arrow pointing toward it
                float dx = corpseRender.x - frame.playerRender.x;
                float dy = corpseRender.y - frame.playerRender.y;
                // Only the direction is wanted here, and the projection's
                // scale is uniform and positive, so it falls out of the
                // normalisation below.
                const glm::vec2 dir = renderDeltaToMinimapOffset(dx, dy, frame.view);
                float rx = dir.x;
                float ry = dir.y;
                float len = std::sqrt(rx * rx + ry * ry);
                if (len > 0.001f) {
                    float nx = rx / len;
                    float ny = ry / len;
                    // The stock corpse arrow, on the rim, turned toward it.
                    auto* mm = services_.renderer ? services_.renderer->getMinimap() : nullptr;
                    const ImVec2 rim = minimapRimPoint(frame.centerX, frame.centerY,
                                                       frame.mapRadius, nx, ny,
                                                       mm && mm->isSquareShape());
                    const float ax = rim.x;
                    const float ay = rim.y;
                    drawRotated(frame.drawList,
                                minimapArt(kCorpseArrowPath, services_.assetManager, services_.window),
                                ax, ay, kEdgeArrowSize * scale, nx, ny);
                    // Tooltip on hover
                    if (cursorNearBlip(ax, ay, kArrowHoverRadius)) {
                        float dist = gameHandler.getCorpseDistance();
                        if (dist >= 0.0f)
                            ImGui::SetTooltip("Your corpse (%.0f yd)", dist);
                        else
                            ImGui::SetTooltip("Your corpse");
                    }
                }
            }
        }
    }

}

// The player, at the centre, pointing where the camera looks.
//
// On a rotating minimap the map itself turns so forward is screen-up; on a
// fixed one the arrow turns instead.
void GameScreen::renderMinimapPlayerArrow(const MinimapFrame& frame) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    // Player position arrow at minimap center, pointing in camera facing direction.
    // On a rotating minimap the map already turns so forward = screen-up; on a fixed
    // minimap we rotate the arrow to match the player's compass heading.
    {
        // Compute screen-space facing direction for the arrow.
        // frame.bearing = clockwise angle from screen-north (0 = facing north/up).
        float arrowAngle = 0.0f; // 0 = pointing up (north)
        if (!minimap->isRotateWithCamera()) {
            // Fixed minimap: arrow must show actual facing relative to north.
            // Match the mirrored minimap texture by flipping the arrow's
            // visual north/south component.
            arrowAngle = -glm::radians(renderer->getCharacterYaw());
        }
        // Screen direction the arrow tip points toward
        float nx =  std::sin(arrowAngle); // screen +X = east
        float ny = -std::cos(arrowAngle); // screen -Y = north

        // The stock arrow, Interface\Minimap\MinimapArrow, at the 40x40 that
        // Minimap.lua sets with SetPlayerTextureWidth/Height.
        if (VkDescriptorSet arrow = minimapArt(kPlayerArrowPath, services_.assetManager,
                                               services_.window)) {
            drawRotated(frame.drawList, arrow, frame.centerX, frame.centerY,
                        kPlayerArrowSize * minimapArtScale(frame.mapRadius), nx, ny);
            return;
        }

        // Only without the texture: a chevron-style arrow - tip, two base
        // corners, and a notch at the back.
        const float tipLen  = 8.0f;  // tip forward distance
        const float baseW   = 5.0f;  // half-width at base
        const float notchIn = 3.0f;  // how far back the center notch sits
        // Perpendicular direction (rotated 90°)
        float px =  ny; // perpendicular x
        float py = -nx; // perpendicular y

        ImVec2 tip  (frame.centerX + nx * tipLen,  frame.centerY + ny * tipLen);
        ImVec2 baseL(frame.centerX - nx * baseW + px * baseW,  frame.centerY - ny * baseW + py * baseW);
        ImVec2 baseR(frame.centerX - nx * baseW - px * baseW,  frame.centerY - ny * baseW - py * baseW);
        ImVec2 notch(frame.centerX - nx * (baseW - notchIn),   frame.centerY - ny * (baseW - notchIn));

        // Fill: bright white with slight gold tint, dark outline for readability
        frame.drawList->AddTriangleFilled(tip, baseL, notch, IM_COL32(255, 248, 200, 245));
        frame.drawList->AddTriangleFilled(tip, notch, baseR, IM_COL32(255, 248, 200, 245));
        frame.drawList->AddTriangle(tip, baseL, notch, IM_COL32(60, 40, 0, 200), 1.2f);
        frame.drawList->AddTriangle(tip, notch, baseR, IM_COL32(60, 40, 0, 200), 1.2f);
    }

}

// The wheel and the ctrl+click, when this client owns the ring.
//
// Both are FrameXML's when it draws the minimap: Minimap_OnClick pings through
// PingLocation and the cluster's own buttons zoom, so running these too would
// ping twice and zoom two steps a notch.
void GameScreen::handleMinimapInput(const MinimapFrame& frame, game::GameHandler& gameHandler,
                                    bool minimapInputBlocked) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    // Skip minimap input when an ImGui window (bag, settings, etc.) is in front.
    //
    // ...and entirely when FrameXML draws the minimap, because then the frame
    // is a widget with its own handlers: Minimap_OnClick pings through
    // PingLocation and the cluster's zoom buttons and OnMouseWheel change the
    // zoom. Running both would ping twice and zoom two steps per notch.

    // Scroll wheel over minimap → zoom in/out
    if (!minimapInputBlocked) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (cursorOverMinimap(frame.centerX, frame.centerY, frame.mapRadius)) {
                if (wheel > 0.0f)
                    minimap->zoomIn();
                else
                    minimap->zoomOut();
            }
        }
    }

    // Ctrl+click on minimap → send minimap ping to party
    if (!minimapInputBlocked && ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyCtrl) {
        ImVec2 mouse = ImGui::GetMousePos();
        float mdx = mouse.x - frame.centerX;
        float mdy = mouse.y - frame.centerY;
        if (cursorOverMinimap(frame.centerX, frame.centerY, frame.mapRadius)) {
            // frame.playerRender is in render coords; add the delta the click means
            // to get a render position, then convert to canonical.
            const glm::vec2 d = minimapOffsetToRenderDelta(mdx, mdy, frame.view);
            glm::vec3 clickRender = frame.playerRender + glm::vec3(d.x, d.y, 0.0f);
            glm::vec3 clickCanon = core::coords::renderToCanonical(clickRender);
            gameHandler.sendMinimapPing(clickCanon.x, clickCanon.y);
        }
    }

}

// What is written on and around the ring: the coordinates, the zone name, the
// instance difficulty, and the tooltip and menu the cursor brings up.
//
// Marks go on the map; these describe it.
void GameScreen::renderMinimapReadouts(const MinimapFrame& frame, game::GameHandler& gameHandler,
                                       bool minimapInputBlocked) {
    auto* renderer = services_.renderer;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    if (!minimap) return;
    ImDrawList* drawList = frame.drawList;
    // Optional persistent coordinate display below the minimap.
    if (settingsPanel_.showMinimapCoordinates_) {
        glm::vec3 playerCanon = core::coords::renderToCanonical(frame.playerRender);
        char coordBuf[32];
        std::snprintf(coordBuf, sizeof(coordBuf), "%.1f, %.1f", playerCanon.x, playerCanon.y);

        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize();
        ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, coordBuf);

        float tx = frame.centerX - textSz.x * 0.5f;
        float ty = frame.centerY + frame.mapRadius + 3.0f;

        // Semi-transparent dark background pill
        float pad = 3.0f;
        drawList->AddRectFilled(
            ImVec2(tx - pad, ty - pad),
            ImVec2(tx + textSz.x + pad, ty + textSz.y + pad),
            IM_COL32(0, 0, 0, 140), 4.0f);
        // Coordinate text in warm yellow
        drawList->AddText(font, fontSize, ImVec2(tx, ty), IM_COL32(230, 220, 140, 255), coordBuf);
    }

    // Zone name display - drawn inside the top edge of the minimap circle
    {
        std::string zoneName;
        // The live terrain-derived zone first; the server's only if that has
        // nothing. The server tells us its zone on SMSG_INIT_WORLD_STATES and
        // at no other time, so preferring it left this label naming the last
        // zone the server announced rather than the one being walked through.
        uint32_t zoneId = renderer ? renderer->getCurrentZoneId() : 0u;
        const uint32_t serverZoneId = gameHandler.getWorldStateZoneId();
        if (zoneId == 0) zoneId = serverZoneId;
        // Said once each time the pair changes. The two disagreeing is not by
        // itself a fault - the server only revises its answer when it notices
        // a zone change - but a disagreement that persists while standing
        // still means the server has the player somewhere else, which is also
        // why no creatures would arrive. This prints both names and the
        // position, which is what tells those two cases apart.
        if (serverZoneId != 0 && zoneId != 0 && serverZoneId != zoneId) {
            static uint64_t lastReported = 0;
            const uint64_t pair = (static_cast<uint64_t>(serverZoneId) << 32) | zoneId;
            if (pair != lastReported) {
                lastReported = pair;
                const auto& mi = gameHandler.getMovementInfo();
                LOG_WARNING("Zone disagreement: server says ", serverZoneId, " (",
                            gameHandler.getWhoAreaName(serverZoneId),
                            "), terrain under the player says ", zoneId, " (",
                            gameHandler.getWhoAreaName(zoneId),
                            ") at canonical ", mi.x, ", ", mi.y, ", ", mi.z);
            }
        }
        if (zoneId != 0) {
            zoneName = gameHandler.getWhoAreaName(zoneId);
            if (zoneName.empty()) {
                if (auto* zmRenderer = renderer ? renderer->getZoneManager() : nullptr) {
                    if (const game::ZoneInfo* zi = zmRenderer->getZoneInfo(zoneId)) {
                        zoneName = zi->name;
                    }
                }
            }
        }
        if (zoneName.empty() && renderer) {
            zoneName = renderer->getCurrentZoneName();
        }
        if (!zoneName.empty()) {
            ImFont* font = ImGui::GetFont();
            float fontSize = ImGui::GetFontSize();

            // FrameXML's MinimapZoneText names the zone above the minimap, so
            // naming it again inside the circle put it on screen twice. The
            // name is drawn here only when this client owns the minimap. No
            // weather glyph: the stock minimap shows none.
            const bool nameIsOurs = !frameXmlOwns(UiElement::Minimap);
            const std::string fullLabel = nameIsOurs ? zoneName : std::string();
            if (!fullLabel.empty()) {
                ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, fullLabel.c_str());
                float tx = frame.centerX - ts.x * 0.5f;
                float ty = frame.centerY - frame.mapRadius + 4.0f;  // just inside the top edge
                float pad = 2.0f;
                drawList->AddRectFilled(
                    ImVec2(tx - pad, ty - pad),
                    ImVec2(tx + ts.x + pad, ty + ts.y + pad),
                    IM_COL32(0, 0, 0, 160), 2.0f);
                drawList->AddText(font, fontSize, ImVec2(tx + 1.0f, ty + 1.0f),
                                  IM_COL32(0, 0, 0, 180), zoneName.c_str());
                drawList->AddText(font, fontSize, ImVec2(tx, ty),
                                  IM_COL32(255, 230, 150, 220), zoneName.c_str());
            }
        }
    }

    // Instance difficulty indicator - just below zone name, inside minimap top edge
    if (gameHandler.isInInstance()) {
        uint32_t diff = gameHandler.getInstanceDifficulty();
        const char* named = game::instanceDifficultyName(diff);
        const char* label = named ? named : "Unknown";

        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize() * 0.85f;
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        float tx = frame.centerX - ts.x * 0.5f;
        // Position below zone name: top edge + zone font size + small gap
        float ty = frame.centerY - frame.mapRadius + 4.0f + ImGui::GetFontSize() + 2.0f;
        float pad = 2.0f;

        // Color-code: heroic=orange, normal=light gray
        ImU32 bgCol = gameHandler.isInstanceHeroic() ? IM_COL32(120, 60, 0, 180) : IM_COL32(0, 0, 0, 160);
        ImU32 textCol = gameHandler.isInstanceHeroic() ? IM_COL32(255, 180, 50, 255) : IM_COL32(200, 200, 200, 220);

        drawList->AddRectFilled(
            ImVec2(tx - pad, ty - pad),
            ImVec2(tx + ts.x + pad, ty + ts.y + pad),
            bgCol, 2.0f);
        drawList->AddText(font, fontSize, ImVec2(tx, ty), textCol, label);
    }

    // Hover tooltip and right-click context menu
    {
        ImVec2 mouse = ImGui::GetMousePos();
        float mdx = mouse.x - frame.centerX;
        float mdy = mouse.y - frame.centerY;
        bool overMinimap = !minimapInputBlocked && cursorOverMinimap(frame.centerX, frame.centerY, frame.mapRadius);

        if (overMinimap) {
            ImGui::BeginTooltip();
            if (settingsPanel_.showMinimapCoordinates_) {
                // Compute the world coordinate under the mouse cursor.
                const glm::vec2 hover = minimapOffsetToRenderDelta(mdx, mdy, frame.view);
                glm::vec3 hoverRender(frame.playerRender.x + hover.x, frame.playerRender.y + hover.y,
                                      frame.playerRender.z);
                glm::vec3 hoverCanon = core::coords::renderToCanonical(hoverRender);
                ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.5f, 1.0f), "%.1f, %.1f", hoverCanon.x, hoverCanon.y);
            }
            ImGui::TextColored(colors::kMediumGray, "Ctrl+click to ping");
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##minimapContextMenu");
            }
        }

        if (ImGui::BeginPopup("##minimapContextMenu")) {
            ImGui::TextColored(ui::colors::kTooltipGold, "Minimap");
            ImGui::Separator();

            // Zoom controls
            if (ImGui::MenuItem("Zoom In")) {
                minimap->zoomIn();
            }
            if (ImGui::MenuItem("Zoom Out")) {
                minimap->zoomOut();
            }

            ImGui::Separator();

            // Toggle options with checkmarks.
            //
            // Each goes through the setter and is saved, as the panel's own
            // boxes are. Written to the members alone, a toggle lasted until
            // the client closed: nothing saved it, and rotation is bound to a
            // CVar whose stored value is applied over the file at start-up.
            const auto toggle = [this](const char* key, bool on) {
                settingsPanel_.setSettingValue(key, on ? "1" : "0");
                saveSettings();
            };
            const bool rotWithCam = minimap->isRotateWithCamera();
            if (ImGui::MenuItem("Rotate with Camera", nullptr, rotWithCam)) {
                toggle("minimaprotate", !rotWithCam);
            }

            const bool squareShape = minimap->isSquareShape();
            if (ImGui::MenuItem("Square Shape", nullptr, squareShape)) {
                toggle("minimapsquare", !squareShape);
            }

            const bool npcDots = settingsPanel_.minimapNpcDots_;
            if (ImGui::MenuItem("Show NPC Dots", nullptr, npcDots)) {
                toggle("minimapnpcdots", !npcDots);
            }

            ImGui::EndPopup();
        }
    }


}

void GameScreen::renderMinimapMarkers(game::GameHandler& gameHandler) {
    const auto& statuses = gameHandler.getNpcQuestStatuses();
    auto* renderer = services_.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    auto* window = services_.window;
    if (!camera || !minimap || !window) return;

    float screenW = static_cast<float>(window->getWidth());

    // Where the map is, which is not always where this client would put it.
    //
    // These used to be the corner this client's own minimap occupies, and that
    // held for as long as the marker pass only ran when this client drew the
    // ring. It draws blips on FrameXML's minimap too now, and that one sits
    // wherever the Minimap widget was laid out - so the rect is asked for
    // rather than assumed, and the corner is the fallback for when nothing has
    // placed it.
    float mapSize = 200.0f;
    float margin = 10.0f;
    float mapRadius = mapSize * 0.5f;
    float centerX = screenW - margin - mapRadius;
    float centerY = margin + mapRadius;
    // Nothing placed it and FrameXML owns the ring: its minimap is hidden or
    // was never built, so there is no map to put blips on and the fallback
    // corner would scatter them over empty screen.
    if (frameXmlOwns(UiElement::Minimap) && !minimap->hasScreenRect()) return;
    if (minimap->hasScreenRect() && minimap->screenRectW() > 0.0f) {
        mapRadius = minimap->screenRectW() * 0.5f;
        centerX = minimap->screenRectX() + mapRadius;
        centerY = minimap->screenRectY() + minimap->screenRectH() * 0.5f;
    }
    float viewRadius = minimap->getViewRadius();

    // Use the exact same minimap center as Renderer::renderWorld() to keep markers anchored.
    glm::vec3 playerRender = camera->getPosition();
    if (renderer->getCharacterInstanceId() != 0) {
        playerRender = renderer->getCharacterPosition();
    }

    // Camera bearing for minimap rotation
    float bearing = 0.0f;
    float cosB = 1.0f;
    float sinB = 0.0f;
    if (minimap->isRotateWithCamera()) {
        glm::vec3 fwd = camera->getForward();
        // Render space: +X=West, +Y=North. Camera fwd=(cos(yaw),sin(yaw)).
        // Clockwise bearing from North: atan2(fwd.y, -fwd.x).
        bearing = std::atan2(fwd.y, -fwd.x);
        cosB = std::cos(bearing);
        sinB = std::sin(bearing);
    }

    // The same view the Lua binding uses, so a ctrl+click here and a
    // Minimap:PingLocation from the interface land on the same world point.
    const MinimapView minimapView{.viewRadius = viewRadius, .mapRadius = mapRadius, .cosBearing = cosB, .sinBearing = sinB};

    // Behind windows, in front of the map.
    //
    // The foreground list is drawn after every ImGui window, so a blip sat on
    // top of anything opened over the minimap - a bag, the character sheet,
    // the map itself. The background list is drawn before them and after the
    // widget renderer, which puts these above FrameXML's ring and border (that
    // pass runs earlier in the frame and into this same list) and underneath
    // any window, which is where WoW keeps them.
    //
    // Safe because nothing draws the minimap in a window: the ring and border
    // are FrameXML's, and the map surface itself comes from the 3D renderer,
    // which is beneath all of ImGui either way.
    auto* drawList = ImGui::GetBackgroundDrawList();

    // Partition the entity map once. Marker categories below can traverse their
    // compact type-specific lists instead of rescanning every entity 4-5 times.
    // Reused across frames to avoid three vector allocations per frame; the
    // clear below releases the previous frame's shared_ptrs on re-entry.
    static thread_local std::vector<std::shared_ptr<game::Entity>> minimapUnits;
    static thread_local std::vector<std::shared_ptr<game::Entity>> minimapPlayers;
    static thread_local std::vector<std::shared_ptr<game::Entity>> minimapGameObjects;
    minimapUnits.clear();
    minimapPlayers.clear();
    minimapGameObjects.clear();
    const auto& allEntities = gameHandler.getEntityManager().getEntities();
    minimapUnits.reserve(allEntities.size() / 2);
    minimapPlayers.reserve(allEntities.size() / 8);
    minimapGameObjects.reserve(allEntities.size() / 4);
    for (const auto& [guid, entity] : allEntities) {
        (void)guid;
        if (!entity) continue;
        switch (entity->getType()) {
            case game::ObjectType::UNIT:       minimapUnits.push_back(entity); break;
            case game::ObjectType::PLAYER:     minimapPlayers.push_back(entity); break;
            case game::ObjectType::GAMEOBJECT: minimapGameObjects.push_back(entity); break;
            default: break;
        }
    }

    // Everything a marker needs to place itself, in one place - the categories
    // below take this rather than eight locals each.
    const MinimapFrame frame{.drawList = drawList, .centerX = centerX, .centerY = centerY, .mapRadius = mapRadius, .bearing = bearing,
                             .playerRender = playerRender, .view = minimapView};

    // Build sets of entries that are incomplete objectives for tracked quests.
    // minimapQuestEntries: NPC creature entries (npcOrGoId > 0)
    // minimapQuestGoEntries: game object entries (npcOrGoId < 0, stored as abs value)
    refreshQuestObjectiveCache(gameHandler);
    const auto& minimapQuestEntries = minimapQuestCreatureEntries_;
    const auto& minimapQuestGoEntries = minimapQuestGameObjectEntries_;

    // Bottom to top, as the stock client stacks them: quest areas under
    // everything, then tracked things, quest marks, the group, the player.
    renderMinimapGossipPois(frame, gameHandler);
    renderMinimapNpcDots(frame, minimapUnits, minimapQuestEntries);
    renderMinimapFlightMasters(frame, minimapUnits);
    renderMinimapRares(frame, minimapUnits, gameHandler);
    renderMinimapPlayerDots(frame, minimapPlayers, gameHandler);
    renderMinimapLootCorpses(frame, minimapUnits);
    renderMinimapObjectDots(frame, minimapGameObjects, minimapQuestGoEntries, gameHandler);
    renderMinimapChests(frame, minimapGameObjects, gameHandler);

    renderMinimapQuestKills(frame, minimapUnits, statuses, gameHandler);
    renderMinimapQuestGivers(frame, statuses, gameHandler);
    renderMinimapPings(frame, gameHandler);
    renderMinimapPartyDots(frame, gameHandler);
    renderMinimapBattlegroundPositions(frame, gameHandler);
    renderMinimapCorpseMarker(frame, gameHandler);
    renderMinimapPlayerArrow(frame);
    // Blocked by any window in front, and entirely when FrameXML draws the
    // minimap - then the ring is a widget with its own handlers, and running
    // both would ping twice and zoom two steps a notch. Both halves below need
    // the answer, so it is worked out once here.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool minimapInputBlocked =
        (g.HoveredWindow != nullptr) || frameXmlOwns(UiElement::Minimap);
    handleMinimapInput(frame, gameHandler, minimapInputBlocked);
    renderMinimapReadouts(frame, gameHandler, minimapInputBlocked);
    // The furniture around the map - zoom buttons, clock, indicators - is
    // FrameXML's when FrameXML draws the ring, so it answers the ownership
    // question separately from the blips above.
    if (!frameXmlOwns(UiElement::Minimap)) {
        renderMinimapChrome(gameHandler, centerX, centerY, mapRadius);
    }
}
void GameScreen::saveSettings() {
    std::string path = SettingsPanel::getSettingsPath();
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save settings to ", path);
        return;
    }

    // Interface
    out << "ui_opacity=" << settingsPanel_.pendingUiOpacity << "\n";
    out << "window_ui_scale=" << settingsPanel_.pendingWindowUiScale << "\n";
    out << "scroll_speed=" << settingsPanel_.pendingScrollSpeed << "\n";
    out << "minimap_rotate=" << (settingsPanel_.pendingMinimapRotate ? 1 : 0) << "\n";
    out << "minimap_square=" << (settingsPanel_.pendingMinimapSquare ? 1 : 0) << "\n";
    out << "minimap_npc_dots=" << (settingsPanel_.pendingMinimapNpcDots ? 1 : 0) << "\n";
    out << "show_minimap_clock=" << (settingsPanel_.pendingShowMinimapClock ? 1 : 0) << "\n";
    out << "show_minimap_coordinates=" << (settingsPanel_.pendingShowMinimapCoordinates ? 1 : 0) << "\n";
    out << "show_latency_meter=" << (settingsPanel_.pendingShowLatencyMeter ? 1 : 0) << "\n";
    out << "check_for_updates=" << (settingsPanel_.pendingCheckForUpdates ? 1 : 0) << "\n";
    out << "content_packs=" << (settingsPanel_.pendingContentPacks ? 1 : 0) << "\n";
    out << "show_dps_meter=" << (settingsPanel_.showDPSMeter_ ? 1 : 0) << "\n";
    // Only once dragged. Interface units, where the old dps_meter_x and _y
    // were the ImGui window's pixels; those are no longer read.
    if (settingsPanel_.dpsMeterLeft_ >= 0.0f) {
        out << "dps_meter_left=" << settingsPanel_.dpsMeterLeft_ << "\n";
        out << "dps_meter_top=" << settingsPanel_.dpsMeterTop_ << "\n";
    }
    out << "show_cooldown_tracker=" << (settingsPanel_.showCooldownTracker_ ? 1 : 0) << "\n";
    out << "show_rare_tracker=" << (settingsPanel_.showRareTracker_ ? 1 : 0) << "\n";
    out << "map_window=" << (settingsPanel_.showMapWindow_ ? 1 : 0) << "\n";
    out << "show_chest_tracker=" << (settingsPanel_.showChestTracker_ ? 1 : 0) << "\n";
    out << "separate_bags=" << (settingsPanel_.pendingSeparateBags ? 1 : 0) << "\n";
    out << "show_keyring=" << (settingsPanel_.pendingShowKeyring ? 1 : 0) << "\n";
    out << "bag_scale=" << settingsPanel_.pendingBagScale << "\n";
    out << "show_micro_menu=" << (settingsPanel_.pendingShowMicroMenu ? 1 : 0) << "\n";
    out << "chat_box_visible=" << (settingsPanel_.pendingChatBoxVisible ? 1 : 0) << "\n";
    out << "idle_camera_orbit=" << (settingsPanel_.pendingIdleCameraOrbit ? 1 : 0) << "\n";
    out << "buff_bar_scale=" << settingsPanel_.pendingBuffBarScale << "\n";
    out << "action_bar_scale=" << settingsPanel_.pendingActionBarScale << "\n";
    out << "nameplate_scale=" << settingsPanel_.nameplateScale_ << "\n";
    out << "show_friendly_nameplates=" << (settingsPanel_.showFriendlyNameplates_ ? 1 : 0) << "\n";
    out << "show_enemy_nameplates=" << (settingsPanel_.showEnemyNameplates_ ? 1 : 0) << "\n";
    out << "show_action_bar2=" << (settingsPanel_.pendingShowActionBar2 ? 1 : 0) << "\n";
    out << "action_bar2_offset_x=" << settingsPanel_.pendingActionBar2OffsetX << "\n";
    out << "action_bar2_offset_y=" << settingsPanel_.pendingActionBar2OffsetY << "\n";
    out << "show_right_bar=" << (settingsPanel_.pendingShowRightBar ? 1 : 0) << "\n";
    out << "show_left_bar=" << (settingsPanel_.pendingShowLeftBar ? 1 : 0) << "\n";
    out << "right_bar_offset_y=" << settingsPanel_.pendingRightBarOffsetY << "\n";
    out << "left_bar_offset_y=" << settingsPanel_.pendingLeftBarOffsetY << "\n";
    out << "damage_flash=" << (settingsPanel_.damageFlashEnabled_ ? 1 : 0) << "\n";
    out << "low_health_vignette=" << (settingsPanel_.lowHealthVignetteEnabled_ ? 1 : 0) << "\n";

    // Audio
    out << "sound_muted=" << (settingsPanel_.soundMuted_ ? 1 : 0) << "\n";
    // The switches that came off the game's own Sound panel. Their cvars are
    // saved too, but the settings file is what the panel opens from.
    out << "sound_in_background=" << (settingsPanel_.pendingSoundInBackground ? 1 : 0) << "\n";
    out << "enable_music=" << (settingsPanel_.pendingEnableMusic ? 1 : 0) << "\n";
    out << "loop_music=" << (settingsPanel_.pendingLoopMusic ? 1 : 0) << "\n";
    out << "enable_ambience=" << (settingsPanel_.pendingEnableAmbience ? 1 : 0) << "\n";
    out << "error_speech=" << (settingsPanel_.pendingErrorSpeech ? 1 : 0) << "\n";
    out << "enable_sound_effects=" << (settingsPanel_.pendingEnableSoundEffects ? 1 : 0) << "\n";
    out << "use_original_soundtrack=" << (settingsPanel_.pendingUseOriginalSoundtrack ? 1 : 0) << "\n";
    out << "master_volume=" << settingsPanel_.pendingMasterVolume << "\n";
    out << "effects_volume=" << settingsPanel_.pendingEffectsVolume << "\n";
    out << "music_volume=" << settingsPanel_.pendingMusicVolume << "\n";
    out << "ambient_volume=" << settingsPanel_.pendingAmbientVolume << "\n";
    out << "bell_volume=" << settingsPanel_.pendingBellVolume << "\n";
    out << "ui_volume=" << settingsPanel_.pendingUiVolume << "\n";
    out << "combat_volume=" << settingsPanel_.pendingCombatVolume << "\n";
    out << "spell_volume=" << settingsPanel_.pendingSpellVolume << "\n";
    out << "movement_volume=" << settingsPanel_.pendingMovementVolume << "\n";
    out << "footstep_volume=" << settingsPanel_.pendingFootstepVolume << "\n";
    out << "npc_voice_volume=" << settingsPanel_.pendingNpcVoiceVolume << "\n";
    out << "mount_volume=" << settingsPanel_.pendingMountVolume << "\n";
    out << "activity_volume=" << settingsPanel_.pendingActivityVolume << "\n";
    out << "character_speech=" << (settingsPanel_.pendingCharacterSpeech ? 1 : 0) << "\n";

    // Gameplay / display pacing
    out << "fullscreen=" << (settingsPanel_.pendingFullscreen ? 1 : 0) << "\n";
    out << "resolution_width=" << settingsPanel_.pendingResolutionWidth << "\n";
    out << "resolution_height=" << settingsPanel_.pendingResolutionHeight << "\n";
    out << "vsync=" << (settingsPanel_.pendingVsync ? 1 : 0) << "\n";
    out << "auto_loot=" << (settingsPanel_.pendingAutoLoot ? 1 : 0) << "\n";
    out << "auto_sell_grey=" << (settingsPanel_.pendingAutoSellGrey ? 1 : 0) << "\n";
    out << "auto_repair=" << (settingsPanel_.pendingAutoRepair ? 1 : 0) << "\n";
    out << "secure_ability_toggle=" << (settingsPanel_.pendingSecureAbilityToggle ? 1 : 0) << "\n";
    out << "auto_face_target=" << (settingsPanel_.pendingAutoFaceTarget ? 1 : 0) << "\n";
    out << "graphics_preset=" << static_cast<int>(settingsPanel_.currentGraphicsPreset) << "\n";
    out << "ground_clutter_density=" << settingsPanel_.pendingGroundClutterDensity << "\n";
    // The five that came off the game's own Effects panel. Saved here like any
    // other setting: reachable through a cvar is not the same as remembered,
    // and without these they would work for the session and open on their
    // defaults at the next login.
    out << "ground_clutter_distance=" << settingsPanel_.pendingGroundClutterDistance << "\n";
    out << "particle_density=" << settingsPanel_.pendingParticleDensity << "\n";
    out << "weather_detail=" << settingsPanel_.pendingWeatherDetail << "\n";
    out << "environment_detail=" << settingsPanel_.pendingEnvironmentDetail << "\n";
    out << "texture_filtering=" << settingsPanel_.pendingTextureFiltering << "\n";
    out << "grass_enabled=" << (settingsPanel_.pendingGrassEnabled ? 1 : 0) << "\n";
    out << "grass_density=" << settingsPanel_.pendingGrassDensity << "\n";
    out << "grass_height=" << settingsPanel_.pendingGrassHeight << "\n";
    out << "grass_distance=" << settingsPanel_.pendingGrassDistance << "\n";
    out << "shadows=" << (settingsPanel_.pendingShadows ? 1 : 0) << "\n";
    out << "shadow_distance=" << settingsPanel_.pendingShadowDistance << "\n";
    out << "shadow_strength=" << settingsPanel_.pendingShadowStrength << "\n";
    out << "foliage_wind=" << settingsPanel_.pendingFoliageWind << "\n";
    out << "view_distance=" << settingsPanel_.pendingViewDistance << "\n";
    out << "fog_sky_blend=" << settingsPanel_.pendingFogSkyBlend << "\n";
    out << "fog_strength=" << settingsPanel_.pendingFogStrength << "\n";
    out << "light_shafts=" << settingsPanel_.pendingVolumetricFog << "\n";
    out << "ray_traced_lighting=" << settingsPanel_.pendingRtLighting << "\n";
    out << "mist_density=" << settingsPanel_.pendingVolumetricDensity << "\n";
    out << "sharp_stars=" << (settingsPanel_.pendingSharpStars ? 1 : 0) << "\n";
    out << "sun_shafts=" << (settingsPanel_.pendingSunShafts ? 1 : 0) << "\n";
    out << "shoreline_foam=" << (settingsPanel_.pendingShorelineFoam ? 1 : 0) << "\n";
    out << "wet_ground=" << (settingsPanel_.pendingWetGround ? 1 : 0) << "\n";
    out << "brightness=" << settingsPanel_.pendingBrightness << "\n";
    out << "water_refraction=" << (settingsPanel_.pendingWaterRefraction ? 1 : 0) << "\n";
    out << "antialiasing=" << settingsPanel_.pendingAntiAliasing << "\n";
    out << "fxaa=" << (settingsPanel_.pendingFXAA ? 1 : 0) << "\n";
    out << "normal_mapping=" << (settingsPanel_.pendingNormalMapping ? 1 : 0) << "\n";
    out << "normal_map_strength=" << settingsPanel_.pendingNormalMapStrength << "\n";
    out << "lens_flare=" << settingsPanel_.pendingLensFlare << "\n";
    out << "frame_cap=" << settingsPanel_.pendingFrameCap << "\n";
    out << "pom=" << (settingsPanel_.pendingPOM ? 1 : 0) << "\n";
    out << "pom_quality=" << settingsPanel_.pendingPOMQuality << "\n";
    out << "upscaling_mode=" << settingsPanel_.pendingUpscalingMode << "\n";
    out << "fsr=" << (settingsPanel_.pendingFSR ? 1 : 0) << "\n";
    out << "fsr_quality=" << settingsPanel_.pendingFSRQuality << "\n";
    out << "fsr_sharpness=" << settingsPanel_.pendingFSRSharpness << "\n";
    out << "fsr2_jitter_sign=" << settingsPanel_.pendingFSR2JitterSign << "\n";
    out << "fsr2_mv_scale_x=" << settingsPanel_.pendingFSR2MotionVecScaleX << "\n";
    out << "fsr2_mv_scale_y=" << settingsPanel_.pendingFSR2MotionVecScaleY << "\n";
    out << "amd_fsr3_framegen=" << (settingsPanel_.pendingAMDFramegen ? 1 : 0) << "\n";

    // Controls
    out << "mouse_sensitivity=" << settingsPanel_.pendingMouseSensitivity << "\n";
    out << "invert_mouse=" << (settingsPanel_.pendingInvertMouse ? 1 : 0) << "\n";
    out << "camera_stiffness=" << settingsPanel_.pendingCameraStiffness << "\n";
    out << "camera_follow_style=" << settingsPanel_.pendingCameraFollowStyle << "\n";
    out << "camera_max_distance=" << settingsPanel_.pendingCameraMaxDistance << "\n";
    out << "camera_pivot_height=" << settingsPanel_.pendingPivotHeight << "\n";
    out << "camera_smooth_follow=" << (settingsPanel_.pendingSmoothCameraFollow ? 1 : 0) << "\n";
    out << "gamepad=" << (settingsPanel_.pendingGamepadEnabled ? 1 : 0) << "\n";
    out << "gamepad_look_speed=" << settingsPanel_.pendingGamepadLookSpeed << "\n";
    out << "gamepad_invert_look=" << (settingsPanel_.pendingGamepadInvertLook ? 1 : 0) << "\n";
    out << "gamepad_deadzone=" << settingsPanel_.pendingGamepadDeadzone << "\n";
    out << "fov=" << settingsPanel_.pendingFov << "\n";
    out << "camera_shake=" << settingsPanel_.pendingCameraShake << "\n";

    // Quest tracker position/size

    // Chat
    out << "chat_timestamps=" << (chatPanel_.chatShowTimestamps ? 1 : 0) << "\n";
    out << "chat_font_size=" << chatPanel_.chatFontSize << "\n";
    out << "chat_autojoin_general=" << (chatPanel_.chatAutoJoinGeneral ? 1 : 0) << "\n";
    out << "chat_autojoin_trade=" << (chatPanel_.chatAutoJoinTrade ? 1 : 0) << "\n";
    out << "chat_autojoin_localdefense=" << (chatPanel_.chatAutoJoinLocalDefense ? 1 : 0) << "\n";
    out << "chat_autojoin_lfg=" << (chatPanel_.chatAutoJoinLFG ? 1 : 0) << "\n";
    out << "chat_autojoin_local=" << (chatPanel_.chatAutoJoinLocal ? 1 : 0) << "\n";
    out << "chat_bubbles=" << (chatPanel_.chatShowBubbles ? 1 : 0) << "\n";
    out << "chat_bubbles_party=" << (chatPanel_.chatPartyBubbles ? 1 : 0) << "\n";

    out.close();

    LOG_INFO("Settings saved to ", path);
}

namespace {
/// What the window scale's own row accepts, so the loader cannot clamp to a
/// range the control no longer has.
std::pair<float, float> windowUiScaleRange() {
    float lo = 0.75f, hi = 3.0f;
    settingRange("windowuiscale", lo, hi);
    return {lo, hi};
}
}  // namespace

void GameScreen::loadSettings() {
    std::string path = SettingsPanel::getSettingsPath();
    std::ifstream in(path);
    float currentDisplayHeight = settingsPanel_.pendingResolutionHeight;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().DisplaySize.y > 0.0f) {
        currentDisplayHeight = ImGui::GetIO().DisplaySize.y;
    }
    settingsPanel_.pendingBagScale =
        SettingsPanel::recommendedPixelScale(currentDisplayHeight, 0.75f, 1.5f);
    settingsPanel_.pendingWindowUiScale =
        SettingsPanel::recommendedPixelScale(currentDisplayHeight, 0.75f, 1.5f);
    // The action bar's slots are 48 pixels times this, and nothing else moves
    // them - the buff bar beside them scales itself with the screen height, and
    // the bars did not. Only the default: a saved value still wins, so nobody's
    // bars move under them. Scaling inside the panel the way the buff bar does
    // would be the better shape and would change what an existing 1.0 means.
    settingsPanel_.pendingActionBarScale =
        SettingsPanel::recommendedPixelScale(currentDisplayHeight, 0.5f, 2.0f);
    if (!in.is_open()) return;

    bool bagScaleLoaded = false;
    bool windowScaleLoaded = false;
    bool actionBarScaleLoaded = false;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        try {
            // Interface
            if (key == "ui_opacity") {
                int v = std::stoi(val);
                if (v >= 20 && v <= 100) {
                    settingsPanel_.pendingUiOpacity = v;
                    settingsPanel_.uiOpacity_ = static_cast<float>(v) / 100.0f;
                }
            } else if (key == "window_ui_scale") {
                settingsPanel_.pendingWindowUiScale =
                    std::clamp(std::stof(val), windowUiScaleRange().first,
                               windowUiScaleRange().second);
                windowScaleLoaded = true;
            } else if (key == "scroll_speed") {
                settingsPanel_.pendingScrollSpeed = std::clamp(std::stof(val), 0.25f, 4.0f);
            } else if (key == "minimap_rotate") {
                // Honoured since 2026-09-06. It was read and dropped before,
                // every run starting north-up, until the rotated map had been
                // looked at on screen; the minimap's own menu had been turning
                // it within a session all along, and the row now saves it.
                const bool rotate = (std::stoi(val) != 0);
                settingsPanel_.minimapRotate_ = rotate;
                settingsPanel_.pendingMinimapRotate = rotate;
            } else if (key == "minimap_square") {
                int v = std::stoi(val);
                settingsPanel_.minimapSquare_ = (v != 0);
                settingsPanel_.pendingMinimapSquare = settingsPanel_.minimapSquare_;
            } else if (key == "minimap_npc_dots") {
                int v = std::stoi(val);
                settingsPanel_.minimapNpcDots_ = (v != 0);
                settingsPanel_.pendingMinimapNpcDots = settingsPanel_.minimapNpcDots_;
            } else if (key == "show_minimap_clock") {
                settingsPanel_.showMinimapClock_ = (std::stoi(val) != 0);
                settingsPanel_.pendingShowMinimapClock = settingsPanel_.showMinimapClock_;
            } else if (key == "show_minimap_coordinates") {
                settingsPanel_.showMinimapCoordinates_ = (std::stoi(val) != 0);
                settingsPanel_.pendingShowMinimapCoordinates = settingsPanel_.showMinimapCoordinates_;
            } else if (key == "show_latency_meter") {
                settingsPanel_.showLatencyMeter_ = (std::stoi(val) != 0);
                settingsPanel_.pendingShowLatencyMeter = settingsPanel_.showLatencyMeter_;
            } else if (key == "show_dps_meter") {
                settingsPanel_.showDPSMeter_ = (std::stoi(val) != 0);
            } else if (key == "dps_meter_left") {
                settingsPanel_.dpsMeterLeft_ = std::stof(val);
            } else if (key == "dps_meter_top") {
                settingsPanel_.dpsMeterTop_ = std::stof(val);
            } else if (key == "show_cooldown_tracker") {
                settingsPanel_.showCooldownTracker_ = (std::stoi(val) != 0);
            } else if (key == "show_rare_tracker") {
                settingsPanel_.showRareTracker_ = (std::stoi(val) != 0);
            } else if (key == "map_window") {
                settingsPanel_.showMapWindow_ = (std::stoi(val) != 0);
            } else if (key == "show_chest_tracker") {
                settingsPanel_.showChestTracker_ = (std::stoi(val) != 0);
            } else if (key == "separate_bags") {
                settingsPanel_.pendingSeparateBags = (std::stoi(val) != 0);
            } else if (key == "show_keyring") {
                settingsPanel_.pendingShowKeyring = (std::stoi(val) != 0);
            } else if (key == "bag_scale") {
                settingsPanel_.pendingBagScale = std::clamp(std::stof(val), 0.75f, 1.5f);
                bagScaleLoaded = true;
            } else if (key == "chat_box_visible") {
                settingsPanel_.pendingChatBoxVisible = (std::stoi(val) != 0);
            } else if (key == "show_micro_menu") {
                settingsPanel_.pendingShowMicroMenu = (std::stoi(val) != 0);
            } else if (key == "idle_camera_orbit") {
                settingsPanel_.pendingIdleCameraOrbit = (std::stoi(val) != 0);
            } else if (key == "buff_bar_scale") {
                settingsPanel_.pendingBuffBarScale = std::clamp(std::stof(val), 0.75f, 1.5f);
            } else if (key == "action_bar_scale") {
                settingsPanel_.pendingActionBarScale = std::clamp(std::stof(val), 0.5f, 2.0f);
                actionBarScaleLoaded = true;
            } else if (key == "nameplate_scale") {
                settingsPanel_.nameplateScale_ = std::clamp(std::stof(val), 0.5f, 2.0f);
            } else if (key == "show_friendly_nameplates") {
                settingsPanel_.showFriendlyNameplates_ = (std::stoi(val) != 0);
            } else if (key == "show_enemy_nameplates") {
                settingsPanel_.showEnemyNameplates_ = (std::stoi(val) != 0);
            } else if (key == "show_action_bar2") {
                settingsPanel_.pendingShowActionBar2 = (std::stoi(val) != 0);
            } else if (key == "action_bar2_offset_x") {
                settingsPanel_.pendingActionBar2OffsetX = std::clamp(std::stof(val), -600.0f, 600.0f);
            } else if (key == "action_bar2_offset_y") {
                settingsPanel_.pendingActionBar2OffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "show_right_bar") {
                settingsPanel_.pendingShowRightBar = (std::stoi(val) != 0);
            } else if (key == "show_left_bar") {
                settingsPanel_.pendingShowLeftBar = (std::stoi(val) != 0);
            } else if (key == "right_bar_offset_y") {
                settingsPanel_.pendingRightBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "left_bar_offset_y") {
                settingsPanel_.pendingLeftBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "damage_flash") {
                settingsPanel_.damageFlashEnabled_ = (std::stoi(val) != 0);
            } else if (key == "low_health_vignette") {
                settingsPanel_.lowHealthVignetteEnabled_ = (std::stoi(val) != 0);
            }
            // Audio
            else if (key == "sound_muted") {
                settingsPanel_.soundMuted_ = (std::stoi(val) != 0);
                if (settingsPanel_.soundMuted_) {
                    // Apply mute on load; settingsPanel_.preMuteVolume_ will be set when AudioEngine is available
                    audio::AudioEngine::instance().setMasterVolume(0.0f);
                }
            }
            else if (key == "sound_in_background") settingsPanel_.pendingSoundInBackground = (val != "0");
            else if (key == "enable_music") settingsPanel_.pendingEnableMusic = (val != "0");
            else if (key == "loop_music") settingsPanel_.pendingLoopMusic = (val != "0");
            else if (key == "enable_ambience") settingsPanel_.pendingEnableAmbience = (val != "0");
            else if (key == "error_speech") settingsPanel_.pendingErrorSpeech = (val != "0");
            else if (key == "enable_sound_effects") settingsPanel_.pendingEnableSoundEffects = (val != "0");
            else if (key == "use_original_soundtrack") settingsPanel_.pendingUseOriginalSoundtrack = (std::stoi(val) != 0);
            else if (key == "master_volume") settingsPanel_.pendingMasterVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "effects_volume") settingsPanel_.pendingEffectsVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "music_volume") settingsPanel_.pendingMusicVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ambient_volume") settingsPanel_.pendingAmbientVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "bell_volume") settingsPanel_.pendingBellVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ui_volume") settingsPanel_.pendingUiVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "combat_volume") settingsPanel_.pendingCombatVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "spell_volume") settingsPanel_.pendingSpellVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "movement_volume") settingsPanel_.pendingMovementVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "footstep_volume") settingsPanel_.pendingFootstepVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "npc_voice_volume") settingsPanel_.pendingNpcVoiceVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "mount_volume") settingsPanel_.pendingMountVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "activity_volume") settingsPanel_.pendingActivityVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "character_speech") settingsPanel_.pendingCharacterSpeech = (std::stoi(val) != 0);
            // Gameplay / display pacing
            else if (key == "fullscreen") {
                settingsPanel_.pendingFullscreen = (std::stoi(val) != 0);
                settingsPanel_.displaySettingsLoaded_ = true;
            }
            else if (key == "resolution_width") {
                settingsPanel_.pendingResolutionWidth = std::clamp(std::stoi(val), 640, 7680);
                settingsPanel_.displaySettingsLoaded_ = true;
            }
            else if (key == "resolution_height") {
                settingsPanel_.pendingResolutionHeight = std::clamp(std::stoi(val), 480, 4320);
                settingsPanel_.displaySettingsLoaded_ = true;
            }
            else if (key == "vsync") settingsPanel_.pendingVsync = (std::stoi(val) != 0);
            else if (key == "auto_loot") settingsPanel_.pendingAutoLoot = (std::stoi(val) != 0);
            else if (key == "auto_sell_grey") settingsPanel_.pendingAutoSellGrey = (std::stoi(val) != 0);
            else if (key == "auto_repair") settingsPanel_.pendingAutoRepair = (std::stoi(val) != 0);
            else if (key == "secure_ability_toggle") settingsPanel_.pendingSecureAbilityToggle = (std::stoi(val) != 0);
            else if (key == "auto_face_target") settingsPanel_.pendingAutoFaceTarget = (std::stoi(val) != 0);
            else if (key == "graphics_preset") {
                int presetVal = std::clamp(std::stoi(val), 0,
                                           static_cast<int>(SettingsPanel::GraphicsPreset::CLASSIC));
                settingsPanel_.currentGraphicsPreset = static_cast<SettingsPanel::GraphicsPreset>(presetVal);
                settingsPanel_.pendingGraphicsPreset = settingsPanel_.currentGraphicsPreset;
            }
            else if (key == "ground_clutter_density") settingsPanel_.pendingGroundClutterDensity = std::clamp(std::stoi(val), 0, 150);
            else if (key == "ground_clutter_distance") settingsPanel_.pendingGroundClutterDistance = std::clamp(std::stoi(val), 70, 300);
            else if (key == "particle_density") settingsPanel_.pendingParticleDensity = std::clamp(std::stoi(val), 10, 100);
            else if (key == "weather_detail") settingsPanel_.pendingWeatherDetail = std::clamp(std::stoi(val), 0, 3);
            else if (key == "environment_detail") settingsPanel_.pendingEnvironmentDetail = std::clamp(std::stoi(val), 50, 150);
            else if (key == "texture_filtering") settingsPanel_.pendingTextureFiltering = std::clamp(std::stoi(val), 0, 4);
            else if (key == "grass_enabled") settingsPanel_.pendingGrassEnabled = (std::stoi(val) != 0);
            else if (key == "grass_density") settingsPanel_.pendingGrassDensity = std::clamp(std::stoi(val), 0, 300);
            else if (key == "grass_height") settingsPanel_.pendingGrassHeight = std::clamp(std::stoi(val), 50, 300);
            else if (key == "grass_distance") settingsPanel_.pendingGrassDistance = std::clamp(std::stoi(val), 30, 800);
            else if (key == "shadows") settingsPanel_.pendingShadows = (std::stoi(val) != 0);
            else if (key == "shadow_distance") settingsPanel_.pendingShadowDistance = std::clamp(std::stof(val), 40.0f, 500.0f);
            else if (key == "shadow_strength") settingsPanel_.pendingShadowStrength = std::clamp(std::stof(val), 0.5f, 1.5f);
            else if (key == "foliage_wind") settingsPanel_.pendingFoliageWind = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "view_distance") settingsPanel_.pendingViewDistance = std::clamp(std::stof(val), 400.0f, 2400.0f);
            else if (key == "fog_sky_blend") settingsPanel_.pendingFogSkyBlend = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "fog_strength") settingsPanel_.pendingFogStrength = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "light_shafts") settingsPanel_.pendingVolumetricFog = std::clamp(std::stoi(val), 0, 3);
            else if (key == "ray_traced_lighting") settingsPanel_.pendingRtLighting = std::clamp(std::stoi(val), 0, 3);
            else if (key == "mist_density") settingsPanel_.pendingVolumetricDensity = std::clamp(std::stof(val), 0.0f, 3.0f);
            else if (key == "sharp_stars") settingsPanel_.pendingSharpStars = (val == "1");
            else if (key == "sun_shafts") settingsPanel_.pendingSunShafts = (val == "1");
            else if (key == "shoreline_foam") settingsPanel_.pendingShorelineFoam = (val == "1");
            else if (key == "wet_ground") settingsPanel_.pendingWetGround = (val == "1");
            // No apply here either: brightness is on the graphics load list,
            // which is walked once the renderer exists. This branch ran from
            // the constructor, where services_.renderer is still null.
            else if (key == "brightness") settingsPanel_.pendingBrightness = std::clamp(std::stoi(val), 0, 100);
            else if (key == "water_refraction") settingsPanel_.pendingWaterRefraction = (std::stoi(val) != 0);
            else if (key == "antialiasing") settingsPanel_.pendingAntiAliasing = std::clamp(std::stoi(val), 0, 3);
            else if (key == "fxaa") settingsPanel_.pendingFXAA = (std::stoi(val) != 0);
            else if (key == "check_for_updates") settingsPanel_.pendingCheckForUpdates = (std::stoi(val) != 0);
            else if (key == "content_packs") settingsPanel_.pendingContentPacks = (std::stoi(val) != 0);
            else if (key == "normal_mapping") settingsPanel_.pendingNormalMapping = (std::stoi(val) != 0);
            else if (key == "normal_map_strength") settingsPanel_.pendingNormalMapStrength = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "lens_flare") settingsPanel_.pendingLensFlare = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "frame_cap") settingsPanel_.pendingFrameCap = std::clamp(std::stoi(val), 0, 6);
            else if (key == "pom") settingsPanel_.pendingPOM = (std::stoi(val) != 0);
            else if (key == "pom_quality") settingsPanel_.pendingPOMQuality = std::clamp(std::stoi(val), 0, 2);
            else if (key == "upscaling_mode") {
                settingsPanel_.pendingUpscalingMode = std::clamp(std::stoi(val), 0, 2);
                settingsPanel_.pendingFSR = (settingsPanel_.pendingUpscalingMode == 1);
            } else if (key == "fsr") {
                settingsPanel_.pendingFSR = (std::stoi(val) != 0);
                // Backward compatibility: old configs only had fsr=0/1.
                if (settingsPanel_.pendingUpscalingMode == 0 && settingsPanel_.pendingFSR) settingsPanel_.pendingUpscalingMode = 1;
            }
            else if (key == "fsr_quality") settingsPanel_.pendingFSRQuality = std::clamp(std::stoi(val), 0, 3);
            else if (key == "fsr_sharpness") settingsPanel_.pendingFSRSharpness = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "fsr2_jitter_sign") settingsPanel_.pendingFSR2JitterSign = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_x") settingsPanel_.pendingFSR2MotionVecScaleX = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_y") settingsPanel_.pendingFSR2MotionVecScaleY = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "amd_fsr3_framegen") settingsPanel_.pendingAMDFramegen = (std::stoi(val) != 0);
            // Controls
            else if (key == "mouse_sensitivity") settingsPanel_.pendingMouseSensitivity = std::clamp(std::stof(val), 0.05f, 1.0f);
            else if (key == "invert_mouse") settingsPanel_.pendingInvertMouse = (std::stoi(val) != 0);
            else if (key == "camera_stiffness") settingsPanel_.pendingCameraStiffness = std::clamp(std::stof(val), 5.0f, 100.0f);
            else if (key == "camera_follow_style") settingsPanel_.pendingCameraFollowStyle = std::clamp(std::stoi(val), 0, 3);
            else if (key == "camera_max_distance") settingsPanel_.pendingCameraMaxDistance = std::clamp(std::stoi(val), 22, 50);
            else if (key == "camera_pivot_height") settingsPanel_.pendingPivotHeight = std::clamp(std::stof(val), 0.0f, 3.0f);
            else if (key == "camera_smooth_follow") settingsPanel_.pendingSmoothCameraFollow = (std::stoi(val) != 0);
            else if (key == "gamepad") settingsPanel_.pendingGamepadEnabled = (std::stoi(val) != 0);
            else if (key == "gamepad_look_speed") settingsPanel_.pendingGamepadLookSpeed = std::clamp(std::stof(val), 60.0f, 540.0f);
            else if (key == "gamepad_invert_look") settingsPanel_.pendingGamepadInvertLook = (std::stoi(val) != 0);
            else if (key == "gamepad_deadzone") settingsPanel_.pendingGamepadDeadzone = std::clamp(std::stof(val), 0.0f, 0.5f);
            // No apply here: this runs from the constructor, where there is
            // no renderer to hand it to. applyCameraControlSettings does it,
            // and this function ends by calling it.
            else if (key == "fov") settingsPanel_.pendingFov = std::clamp(std::stof(val), 45.0f, 110.0f);
            else if (key == "camera_shake") settingsPanel_.pendingCameraShake = std::clamp(std::stof(val), 0.0f, 1.0f);
            // Quest tracker position/size
            // Chat
            else if (key == "chat_timestamps") chatPanel_.chatShowTimestamps = (std::stoi(val) != 0);
            else if (key == "chat_font_size") chatPanel_.chatFontSize = std::clamp(std::stoi(val), 0, 2);
            else if (key == "chat_autojoin_general") chatPanel_.chatAutoJoinGeneral = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_trade") chatPanel_.chatAutoJoinTrade = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_localdefense") chatPanel_.chatAutoJoinLocalDefense = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_lfg") chatPanel_.chatAutoJoinLFG = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_local") chatPanel_.chatAutoJoinLocal = (std::stoi(val) != 0);
            else if (key == "chat_bubbles") chatPanel_.chatShowBubbles = (std::stoi(val) != 0);
            else if (key == "chat_bubbles_party") chatPanel_.chatPartyBubbles = (std::stoi(val) != 0);
        } catch (...) {}
    }

    if (!bagScaleLoaded || !windowScaleLoaded || !actionBarScaleLoaded) {
        // The height the file turned out to hold, now that it has been read -
        // this runs from the constructor, where there is no display to ask.
        const float defaultHeight = settingsPanel_.displaySettingsLoaded_
            ? static_cast<float>(settingsPanel_.pendingResolutionHeight)
            : currentDisplayHeight;
        if (!bagScaleLoaded) {
            settingsPanel_.pendingBagScale =
                SettingsPanel::recommendedPixelScale(defaultHeight, 0.75f, 1.5f);
        }
        if (!windowScaleLoaded) {
            settingsPanel_.pendingWindowUiScale =
                SettingsPanel::recommendedPixelScale(defaultHeight, 0.75f, 1.5f);
        }
        if (!actionBarScaleLoaded) {
            settingsPanel_.pendingActionBarScale =
                SettingsPanel::recommendedPixelScale(defaultHeight, 0.5f, 2.0f);
        }
    }

    // Apply immediately if services are already wired. The constructor loads
    // settings before renderer injection, so setServices() applies them again.
    applyCameraControlSettings();
    settingsPanel_.applyLoadedSettings();

    LOG_INFO("Settings loaded from ", path);
}

// ============================================================
// Mail Window
// ============================================================



// ============================================================
// Bank Window
// ============================================================


// ============================================================
// Guild Bank Window
// ============================================================


// ============================================================
// Auction House Window
// ============================================================



// ---------------------------------------------------------------------------
// Dungeon Finder window (toggle with hotkey or bag-bar button)
// ---------------------------------------------------------------------------
// ============================================================
// Instance Lockouts
// ============================================================




// ─── Threat Window ────────────────────────────────────────────────────────────
// ─── BG Scoreboard ────────────────────────────────────────────────────────────






}} // namespace wowee::ui
