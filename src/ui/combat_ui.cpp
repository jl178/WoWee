// ============================================================
// CombatUI - extracted from GameScreen
// Owns all combat-related UI rendering: cast bar, cooldown tracker,
// raid warning overlay, combat text, DPS meter, buff bar,
// battleground score HUD, combat log, threat window, BG scoreboard.
// ============================================================
#include "ui/combat_ui.hpp"
#include "ui/interface_fonts.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/buff_bar_layout.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/settings_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "game/game_handler.hpp"
#include "game/bg_score_defs.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;
} // anonymous namespace

namespace wowee {
namespace ui {


// ============================================================
// Cast Bar
// ============================================================


// ============================================================
// Raid Warning / Boss Emote Center-Screen Overlay
// ============================================================

// ============================================================================
// The sound a whisper makes
// ============================================================================
//
// What is left of the raid warning overlay. RaidWarningFrame is FrameXML's and
// answers CHAT_MSG_RAID_WARNING itself, so the banner went; the walk through
// new chat lines stayed, because the whisper sound was being played from
// inside it and belongs to no element - FrameXML plays none for a whisper.
void CombatUI::playSoundsForNewChat(game::GameHandler& gameHandler) {
    const auto& chatHistory = gameHandler.getChatHistory();
    const size_t newCount = chatHistory.size();
    if (newCount <= raidWarnChatSeenCount_) return;

    for (size_t i = raidWarnChatSeenCount_; i < newCount; ++i) {
        if (chatHistory[i].type != game::ChatType::WHISPER) continue;
        if (auto* ac = services_.audioCoordinator) {
            if (auto* ui = ac->getUiSoundManager()) ui->playWhisperReceived();
        }
    }
    raidWarnChatSeenCount_ = newCount;
}


// ============================================================
// Floating Combat Text
// ============================================================

// Not gated, and there is no element to gate it on.
//
// FrameXML's own floating combat text lives in Blizzard_CombatText, which is
// load-on-demand and reached from exactly one place: the float-mode dropdown in
// the Interface Options combat panel, which calls UIParentLoadAddOn for it. So
// it is absent until a player touches that setting, and drawing beside it after
// that is a latent double rather than a live one.
//
// Left alone deliberately. Covering it means a new UiElement and a suppression
// entry, which is a decision about what this interface owns rather than a fix
// to something broken - and the client's combat text is the one on screen today.
void CombatUI::renderCombatText(game::GameHandler& gameHandler) {
    const auto& entries = gameHandler.getCombatText();
    if (entries.empty()) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    // Camera for world-space projection
    auto* appRenderer = services_.renderer;
    rendering::Camera* camera = appRenderer ? appRenderer->getCamera() : nullptr;
    glm::mat4 viewProj;
    if (camera) viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    // DAMAGE_TEXT_FONT at COMBAT_TEXT_HEIGHT 25, in the stock UI's 768-tall
    // units (Blizzard_CombatText.lua); this drew the 15px interface font.
    ImFont* font = interfaceFace("frizqt__.ttf");
    if (!font) font = ImGui::GetFont();
    const float uiPx = screenH / 768.0f;
    const float baseFontSize = 25.0f * uiPx;

    // HUD fallback: entries without world-space anchor use classic screen-position layout.
    // We still need an ImGui window for those.
    const float hudIncomingX = screenW * 0.40f;
    const float hudOutgoingX = screenW * 0.68f;
    int hudInIdx = 0, hudOutIdx = 0;
    bool needsHudWindow = false;

    // What kind of number the player asked to see.
    //
    // The interface offers eighteen of these and read none of them, so every
    // kind was drawn together. Three cover what this client actually produces;
    // the rest name things it does not draw at all - reputation changes, aura
    // gains, combo points - and there is nothing yet for them to filter.
    //
    // Read once for the frame rather than per entry: a busy fight puts dozens
    // through this loop and the answer cannot change inside it.
    const bool showDamage =
        addons::storedCVarValue("fctDamage", "1") != "0";
    const bool showHealing =
        addons::storedCVarValue("fctHealing", "1") != "0";
    const bool showMisses =
        addons::storedCVarValue("fctDodgeParryMiss", "1") != "0";

    const auto wanted = [&](game::CombatTextEntry::Type t) {
        using T = game::CombatTextEntry;
        switch (t) {
            case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::CRIT_DAMAGE:
            case T::PERIODIC_DAMAGE: case T::ENVIRONMENTAL: case T::GLANCING:
            case T::CRUSHING:
                return showDamage;
            case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
                return showHealing;
            case T::MISS: case T::DODGE: case T::PARRY: case T::BLOCK:
            case T::EVADE: case T::IMMUNE: case T::DEFLECT: case T::REFLECT:
            case T::RESIST: case T::ABSORB:
                return showMisses;
            default:
                // Everything else - energy, experience, honour, dispels - has
                // no switch of its own here and is left alone rather than
                // hidden by somebody else's.
                return true;
        }
    };

    for (const auto& entry : entries) {
        if (!wanted(entry.type)) continue;
        const float alpha = 1.0f - (entry.age / game::CombatTextEntry::LIFETIME);
        const bool outgoing = entry.isPlayerSource;

        // --- Format text and color (identical logic for both world and HUD paths) ---
        ImVec4 color;
        char text[128];
        switch (entry.type) {
            case game::CombatTextEntry::MELEE_DAMAGE:
            case game::CombatTextEntry::SPELL_DAMAGE:
                snprintf(text, sizeof(text), "-%d", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 1.0f, 0.3f, alpha) :
                    ImVec4(1.0f, 0.3f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::CRIT_DAMAGE:
                snprintf(text, sizeof(text), "-%d!", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 0.8f, 0.0f, alpha) :
                    ImVec4(1.0f, 0.5f, 0.0f, alpha);
                break;
            case game::CombatTextEntry::HEAL:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::CRIT_HEAL:
                snprintf(text, sizeof(text), "+%d!", entry.amount);
                color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::MISS:
                snprintf(text, sizeof(text), "Miss");
                color = ImVec4(0.7f, 0.7f, 0.7f, alpha);
                break;
            case game::CombatTextEntry::DODGE:
                snprintf(text, sizeof(text), outgoing ? "Dodge" : "You Dodge");
                color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::PARRY:
                snprintf(text, sizeof(text), outgoing ? "Parry" : "You Parry");
                color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::BLOCK:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), outgoing ? "Block %d" : "You Block %d", entry.amount);
                else
                    snprintf(text, sizeof(text), outgoing ? "Block" : "You Block");
                color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::EVADE:
                snprintf(text, sizeof(text), outgoing ? "Evade" : "You Evade");
                color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::PERIODIC_DAMAGE:
                snprintf(text, sizeof(text), "-%d", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 0.9f, 0.3f, alpha) :
                    ImVec4(1.0f, 0.4f, 0.4f, alpha);
                break;
            case game::CombatTextEntry::PERIODIC_HEAL:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                color = ImVec4(0.4f, 1.0f, 0.5f, alpha);
                break;
            case game::CombatTextEntry::ENVIRONMENTAL: {
                const char* envLabel = "";
                switch (entry.powerType) {
                    case 0: envLabel = "Fatigue "; break;
                    case 1: envLabel = "Drowning "; break;
                    case 2: envLabel = ""; break;
                    case 3: envLabel = "Lava "; break;
                    case 4: envLabel = "Slime "; break;
                    case 5: envLabel = "Fire "; break;
                    default: envLabel = ""; break;
                }
                snprintf(text, sizeof(text), "%s-%d", envLabel, entry.amount);
                color = ImVec4(0.9f, 0.5f, 0.2f, alpha);
                break;
            }
            case game::CombatTextEntry::ENERGIZE:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                switch (entry.powerType) {
                    case 1:  color = ImVec4(1.0f, 0.2f, 0.2f, alpha); break;
                    case 2:  color = ImVec4(1.0f, 0.6f, 0.1f, alpha); break;
                    case 3:  color = ImVec4(1.0f, 0.9f, 0.2f, alpha); break;
                    case 6:  color = ImVec4(0.3f, 0.9f, 0.8f, alpha); break;
                    default: color = ImVec4(0.3f, 0.6f, 1.0f, alpha); break;
                }
                break;
            case game::CombatTextEntry::POWER_DRAIN:
                snprintf(text, sizeof(text), "-%d", entry.amount);
                switch (entry.powerType) {
                    case 1:  color = ImVec4(1.0f, 0.35f, 0.35f, alpha); break;
                    case 2:  color = ImVec4(1.0f, 0.7f, 0.2f, alpha); break;
                    case 3:  color = ImVec4(1.0f, 0.95f, 0.35f, alpha); break;
                    case 6:  color = ImVec4(0.45f, 0.95f, 0.85f, alpha); break;
                    default: color = ImVec4(0.45f, 0.75f, 1.0f, alpha); break;
                }
                break;
            case game::CombatTextEntry::XP_GAIN:
                snprintf(text, sizeof(text), "+%d XP", entry.amount);
                color = ImVec4(0.7f, 0.3f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::IMMUNE:
                snprintf(text, sizeof(text), "Immune!");
                color = ImVec4(0.9f, 0.9f, 0.9f, alpha);
                break;
            case game::CombatTextEntry::ABSORB:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), "Absorbed %d", entry.amount);
                else
                    snprintf(text, sizeof(text), "Absorbed");
                color = ImVec4(0.5f, 0.8f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::RESIST:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), "Resisted %d", entry.amount);
                else
                    snprintf(text, sizeof(text), "Resisted");
                color = ImVec4(0.7f, 0.7f, 0.7f, alpha);
                break;
            case game::CombatTextEntry::DEFLECT:
                snprintf(text, sizeof(text), outgoing ? "Deflect" : "You Deflect");
                color = outgoing ? ImVec4(0.7f, 0.7f, 0.7f, alpha)
                                 : ImVec4(0.5f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::REFLECT: {
                const std::string& reflectName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!reflectName.empty())
                    snprintf(text, sizeof(text), outgoing ? "Reflected: %s" : "Reflect: %s", reflectName.c_str());
                else
                    snprintf(text, sizeof(text), outgoing ? "Reflected" : "You Reflect");
                color = outgoing ? ImVec4(0.85f, 0.75f, 1.0f, alpha)
                                 : ImVec4(0.75f, 0.85f, 1.0f, alpha);
                break;
            }
            case game::CombatTextEntry::PROC_TRIGGER: {
                const std::string& procName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!procName.empty())
                    snprintf(text, sizeof(text), "%s!", procName.c_str());
                else
                    snprintf(text, sizeof(text), "PROC!");
                color = ImVec4(1.0f, 0.85f, 0.0f, alpha);
                break;
            }
            case game::CombatTextEntry::DISPEL:
                if (entry.spellId != 0) {
                    const std::string& dispelledName = gameHandler.getSpellName(entry.spellId);
                    if (!dispelledName.empty())
                        snprintf(text, sizeof(text), "Dispel %s", dispelledName.c_str());
                    else
                        snprintf(text, sizeof(text), "Dispel");
                } else {
                    snprintf(text, sizeof(text), "Dispel");
                }
                color = ImVec4(0.6f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::STEAL:
                if (entry.spellId != 0) {
                    const std::string& stolenName = gameHandler.getSpellName(entry.spellId);
                    if (!stolenName.empty())
                        snprintf(text, sizeof(text), "Spellsteal %s", stolenName.c_str());
                    else
                        snprintf(text, sizeof(text), "Spellsteal");
                } else {
                    snprintf(text, sizeof(text), "Spellsteal");
                }
                color = ImVec4(0.8f, 0.7f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::INTERRUPT: {
                const std::string& interruptedName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!interruptedName.empty())
                    snprintf(text, sizeof(text), "Interrupt %s", interruptedName.c_str());
                else
                    snprintf(text, sizeof(text), "Interrupt");
                color = ImVec4(1.0f, 0.6f, 0.9f, alpha);
                break;
            }
            case game::CombatTextEntry::INSTAKILL:
                snprintf(text, sizeof(text), outgoing ? "Kill!" : "Killed!");
                color = outgoing ? ImVec4(1.0f, 0.25f, 0.25f, alpha)
                                 : ImVec4(1.0f, 0.1f, 0.1f, alpha);
                break;
            case game::CombatTextEntry::HONOR_GAIN:
                snprintf(text, sizeof(text), "+%d Honor", entry.amount);
                color = ImVec4(1.0f, 0.85f, 0.0f, alpha);
                break;
            case game::CombatTextEntry::GLANCING:
                snprintf(text, sizeof(text), "~%d", entry.amount);
                color = outgoing ?
                    ImVec4(0.75f, 0.75f, 0.5f, alpha) :
                    ImVec4(0.75f, 0.35f, 0.35f, alpha);
                break;
            case game::CombatTextEntry::CRUSHING:
                snprintf(text, sizeof(text), "%d!", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 0.55f, 0.1f, alpha) :
                    ImVec4(1.0f, 0.15f, 0.15f, alpha);
                break;
            default:
                snprintf(text, sizeof(text), "%d", entry.amount);
                color = ImVec4(1.0f, 1.0f, 1.0f, alpha);
                break;
        }

        // --- Rendering style ---
        bool isCrit = (entry.type == game::CombatTextEntry::CRIT_DAMAGE ||
                       entry.type == game::CombatTextEntry::CRIT_HEAL);
        float renderFontSize = baseFontSize;
        if (isCrit) {  // the stock crit pop: 30 -> 60 in 0.05s, back to 30 by 0.2s
            constexpr float kMin = 30.0f, kMax = 60.0f, kUp = 0.05f, kDown = 0.2f;
            const float t = entry.age;
            float h = kMin;
            if (t <= kUp) h = kMin + (kMax - kMin) * (t / kUp);
            else if (t <= kDown) h = kMax - (kMax - kMin) * ((t - kUp) / (kDown - kUp));
            renderFontSize = h * uiPx;
        }

        ImU32 shadowCol = IM_COL32(0, 0, 0, static_cast<int>(alpha * 180));
        ImU32 textCol   = ImGui::ColorConvertFloat4ToU32(color);

        // --- Try world-space anchor if we have a destination entity ---
        // Types that should always stay as HUD elements (no world anchor)
        bool isHudOnly = (entry.type == game::CombatTextEntry::XP_GAIN ||
                          entry.type == game::CombatTextEntry::HONOR_GAIN ||
                          entry.type == game::CombatTextEntry::PROC_TRIGGER);

        bool rendered = false;
        if (!isHudOnly && camera && entry.dstGuid != 0) {
            // Look up the destination entity's render position
            glm::vec3 renderPos;
            bool havePos = core::Application::getInstance().getRenderPositionForGuid(entry.dstGuid, renderPos);
            if (!havePos) {
                // Fallback to entity canonical position
                auto entity = gameHandler.getEntityManager().getEntity(entry.dstGuid);
                if (entity) {
                    auto* unit = entity->isUnit() ? static_cast<game::Unit*>(entity.get()) : nullptr;
                    if (unit) {
                        renderPos = core::coords::canonicalToRender(
                            glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
                        havePos = true;
                    }
                }
            }

            if (havePos) {
                // Float upward from above the entity's head
                renderPos.z += 2.5f + entry.age * 1.2f;

                // Project to screen
                glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
                if (clipPos.w > 0.01f) {
                    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                    if (ndc.x >= -1.5f && ndc.x <= 1.5f && ndc.y >= -1.5f && ndc.y <= 1.5f) {
                        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
                        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

                        // Horizontal stagger using the random seed
                        sx += entry.xSeed * 40.0f;

                        // Center the text horizontally on the projected point
                        ImVec2 ts = font->CalcTextSizeA(renderFontSize, FLT_MAX, 0.0f, text);
                        sx -= ts.x * 0.5f;

                        // Clamp to screen bounds
                        sx = std::max(2.0f, std::min(sx, screenW - ts.x - 2.0f));

                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx + 1.0f, sy + 1.0f), shadowCol, text);
                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx, sy), textCol, text);
                        rendered = true;
                    }
                }
            }
        }

        // --- HUD fallback for entries without world anchor or HUD-only types ---
        if (!rendered) {
            if (!needsHudWindow) {
                needsHudWindow = true;
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(screenW, 400));
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
                ImGui::Begin("##CombatText", nullptr, flags);
            }

            float yOffset = 200.0f - entry.age * 60.0f;
            int& idx = outgoing ? hudOutIdx : hudInIdx;
            float baseX = outgoing ? hudOutgoingX : hudIncomingX;
            float xOffset = baseX + (idx % 3 - 1) * 60.0f;
            ++idx;

            ImGui::SetCursorPos(ImVec2(xOffset, yOffset));
            ImVec2 screenPos = ImGui::GetCursorScreenPos();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(font, renderFontSize, ImVec2(screenPos.x + 1.0f, screenPos.y + 1.0f),
                        shadowCol, text);
            dl->AddText(font, renderFontSize, screenPos, textCol, text);

            ImVec2 ts = font->CalcTextSizeA(renderFontSize, FLT_MAX, 0.0f, text);
            ImGui::Dummy(ts);
        }
    }

    if (needsHudWindow) {
        ImGui::End();
    }
}

} // namespace ui
} // namespace wowee
