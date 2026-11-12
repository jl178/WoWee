#include "ui/framexml_takeover.hpp"
#include "core/env_flag.hpp"
#include "game/game_handler.hpp"
#include "game/achievement_criteria.hpp"
#include "game/packed_time.hpp"
#include "game/global_strings.hpp"
#include "game/combat_log_event.hpp"
#include "game/stock_combat_log.hpp"
#include "game/protocol_constants.hpp"
#include "game/game_utils.hpp"
#include "game/chat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/social_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/warden_handler.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_memory.hpp"
#include "game/warden_module.hpp"
#include "game/packet_parsers.hpp"
#include "game/transport_manager.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/expansion_profile.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/spell_visual_system.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "auth/crypto.hpp"
#include "core/coordinates.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <random>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <openssl/sha.h>
#include <openssl/hmac.h>


namespace wowee {
namespace game {

namespace {

bool isAuthCharPipelineOpcode(LogicalOpcode op) {
    switch (op) {
        case Opcode::SMSG_AUTH_CHALLENGE:
        case Opcode::SMSG_AUTH_RESPONSE:
        case Opcode::SMSG_CLIENTCACHE_VERSION:
        case Opcode::SMSG_TUTORIAL_FLAGS:
        case Opcode::SMSG_WARDEN_DATA:
        case Opcode::SMSG_CHAR_ENUM:
        case Opcode::SMSG_CHAR_CREATE:
        case Opcode::SMSG_CHAR_DELETE:
            return true;
        default:
            return false;
    }
}

int incomingPacketsBudgetPerUpdate(WorldState state) {
    static const int inWorldBudget =
        core::envIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKETS", 24, 1, 512);
    static const int loginBudget =
        core::envIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKETS_LOGIN", 96, 1, 512);
    return state == WorldState::IN_WORLD ? inWorldBudget : loginBudget;
}

float incomingPacketBudgetMs(WorldState state) {
    static const int inWorldBudgetMs =
        core::envIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKET_MS", 2, 1, 50);
    static const int loginBudgetMs =
        core::envIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKET_MS_LOGIN", 8, 1, 50);
    return static_cast<float>(state == WorldState::IN_WORLD ? inWorldBudgetMs : loginBudgetMs);
}

float slowPacketLogThresholdMs() {
    static const int thresholdMs =
        core::envIntClamped("WOWEE_NET_SLOW_PACKET_LOG_MS", 10, 1, 60000);
    return static_cast<float>(thresholdMs);
}

constexpr size_t kMaxQueuedInboundPackets = 4096;

} // end anonymous namespace

// The rune bar draws itself from these two events and from nothing else. All
// three rune packets stored their state and announced none of it, so a death
// knight's runes were drawn once at login and then stood still.
//
// "usable" is sent as a present argument or no argument at all rather than as
// one and zero, because zero is true in Lua: an argument saying the rune is on
// cooldown would read as one saying it is ready.
void GameHandler::fireRuneUpdate(uint32_t index) {
    if (index >= playerRunes_.size()) return;
    const std::string oneBased = std::to_string(index + 1);
    fireAddonEvent("RUNE_TYPE_UPDATE", {oneBased});
    if (playerRunes_[index].ready) {
        fireAddonEvent("RUNE_POWER_UPDATE", {oneBased, "1"});
    } else {
        fireAddonEvent("RUNE_POWER_UPDATE", {oneBased});
    }
}

// The opcodes this handler answers itself, grouped by what they are about: the
// session handshake, XP and exploration, corpses and combat state, guilds and
// loot and vendors, teleports and taxis and battlegrounds, world states, action
// buttons, levelling.
void GameHandler::registerCoreOpcodes() {
    // -----------------------------------------------------------------------
    // Auth / session / pre-world handshake
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_AUTH_CHALLENGE] = [this](network::Packet& packet) {
        if (state == WorldState::CONNECTED)
            handleAuthChallenge(packet);
        else
            LOG_WARNING("Unexpected SMSG_AUTH_CHALLENGE in state: ", worldStateName(state));
    };
    dispatchTable_[Opcode::SMSG_AUTH_RESPONSE] = [this](network::Packet& packet) {
        if (state == WorldState::AUTH_SENT)
            handleAuthResponse(packet);
        else
            LOG_WARNING("Unexpected SMSG_AUTH_RESPONSE in state: ", worldStateName(state));
    };
    dispatchTable_[Opcode::SMSG_CHAR_CREATE] = [this](network::Packet& packet) {
        handleCharCreateResponse(packet);
    };
    dispatchTable_[Opcode::SMSG_CHAR_DELETE] = [this](network::Packet& packet) {
        uint8_t result = packet.readUInt8();
        lastCharDeleteResult_ = result;
        pendingCharDeleteResponse_ = false;
        bool success = (result == 0x00 || result == 0x47);
        LOG_INFO("SMSG_CHAR_DELETE result: ", static_cast<int>(result), success ? " (success)" : " (failed)");
        requestCharacterList();
        std::string msg;
        if (success) {
            msg = "Character deleted.";
        } else {
            // Map known CHAR_DELETE_* result codes to user-friendly messages
            switch (result) {
                case 0x31: msg = "Delete failed: character is a guild leader. Transfer leadership first."; break;
                case 0x32: msg = "Delete failed: character is in an arena team."; break;
                case 0x3A: msg = "Delete failed: character has mail. Check mailbox first."; break;
                default:   msg = "Delete failed (server error code " + std::to_string(static_cast<int>(result)) + ")."; break;
            }
        }
        if (charDeleteCallback_) charDeleteCallback_(success, msg);
    };
    dispatchTable_[Opcode::SMSG_CHAR_ENUM] = [this](network::Packet& packet) {
        if (state == WorldState::CHAR_LIST_REQUESTED)
            handleCharEnum(packet);
        else
            LOG_WARNING("Unexpected SMSG_CHAR_ENUM in state: ", worldStateName(state));
    };
    registerHandler(Opcode::SMSG_CHARACTER_LOGIN_FAILED, &GameHandler::handleCharLoginFailed);
    dispatchTable_[Opcode::SMSG_LOGIN_VERIFY_WORLD] = [this](network::Packet& packet) {
        if (state == WorldState::ENTERING_WORLD || state == WorldState::IN_WORLD)
            handleLoginVerifyWorld(packet);
        else
            LOG_WARNING("Unexpected SMSG_LOGIN_VERIFY_WORLD in state: ", worldStateName(state));
    };
    registerHandler(Opcode::SMSG_LOGIN_SETTIMESPEED, &GameHandler::handleLoginSetTimeSpeed);
    registerHandler(Opcode::SMSG_CLIENTCACHE_VERSION, &GameHandler::handleClientCacheVersion);
    registerHandler(Opcode::SMSG_TUTORIAL_FLAGS, &GameHandler::handleTutorialFlags);
    registerHandler(Opcode::SMSG_ACCOUNT_DATA_TIMES, &GameHandler::handleAccountDataTimes);
    registerHandler(Opcode::SMSG_MOTD, &GameHandler::handleMotd);
    registerHandler(Opcode::SMSG_NOTIFICATION, &GameHandler::handleNotification);
    registerHandler(Opcode::SMSG_PONG, &GameHandler::handlePong);

    // -----------------------------------------------------------------------
    // World object updates + entity queries (delegated to EntityController)
    // -----------------------------------------------------------------------
    entityController_->registerOpcodes(dispatchTable_);

    // -----------------------------------------------------------------------
    // Item push / logout
    // -----------------------------------------------------------------------
    registerSkipHandler(Opcode::SMSG_ADDON_INFO);
    registerSkipHandler(Opcode::SMSG_EXPECTED_SPAM_RECORDS);

    // -----------------------------------------------------------------------
    // XP / exploration
    // -----------------------------------------------------------------------
    registerHandler(Opcode::SMSG_LOG_XPGAIN, &GameHandler::handleXpGain);
    dispatchTable_[Opcode::SMSG_EXPLORATION_EXPERIENCE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t areaId   = packet.readUInt32();
            uint32_t xpGained = packet.readUInt32();
            if (xpGained > 0) {
                std::string areaName = getAreaName(areaId);
                std::string msg;
                if (!areaName.empty()) {
                    msg = "Discovered " + areaName + "! Gained " + std::to_string(xpGained) + " experience.";
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "Discovered new area! Gained %u experience.", xpGained);
                    msg = buf;
                }
                addLocalChatLine(game::ChatType::COMBAT_XP_GAIN, msg);
                addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(xpGained), 0, true);
            }
        }
    };

    // SMSG_PET_NAME_QUERY_RESPONSE is EntityController's (the pet name cache,
    // for every pet in view, not only the player's).

    // -----------------------------------------------------------------------
    // Entity delta updates: health / power / world state / combo / timers / PvP
    // (SMSG_HEALTH_UPDATE, SMSG_POWER_UPDATE, SMSG_UPDATE_COMBO_POINTS,
    //  SMSG_PVP_CREDIT, SMSG_PROCRESIST → moved to CombatHandler)
    // -----------------------------------------------------------------------
    // Wow.exe 0x526530 case 0x2C3: the value set, UPDATE_WORLD_STATES. World
    // state 3191 (the arena season) also raises ARENA_SEASON_WORLD_STATE
    // when it changes (0x549440).
    dispatchTable_[Opcode::SMSG_UPDATE_WORLD_STATE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint32_t field = packet.readUInt32();
        uint32_t value = packet.readUInt32();
        auto it = worldStates_.find(field);
        const bool changed = it == worldStates_.end() || it->second != value;
        worldStates_[field] = value;
        if (changed && field == 3191) fireAddonEvent("ARENA_SEASON_WORLD_STATE", {});
        LOG_DEBUG("SMSG_UPDATE_WORLD_STATE: field=", field, " value=", value);
        fireAddonEvent("UPDATE_WORLD_STATES", {});
    };
    // 0x526530 case 0x4F7 -> 0x5486E0: the server's clock, kept as an offset
    // from the local one; the %<id>k world state timers count to it. No event.
    dispatchTable_[Opcode::SMSG_WORLD_STATE_UI_TIMER_UPDATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            const uint32_t serverTime = packet.readUInt32();
            worldStateClockOffset_ = static_cast<int64_t>(serverTime) -
                                     static_cast<int64_t>(std::time(nullptr));
        }
    };
    dispatchTable_[Opcode::SMSG_START_MIRROR_TIMER] = [this](network::Packet& packet) {
        // type(4) + value(4) + maxValue(4) + scale(4) + paused(1) + spellId(4).
        // The last two were being read the other way round: harmless while the
        // spell id is 0, but a non-zero one would land its high byte in paused and
        // freeze the bar.
        if (!packet.hasRemaining(21)) return;
        uint32_t type  = packet.readUInt32();
        int32_t  value = static_cast<int32_t>(packet.readUInt32());
        int32_t  maxV  = static_cast<int32_t>(packet.readUInt32());
        int32_t  scale = static_cast<int32_t>(packet.readUInt32());
        uint8_t  paused = packet.readUInt8();
        /*uint32_t spellId =*/ packet.readUInt32();
        if (type < 3) {
            mirrorTimers_[type].value     = value;
            mirrorTimers_[type].maxValue  = maxV;
            mirrorTimers_[type].scale     = scale;
            mirrorTimers_[type].paused    = (paused != 0);
            mirrorTimers_[type].active    = true;
            mirrorTimers_[type].pendingMs = 0.0f;  // server re-sync; drop the local carry
            // The interface names its timers rather than numbering them, and
            // wants a label to draw beside the bar: MirrorTimer_Show(timer,
            // value, maxvalue, scale, paused, label). Sending the type as a
            // number and omitting the label left it matching no timer and
            // dividing a nil.
            // The names WoW passes, not the wire's own ordering words - see
            // kMirrorTimerNames, which every path that names a timer shares.
            const char* timerName = (type < 3) ? kMirrorTimerNames[type] : "BREATH";
            const char* timerLabel = (type < 3) ? kMirrorTimerLabels[type] : "Breath";
            // Said once per timer, because nothing on this side decides any of
            // it. The length of a breath is the server's: three minutes plus
            // whatever SPELL_AURA_MOD_WATER_BREATHING adds, which for a
            // Forsaken is 300% more and so twelve. A maxValue of 180000 on an
            // undead character means the racial is not being applied there,
            // and no change here can put it right.
            static bool saidTimer[3] = {false, false, false};
            if (!saidTimer[type]) {
                saidTimer[type] = true;
                LOG_INFO("Mirror timer ", timerName, " from the server: value=", value,
                            "ms of max=", maxV, "ms scale=", scale,
                            " paused=", (paused != 0 ? 1 : 0));
            }
            fireAddonEvent("MIRROR_TIMER_START", {
                    timerName, std::to_string(value),
                    std::to_string(maxV), std::to_string(scale),
                    paused ? "1" : "0", timerLabel});
        }
    };
    dispatchTable_[Opcode::SMSG_STOP_MIRROR_TIMER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t type = packet.readUInt32();
        if (type < 3) {
            mirrorTimers_[type].active = false;
            mirrorTimers_[type].value  = 0;
            // Named, like the start above: MirrorTimer_Hide matches on the
            // timer's name, so a number hides nothing.
            fireAddonEvent("MIRROR_TIMER_STOP",
                           {type < 3 ? kMirrorTimerNames[type] : "BREATH"});
        }
    };
    dispatchTable_[Opcode::SMSG_PAUSE_MIRROR_TIMER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) return;
        uint32_t type   = packet.readUInt32();
        uint8_t  paused = packet.readUInt8();
        if (type < 3) {
            mirrorTimers_[type].paused = (paused != 0);
            fireAddonEvent("MIRROR_TIMER_PAUSE", {paused ? "1" : "0"});
        }
    };

    // -----------------------------------------------------------------------
    // Cast result / spell proc
    // (SMSG_CAST_RESULT, SMSG_SPELL_FAILED_OTHER → moved to SpellHandler)
    // (SMSG_PROCRESIST → moved to CombatHandler)
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Pet stable
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::MSG_LIST_STABLED_PETS] = [this](network::Packet& packet) {
        if (state == WorldState::IN_WORLD) handleListStabledPets(packet);
    };
    dispatchTable_[Opcode::SMSG_STABLE_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const uint8_t result = packet.readUInt8();
        LOG_INFO("SMSG_STABLE_RESULT: result=", static_cast<int>(result));
        // Wow.exe 0x5A17F0: 1 is ERR_NOT_ENOUGH_MONEY; 8 (stabled), 9
        // (unstabled) and 10 (a slot bought - one more slot) ask for the list
        // again; 11 raises PET_STABLE_UPDATE; 12 is the tame error for an
        // exotic pet the player cannot control. The rest say nothing.
        switch (result) {
            case 0x01:
                displayGameMessage("ERR_NOT_ENOUGH_MONEY", "You don't have enough money.");
                break;
            case 0x0A:
                if (stableNumSlots_ < 4) ++stableNumSlots_;
                [[fallthrough]];
            case 0x08:
            case 0x09:
                if (stableMasterGuid_ != 0 && socket) {
                    auto refreshPkt = ListStabledPetsPacket::build(stableMasterGuid_);
                    socket->send(refreshPkt);
                }
                break;
            case 0x0B:
                fireAddonEvent("PET_STABLE_UPDATE", {});
                break;
            case 0x0C: {
                std::string why = globalString(services().assetManager, "PETTAME_CANTCONTROLEXOTIC");
                if (why.empty()) why = "You are unable to control exotic creatures";
                displayGameMessage("ERR_TAME_FAILED", why + ".", {why});
                break;
            }
            default:
                break;
        }
    };

    // -----------------------------------------------------------------------
    // Titles / achievements / character services
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_TITLE_EARNED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint32_t titleBit = packet.readUInt32();
        uint32_t isLost   = packet.readUInt32();
        loadTitleNameCache();
        std::string titleStr;
        auto tit = titleNameCache_.find(titleBit);
        if (tit != titleNameCache_.end() && !tit->second.empty()) {
            const auto& ln = lookupName(playerGuid);
            const std::string& pName = ln.empty() ? std::string("you") : ln;
            const std::string& fmt = tit->second;
            size_t pos = fmt.find("%s");
            if (pos != std::string::npos)
                titleStr = fmt.substr(0, pos) + pName + fmt.substr(pos + 2);
            else
                titleStr = fmt;
        }
        if (isLost) knownTitleBits_.erase(titleBit);
        else        knownTitleBits_.insert(titleBit);
        // And announce it. The set is read by IsTitleKnown, which the character
        // sheet's title dropdown builds itself from - but only when told to
        // rebuild, and this is what tells it. Without it a title earned mid
        // session did not appear in the list until the sheet was reopened.
        fireAddonEvent("KNOWN_TITLES_UPDATE", {});
        // NEW_TITLE_EARNED / OLD_TITLE_LOST, plain chat lines with the title
        // written out for the player (Wow.exe 0x50C520); an unknown title
        // says nothing.
        if (!titleStr.empty()) {
            if (isLost) addGlobalStringChatLine("OLD_TITLE_LOST", "You have lost the title '%s'.", {titleStr});
            else addGlobalStringChatLine("NEW_TITLE_EARNED", "You have earned the title '%s'.", {titleStr});
        }
        LOG_INFO("SMSG_TITLE_EARNED: bit=", titleBit, " lost=", isLost, " title='", titleStr, "'");
    };
    dispatchTable_[Opcode::SMSG_LEARNED_DANCE_MOVES] = [](network::Packet& packet) {
        LOG_DEBUG("SMSG_LEARNED_DANCE_MOVES: ignored (size=", packet.getSize(), ")");
    };
    dispatchTable_[Opcode::SMSG_CHAR_RENAME] = [](network::Packet& packet) {
        // uint8 result, and the guid and the new name only when it succeeded.
        //
        // This read a four-byte result and required thirteen bytes. A refusal
        // is one byte, so no rename error was ever shown; a success carries the
        // name, so it usually cleared the guard and then took three bytes of
        // the guid as part of the result - which read as an enormous number,
        // fell past the error table and reported the rename as failed.
        if (packet.hasRemaining(1)) {
            const uint8_t result = packet.readUInt8();
            std::string newName;
            if (result == 0 && packet.hasRemaining(9)) {
                /*uint64_t guid =*/ packet.readUInt64();
                newName = packet.readString();
            }
            // A character-screen answer (Wow.exe 0x4DA090): the glue screen's
            // dialog words a failure; nothing goes to the world's chat.
            LOG_INFO("SMSG_CHAR_RENAME: result=", static_cast<int>(result),
                     " newName=", newName);
        }
    };

    // -----------------------------------------------------------------------
    // Bind / heartstone / phase / barber / corpse
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_PLAYERBOUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        /*uint64_t binderGuid =*/ packet.readUInt64();
        uint32_t mapId  = packet.readUInt32();
        uint32_t zoneId = packet.readUInt32();
        homeBindMapId_  = mapId;
        homeBindZoneId_ = zoneId;
        // ERR_DEATHBIND_SUCCESS_S with the area's name, a chat line, every
        // time (Wow.exe 0x6E2E90, opcode 0x158).
        const std::string zoneName = getAreaName(zoneId);
        if (!zoneName.empty())
            displayGameMessage("ERR_DEATHBIND_SUCCESS_S", zoneName + " is now your home.", {zoneName});
    };
    // The innkeeper asking whether to make this the player's home. It was
    // skipped because this client's own gossip window never waited for it - it
    // matches the option's English text and sends the activate itself. FrameXML
    // draws the gossip window now and follows the real flow: the server asks,
    // a popup accepts, and the reply goes back.
    dispatchTable_[Opcode::SMSG_BINDER_CONFIRM] = [this](network::Packet& packet) {
        binderGuid_ = packet.hasRemaining(8) ? packet.readUInt64() : 0;
        // Who is asking. StaticPopup_Show("CONFIRM_BINDER", arg1) puts the name
        // into "Do you want to make <name> your new home?" - without it the
        // question had a hole where the innkeeper should be.
        fireAddonEvent("CONFIRM_BINDER", {lookupName(binderGuid_)});
    };
    registerSkipHandler(Opcode::SMSG_SET_PHASE_SHIFT);
    dispatchTable_[Opcode::SMSG_TOGGLE_XP_GAIN] = [](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint8_t enabled = packet.readUInt8();
        // Console only (Wow.exe: "XP Gain Enabled" / "XP Gain Disabled").
        LOG_INFO("[console] ", enabled ? "XP Gain Enabled" : "XP Gain Disabled");
    };
    dispatchTable_[Opcode::SMSG_BINDZONEREPLY] = [](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // Read and dropped: Wow.exe (opcode 0x157) says nothing.
            (void)packet.readUInt32();
        }
    };
    dispatchTable_[Opcode::SMSG_CHANGEPLAYER_DIFFICULTY_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // The raid difficulty change's stages as Wow.exe (0x52C460) says
            // them: 0 the new difficulty stored, 1-5 and 7 refusals (red),
            // 8 a map's name as a chat line, 9 done -
            // ERR_PLAYER_DIFFICULTY_CHANGED_S with PLAYER_DIFFICULTY<n+1>.
            const uint32_t result = packet.readUInt32();
            switch (result) {
                case 0:
                    if (packet.hasRemaining(1)) pendingPlayerDifficulty_ = packet.readUInt8();
                    break;
                case 1: {
                    const uint32_t secs = packet.hasRemaining(4) ? packet.readUInt32() : 0;
                    const std::string when = secs >= 60 ? std::to_string(secs / 60) + " min"
                                                        : std::to_string(secs) + " sec";
                    displayGameMessage("ERR_DIFFICULTY_CHANGE_COOLDOWN_S",
                        "Raid difficulty has changed recently, and may not change again for " + when + ".", {when});
                    break;
                }
                case 2: displayGameMessage("ERR_DIFFICULTY_CHANGE_WORLDSTATE", "Raid difficulty cannot be changed at this time. An event is in progress."); break;
                case 3: displayGameMessage("ERR_DIFFICULTY_CHANGE_ENCOUNTER", "Raid difficulty cannot be changed at this time. An encounter is in progress."); break;
                case 4: displayGameMessage("ERR_DIFFICULTY_CHANGE_COMBAT", "Raid difficulty cannot be changed at this time. A player is in combat."); break;
                case 5: displayGameMessage("ERR_DIFFICULTY_CHANGE_PLAYER_BUSY", "Raid difficulty cannot be changed at this time. A player is busy."); break;
                case 7: displayGameMessage("ERR_DIFFICULTY_CHANGE_ALREADY_STARTED", "A raid difficulty change is currently in progress."); break;
                case 8: {
                    const uint32_t mapId = packet.hasRemaining(4) ? packet.readUInt32() : 0;
                    const std::string mapName = getMapName(mapId);
                    if (!mapName.empty()) addSystemChatMessage(mapName);
                    break;
                }
                case 9: {
                    fireAddonEvent("PLAYER_DIFFICULTY_CHANGED", {});
                    const std::string key = "PLAYER_DIFFICULTY" + std::to_string(pendingPlayerDifficulty_ + 1);
                    std::string label = globalString(services_.assetManager, key);
                    if (label.empty()) label = pendingPlayerDifficulty_ ? "Heroic" : "Normal";
                    displayGameMessage("ERR_PLAYER_DIFFICULTY_CHANGED_S", "Difficulty set to " + label + ".", {label});
                    break;
                }
                default: break;
            }
        }
        packet.skipAll();
    };
    // ERR_CORPSE_IS_NOT_IN_INSTANCE, the red error (Wow.exe 0x526530).
    dispatchTable_[Opcode::SMSG_CORPSE_NOT_IN_INSTANCE] = [this](network::Packet& /*packet*/) {
        displayGameMessage("ERR_CORPSE_IS_NOT_IN_INSTANCE", "Your corpse is not in that instance");
    };
    dispatchTable_[Opcode::SMSG_CROSSED_INEBRIATION_THRESHOLD] = [this](network::Packet& packet) {
        if (packet.hasRemaining(16)) {
            uint64_t guid      = packet.readUInt64();
            uint32_t threshold = packet.readUInt32();
            uint32_t itemId    = packet.readUInt32();
            // DRUNK_MESSAGE_SELF<n+1> / _OTHER<n+1>, or the _ITEM_ forms
            // naming what was drunk, as plain chat lines - for anyone the
            // client can see, not only the player (Wow.exe 0x6D01B0,
            // 0x6CFFF0).
            {
                const bool self = guid == playerGuid;
                const std::string n = std::to_string(std::min(threshold, 3u) + 1);
                std::string itemName;
                if (itemId != 0) {
                    if (const ItemQueryResponseData* info = getItemInfo(itemId)) itemName = info->name;
                }
                std::string who;
                if (!self) {
                    who = lookupName(guid);
                    if (auto e = getEntityManager().getEntity(guid))
                        if (auto* u = dynamic_cast<Unit*>(e.get()); u && !u->getName().empty()) who = u->getName();
                }
                if (self || !who.empty()) {
                    if (!itemName.empty()) {
                        if (self) addGlobalStringChatLine(("DRUNK_MESSAGE_ITEM_SELF" + n).c_str(), "%s", {itemName});
                        else addGlobalStringChatLine(("DRUNK_MESSAGE_ITEM_OTHER" + n).c_str(), "%s %s", {who, itemName});
                    } else {
                        if (self) addGlobalStringChatLine(("DRUNK_MESSAGE_SELF" + n).c_str(), "");
                        else addGlobalStringChatLine(("DRUNK_MESSAGE_OTHER" + n).c_str(), "%s", {who});
                    }
                }
            }
            if (guid == playerGuid) {
                const float amount = static_cast<float>(std::min(threshold, 3u)) / 3.0f;
                if (auto* renderer = services_.renderer) {
                    if (auto* camera = renderer->getCameraController()) {
                        camera->setIntoxication(amount);
                    }
                    if (auto* post = renderer->getPostProcessPipeline()) {
                        post->setIntoxication(amount);
                    }
                }
            }
            LOG_DEBUG("SMSG_CROSSED_INEBRIATION_THRESHOLD: guid=0x", std::hex, guid,
                      std::dec, " threshold=", threshold, " itemId=", itemId);
        }
    };
    dispatchTable_[Opcode::SMSG_CLEAR_FAR_SIGHT_IMMEDIATE] = [](network::Packet& /*packet*/) {
        LOG_DEBUG("SMSG_CLEAR_FAR_SIGHT_IMMEDIATE");
    };
    registerSkipHandler(Opcode::SMSG_COMBAT_EVENT_FAILED);
    dispatchTable_[Opcode::SMSG_FORCE_ANIM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint64_t animGuid = packet.readPackedGuid();
            if (packet.hasRemaining(4)) {
                uint32_t animId = packet.readUInt32();
                if (emoteAnimCallback_) emoteAnimCallback_(animGuid, animId, /*isState=*/false);
            }
        }
    };
    // Consume silently - opcodes we receive but don't need to act on
    for (auto op : {
        Opcode::SMSG_FLIGHT_SPLINE_SYNC, Opcode::SMSG_FORCE_DISPLAY_UPDATE,
        Opcode::SMSG_FORCE_SEND_QUEUED_PACKETS, Opcode::SMSG_FORCE_SET_VEHICLE_REC_ID,
        Opcode::SMSG_CORPSE_MAP_POSITION_QUERY_RESPONSE, Opcode::SMSG_DAMAGE_CALC_LOG,
        Opcode::SMSG_DYNAMIC_DROP_ROLL_RESULT, Opcode::SMSG_DESTRUCTIBLE_BUILDING_DAMAGE,
    }) { registerSkipHandler(op); }

    // Game object despawn animation: the model plays its Despawn sequence
    // (AnimationData 157) before the destroy that follows removes it. It used
    // to send state 0, which is ACTIVE - every despawning object swung open.
    dispatchTable_[Opcode::SMSG_GAMEOBJECT_DESPAWN_ANIM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint64_t guid = packet.readUInt64();
        constexpr uint32_t kAnimDespawn = 157;   // rendering::anim::DESPAWN
        if (gameObjectCustomAnimCallback_) gameObjectCustomAnimCallback_(guid, kAnimDespawn);
    };
    // Game object reset state: back to GO_STATE_READY (1, closed). 0 is
    // ACTIVE (open), which is what this used to send.
    dispatchTable_[Opcode::SMSG_GAMEOBJECT_RESET_STATE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint64_t guid = packet.readUInt64();
        if (gameObjectStateCallback_) gameObjectStateCallback_(guid, 1);
    };
    dispatchTable_[Opcode::SMSG_FORCED_DEATH_UPDATE] = [this](network::Packet& packet) {
        playerDead_ = true;
        corpseX_ = movementInfo.y;
        corpseY_ = movementInfo.x;
        corpseZ_ = movementInfo.z;
        corpseMapId_ = currentMapId_;
        corpsePositionValid_ = true;
        if (ghostStateCallback_) ghostStateCallback_(false);
        // PLAYER_DEAD is the whole announcement (Wow.exe 0x6DC070,
        // 0x520F70); there is no chat line.
        fireAddonEvent("PLAYER_DEAD", {});
        LOG_INFO("SMSG_FORCED_DEATH_UPDATE: player force-killed");
        packet.skipAll();
    };
    // SMSG_DEFENSE_MESSAGE - moved to ChatHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_CORPSE_RECLAIM_DELAY] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t delayMs = packet.readUInt32();
            auto nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            corpseReclaimAvailableMs_ = nowMs + delayMs;
            LOG_INFO("SMSG_CORPSE_RECLAIM_DELAY: ", delayMs, "ms");
        }
    };
    dispatchTable_[Opcode::SMSG_DEATH_RELEASE_LOC] = [this](network::Packet& packet) {
        if (packet.hasRemaining(16)) {
            uint32_t relMapId = packet.readUInt32();
            float relX = packet.readFloat(), relY = packet.readFloat(), relZ = packet.readFloat();
            // Read and logged and nothing else, until now: this is where the
            // server says the nearest spirit healer is, and a ghost with no
            // marker for it has to go looking.
            //
            // Map id -1 is the withdrawal. ResurrectPlayer sends it to take the
            // marker down, so it is not a graveyard at (0,0,0).
            if (relMapId == 0xFFFFFFFFu) {
                deathReleaseValid_ = false;
                LOG_INFO("SMSG_DEATH_RELEASE_LOC: spirit healer position cleared");
            } else {
                deathReleaseValid_ = true;
                deathReleaseMapId_ = relMapId;
                deathReleaseCanonical_ =
                    core::coords::serverToCanonical(glm::vec3(relX, relY, relZ));
                LOG_INFO("SMSG_DEATH_RELEASE_LOC (graveyard spawn): map=", relMapId,
                         " x=", relX, " y=", relY, " z=", relZ);
            }
        }
    };
    dispatchTable_[Opcode::SMSG_ENABLE_BARBER_SHOP] = [this](network::Packet& /*packet*/) {
        LOG_INFO("SMSG_ENABLE_BARBER_SHOP: barber shop available");
        barberShopOpen_ = true;
        fireAddonEvent("BARBER_SHOP_OPEN", {});
    };

    // ---- Batch 3: Corpse/gametime, combat clearing, mount, loot notify,
    //                movement/speed/flags, attack, spells, group ----

    dispatchTable_[Opcode::MSG_CORPSE_QUERY] = [this](network::Packet& packet) {
        // found(1) + travelMapId(4) + x,y,z(12) + corpseMapId(4) + unk(4).
        //
        // Two map ids, and they are not the same question. The first is where
        // the coordinates are: for a corpse left in a dungeon the server
        // answers with the instance's *entrance* on the outdoor map, because
        // that is where a ghost has to walk to. The second is the map the
        // corpse is actually lying on.
        //
        // This paired the coordinates with the second, and everything that
        // reads corpseMapId_ compares it against the map the player is
        // standing on - every other writer sets it to exactly that. So a
        // corpse in a dungeon gave a position on the outdoor map filed under
        // the dungeon's id, and the check `currentMapId_ != corpseMapId_`
        // refused to show it precisely where it would have been useful.
        if (!packet.hasRemaining(1)) return;
        const uint8_t found = packet.readUInt8();
        if (!found || !packet.hasRemaining(20)) return;
        const uint32_t travelMapId = packet.readUInt32();
        const float cx = packet.readFloat();
        const float cy = packet.readFloat();
        const float cz = packet.readFloat();
        const uint32_t corpseMapId = packet.readUInt32();

        corpseX_ = cx;
        corpseY_ = cy;
        corpseZ_ = cz;
        corpseMapId_ = travelMapId;
        corpseQueryTravelMapId_ = travelMapId;
        corpseActualMapId_ = corpseMapId;
        corpsePositionValid_ = true;
        LOG_INFO("MSG_CORPSE_QUERY: walk to (", cx, ",", cy, ",", cz,
                 ") on map ", travelMapId,
                 corpseMapId != travelMapId
                     ? " (the corpse itself is inside map " + std::to_string(corpseMapId) + ")"
                     : "");
    };
    // ERR_FEIGN_DEATH_RESISTED ("Resisted"), the red error (Wow.exe 0x7FD950).
    dispatchTable_[Opcode::SMSG_FEIGN_DEATH_RESISTED] = [this](network::Packet& /*packet*/) {
        displayGameMessage("ERR_FEIGN_DEATH_RESISTED", "Resisted");
    };
    dispatchTable_[Opcode::SMSG_CHANNEL_MEMBER_COUNT] = [](network::Packet& packet) {
        std::string chanName = packet.readString();
        if (packet.hasRemaining(5)) {
            /*uint8_t flags =*/ packet.readUInt8();
            uint32_t count = packet.readUInt32();
            LOG_DEBUG("SMSG_CHANNEL_MEMBER_COUNT: channel=", chanName, " members=", count);
        }
    };
    for (auto op : { Opcode::SMSG_GAMETIME_SET, Opcode::SMSG_GAMETIME_UPDATE }) {
        dispatchTable_[op] = [this](network::Packet& packet) {
            if (packet.hasRemaining(4)) {
                const WowDate t = unpackWowPackedTime(packet.readUInt32());
                gameTimeSetAt_ = std::chrono::steady_clock::now();
                gameTime_ = static_cast<float>(t.hour) +
                            static_cast<float>(t.minute) / 60.0f;
                // Said out loud, because this is where realm time actually
                // comes from on servers that never send SMSG_LOGIN_SETTIMESPEED
                // - which logged its arrival and so looked like the only
                // source. Chasing a frozen sky through a log with no line for
                // this in it led to exactly the wrong conclusion.
                LOG_INFO("Realm time: ", t.hour, ":", t.minute,
                         " (gameTime=", gameTime_, "h)");
            }
            packet.skipAll();
        };
    }
    dispatchTable_[Opcode::SMSG_GAMESPEED_SET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            const WowDate t = unpackWowPackedTime(packet.readUInt32());
            float timeSpeed = packet.readFloat();
            gameTimeSetAt_ = std::chrono::steady_clock::now();
            gameTime_ = static_cast<float>(t.hour) +
                        static_cast<float>(t.minute) / 60.0f;
            timeSpeed_ = timeSpeed;
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_GAMETIMEBIAS_SET] = [](network::Packet& packet) {
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_ACHIEVEMENT_DELETED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t achId = packet.readUInt32();
            earnedAchievements_.erase(achId);
            achievementDates_.erase(achId);
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_CRITERIA_DELETED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t critId = packet.readUInt32();
            criteriaProgress_.erase(critId);
        }
        packet.skipAll();
    };

    // Combat clearing
    dispatchTable_[Opcode::SMSG_BREAK_TARGET] = [this](network::Packet& packet) {
        // Packed, not plain: Unit::BreakTarget sizes the packet with
        // GetPackGUID().size() and writes GetPackGUID(). Read as a uint64 the
        // guid never matched the target, so the target was never dropped.
        if (packet.hasRemaining(1)) {
            uint64_t bGuid = packet.readPackedGuid();
            if (bGuid == targetGuid) targetGuid = 0;
        }
    };
    dispatchTable_[Opcode::SMSG_CLEAR_TARGET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t cGuid = packet.readUInt64();
            if (cGuid == 0 || cGuid == targetGuid) targetGuid = 0;
        }
    };

    // Mount/dismount
    dispatchTable_[Opcode::SMSG_DISMOUNT] = [this](network::Packet& packet) {
        // TBC/WotLK identify the dismounting unit with a packed GUID. Applying
        // every nearby unit's dismount to the local player made an enemy
        // dismounting to attack knock us off our own mount. Classic variants
        // may send the legacy empty packet, which is implicitly local.
        uint64_t dismountGuid = playerGuid;
        if (packet.hasRemaining(1)) {
            dismountGuid = packet.readPackedGuid();
            if (dismountGuid == 0) {
                LOG_WARNING("Ignoring SMSG_DISMOUNT with an invalid packed GUID");
                return;
            }
        }
        if (dismountGuid != playerGuid) {
            LOG_DEBUG("Remote SMSG_DISMOUNT: guid=0x", std::hex,
                      dismountGuid, std::dec);
            if (otherPlayerMountCallback_) {
                otherPlayerMountCallback_(dismountGuid, 0);
            }
            return;
        }

        // UNIT_FIELD_MOUNTDISPLAYID is the authoritative persistent mount
        // state.  Some realms emit an isolated SMSG_DISMOUNT while processing
        // damage (notably periodic poison) without actually removing the mount
        // aura or clearing the update field.  Treating that transient packet as
        // state made the local model dismount even though the player remained
        // mounted server-side.  A real dismount also clears the update field;
        // let that values update drive the visual when the two signals disagree.
        if (isActiveExpansion("wotlk")) {
            const uint16_t mountField = fieldIndex(UF::UNIT_FIELD_MOUNTDISPLAYID);
            auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
            const uint32_t serverMountDisplay =
                (playerEntity && mountField != 0xFFFF && playerEntity->hasField(mountField))
                    ? playerEntity->getField(mountField)
                    : 0;
            // ...unless the player asked to dismount, in which case a still-set
            // field is exactly the staleness this packet is arriving to correct.
            // Discarding it there threw away the one authoritative confirmation
            // the dismount had.
            const bool awaitingOurOwnDismount =
                movementHandler_ && movementHandler_->isDismountPending();
            if (serverMountDisplay != 0 && !awaitingOurOwnDismount) {
                LOG_WARNING("Ignoring transient SMSG_DISMOUNT while authoritative mount field is ",
                            serverMountDisplay,
                            " casting=", isCasting(),
                            " channeling=", isChanneling(),
                            " spell=", getCurrentCastSpellId());
                return;
            }
            if (movementHandler_) movementHandler_->clearDismountPending();
        }
        currentMountDisplayId_ = 0;
        if (mountCallback_) mountCallback_(0);
    };
    dispatchTable_[Opcode::SMSG_MOUNTRESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t result = packet.readUInt32();
        // MountResult 0-9, one GameError each (Wow.exe 0x6CEDF0, table
        // 0xADA1AC); 10 (OK) and anything else say nothing.
        static const char* const kKeys[] = {
            "ERR_MOUNT_INVALIDMOUNTEE", "ERR_MOUNT_TOOFARAWAY", "ERR_MOUNT_ALREADYMOUNTED",
            "ERR_MOUNT_NOTMOUNTABLE", "ERR_MOUNT_NOTYOURPET", "ERR_MOUNT_OTHER",
            "ERR_MOUNT_LOOTING", "ERR_MOUNT_RACECANTMOUNT", "ERR_MOUNT_SHAPESHIFTED",
            "ERR_MOUNT_FORCEDDISMOUNT",
        };
        if (result < 10) displayGameMessage(kKeys[result], "You can't mount now.");
    };
    dispatchTable_[Opcode::SMSG_DISMOUNTRESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t result = packet.readUInt32();
        // DismountResult 0-2, one GameError each (Wow.exe 0x6CEE10, table
        // 0xADA1D8); 3 (OK) says nothing. This raised its own sentence twice.
        static const char* const kKeys[] = {"ERR_DISMOUNT_NOPET", "ERR_DISMOUNT_NOTMOUNTED",
                                            "ERR_DISMOUNT_NOTYOURPET"};
        if (result < 3) displayGameMessage(kKeys[result], "You can't dismount now.");
    };

    // Camera shake
    dispatchTable_[Opcode::SMSG_CAMERA_SHAKE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t shakeId   = packet.readUInt32();
            uint32_t shakeType = packet.readUInt32();
            (void)shakeType;
            float magnitude = (shakeId < 50) ? 0.04f : 0.08f;
            if (cameraShakeCallback_)
                cameraShakeCallback_(magnitude, 18.0f, 0.5f);
        }
    };

    // (SMSG_PLAY_SPELL_VISUAL, SMSG_CLEAR_COOLDOWN, SMSG_MODIFY_COOLDOWN → moved to SpellHandler)

    // ---- Batch 4: Ready check, duels, guild, loot/gossip/vendor, factions, spell mods ----

    // Guild
    registerHandler(Opcode::SMSG_PET_SPELLS, &GameHandler::handlePetSpells);

    // Loot/gossip/vendor delegates
    registerHandler(Opcode::SMSG_SUMMON_REQUEST, &GameHandler::handleSummonRequest);
    dispatchTable_[Opcode::SMSG_SUMMON_CANCEL] = [this](network::Packet& /*packet*/) {
        pendingSummonRequest_ = false;
        // social_handler raised the prompt with CONFIRM_SUMMON; uiparent.lua
        // takes this one down again. Told only in chat, the dialog stays on
        // screen offering a summon the server has already withdrawn.
        // The event alone (Wow.exe 0x6CBCF0).
        fireAddonEvent("CANCEL_SUMMON", {});
    };

    // Bind point
    dispatchTable_[Opcode::SMSG_BINDPOINTUPDATE] = [this](network::Packet& packet) {
        BindPointUpdateData data;
        if (BindPointUpdateParser::parse(packet, data)) {
            glm::vec3 canonical = core::coords::serverToCanonical(
                glm::vec3(data.x, data.y, data.z));
            bool wasSet = hasHomeBind_;
            bool changed =
                !hasHomeBind_ ||
                homeBindMapId_ != data.mapId ||
                homeBindZoneId_ != data.zoneId ||
                glm::length(homeBindPos_ - canonical) > 0.5f;
            hasHomeBind_ = true;
            homeBindMapId_ = data.mapId;
            homeBindZoneId_ = data.zoneId;
            homeBindPos_ = canonical;
            if (bindPointCallback_)
                bindPointCallback_(data.mapId, canonical.x, canonical.y, canonical.z);
            // Stored in silence (Wow.exe 0x6CEEC0): SMSG_PLAYERBOUND is the
            // one that says "<zone> is now your home."
            (void)wasSet; (void)changed;
        }
    };

    // Spirit healer / resurrect
    dispatchTable_[Opcode::SMSG_SPIRIT_HEALER_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint64_t npcGuid = packet.readUInt64();
        if (npcGuid) {
            resurrectCasterGuid_ = npcGuid;
            resurrectCasterName_ = "";
            resurrectIsSpiritHealer_ = true;
            resurrectRequestPending_ = true;
            // The spirit healer's prompt is CONFIRM_XP_LOSS, not
            // RESURRECT_REQUEST - uiparent.lua answers it with the durability
            // and sickness warning and its Accept calls AcceptXPLoss. Nothing
            // fired it, and the only thing that could reach
            // activateSpiritHealer was this client's own gossip window, so with
            // gossip handed over there was no way left to accept at all.
            fireAddonEvent("CONFIRM_XP_LOSS", {});
        }
    };
    dispatchTable_[Opcode::SMSG_RESURRECT_REQUEST] = [this](network::Packet& packet) {
        // The stock reader (0x6dbd00):
        //
        //   uint64 casterGuid
        //   uint32 nameLength      (the name plus its terminator)
        //   char[nameLength] name
        //   uint8  -> ResurrectHasSickness
        //   uint8  -> ResurrectHasTimer
        //
        // and nothing after. The server's own comments call the first byte a
        // second terminator and the second "sickness" (1 from a creature), but
        // what the interface sees is what the client stored: a player's
        // resurrection has neither, so FrameXML's ShowResurrectRequest picks
        // RESURRECT_NO_TIMER; a creature's has the timer and gets
        // RESURRECT_NO_SICKNESS. Reading the second byte as sickness offered
        // every player's resurrection as the timed, sickness-free one.
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        const uint64_t casterGuid = packet.readUInt64();
        /*uint32_t nameLength =*/ packet.readUInt32();
        std::string casterName = packet.readString();
        const uint8_t first = packet.hasRemaining(1) ? packet.readUInt8() : 0;
        const uint8_t second = packet.hasRemaining(1) ? packet.readUInt8() : 0;
        packet.skipAll();

        if (!casterGuid) return;
        const auto offer = stock_death::resurrectOffer(first, second);
        resurrectCasterGuid_ = casterGuid;
        resurrectIsSpiritHealer_ = false;
        resurrectHasSickness_ = offer.sickness;
        resurrectHasTimer_ = offer.timer;
        resurrectCasterName_ = casterName.empty() ? lookupName(casterGuid) : casterName;
        resurrectRequestPending_ = true;
        // Raised only while the player is dead or a ghost (0x6dbd00 asks the
        // player's vtable +0x128 first); an offer to the living is kept for
        // AcceptResurrect but shows nothing.
        if (playerDead_ || releasedSpirit_) {
            fireAddonEvent("RESURRECT_REQUEST", {resurrectCasterName_});
        }
    };

    // Time sync
    dispatchTable_[Opcode::SMSG_TIME_SYNC_REQ] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t counter = packet.readUInt32();
        if (socket) {
            network::Packet resp(wireOpcode(Opcode::CMSG_TIME_SYNC_RESP));
            resp.writeUInt32(counter);
            resp.writeUInt32(nextMovementTimestampMs());
            socket->send(resp);
        }
    };

    // (SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED → moved to InventoryHandler)

    // Minimap ping
    dispatchTable_[Opcode::MSG_MINIMAP_PING] = [this](network::Packet& packet) {
        // A full guid in every expansion; WotLK does not pack this one. Then
        // the spot in world order, north then west - what 0x006E2E90 hands
        // straight to 0x0057EB80.
        if (!packet.hasRemaining(8)) return;
        uint64_t senderGuid = packet.readUInt64();
        if (!packet.hasRemaining(8)) return;
        const float serverX = packet.readFloat();
        const float serverY = packet.readFloat();
        const glm::vec3 canon = core::coords::serverToCanonical(glm::vec3(serverX, serverY, 0.0f));
        MinimapPing ping;
        ping.senderGuid = senderGuid;
        ping.wowX = canon.x;
        ping.wowY = canon.y;
        ping.age  = 0.0f;
        minimapPings_.push_back(ping);
        // MapPing is minimap.lua's: MinimapPing_OnEvent plays it on
        // MINIMAP_PING, fired below. Only a minimap that is not FrameXML's
        // needs it from here; with both, every ping sounded twice.
        if (senderGuid != playerGuid && !ui::frameXmlOwns(ui::UiElement::Minimap)) {
            withSoundManager(&audio::AudioCoordinator::getUiSoundManager, [](auto* sfx) { sfx->playMinimapPing(); });
        }
        notifyMinimapPing(senderGuid, serverX, serverY);
    };
    dispatchTable_[Opcode::SMSG_ZONE_UNDER_ATTACK] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t areaId = packet.readUInt32();
            const std::string areaName = getAreaName(areaId);
            // Wow.exe 0x50C3C0: no event - ZONE_UNDER_ATTACK is a string, not
            // one 3.3.5 raises. The formatted line goes to the defense
            // channels (ChatHandler::postDefenseLine).
            std::string fmt = globalString(services_.assetManager, "ZONE_UNDER_ATTACK");
            if (fmt.empty()) fmt = "|cffffff00%s is under attack!|r";
            if (chatHandler_) chatHandler_->postDefenseLine(areaId, formatGlobalString(fmt, {areaName}));
        }
    };

    // Spirit healer time / durability
    dispatchTable_[Opcode::SMSG_AREA_SPIRIT_HEALER_TIME] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        const uint64_t guid = packet.readUInt64();
        const uint32_t timeMs = packet.readUInt32();
        // Wow.exe 0x524B10: only for the spirit guide the client asked, and
        // only a positive time - then the wave's time and
        // AREA_SPIRIT_HEALER_IN_RANGE, whose dialog counts it down.
        updateAreaSpiritHealer();
        if (guid != areaSpiritHealerGuid_ || static_cast<int32_t>(timeMs) <= 0) return;
        areaSpiritHealerExpiresMs_ = bf::clockMs() + timeMs;
        fireAddonEvent("AREA_SPIRIT_HEALER_IN_RANGE", {});
    };
    dispatchTable_[Opcode::SMSG_DURABILITY_DAMAGE_DEATH] = [this](network::Packet& packet) {
        // DURABILITYDAMAGE_DEATH as it stands, a plain chat line; the
        // percentage the server sends is not part of it (Wow.exe 0x50C810).
        packet.skipAll();
        addGlobalStringChatLine("DURABILITYDAMAGE_DEATH", "Your equipped items suffer a 10% durability loss.");
    };

    // (SMSG_INITIALIZE_FACTIONS, SMSG_SET_FACTION_STANDING,
    //  SMSG_SET_FACTION_ATWAR, SMSG_SET_FACTION_VISIBLE → moved to SocialHandler)
    dispatchTable_[Opcode::SMSG_FEATURE_SYSTEM_STATUS] = [](network::Packet& packet) {
        packet.skipAll();
    };

    // (SMSG_SET_FLAT_SPELL_MODIFIER, SMSG_SET_PCT_SPELL_MODIFIER, SMSG_SPELL_DELAYED → moved to SpellHandler)

    // Proficiency
    dispatchTable_[Opcode::SMSG_SET_PROFICIENCY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) return;
        uint8_t  itemClass = packet.readUInt8();
        uint32_t mask      = packet.readUInt32();
        if (itemClass == 2) weaponProficiency_ = mask;
        else if (itemClass == 4) armorProficiency_ = mask;
    };

    // Loot money / misc consume
    for (auto op : { Opcode::SMSG_LOOT_CLEAR_MONEY, Opcode::SMSG_NPC_TEXT_UPDATE }) {
        dispatchTable_[op] = [](network::Packet& /*packet*/) {};
    }

    // Play sound
    dispatchTable_[Opcode::SMSG_PLAY_SOUND] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playSoundCallback_) playSoundCallback_(soundId);
        }
    };

    // SMSG_SERVER_MESSAGE - moved to ChatHandler::registerOpcodes
    // SMSG_CHAT_SERVER_MESSAGE - moved to ChatHandler::registerOpcodes
    // SMSG_AREA_TRIGGER_MESSAGE - moved to ChatHandler::registerOpcodes
    // A new character's intro. This client does not play cinematics, so it
    // answers as a stock player skipping one with Escape: COMPLETE_CINEMATIC.
    // It used to send NEXT_CINEMATIC_CAMERA, which tells the server the
    // cinematic has begun - AzerothCore then moves the player's viewpoint along
    // the camera path for its whole length and sends what is near the camera,
    // so a new character stood in an empty Northshire for two minutes before
    // any NPC around them arrived.
    dispatchTable_[Opcode::SMSG_TRIGGER_CINEMATIC] = [this](network::Packet& packet) {
        packet.skipAll();
        network::Packet done(wireOpcode(Opcode::CMSG_COMPLETE_CINEMATIC));
        socket->send(done);
    };

    // ---- Batch 5: Teleport, taxi, BG, LFG, arena, movement relay, mail, bank, auction, quests ----

    // Teleport
    dispatchTable_[Opcode::SMSG_TRANSFER_PENDING] = [](network::Packet& packet) {
        uint32_t pendingMapId = packet.readUInt32();
        if (packet.hasRemaining(8)) {
            packet.readUInt32(); // transportEntry
            packet.readUInt32(); // transportMapId
        }
        (void)pendingMapId;
    };
    dispatchTable_[Opcode::SMSG_TRANSFER_ABORTED] = [this](network::Packet& packet) {
        // uint32 mapId + uint8 reason, and a further uint8 for exactly three of
        // them - the expansion level, the difficulty and the unique message.
        //
        // Every code from 0x01 on named the wrong failure. The table here was
        // shifted against Player.h's own enum: 0x02 is a full instance and was
        // read as a missing expansion, 0x06 is an encounter in progress and was
        // read as a full instance, 0x07 is the missing expansion and was read
        // as the encounter. Nothing above 0x09 was known at all, which
        // includes the one a map refusing entry outright sends.
        //
        // So a refusal always said something, and what it said was not why.
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t mapId = packet.readUInt32();
        const uint8_t reason = packet.hasRemaining(1) ? packet.readUInt8() : 0;
        const uint8_t extra = packet.hasRemaining(1) ? packet.readUInt8() : 0;
        packet.skipAll();
        // Each reason's TRANSFER_ABORT_* text with the map's name, a plain
        // chat line (Wow.exe 0x403910); reason 5, unknown reasons and maps
        // Map.dbc does not name say nothing.
        std::string key;
        switch (reason) {
            case 0x01: key = "TRANSFER_ABORT_ERROR"; break;
            case 0x02: key = "TRANSFER_ABORT_MAX_PLAYERS"; break;
            case 0x03: case 0x0C: case 0x0D: case 0x0E: key = "TRANSFER_ABORT_NOT_FOUND"; break;
            case 0x04: key = "TRANSFER_ABORT_TOO_MANY_INSTANCES"; break;
            case 0x06: key = "TRANSFER_ABORT_ZONE_IN_COMBAT"; break;
            case 0x07: key = "TRANSFER_ABORT_INSUF_EXPAN_LVL" + std::to_string(extra); break;
            case 0x08: key = "TRANSFER_ABORT_DIFFICULTY" + std::to_string(extra + 1); break;
            case 0x09: key = "TRANSFER_ABORT_UNIQUE_MESSAGE" + std::to_string(extra); break;
            case 0x0A: key = "TRANSFER_ABORT_TOO_MANY_REALM_INSTANCES"; break;
            case 0x0B: key = "TRANSFER_ABORT_NEED_GROUP"; break;
            case 0x0F: key = "TRANSFER_ABORT_REALM_ONLY"; break;
            case 0x10: key = "TRANSFER_ABORT_MAP_NOT_ALLOWED"; break;
            default: break;
        }
        const std::string mapName = getMapName(mapId);
        if (!key.empty() && !mapName.empty())
            addGlobalStringChatLine(key.c_str(), "Transfer Aborted", {mapName});
        LOG_WARNING("SMSG_TRANSFER_ABORTED: reason 0x", std::hex,
                    static_cast<int>(reason), std::dec, " map ", mapId);
    };

    // Taxi
    dispatchTable_[Opcode::SMSG_STANDSTATE_UPDATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            const uint8_t newStandState = packet.readUInt8();
            if (newStandState == standState_) {
                return;
            }
            standState_ = newStandState;
            // Out of the chair is out of the barber, as on the real client;
            // closing it here also takes the unbought preview off.
            if (barberShopOpen_ &&
                !(standState_ == 2 || (standState_ >= 4 && standState_ <= 6))) {
                endBarberShop(false);
            }
            // 0=stand, 1=sit, 2-6=sit variants, 7=dead, 8=kneel. Logged because a
            // wrong state here is indistinguishable, in-game, from a wrong animation.
            // At warning level: the file log filters info out by default.
            LOG_WARNING("SMSG_STANDSTATE_UPDATE: standState=", static_cast<int>(standState_));
            if (standStateCallback_) standStateCallback_(standState_);
        }
    };
    // ERR_NEWTAXIPATH, the yellow info text with TaxiNodeDiscovered
    // (Wow.exe 0x6E2E90, opcode 0x1AF).
    dispatchTable_[Opcode::SMSG_NEW_TAXI_PATH] = [this](network::Packet& /*packet*/) {
        displayGameMessage("ERR_NEWTAXIPATH", "New flight path discovered!");
    };

    // (MSG_TALENT_WIPE_CONFIRM → registered by SpellHandler, which owns the
    // pending-wipe state the confirm dialog reads. Handling it here wrote to
    // dead duplicate members and the dialog never appeared.)

    // (SMSG_CHANNEL_LIST → moved to ChatHandler)
    // (SMSG_GROUP_SET_LEADER → moved to SocialHandler)

    // Gameobject / page text (entity queries moved to EntityController::registerOpcodes)
    dispatchTable_[Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM] = [this](network::Packet& packet) {
        if (packet.getSize() < 12) return;
        uint64_t guid = packet.readUInt64();
        uint32_t animId = packet.readUInt32();
        bool ownedFishingBite = false;
        auto goEnt = entityController_->getEntityManager().getEntity(guid);
        if (goEnt && goEnt->getType() == ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<GameObject>(goEnt);
            // Only treat custom animation as a bite when this is our bobber.
            // AzerothCore sends the GO's animation progress (normally 100), not
            // M2 animation id 0, so the packet payload itself cannot identify it.
            uint64_t createdBy = static_cast<uint64_t>(go->getField(6))
                               | (static_cast<uint64_t>(go->getField(7)) << 32);
            auto* info = getCachedGameObjectInfo(go->getEntry());
            ownedFishingBite = createdBy == playerGuid && (!info || info->type == 17);
            if (ownedFishingBite) {
                hookedFishingBobberGuid_ = guid;
                // G_FishingBobber.m2 sequence 153 is its authored 1.33s bite/splash
                // animation; sequence 0 is the normal three-second idle bob.
                constexpr uint32_t kFishingBobberBiteAnimation = 153;
                if (gameObjectCustomAnimCallback_)
                    gameObjectCustomAnimCallback_(guid, kFishingBobberBiteAnimation);
                // No words: the splash and its sound are the stock client's
                // whole signal (Wow.exe 0x70BE90 plays the animation only).
                // "Fishing Hooked" (3355), GameObjectDisplayInfo 668's
                // Sound[6], is the sequence's own $GC0 event at 200 ms: the
                // model plays it at the bobber (0x70c050 -> 0x70bf70).
                LOG_WARNING("Fishing bite ready: guid=0x", std::hex, guid, std::dec,
                            " serverAnimProgress=", animId);
            }
        }
        // The payload is a custom animation index, which the game plays as
        // Custom0 + n (AnimationData 153-156) - only for n < 4 (0x70BE90);
        // anything else plays nothing. AzerothCore's bobber bite sends its
        // anim progress, 0, so another player's bobber splashes as Custom0.
        if (!ownedFishingBite && gameObjectCustomAnimCallback_ && animId < 4) {
            constexpr uint32_t kAnimCustom0 = 153;   // rendering::anim::CUSTOM_0
            gameObjectCustomAnimCallback_(guid, kAnimCustom0 + animId);
        }
    };

    // Item refund / socket gems / item time
    dispatchTable_[Opcode::SMSG_ITEM_REFUND_RESULT] = [this](network::Packet& packet) {
        // Wow.exe (0x6D9B40): a refund is ITEM_REFUND_MSG and then a plain
        // line for each part of it - the money, honor, arena points and up to
        // five currencies (CURRENCY_AMOUNT_REFUND_FORMAT); a refusal is
        // ERR_INV_FULL (10), ERR_CURRENCY_FULL (11) or the plain
        // UNABLE_TO_REFUND_ITEM.
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        packet.readUInt64(); // itemGuid
        const uint32_t result = packet.readUInt32();
        if (result == 10) { displayGameMessage("ERR_INV_FULL", "Inventory is full."); packet.skipAll(); return; }
        if (result == 11) { displayGameMessage("ERR_CURRENCY_FULL", "You can't carry any more of that currency."); packet.skipAll(); return; }
        if (result != 0) {
            addGlobalStringChatLine("UNABLE_TO_REFUND_ITEM", "Sorry, unable to refund that item.");
            packet.skipAll();
            return;
        }
        if (!packet.hasRemaining(12 + 40)) { packet.skipAll(); return; }
        const uint32_t money = packet.readUInt32();
        const uint32_t honor = packet.readUInt32();
        const uint32_t arena = packet.readUInt32();
        uint32_t items[5], counts[5];
        for (int i = 0; i < 5; ++i) { items[i] = packet.readUInt32(); counts[i] = packet.readUInt32(); }
        addGlobalStringChatLine("ITEM_REFUND_MSG", "Item was refunded. Received a refund of:");
        if (money > 0) addSystemChatMessage(formatMoneyText(money));
        auto currencyLine = [this](uint32_t amount, const std::string& what) {
            addGlobalStringChatLine("CURRENCY_AMOUNT_REFUND_FORMAT", "%d %s", {std::to_string(amount), what});
        };
        if (honor > 0) {
            std::string label = globalString(services_.assetManager, "HONOR_POINTS");
            currencyLine(honor, label.empty() ? "Honor Points" : label);
        }
        if (arena > 0) {
            std::string label = globalString(services_.assetManager, "ARENA_POINTS");
            currencyLine(arena, label.empty() ? "Arena Points" : label);
        }
        for (int i = 0; i < 5; ++i) {
            if (counts[i] == 0 || items[i] == 0) continue;
            if (const ItemQueryResponseData* info = getItemInfo(items[i]))
                currencyLine(counts[i], info->name);
        }
    };
    dispatchTable_[Opcode::SMSG_SOCKET_GEMS_RESULT] = [](network::Packet& packet) {
        // There is no result field. Item::SendUpdateSockets writes the item's
        // guid and then the four enchantment ids now in its sockets, and sends
        // it only when the sockets were actually changed - so arriving *is* the
        // success. This read a uint32 at offset zero and called it the result,
        // which is the low half of the item's guid: never zero, so socketing a
        // gem always reported that it had failed.
        if (!packet.hasRemaining(8)) return;
        const uint64_t itemGuid = packet.readUInt64();
        // Nothing said: Wow.exe (0x6E6330 -> 0x7081B0) writes the new
        // enchantments into the item and refreshes it.
        LOG_DEBUG("SMSG_SOCKET_GEMS_RESULT: itemGuid=0x", std::hex, itemGuid, std::dec);
    };
    dispatchTable_[Opcode::SMSG_ITEM_TIME_UPDATE] = [](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            packet.readUInt64(); // itemGuid
            packet.readUInt32(); // durationMs
        }
    };

    // ---- Batch 6: Spell miss / env damage / control / spell failure ----


    // ---- Achievement / fishing delegates ----
    dispatchTable_[Opcode::SMSG_ALL_ACHIEVEMENT_DATA] = [this](network::Packet& packet) {
        handleAllAchievementData(packet);
    };
    // 0x6E2E90 cases 0x1C8 / 0x1C9: DisplayError 0x151 ERR_FISH_NOT_HOOKED
    // ("No fish are hooked.") and 0x152 ERR_FISH_ESCAPED ("Your fish got
    // away!") - both kind 1 in the GameError table, the yellow info line,
    // not the red error and not chat.
    dispatchTable_[Opcode::SMSG_FISH_NOT_HOOKED] = [this](network::Packet& /*packet*/) {
        hookedFishingBobberGuid_ = 0;
        displayGameMessage("ERR_FISH_NOT_HOOKED", "No fish are hooked.", {});
    };
    dispatchTable_[Opcode::SMSG_FISH_ESCAPED] = [this](network::Packet& /*packet*/) {
        hookedFishingBobberGuid_ = 0;
        displayGameMessage("ERR_FISH_ESCAPED", "Your fish got away!", {});
    };

    // ---- Auto-repeat / auras / dispel / totem ----
    dispatchTable_[Opcode::SMSG_CANCEL_AUTO_REPEAT] = [this](network::Packet& packet) {
        // The server saying a repeating spell has stopped. Nothing in this
        // client's own interface reads it, which is why it did nothing - but
        // the action button flashes for as long as one is running and stops on
        // exactly this. WotLK servers name the shooter and tell everyone
        // nearby; older ones send it empty, to the shooter alone.
        if (packet.hasRemaining(1) &&
            (!packet.hasFullPackedGuid() || packet.readPackedGuid() != playerGuid)) {
            return;
        }
        if (spellHandler_) spellHandler_->clearAutoRepeat();
        fireAddonEvent("STOP_AUTOREPEAT_SPELL", {});
    };


    // ---- Batch 7: World states, action buttons, level-up, vendor, inventory ----

    // ---- SMSG_INIT_WORLD_STATES ----
    dispatchTable_[Opcode::SMSG_INIT_WORLD_STATES] = [this](network::Packet& packet) {
        // Wow.exe 0x526530 case 0x2C2: map, zone, area (0x548970 - a change
        // rebuilds the WorldStateUI lines), then the pairs, each set without
        // clearing the rest (0x549440). UPDATE_WORLD_STATES only when the
        // place or a value changed. Classic/Turtle have no area.
        if (!packet.hasRemaining(10)) {
            LOG_WARNING("SMSG_INIT_WORLD_STATES too short: ", packet.getSize(), " bytes");
            return;
        }
        const uint32_t mapId = packet.readUInt32();
        const uint32_t newZoneId = packet.readUInt32();
        uint32_t areaId = 0;
        const bool hasAreaId = isActiveExpansion("tbc") || isActiveExpansion("wotlk");
        if (hasAreaId && packet.getRemainingSize() >= 6) areaId = packet.readUInt32();
        bool changed = mapId != worldStateMapId_ || newZoneId != worldStateZoneId_ ||
                       areaId != worldStateAreaId_;
        worldStateMapId_ = mapId;
        if (newZoneId != worldStateZoneId_ && newZoneId != 0) {
            worldStateZoneId_ = newZoneId;
            // Once: the renderer's own area lookup usually saw the
            // crossing first and announced it already.
            if (announceZoneChange(newZoneId)) fireAddonEvent("ZONE_CHANGED", {});
        } else {
            worldStateZoneId_ = newZoneId;
        }
        worldStateAreaId_ = areaId;
        uint16_t count = packet.readUInt16();
        const size_t available = packet.getRemainingSize();
        if (available < static_cast<size_t>(count) * 8) {
            LOG_WARNING("SMSG_INIT_WORLD_STATES truncated: header=", count, " pairs, ",
                        available, " bytes");
            count = static_cast<uint16_t>(available / 8);
        }
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t key = packet.readUInt32();
            const uint32_t val = packet.readUInt32();
            auto it = worldStates_.find(key);
            if (it == worldStates_.end() || it->second != val) {
                changed = true;
                if (key == 3191) fireAddonEvent("ARENA_SEASON_WORLD_STATE", {});
            }
            worldStates_[key] = val;
        }
        if (changed) fireAddonEvent("UPDATE_WORLD_STATES", {});
    };

    // ---- SMSG_ACTION_BUTTONS ----
    dispatchTable_[Opcode::SMSG_ACTION_BUTTONS] = [this](network::Packet& packet) {
        // Slot encoding differs by expansion:
        //   Classic/Turtle: uint16 actionId + uint8 type + uint8 misc
        //     type: 0=spell, 1=item, 64=macro
        //   TBC/WotLK: uint32 packed = actionId | (type << 24)
        //     type: 0x00=spell, 0x80=item, 0x40=macro
        // Format differences:
        //   Classic 1.12: no mode byte, 120 slots (480 bytes)
        //   TBC 2.4.3:    no mode byte, 132 slots (528 bytes)
        //   WotLK 3.3.5a: uint8 mode + 144 slots (577 bytes)
        size_t rem = packet.getRemainingSize();
        const bool hasModeByteExp = isActiveExpansion("wotlk");
        int serverBarSlots;
        if (isClassicLikeExpansion()) {
            serverBarSlots = 120;
        } else if (isActiveExpansion("tbc")) {
            serverBarSlots = 132;
        } else {
            serverBarSlots = 144;
        }
        // WotLK mode byte (AzerothCore Player::SendActionButtons):
        //   0/1  the button list follows, all 144, 0 for an empty button
        //   2    no list - clear every button. Sent on a dual-spec swap
        //        before the new spec's list (ActivateSpec: mode 2, then 1).
        // Ignoring it left the old spec's buttons wherever the new spec had
        // none, and the new spec's zeros were skipped rather than applied.
        uint8_t mode = 1;
        if (hasModeByteExp) {
            if (rem < 1) return;
            mode = packet.readUInt8();
            rem--;
        }
        if (mode == 2) {
            for (auto& slot : actionBar) slot = ActionBarSlot{};
            actionBarClearedByServer_ = true;
            fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
            packet.skipAll();
            return;
        }
        std::array<uint32_t, ACTION_BAR_SLOTS> packedSlots{};
        bool anyButton = false;
        for (int i = 0; i < serverBarSlots; ++i) {
            if (rem < 4) return;
            uint32_t packed = packet.readUInt32();
            rem -= 4;
            if (i >= ACTION_BAR_SLOTS) continue;
            packedSlots[i] = packed;
            if (packed != 0) anyButton = true;
        }
        // A list with no buttons at all, not following a mode-2 clear, is
        // the one case left alone: it is the login list of a server that
        // keeps no buttons for this character, and the bar loaded from the
        // character config (or the Attack/Hearthstone defaults) is all
        // there is. Any other list is the server's whole bar, zeros included.
        const bool keepLocalBar = !anyButton && !actionBarClearedByServer_;
        actionBarClearedByServer_ = false;
        // Only the slots the server's format has: a Classic or TBC list is
        // shorter than this bar, and the pages past it are client-side.
        const int listed = std::min(serverBarSlots, static_cast<int>(ACTION_BAR_SLOTS));
        for (int i = 0; i < listed && !keepLocalBar; ++i) {
            const uint32_t packed = packedSlots[i];
            if (packed == 0) {
                actionBar[i] = ActionBarSlot{};
                continue;
            }
            uint8_t type = 0;
            uint32_t id = 0;
            if (isClassicLikeExpansion()) {
                id = packed & 0x0000FFFFu;
                type = static_cast<uint8_t>((packed >> 16) & 0xFF);
            } else {
                type = static_cast<uint8_t>((packed >> 24) & 0xFF);
                id = packed & 0x00FFFFFFu;
            }
            // An equipment set's index starts at 0 (ACTION_BUTTON_EQSET).
            if (id == 0 && type != 0x20) continue;
            ActionBarSlot slot;
            switch (type) {
                case 0x00: slot.type = ActionBarSlot::SPELL; slot.id = id; break;
                case 0x01: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // Classic item
                case 0x80: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // TBC/WotLK item
                case 0x40: slot.type = ActionBarSlot::MACRO; slot.id = id; break;  // macro (all expansions)
                case 0x20: slot.type = ActionBarSlot::EQUIPMENT_SET; slot.id = id; break;  // gear set (WotLK)
                default:   continue;  // unknown - leave as-is
            }
            actionBar[i] = slot;
        }
        // Apply any pending cooldowns from spellHandler's cooldowns to newly populated slots.
        // SMSG_SPELL_COOLDOWN often arrives before SMSG_ACTION_BUTTONS during login,
        // so the per-slot cooldownRemaining would be 0 without this sync.
        if (spellHandler_) {
            const auto& cooldowns = spellHandler_->getSpellCooldowns();
            for (auto& slot : actionBar) {
                if (slot.type == ActionBarSlot::SPELL && slot.id != 0) {
                    auto cdIt = cooldowns.find(slot.id);
                    if (cdIt != cooldowns.end() && cdIt->second > 0.0f) {
                        slot.cooldownRemaining = cdIt->second;
                        slot.cooldownTotal     = cdIt->second;
                    }
                } else if (slot.type == ActionBarSlot::ITEM && slot.id != 0) {
                    // Items (potions, trinkets): look up the item's on-use spell
                    // and check if that spell has a pending cooldown.
                    const auto* qi = getItemInfo(slot.id);
                    if (qi && qi->valid) {
                        for (const auto& sp : qi->spells) {
                            if (sp.spellId == 0) continue;
                            auto cdIt = cooldowns.find(sp.spellId);
                            if (cdIt != cooldowns.end() && cdIt->second > 0.0f) {
                                slot.cooldownRemaining = cdIt->second;
                                slot.cooldownTotal     = cdIt->second;
                                break;
                            }
                        }
                    }
                }
            }
        }
        LOG_INFO("SMSG_ACTION_BUTTONS: populated action bar from server");
        // Zero means every slot, which is what the whole bar arriving at
        // once is. Sent with no argument the buttons compared nil against
        // zero and against their own action, matched neither, and the bar
        // the server had just sent drew nothing.
        fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
        packet.skipAll();
    };

    // ---- SMSG_LEVELUP_INFO / SMSG_LEVELUP_INFO_ALT (shared body) ----
    for (auto op : {Opcode::SMSG_LEVELUP_INFO, Opcode::SMSG_LEVELUP_INFO_ALT}) {
        dispatchTable_[op] = [this](network::Packet& packet) {
            // Server-authoritative level-up event.
            // WotLK layout: uint32 level, health, then one delta per power
            // (MAX_POWERS = 7: mana first) and one per stat (5). The deltas are
            // signed differences written as uint32.
            if (packet.hasRemaining(4)) {
                uint32_t newLevel = packet.readUInt32();
                if (newLevel > 0) {
                    // Classic and TBC send five powers; count them from the
                    // size rather than assume.
                    constexpr size_t kStats = 5;
                    const size_t words = packet.getRemainingSize() / 4;
                    const size_t kPowers = words >= 1 + kStats + 1 ? words - 1 - kStats : 0;
                    lastLevelUpDeltas_ = {};
                    if (kPowers > 0) {
                        auto next = [&packet] { return static_cast<int32_t>(packet.readUInt32()); };
                        lastLevelUpDeltas_.hp   = next();
                        lastLevelUpDeltas_.mana = next();
                        for (size_t i = 1; i < kPowers; ++i) next();
                        lastLevelUpDeltas_.str   = next();
                        lastLevelUpDeltas_.agi   = next();
                        lastLevelUpDeltas_.sta   = next();
                        lastLevelUpDeltas_.intel = next();
                        lastLevelUpDeltas_.spi   = next();
                    }
                    uint32_t oldLevel = serverPlayerLevel_;
                    serverPlayerLevel_ = std::max(serverPlayerLevel_, newLevel);
                    // Update the character-list entry so the selection screen
                    // shows the correct level if the player logs out and back.
                    for (auto& ch : characters) {
                        if (ch.guid == playerGuid) {
                            ch.level = serverPlayerLevel_;
                            break;  // was 'return' - must NOT exit here or level-up notification is skipped
                        }
                    }
                    if (newLevel > oldLevel) {
                        // Not when the interface writes it: chatframe.lua's
                        // PLAYER_LEVEL_UP branch formats LEVEL_UP from arg1
                        // and adds the health and mana gains under it, so this
                        // line was a second, shorter copy above its own.
                        if (!ui::frameXmlOwns(ui::UiElement::Chat)) {
                            addSystemChatMessage("You have reached level " +
                                                 std::to_string(newLevel) + "!");
                        }
                        // LEVELUP (888) is the $SND event of
                        // Spells\LevelUp\LevelUp.m2, which the callback shows
                        // on the player; it plays the sound by itself only
                        // when the model cannot be shown.
                        if (levelUpCallback_) levelUpCallback_(newLevel);
                        else withSoundManager(&audio::AudioCoordinator::getUiSoundManager, [](auto* sfx) { sfx->playLevelUp(); });
                        // All nine, in the order the interface reads them:
                        // level, health, power, talent points, then the five
                        // stats. The chat frame tests the third against zero
                        // to decide whether to mention mana, so sending only
                        // the level was an error on every level gained - and
                        // every one of these was already parsed above.
                        //
                        // Talent points are zero: the packet carries no such
                        // delta, and zero is what stops the interface claiming
                        // a point was gained.
                        fireAddonEvent("PLAYER_LEVEL_UP", {
                            std::to_string(newLevel),
                            std::to_string(lastLevelUpDeltas_.hp),
                            std::to_string(lastLevelUpDeltas_.mana),
                            "0",
                            std::to_string(lastLevelUpDeltas_.str),
                            std::to_string(lastLevelUpDeltas_.agi),
                            std::to_string(lastLevelUpDeltas_.sta),
                            std::to_string(lastLevelUpDeltas_.intel),
                            std::to_string(lastLevelUpDeltas_.spi),
                        });
                    }
                }
            }
            packet.skipAll();
        };
    }

    // ---- MSG_RAID_TARGET_UPDATE ----
    dispatchTable_[Opcode::MSG_RAID_TARGET_UPDATE] = [this](network::Packet& packet) {
        RaidTargetUpdateData rtu;
        if (!RaidTargetUpdateParser::parse(packet, rtu)) return;

        if (socialHandler_) {
            // The full list is authoritative, so icons absent from it are gone.
            if (rtu.fullList) {
                for (uint32_t i = 0; i < kRaidMarkCount; ++i)
                    socialHandler_->setRaidTargetGuid(static_cast<uint8_t>(i), 0);
            }
            for (const auto& [icon, guid] : rtu.marks)
                socialHandler_->setRaidTargetGuid(icon, guid);
        }
        LOG_DEBUG("MSG_RAID_TARGET_UPDATE: fullList=", rtu.fullList,
                  " marks=", rtu.marks.size());
        fireAddonEvent("RAID_TARGET_UPDATE", {});
    };

    // ---- SMSG_CRITERIA_UPDATE ----
    dispatchTable_[Opcode::SMSG_CRITERIA_UPDATE] = [this](network::Packet& packet) {
        // The record's shape is in readCriteriaProgressTail, shared with the
        // criteria half of SMSG_ALL_ACHIEVEMENT_DATA.
        if (packet.hasRemaining(4)) {
            uint32_t criteriaId = packet.readUInt32();
            CriteriaProgressRecord rec;
            if (readCriteriaProgressTail(packet, rec)) {
                // A timed criteria whose window ran out reads as zero, which is
                // what the server means by the flag and what the panel shows.
                const uint64_t progress = (rec.flags == 1) ? 0 : rec.counter;
                uint64_t oldProgress = 0;
                auto cpit = criteriaProgress_.find(criteriaId);
                if (cpit != criteriaProgress_.end()) oldProgress = cpit->second;
                criteriaProgress_[criteriaId] = progress;
                LOG_DEBUG("SMSG_CRITERIA_UPDATE: id=", criteriaId, " progress=", progress);
                // Fire addon event for achievement tracking addons
                if (progress != oldProgress)
                    fireAddonEvent("CRITERIA_UPDATE", {std::to_string(criteriaId), std::to_string(progress)});

                // A timed criteria also drives the tracker's countdown. The
                // message says how far into the window the player is; how long
                // the window is comes from the DBC, so the two have to be put
                // together here - the interface reads the pair and works out a
                // start time from them.
                //
                // Fired for any timed criteria rather than only tracked ones.
                // The tracker stores what it is told and draws a timer only
                // inside its tracked-achievement loop, so entries for the rest
                // cost a table row and show nothing; the gain is that tracking
                // an achievement part-way through its timer still shows the
                // right countdown, instead of nothing until the next update.
                ensureAchievementCriteriaLoaded();
                const auto idx = getAchievementCriterionIndex(criteriaId);
                if (idx.timeLimit != 0 && idx.achievementId != 0) {
                    fireAddonEvent("TRACKED_ACHIEVEMENT_UPDATE",
                                   {std::to_string(idx.achievementId),
                                    std::to_string(criteriaId),
                                    std::to_string(rec.timeElapsed),
                                    std::to_string(idx.timeLimit)});
                }
            }
        }
    };

    // ---- SMSG_BARBER_SHOP_RESULT ----
    dispatchTable_[Opcode::SMSG_BARBER_SHOP_RESULT] = [this](network::Packet& packet) {
        // uint32 result: 0=success, 1/3=not enough money, 2=not seated at barber
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            // Wow.exe (0x52E5B0): success is BARBER_SHOP_SUCCESS alone; 1 and 3
            // ERR_NOT_ENOUGH_MONEY, 2 ERR_NOT_BARBER_SITTING.
            if (result == 0) {
                // Bought: the preview is now the real thing, so it is not
                // put back, and the server's PLAYER_BYTES update confirms it.
                fireAddonEvent("BARBER_SHOP_SUCCESS", {});
                endBarberShop(true);
            } else if (result == 1 || result == 3) {
                displayGameMessage("ERR_NOT_ENOUGH_MONEY", "You don't have enough money.");
            } else if (result == 2) {
                displayGameMessage("ERR_NOT_BARBER_SITTING", "You must be sitting in a barber chair");
            }
            LOG_DEBUG("SMSG_BARBER_SHOP_RESULT: result=", result);
        }
    };

}

// Everything else - inspects, quests, auctions, spells, calendars,
// battlefields, voice, and a long tail that is consumed and ignored.
//
// Named for what it is rather than given a theme it does not have: it arrived
// as five batches of whatever was left over, and the honest description of that
// is 'the rest'.
void GameHandler::registerRemainingOpcodes() {
    // -----------------------------------------------------------------------
    // Batch 8-12: Remaining opcodes (inspects, quests, auctions, spells,
    //             calendars, battlefields, voice, misc consume-only)
    // -----------------------------------------------------------------------
    // uint32 currentZoneLightId + uint32 overrideLightId + uint32 transitionMs
    dispatchTable_[Opcode::SMSG_OVERRIDE_LIGHT] = [this](network::Packet& packet) {
        // uint32 currentZoneLightId + uint32 overrideLightId + uint32 transitionMs
        if (packet.hasRemaining(12)) {
            uint32_t zoneLightId     = packet.readUInt32();
            uint32_t overrideLightId = packet.readUInt32();
            uint32_t transitionMs    = packet.readUInt32();
            overrideLightId_      = overrideLightId;
            overrideLightTransMs_ = transitionMs;
            LOG_DEBUG("SMSG_OVERRIDE_LIGHT: zone=", zoneLightId,
                      " override=", overrideLightId, " transition=", transitionMs, "ms");
        }
    };
    // Classic 1.12: uint32 weatherType + float intensity (8 bytes, no isAbrupt)
    // TBC 2.4.3 / WotLK 3.3.5a: uint32 weatherType + float intensity + uint8 isAbrupt (9 bytes)
    dispatchTable_[Opcode::SMSG_WEATHER] = [this](network::Packet& packet) {
        // Classic 1.12: uint32 weatherType + float intensity (8 bytes, no isAbrupt)
        // TBC 2.4.3 / WotLK 3.3.5a: uint32 weatherType + float intensity + uint8 isAbrupt (9 bytes)
        if (packet.hasRemaining(8)) {
            uint32_t wType = packet.readUInt32();
            float wIntensity = packet.readFloat();
            if (packet.hasRemaining(1))
                /*uint8_t isAbrupt =*/ packet.readUInt8();
            applyWeather(wType, wIntensity);
        }
    };
    // Server-script text message - display in system chat
    dispatchTable_[Opcode::SMSG_SCRIPT_MESSAGE] = [](network::Packet& packet) {
        // Wow.exe registers no handler for this opcode: whatever a server
        // sends on it the stock client drops. Logged only.
        std::string msg = packet.readString();
        if (!msg.empty()) LOG_INFO("SMSG_SCRIPT_MESSAGE (not shown): ", msg);
    };
    // uint64 targetGuid + uint64 casterGuid + uint32 spellId + uint32 displayId + uint32 animType
    dispatchTable_[Opcode::SMSG_ENCHANTMENTLOG] = [this](network::Packet& packet) {
        // Packed target, packed caster, item id, enchantment id
        // (AzerothCore EnchantmentLog). Wow.exe (0x751050) writes it to the
        // combat log as ENCHANT_APPLIED - ENCHANT_REMOVED with no caster - and
        // says nothing in chat; enchantments flagged 0x2 are not logged.
        if (!packet.hasRemaining(2)) { packet.skipAll(); return; }
        const uint64_t enchTargetGuid = packet.readPackedGuid();
        const uint64_t enchCasterGuid = packet.readPackedGuid();
        if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
        const uint32_t itemId = packet.readUInt32();
        const uint32_t enchantId = packet.readUInt32();
        packet.skipAll();
        const std::string enchName = getEnchantName(enchantId);
        if (enchName.empty() || !combatHandler_ || !combatHandler_->stockCombatLogActive()) return;
        if (getEnchantFlags(enchantId) & 0x2u) return;  // 0x751050: not logged
        // 0x751050: the enchantment's name, then the item and its name.
        stock_combat_log::Record r;
        r.event = enchCasterGuid ? stock_combat_log::ENCHANT_APPLIED : stock_combat_log::ENCHANT_REMOVED;
        r.src = enchCasterGuid;
        r.dst = enchTargetGuid;
        r.suffix = stock_combat_log::kSufSpellNameText | stock_combat_log::kSufItem;
        r.spellNameText = enchName;
        r.v58 = static_cast<int32_t>(itemId);
        combatHandler_->logCombatEvent(r);
    };
    // auctionId(u32) + action(u32) + error(u32) + itemEntry(u32) + randomPropertyId(u32) + ...
    // action: 0=sold/won, 1=expired, 2=bid placed on your auction
    // auctionHouseId(u32) + auctionId(u32) + bidderGuid(u64) + bidAmount(u32) + outbidAmount(u32) + itemEntry(u32) + randomPropertyId(u32)
    // uint32 auctionId + uint32 itemEntry + uint32 itemRandom - auction expired/cancelled
    // uint64 containerGuid - tells client to open this container
    // The actual items come via update packets; we just log this.
    // PackedGuid (player guid) + uint32 vehicleId
    // vehicleId == 0 means the player left the vehicle
    dispatchTable_[Opcode::SMSG_PLAYER_VEHICLE_DATA] = [this](network::Packet& packet) {
        // Packed guid + u32 vehicle id: that unit is now a vehicle (or no
        // longer, id 0) - the stock client's 0x716DB0 sets or drops its
        // Vehicle_C. Sent to everyone in range when a player mounts a
        // multi-seat mount. It is not "the player entered a vehicle": that is
        // the player's movement going onto a vehicle's seat.
        if (!packet.hasRemaining(1)) return;
        const uint64_t guid = packet.readPackedGuid();
        const uint32_t id = packet.hasRemaining(4) ? packet.readUInt32() : 0u;
        setUnitVehicleId(guid, id);
    };
    // guid(8) + status(1): status 1 = NPC has available/new routes for this player
    dispatchTable_[Opcode::SMSG_TAXINODE_STATUS] = [this](network::Packet& packet) {
        // guid(8) + status(1): status 1 = NPC has available/new routes for this player
        if (packet.hasRemaining(9)) {
            uint64_t npcGuid = packet.readUInt64();
            uint8_t  status  = packet.readUInt8();
            taxiNpcHasRoutes_[npcGuid] = (status != 0);
        }
    };
    // SMSG_GUILD_DECLINE - moved to SocialHandler::registerOpcodes
    // Clear cached talent data so the talent screen reflects the reset.
    dispatchTable_[Opcode::SMSG_TALENTS_INVOLUNTARILY_RESET] = [this](network::Packet& packet) {
        // Clear cached talent data so the talent screen reflects the reset.
        if (spellHandler_) spellHandler_->resetTalentState();
        // The event alone (Wow.exe 0x526530): uiparent.lua raises the
        // TALENTS_INVOLUNTARILY_RESET popup, which is the whole announcement.
        // The talent frame rebuilds from this; the cached state was cleared
        // above and nothing told it to redraw, so it kept the old trees.
        if (addonEventCallback_) addonEventCallback_("TALENTS_INVOLUNTARILY_RESET", {});
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_SET_REST_START] = [](network::Packet& packet) {
        // The rest-XP accumulation start time, not a statement about where the
        // player is standing: the server sends it on login wherever that is.
        // Resting itself comes from PLAYER_FLAGS_RESTING, which entity_controller
        // watches, because that is the one that clears again on leaving an inn.
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_UPDATE_AURA_DURATION] = [this](network::Packet& packet) {
        if (packet.hasRemaining(5)) {
            uint8_t slot       = packet.readUInt8();
            uint32_t durationMs = packet.readUInt32();
            handleUpdateAuraDuration(slot, durationMs);
        }
    };
    dispatchTable_[Opcode::SMSG_ITEM_NAME_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t itemId = packet.readUInt32();
            std::string name = packet.readString();
            if (!itemInfoCache_.count(itemId) && !name.empty()) {
                ItemQueryResponseData stub;
                stub.entry = itemId;
                stub.name  = std::move(name);
                stub.valid = true;
                itemInfoCache_[itemId] = std::move(stub);
            }
        }
        packet.skipAll();
    };
    // A plain guid: Unit::SendMountSpecialAnim writes GetGUID(), not the
    // packed form, so reading it packed consumed the wrong number of bytes.
    dispatchTable_[Opcode::SMSG_MOUNTSPECIAL_ANIM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        const uint64_t guid = packet.readUInt64();
        if (mountSpecialCallback_) mountSpecialCallback_(guid);
    };
    dispatchTable_[Opcode::SMSG_CHAR_CUSTOMIZE] = [](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            // A character-screen answer (Wow.exe 0x4D9190): the glue screen's
            // dialog says it; nothing in the world's chat.
            (void)packet.readUInt8();
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_CHAR_FACTION_CHANGE] = [](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            // As the customization (Wow.exe 0x4D92D0).
            (void)packet.readUInt8();
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_INVALIDATE_PLAYER] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t guid = packet.readUInt64();
            entityController_->invalidatePlayerName(guid);
        }
    };
    // uint32 movieId - we don't play movies; acknowledge immediately.
    dispatchTable_[Opcode::SMSG_TRIGGER_MOVIE] = [this](network::Packet& packet) {
        // uint32 movieId - we don't play movies; acknowledge immediately.
        packet.skipAll();
        // WotLK servers expect CMSG_COMPLETE_MOVIE after the movie finishes;
        // without it, the server may hang or disconnect the client.
        uint16_t wire = wireOpcode(Opcode::CMSG_COMPLETE_MOVIE);
        if (wire != 0xFFFF) {
            network::Packet ack(wire);
            socket->send(ack);
            LOG_DEBUG("SMSG_TRIGGER_MOVIE: skipped, sent CMSG_COMPLETE_MOVIE");
        }
    };
    // Server-side LFG invite timed out (no response within time limit)
    dispatchTable_[Opcode::SMSG_LFG_TIMEDOUT] = [this](network::Packet& packet) {
        // Server-side LFG invite timed out (no response within time limit)
        addSystemChatMessage("Dungeon Finder: Invite timed out.");
        if (openLfgCallback_) openLfgCallback_();
        packet.skipAll();
    };
    // Another party member failed to respond to a LFG role-check in time
    dispatchTable_[Opcode::SMSG_LFG_OTHER_TIMEDOUT] = [this](network::Packet& packet) {
        // Another party member failed to respond to a LFG role-check in time
        addSystemChatMessage("Dungeon Finder: Another player's invite timed out.");
        if (openLfgCallback_) openLfgCallback_();
        packet.skipAll();
    };
    // uint32 result - LFG auto-join attempt failed (player selected auto-join at queue time)
    dispatchTable_[Opcode::SMSG_LFG_AUTOJOIN_FAILED] = [this](network::Packet& packet) {
        // uint32 result - LFG auto-join attempt failed (player selected auto-join at queue time)
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            (void)result;
        }
        addUIError("Dungeon Finder: Auto-join failed.");
        addSystemChatMessage("Dungeon Finder: Auto-join failed.");
        packet.skipAll();
    };
    // No eligible players found for auto-join
    dispatchTable_[Opcode::SMSG_LFG_AUTOJOIN_FAILED_NO_PLAYER] = [this](network::Packet& packet) {
        // No eligible players found for auto-join
        addUIError("Dungeon Finder: No players available for auto-join.");
        addSystemChatMessage("Dungeon Finder: No players available for auto-join.");
        packet.skipAll();
    };
    // Party leader is currently set to Looking for More (LFM) mode
    dispatchTable_[Opcode::SMSG_LFG_LEADER_IS_LFM] = [this](network::Packet& packet) {
        // Party leader is currently set to Looking for More (LFM) mode
        addSystemChatMessage("Your party leader is currently Looking for More.");
        packet.skipAll();
    };
    // uint32 zoneId + uint8 level_min + uint8 level_max - player queued for meeting stone
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_SETQUEUE] = [this](network::Packet& packet) {
        // uint32 zoneId + uint8 level_min + uint8 level_max - player queued for meeting stone
        if (packet.hasRemaining(6)) {
            uint32_t zoneId   = packet.readUInt32();
            uint8_t  levelMin = packet.readUInt8();
            uint8_t  levelMax = packet.readUInt8();
            char buf[128];
            std::string zoneName = getAreaName(zoneId);
            if (!zoneName.empty())
                std::snprintf(buf, sizeof(buf),
                    "You are now in the Meeting Stone queue for %s (levels %u-%u).",
                    zoneName.c_str(), levelMin, levelMax);
            else
                std::snprintf(buf, sizeof(buf),
                    "You are now in the Meeting Stone queue for zone %u (levels %u-%u).",
                    zoneId, levelMin, levelMax);
            addSystemChatMessage(buf);
            // ERR_MEETING_STONE_IN_QUEUE_S, the line this stands for, plays
            // PVPENTERQUEUE.
            playUiMessageSound("ERR_MEETING_STONE_IN_QUEUE_S");
            LOG_INFO("SMSG_MEETINGSTONE_SETQUEUE: zone=", zoneId,
                     " levels=", static_cast<int>(levelMin), "-", static_cast<int>(levelMax));
        }
        packet.skipAll();
    };
    // Server confirms group found and teleport summon is ready
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_COMPLETE] = [this](network::Packet& packet) {
        // Server confirms group found and teleport summon is ready
        addSystemChatMessage("Meeting Stone: Your group is ready! Use the Meeting Stone to summon.");
        LOG_INFO("SMSG_MEETINGSTONE_COMPLETE");
        packet.skipAll();
    };
    // Meeting stone search is still ongoing
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_IN_PROGRESS] = [this](network::Packet& packet) {
        // Meeting stone search is still ongoing
        addSystemChatMessage("Meeting Stone: Searching for group members...");
        LOG_DEBUG("SMSG_MEETINGSTONE_IN_PROGRESS");
        packet.skipAll();
    };
    // uint64 memberGuid - a player was added to your group via meeting stone
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED] = [this](network::Packet& packet) {
        // uint64 memberGuid - a player was added to your group via meeting stone
        if (packet.hasRemaining(8)) {
            uint64_t memberGuid = packet.readUInt64();
            const auto& memberName = lookupName(memberGuid);
            if (!memberName.empty()) {
                addSystemChatMessage("Meeting Stone: " + memberName +
                                     " has been added to your group.");
            } else {
                addSystemChatMessage("Meeting Stone: A new player has been added to your group.");
            }
            LOG_INFO("SMSG_MEETINGSTONE_MEMBER_ADDED: guid=0x", std::hex, memberGuid, std::dec);
        }
    };
    // uint8 reason - failed to join group via meeting stone
    // 0=target_not_in_lfg, 1=target_in_party, 2=target_invalid_map, 3=target_not_available
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_JOINFAILED] = [this](network::Packet& packet) {
        // uint8 reason - failed to join group via meeting stone
        // 0=target_not_in_lfg, 1=target_in_party, 2=target_invalid_map, 3=target_not_available
        static const char* kMeetingstoneErrors[] = {
            "Target player is not using the Meeting Stone.",
            "Target player is already in a group.",
            "You are not in a valid zone for that Meeting Stone.",
            "Target player is not available.",
        };
        if (packet.hasRemaining(1)) {
            uint8_t reason = packet.readUInt8();
            const char* msg = (reason < 4) ? kMeetingstoneErrors[reason]
                                           : "Meeting Stone: Could not join group.";
            addSystemChatMessage(msg);
            LOG_INFO("SMSG_MEETINGSTONE_JOINFAILED: reason=", static_cast<int>(reason));
        }
    };
    // Player was removed from the meeting stone queue (left, or group disbanded)
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_LEAVE] = [this](network::Packet& packet) {
        // Player was removed from the meeting stone queue (left, or group disbanded)
        addSystemChatMessage("You have left the Meeting Stone queue.");
        LOG_DEBUG("SMSG_MEETINGSTONE_LEAVE");
        packet.skipAll();
    };
    // All three of these carry a uint32 from the same enum, and all three read
    // a single byte of it. Little-endian kept the comparison working for the
    // delete, whose value is nine, and broke the other two outright: creating a
    // ticket answers 2 and updating one answers 4, and both were compared
    // against 1 - so a ticket that went through reported that it had not.
    constexpr uint32_t kTicketAlreadyExists = 1, kTicketCreated = 2,
                       kTicketUpdated = 4, kTicketDeleted = 9;
    // A create's or an update's answer, as Wow.exe (0x5AD240) gives it:
    // ERR_TICKET_ALREADY_EXISTS (1), ERR_TICKET_CREATE_ERROR (3),
    // ERR_TICKET_UPDATE_ERROR (5); a success (2, 4) asks for the ticket again,
    // and the answer redraws the help frame. No line of its own.
    auto gmTicketResult = [this](uint32_t res) {
        switch (res) {
            case kTicketAlreadyExists: displayGameMessage("ERR_TICKET_ALREADY_EXISTS", "You already have an open ticket."); break;
            case kTicketCreated:
            case kTicketUpdated: requestGmTicket(); break;
            case 3: displayGameMessage("ERR_TICKET_CREATE_ERROR", "There was an error creating your ticket. Please try again later."); break;
            case 5: displayGameMessage("ERR_TICKET_UPDATE_ERROR", "There was an error updating your ticket. Please try again later."); break;
            default: break;
        }
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_CREATE] = [gmTicketResult](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            gmTicketResult(packet.readUInt32());
        }
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_UPDATETEXT] = [gmTicketResult](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            gmTicketResult(packet.readUInt32());
        }
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_DELETETICKET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // Deleted (9): UPDATE_TICKET with no ticket; anything else
            // ERR_TICKET_DB_ERROR (Wow.exe 0x5AD240).
            const uint32_t res = packet.readUInt32();
            if (res == kTicketDeleted) {
                gmTicketActive_ = false;
                gmTicketText_.clear();
                fireAddonEvent("UPDATE_TICKET", {});
            } else {
                displayGameMessage("ERR_TICKET_DB_ERROR", "Your ticket could not be retrieved. Please try again later.");
            }
        }
    };
    // WotLK 3.3.5a format:
    //   uint8  status  - 6=has open ticket, 3=closed, anything else none
    // If status == 6 (GMTICKET_STATUS_HASTEXT):
    //   cstring ticketText
    //   uint32  ticketAge       (seconds old)
    //   uint32  daysUntilOld    (days remaining before escalation)
    //   float   waitTimeHours   (estimated GM wait time)
    dispatchTable_[Opcode::SMSG_GMTICKET_GETTICKET] = [this](network::Packet& packet) {
        // WotLK 3.3.5a format:
        //   uint8  status  - 6=has open ticket, 3=closed, anything else none
        // If status == 6 (GMTICKET_STATUS_HASTEXT):
        //   cstring ticketText
        //   uint32  ticketAge       (seconds old)
        //   uint32  daysUntilOld    (days remaining before escalation)
        //   float   waitTimeHours   (estimated GM wait time)
        if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
        uint8_t gmStatus = packet.readUInt8();
        // Status 6 = GMTICKET_STATUS_HASTEXT - open ticket with text
        if (gmStatus == 6 && packet.hasRemaining(1)) {
            gmTicketText_    = packet.readString();
            uint32_t ageSec  = (packet.hasRemaining(4)) ? packet.readUInt32() : 0;
            /*uint32_t daysLeft =*/ (packet.hasRemaining(4)) ? packet.readUInt32() : 0;
            gmTicketWaitHours_ = (packet.hasRemaining(4))
                ? packet.readFloat() : 0.0f;
            gmTicketActive_ = true;
            // No chat line (Wow.exe 0x5AD240): the help frame draws it from
            // UPDATE_TICKET.
            LOG_INFO("SMSG_GMTICKET_GETTICKET: open ticket age=", ageSec,
                     "s wait=", gmTicketWaitHours_, "h");

            // The interface measures all of this in days and works the wait out
            // itself, as (oldestTicketTime - ticketAge). This packet gives the
            // wait directly, so the oldest time is the age plus it - which
            // arrives at the same number the server meant.
            //
            // updateTime says how stale the estimate is; anything over an hour
            // is treated as no estimate at all, and this one was computed from
            // the packet that just arrived.
            const double ageDays = static_cast<double>(ageSec) / 86400.0;
            auto days = [](double v) {
                char out[32];
                std::snprintf(out, sizeof(out), "%.6f", v);
                return std::string(out);
            };
            fireAddonEvent("UPDATE_TICKET", {
                "1",                                            // category: has one
                gmTicketText_,
                days(ageDays),
                days(ageDays + gmTicketWaitHours_ / 24.0),
                "0",                                            // just measured
                "0",                                            // not assigned
                "0",                                            // not opened by a GM
            });
        } else if (gmStatus == 3) {
            gmTicketActive_ = false;
            gmTicketText_.clear();
            LOG_INFO("SMSG_GMTICKET_GETTICKET: ticket closed");
            // No arguments at all is how the interface is told there is no
            // ticket: it reads the first one and stops if it is absent.
            fireAddonEvent("UPDATE_TICKET", {});
        } else {
            // Everything else is "no ticket", including 10.
            //
            // 10 was read as "suspended" and announced in chat, and this is
            // asked on every login - so an account that had never filed a
            // ticket was told at each one that the one it did not have had
            // been suspended. 0x0A is GMTICKET_STATUS_DEFAULT, the answer a
            // server gives when the player has no ticket at all, which is why
            // it arrived every time and why nothing else ever followed it.
            gmTicketActive_ = false;
            gmTicketText_.clear();
            LOG_DEBUG("SMSG_GMTICKET_GETTICKET: no open ticket (status=", static_cast<int>(gmStatus), ")");
            fireAddonEvent("UPDATE_TICKET", {});
        }
        packet.skipAll();
    };
    // uint32 status: 1 = GM support available, 0 = offline/unavailable
    dispatchTable_[Opcode::SMSG_GMTICKET_SYSTEMSTATUS] = [this](network::Packet& packet) {
        // uint32 status: 1 = GM support available, 0 = offline/unavailable
        if (packet.hasRemaining(4)) {
            uint32_t sysStatus = packet.readUInt32();
            gmSupportAvailable_ = (sysStatus != 0);
            // UPDATE_GM_STATUS alone (Wow.exe 0x6E2E90, opcode 0x21B).
            LOG_INFO("SMSG_GMTICKET_SYSTEMSTATUS: available=", gmSupportAvailable_);
            // The help frame disables its "open a ticket" button on anything
            // but GMTICKET_QUEUE_STATUS_ENABLED, which is the number 1 - and
            // gets it from here. Parsed and stored and never said, so a closed
            // queue looked open until the ticket was refused.
            fireAddonEvent("UPDATE_GM_STATUS", {gmSupportAvailable_ ? "1" : "0"});
        }
        packet.skipAll();
    };
    // uint8 runeIndex + uint8 newRuneType (0=Blood,1=Unholy,2=Frost,3=Death)
    dispatchTable_[Opcode::SMSG_CONVERT_RUNE] = [this](network::Packet& packet) {
        // uint8 runeIndex + uint8 newRuneType (0=Blood,1=Unholy,2=Frost,3=Death)
        if (!packet.hasRemaining(2)) {
            packet.skipAll();
            return;
        }
        uint8_t idx  = packet.readUInt8();
        uint8_t type = packet.readUInt8();
        if (idx < 6) {
            playerRunes_[idx].type = static_cast<RuneType>(type & 0x3);
            // Stored and never announced: the rune bar redraws a rune on this
            // event and on nothing else, so a rune converted to Death kept the
            // colour it was drawn with at login.
            fireAddonEvent("RUNE_TYPE_UPDATE", {std::to_string(idx + 1)});
        }
    };
    // uint32 count, then per rune: uint8 type, uint8 elapsed.
    //
    // This read a one-byte "ready mask" and six cooldowns, which is neither the
    // shape nor the meaning. Player::ResyncRunes writes a count first, so the
    // mask was the low byte of that count and every cooldown after it was
    // half a type and half an elapsed value.
    //
    // The polarity was inverted as well. The server sends
    // 255 - cooldown * 51, so 255 means the cooldown has fully elapsed and the
    // rune is ready, and 0 means it was just spent - the opposite of what
    // `1 - cd/255` computed.
    //
    // The type is in here too and was never read, so a rune converted to Death
    // kept whatever it was at login.
    dispatchTable_[Opcode::SMSG_RESYNC_RUNES] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) {
            packet.skipAll();
            return;
        }
        const uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count && i < playerRunes_.size(); ++i) {
            if (!packet.hasRemaining(2)) break;
            const uint8_t type    = packet.readUInt8();
            const uint8_t elapsed = packet.readUInt8();
            playerRunes_[i].type = static_cast<RuneType>(type & 0x3);
            playerRunes_[i].readyFraction = elapsed / 255.0f;
            playerRunes_[i].ready = (elapsed >= 255);
            fireRuneUpdate(i);
        }
        packet.skipAll();
    };
    // uint32 runeMask (bit i=1 → rune i just became ready)
    dispatchTable_[Opcode::SMSG_ADD_RUNE_POWER] = [this](network::Packet& packet) {
        // uint32 runeMask (bit i=1 → rune i just became ready)
        if (!packet.hasRemaining(4)) {
            packet.skipAll();
            return;
        }
        uint32_t runeMask = packet.readUInt32();
        for (int i = 0; i < 6; i++) {
            if (runeMask & (1u << i)) {
                playerRunes_[i].ready = true;
                playerRunes_[i].readyFraction = 1.0f;
                fireRuneUpdate(static_cast<uint32_t>(i));
            }
        }
    };

    // uint8 result: 0=success, 1=failed, 2=disabled
    dispatchTable_[Opcode::SMSG_COMPLAIN_RESULT] = [this](network::Packet& packet) {
        // uint8 result: 0=success, 1=failed, 2=disabled
        // Wow.exe (0x6E2E90, opcode 0x3C8) reads two bytes and answers every
        // one with COMPLAINT_ADDED: a calendar complaint (type 12) through
        // CALENDAR_UPDATE_ERROR, any other as a plain chat line.
        if (packet.hasRemaining(1)) {
            packet.readUInt8();
            const uint8_t type = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            if (type == 12) {
                std::string text = globalString(services_.assetManager, "COMPLAINT_ADDED");
                fireAddonEvent("CALENDAR_UPDATE_ERROR", {text.empty() ? "Complaint Registered." : text});
            } else {
                addGlobalStringChatLine("COMPLAINT_ADDED", "Complaint Registered.");
            }
        }
        packet.skipAll();
    };
    // uint32 slot + packed_guid unit (0 packed = clear slot)
    dispatchTable_[Opcode::SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT] = [this](network::Packet& packet) {
        // uint32 type, then a body that depends on it. The first three types
        // carry a packed guid and a one-byte frame priority; the objective and
        // timer types carry one or two bytes; a refresh carries nothing and is
        // four bytes on the wire.
        //
        // What was here read the type as a slot index and the rest as a guid,
        // which put every engaging boss in slot zero - the value of
        // ENCOUNTER_FRAME_ENGAGE - and wrote a disengaging one into slot one
        // instead of clearing it. The five-byte guard also dropped a refresh
        // outright.
        enum : uint32_t {
            kEngage = 0, kDisengage = 1, kUpdatePriority = 2,
            kAddTimer = 3, kEnableObjective = 4, kUpdateObjective = 5,
            kDisableObjective = 6, kRefreshFrames = 7,
        };
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t type = packet.readUInt32();
        if (!socialHandler_) { packet.skipAll(); return; }

        // Wow.exe keeps up to sixteen units, packed and sorted by priority
        // then engage order; bossN is the N-th (0x5ED750, 0x5ED710). An
        // engage of a unit already there, or a priority for one that is not,
        // changes nothing and raises nothing.
        auto& units = socialHandler_->encounterUnits();
        switch (type) {
        case kEngage:
        case kDisengage:
        case kUpdatePriority: {
            if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
            const uint64_t unit = packet.readPackedGuid();
            const uint8_t priority = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            bool changed = false;
            if (type == kEngage) changed = units.engage(unit, priority);
            else if (type == kDisengage) changed = units.disengage(unit);
            else changed = units.updatePriority(unit, priority);
            if (!changed) return;
            break;
        }
        case kRefreshFrames:
            // Nothing changed; the frames are asked to draw themselves again
            // after a unit was destroyed client-side, so the event below is
            // the whole of it.
            break;
        case kAddTimer:
        case kEnableObjective:
        case kDisableObjective:
        case kUpdateObjective:
            // Encounter objective and timer widgets, which nothing draws yet.
            packet.skipAll();
            return;
        default:
            packet.skipAll();
            return;
        }

        // The boss frames redraw all five slots on this event, which is why it
        // carries no argument. Stored and never announced, so an encounter
        // filled its slots and no boss frame appeared.
        fireAddonEvent("INSTANCE_ENCOUNTER_ENGAGE_UNIT", {});
    };
    // charName (cstring) + guid (uint64) + achievementId (uint32) + ...
    dispatchTable_[Opcode::SMSG_SERVER_FIRST_ACHIEVEMENT] = [this](network::Packet& packet) {
        // charName (cstring) + guid (uint64) + achievementId (uint32) + ...
        if (packet.hasData()) {
            std::string charName = packet.readString();
            if (packet.hasRemaining(12)) {
                const uint64_t guid = packet.readUInt64();
                const uint32_t achievementId = packet.readUInt32();
                const uint32_t linkName = packet.hasRemaining(4) ? packet.readUInt32() : 0;
                // An achievement chat line (CHAT_MSG_ACHIEVEMENT) from the
                // earner: SERVER_FIRST_ACHIEVEMENT, or with the link flag
                // PLAYER_SERVER_FIRST_ACHIEVEMENT, $a written out as the
                // achievement's link (Wow.exe 0x50B010). The chat frame puts
                // the earner's name into its %s.
                const char* key = linkName ? "PLAYER_SERVER_FIRST_ACHIEVEMENT" : "SERVER_FIRST_ACHIEVEMENT";
                std::string text = globalString(services_.assetManager, key);
                if (text.empty()) text = "%s has earned the achievement $a!";
                loadAchievementNameCache();
                auto nit = achievementNameCache_.find(achievementId);
                const std::string achName = nit != achievementNameCache_.end() ? nit->second : std::string();
                const std::string link = "|cffffff00|Hachievement:" + std::to_string(achievementId) +
                                         ":0:0:0:0:0:0:0:0:0|h[" + achName + "]|h|r";
                for (size_t pos; (pos = text.find("$a")) != std::string::npos;) text.replace(pos, 2, link);
                MessageChatData line;
                line.type = ChatType::ACHIEVEMENT;
                line.language = ChatLanguage::UNIVERSAL;
                line.senderGuid = guid;
                line.senderName = charName;
                line.achievementId = achievementId;
                line.message = text;
                addLocalChatMessage(line);
            }
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_SUSPEND_COMMS] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t seqIdx = packet.readUInt32();
            if (socket) {
                network::Packet ack(wireOpcode(Opcode::CMSG_SUSPEND_COMMS_ACK));
                ack.writeUInt32(seqIdx);
                socket->send(ack);
            }
        }
    };
    // SMSG_PRE_RESURRECT: packed GUID of the player who can self-resurrect.
    // Sent when the dead player has Reincarnation (Shaman), Twisting Nether (Warlock),
    // or Deathpact (Death Knight passive). The client must send CMSG_SELF_RES to accept.
    dispatchTable_[Opcode::SMSG_PRE_RESURRECT] = [this](network::Packet& packet) {
        // SMSG_PRE_RESURRECT: packed GUID of the player who can self-resurrect.
        // Sent when the dead player has Reincarnation (Shaman), Twisting Nether (Warlock),
        // or Deathpact (Death Knight passive). The client must send CMSG_SELF_RES to accept.
        uint64_t targetGuid = packet.readPackedGuid();
        if (targetGuid == playerGuid || targetGuid == 0) {
            selfResAvailable_ = true;
            LOG_INFO("SMSG_PRE_RESURRECT: self-resurrection available (guid=0x",
                     std::hex, targetGuid, std::dec, ")");
        }
    };
    dispatchTable_[Opcode::SMSG_PLAYERBINDERROR] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // ERR_DEATHBINDALREADYBOUND, whatever the code (Wow.exe 0x6E2E90,
            // opcode 0x1B6).
            (void)packet.readUInt32();
            displayGameMessage("ERR_DEATHBINDALREADYBOUND", "You are already bound here!");
        }
    };
    // The instance boot timer, which this read nothing of.
    //
    // Two uint32s: how long is left in milliseconds, and why. The whole
    // countdown arrives once and nothing more is sent until it stops, so the
    // deadline is worked out here and the remainder counted from it.
    //
    // It also said the wrong thing. The message was "you must be in a raid
    // group to enter this instance", and that is not what the server sends
    // this for: UpdateHomebindTime raises it when the player is standing in an
    // instance they are not valid for - saved to a different id, or in a party
    // inside a raid - and the sixty seconds is until they are teleported to a
    // graveyard. A player got one line of the wrong explanation and no
    // countdown at all.
    dispatchTable_[Opcode::SMSG_RAID_GROUP_ONLY] = [this](network::Packet& packet) {
        uint32_t timerMs = 0, reason = 0;
        if (packet.hasRemaining(8)) {
            timerMs = packet.readUInt32();
            reason = packet.readUInt32();
        }
        packet.skipAll();

        // Wow.exe (0x6E2E90, opcode 0x286) speaks only when there is no
        // timer: ERR_RAID_GROUP_LOWLEVEL, _ONLY, _FULL or
        // _REQUIREMENTS_UNMATCH for reasons 1-4. The countdown has no line.
        if (timerMs == 0) {
            static const char* const kKeys[] = {"ERR_RAID_GROUP_LOWLEVEL", "ERR_RAID_GROUP_ONLY",
                                                "ERR_RAID_GROUP_FULL", "ERR_RAID_GROUP_REQUIREMENTS_UNMATCH"};
            if (reason >= 1 && reason <= 4) displayGameMessage(kKeys[reason - 1], "");
        }
        if (timerMs == 0) {
            instanceBootDeadline_.reset();
            fireAddonEvent("INSTANCE_BOOT_STOP", {});
            return;
        }
        instanceBootDeadline_ = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(timerMs);
        fireAddonEvent("INSTANCE_BOOT_START", {});
    };
    dispatchTable_[Opcode::SMSG_RAID_READY_CHECK_ERROR] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            // ERR_READY_CHECK_THROTTLED, whatever the byte (Wow.exe 0x5725A0).
            (void)packet.readUInt8();
            displayGameMessage("ERR_READY_CHECK_THROTTLED", "You can't do that yet");
        }
    };
    dispatchTable_[Opcode::SMSG_RESET_FAILED_NOTIFY] = [this](network::Packet& packet) {
        // RESET_FAILED_NOTIFY as a plain chat line; the map is not used
        // (Wow.exe 0x50CEE0).
        packet.skipAll();
        addGlobalStringChatLine("RESET_FAILED_NOTIFY",
            "The party leader has attempted to reset the instance you are in.  Please zone out to allow the instance to reset.");
    };
    // uint32 splitType + uint32 deferTime + string realmName
    // Client must respond with CMSG_REALM_SPLIT to avoid session timeout on some servers.
    dispatchTable_[Opcode::SMSG_REALM_SPLIT] = [this](network::Packet& packet) {
        // uint32 splitType + uint32 deferTime + string realmName
        // Client must respond with CMSG_REALM_SPLIT to avoid session timeout on some servers.
        uint32_t splitType = 0;
        if (packet.hasRemaining(4))
            splitType = packet.readUInt32();
        packet.skipAll();
        if (socket) {
            network::Packet resp(wireOpcode(Opcode::CMSG_REALM_SPLIT));
            resp.writeUInt32(splitType);
            resp.writeString("3.3.5");
            socket->send(resp);
            LOG_DEBUG("SMSG_REALM_SPLIT splitType=", splitType, " - sent CMSG_REALM_SPLIT ack");
        }
    };
    dispatchTable_[Opcode::SMSG_REAL_GROUP_UPDATE] = [this](network::Packet& packet) {
        auto rem = [&]() { return packet.getRemainingSize(); };
        if (rem() < 1) return;
        uint8_t newGroupType = packet.readUInt8();
        if (rem() < 4) return;
        uint32_t newMemberFlags = packet.readUInt32();
        if (rem() < 8) return;
        uint64_t newLeaderGuid = packet.readUInt64();

        if (socialHandler_) {
            auto& pd = socialHandler_->mutablePartyData();
            pd.groupType = newGroupType;
            pd.leaderGuid = newLeaderGuid;

            // Update local player's flags in the member list
            uint64_t localGuid = playerGuid;
            for (auto& m : pd.members) {
                if (m.guid == localGuid) {
                    m.flags = static_cast<uint8_t>(newMemberFlags & 0xFF);
                    break;
                }
            }
        }
        LOG_DEBUG("SMSG_REAL_GROUP_UPDATE groupType=", static_cast<int>(newGroupType),
                  " memberFlags=0x", std::hex, newMemberFlags, std::dec,
                  " leaderGuid=", newLeaderGuid);
        fireAddonEvent("PARTY_LEADER_CHANGED", {});
        fireAddonEvent("GROUP_ROSTER_UPDATE", {});
    };
    dispatchTable_[Opcode::SMSG_PLAY_MUSIC] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playMusicCallback_) playMusicCallback_(soundId);
        }
    };
    dispatchTable_[Opcode::SMSG_PLAY_OBJECT_SOUND] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            // uint32 soundId + uint64 sourceGuid
            uint32_t soundId = packet.readUInt32();
            uint64_t srcGuid = packet.readUInt64();
            LOG_DEBUG("SMSG_PLAY_OBJECT_SOUND: id=", soundId, " src=0x", std::hex, srcGuid, std::dec);
            if (playPositionalSoundCallback_) playPositionalSoundCallback_(soundId, srcGuid);
            else if (playSoundCallback_) playSoundCallback_(soundId);
        } else if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playSoundCallback_) playSoundCallback_(soundId);
        }
    };
    // uint64 targetGuid + uint32 visualId (same structure as SMSG_PLAY_SPELL_VISUAL)
    dispatchTable_[Opcode::SMSG_PLAY_SPELL_IMPACT] = [this](network::Packet& packet) {
        // uint64 targetGuid + uint32 visualId (same structure as SMSG_PLAY_SPELL_VISUAL)
        if (!packet.hasRemaining(12)) {
            packet.skipAll(); return;
        }
        uint64_t impTargetGuid = packet.readUInt64();
        uint32_t impVisualId   = packet.readUInt32();
        if (impVisualId == 0) return;
        auto* renderer = services_.renderer;
        if (!renderer) return;
        glm::vec3 spawnPos;
        if (impTargetGuid == playerGuid) {
            spawnPos = renderer->getCharacterPosition();
        } else {
            auto entity = entityController_->getEntityManager().getEntity(impTargetGuid);
            if (!entity) return;
            glm::vec3 canonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
            spawnPos = core::coords::canonicalToRender(canonical);
        }
        // A SpellVisualKit.dbc id (AzerothCore Unit::SendPlaySpellImpact), on the target.
        if (auto* sv = renderer->getSpellVisualSystem())
            sv->playSpellVisualKit(impVisualId, spawnPos, resolveUnitRenderInstance(impTargetGuid));
    };
    // SMSG_READ_ITEM_OK - moved to InventoryHandler::registerOpcodes
    // SMSG_READ_ITEM_FAILED - moved to InventoryHandler::registerOpcodes
    // SMSG_QUERY_QUESTS_COMPLETED_RESPONSE - moved to QuestHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_NPC_WONT_TALK] = [this](network::Packet& packet) {
        addUIError("That creature can't talk to you right now.");
        raiseUiError("That creature can't talk to you right now.");
        packet.skipAll();
    };

    // (SMSG_PET_UNLEARN_CONFIRM → registered by SpellHandler, which owns the
    // pending-unlearn state the confirm dialog reads. Handling it here wrote
    // to dead duplicate members and the dialog never appeared.)
    // These pet opcodes have incompatible formats - just consume the packet.
    // Previously they shared the unlearn handler, which misinterpreted sound IDs
    // or GUID lists as unlearn costs and could trigger a bogus unlearn dialog.
    // The list of every unit the player controls, which the client keeps
    // beside its pet (0x5D6550); nothing in this client reads past the first.
    dispatchTable_[Opcode::SMSG_PET_GUIDS] = [](network::Packet& packet) { packet.skipAll(); };
    // The pet's voice (Unit_C 0x716B90 -> 0x7474B0): action 0 is a special
    // spell and plays the model's PetOrder sound, 1 an attack and its
    // PetAttack sound, on the pet, while Sound_EnablePetSounds is on.
    dispatchTable_[Opcode::SMSG_PET_ACTION_SOUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        const uint64_t unitGuid = packet.readUInt64();
        const uint32_t action = packet.readUInt32();
        if (action > 1 || !petSoundCallback_) return;
        auto e = getEntityManager().getEntity(unitGuid);
        auto u = std::dynamic_pointer_cast<Unit>(e);
        if (!u) return;
        petSoundCallback_(unitGuid, u->getDisplayId(), action == 0 ? 1 : 2,
                          u->getLatestX(), u->getLatestY(), u->getLatestZ());
    };
    // A dismissed pet's farewell, at where it stood (0x716C00): the
    // CreatureModelData row's CreatureSoundData PetDismiss sound.
    dispatchTable_[Opcode::SMSG_PET_DISMISS_SOUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
        const uint32_t modelId = packet.readUInt32();
        const float x = packet.readFloat();
        const float y = packet.readFloat();
        const float z = packet.readFloat();
        if (petSoundCallback_) {
            // Server axes to the client's canonical ones, as positions are
            // everywhere else (x north, y west).
            const glm::vec3 canon = core::coords::serverToCanonical(glm::vec3(x, y, z));
            petSoundCallback_(0, modelId, 3, canon.x, canon.y, canon.z);
        }
    };
    // Server signals that the pet can now be named (first tame)
    dispatchTable_[Opcode::SMSG_PET_RENAMEABLE] = [this](network::Packet& packet) {
        // Server signals that the pet can now be named (first tame)
        petRenameablePending_ = true;
        // The naming dialog opens on this. The flag alone is read by this
        // client's own UI, so the interface's version never appeared.
        if (addonEventCallback_) addonEventCallback_("PET_RENAMEABLE", {});
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_PET_NAME_INVALID] = [this](network::Packet& packet) {
        // result(4), the name, a declension flag(1) and the five declined
        // forms when set (0x6E2E90 case 0x178 -> 0x5218C0): the result picks
        // the ERR_NAME_* line, and a demanded declension raises
        // PET_FORCE_NAME_DECLENSION with the name and its forms.
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t result = packet.readUInt32();
        static const struct { uint32_t code; const char* key; const char* text; } kNameErrors[] = {
            {2, "ERR_NAME_NO_NAME", "Enter a name."},
            {3, "ERR_NAME_TOO_SHORT", "Names must be at least 2 characters long."},
            {4, "ERR_NAME_TOO_LONG", "Names must be no more than 12 characters long."},
            {6, "ERR_NAME_MIXED_LANGUAGES", "Names must contain only one language."},
            {7, "ERR_NAME_PROFANE", "That name contains mature language."},
            {8, "ERR_NAME_RESERVED", "That name is unavailable."},
            {11, "ERR_NAME_THREE_CONSECUTIVE", "You cannot use the same letter three times consecutively."},
            {12, "ERR_NAME_INVALID_SPACE", "You cannot use an apostrophe as the first or last character of your name."},
            {13, "ERR_NAME_CONSECUTIVE_SPACES", "You can only have one apostrophe."},
            {14, "ERR_NAME_RUSSIAN_CONSECUTIVE_SILENT_CHARACTERS", "Invalid name."},
            {15, "ERR_NAME_RUSSIAN_SILENT_CHARACTER_AT_BEGINNING_OR_END", "Invalid name."},
            {16, "ERR_NAME_DECLENSION_DOESNT_MATCH_BASE_NAME", "Invalid name."},
        };
        const char* key = "ERR_NAME_INVALID";
        const char* text = "Names must contain only letters.";
        for (const auto& e : kNameErrors) {
            if (e.code == result) { key = e.key; text = e.text; break; }
        }
        displayGameMessage(key, text);
        const std::string name = packet.hasRemaining(1) ? packet.readString() : std::string();
        if (packet.hasRemaining(1) && packet.readUInt8() != 0) {
            std::vector<std::string> args{name};
            for (int i = 0; i < 5; ++i) args.push_back(packet.hasRemaining(1) ? packet.readString() : std::string());
            fireAddonEvent("PET_FORCE_NAME_DECLENSION", args);
        }
        packet.skipAll();
    };
    // Classic 1.12: PackedGUID + 19×uint32 itemEntries (EQUIPMENT_SLOT_END=19)
    // This opcode is only reachable on Classic servers; TBC/WotLK wire 0x115 maps to
    // SMSG_INSPECT_RESULTS_UPDATE which is handled separately.
    dispatchTable_[Opcode::SMSG_INSPECT] = [this](network::Packet& packet) {
        // Classic 1.12: PackedGUID + 19×uint32 itemEntries (EQUIPMENT_SLOT_END=19)
        // This opcode is only reachable on Classic servers; TBC/WotLK wire 0x115 maps to
        // SMSG_INSPECT_RESULTS_UPDATE which is handled separately.
        if (!packet.hasRemaining(2)) {
            packet.skipAll(); return;
        }
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0) { packet.skipAll(); return; }

        constexpr int kGearSlots = 19;
        size_t needed = kGearSlots * sizeof(uint32_t);
        if (!packet.hasRemaining(needed)) {
            packet.skipAll(); return;
        }

        std::array<uint32_t, 19> items{};
        for (int s = 0; s < kGearSlots; ++s)
            items[s] = packet.readUInt32();

        // Resolve player name
        auto ent = entityController_->getEntityManager().getEntity(guid);
        std::string playerName = "Target";
        if (ent) {
            auto pl = std::dynamic_pointer_cast<Player>(ent);
            if (pl && !pl->getName().empty()) playerName = pl->getName();
        }

        // Populate inspect result immediately (no talent data in Classic SMSG_INSPECT)
        if (socialHandler_) {
            auto& ir = socialHandler_->mutableInspectResult();
            ir.guid           = guid;
            ir.playerName     = playerName;
            ir.totalTalents   = 0;
            ir.unspentTalents = 0;
            ir.talentGroups   = 0;
            ir.activeTalentGroup = 0;
            ir.itemEntries    = items;
            ir.enchantIds     = {};
        }

        cacheInspectedPlayerEquipment(guid, items);

        LOG_INFO("SMSG_INSPECT (Classic): ", playerName, " has gear in ",
                 std::count_if(items.begin(), items.end(),
                               [](uint32_t e) { return e != 0; }), "/19 slots");
        if (addonEventCallback_) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
            fireAddonEvent("INSPECT_READY", {guidBuf});
            // INSPECT_READY is a later expansion's name and nothing in this
            // interface listens for it. The inspect paperdoll refreshes on
            // UNIT_INVENTORY_CHANGED for the unit it is showing, so the gear
            // arrived and the window was never told - it kept whatever it had
            // when it opened. Fired for the unit token this guid answers to,
            // and only when it answers to one: an inspected player who is not
            // the target has no token for the interface to match against.
            const std::string inspectUnit = guidToUnitId(guid);
            if (!inspectUnit.empty()) {
                fireAddonEvent("UNIT_INVENTORY_CHANGED", {inspectUnit});
            }
        }
    };
    // Same wire format as SMSG_COMPRESSED_MOVES: uint8 size + uint16 opcode + payload[]
    dispatchTable_[Opcode::SMSG_MULTIPLE_MOVES] = [this](network::Packet& packet) {
        // Same wire format as SMSG_COMPRESSED_MOVES: uint8 size + uint16 opcode + payload[]
        if (movementHandler_) movementHandler_->handleCompressedMoves(packet);
    };
    // Each sub-packet uses the standard WotLK server wire format:
    //   uint16_be subSize  (includes the 2-byte opcode; payload = subSize - 2)
    //   uint16_le subOpcode
    //   payload  (subSize - 2 bytes)
    dispatchTable_[Opcode::SMSG_MULTIPLE_PACKETS] = [this](network::Packet& packet) {
        // Each sub-packet uses the standard WotLK server wire format:
        //   uint16_be subSize  (includes the 2-byte opcode; payload = subSize - 2)
        //   uint16_le subOpcode
        //   payload  (subSize - 2 bytes)
        const auto& pdata = packet.getData();
        size_t dataLen = pdata.size();
        size_t pos = packet.getReadPos();
        static uint32_t multiPktWarnCount = 0;
        std::vector<network::Packet> subPackets;
        while (pos + 4 <= dataLen) {
            uint16_t subSize = static_cast<uint16_t>(
                (static_cast<uint16_t>(pdata[pos]) << 8) | pdata[pos + 1]);
            if (subSize < 2) break;
            size_t payloadLen = subSize - 2;
            if (pos + 4 + payloadLen > dataLen) {
                if (++multiPktWarnCount <= 10) {
                    LOG_WARNING("SMSG_MULTIPLE_PACKETS: sub-packet overruns buffer at pos=",
                                pos, " subSize=", subSize, " dataLen=", dataLen);
                }
                break;
            }
            uint16_t subOpcode = static_cast<uint16_t>(pdata[pos + 2]) |
                                 (static_cast<uint16_t>(pdata[pos + 3]) << 8);
            std::vector<uint8_t> subPayload(pdata.begin() + pos + 4,
                                            pdata.begin() + pos + 4 + payloadLen);
            subPackets.emplace_back(subOpcode, std::move(subPayload));
            pos += 4 + payloadLen;
        }
        for (auto& subPacket : std::views::reverse(subPackets)) {
            enqueueIncomingPacketFront(std::move(subPacket));
        }
        packet.skipAll();
    };
    // Recruit-A-Friend: a mentor is offering to grant you a level
    dispatchTable_[Opcode::SMSG_PROPOSE_LEVEL_GRANT] = [this](network::Packet& packet) {
        // Recruit-A-Friend: a mentor is offering to grant you a level.
        // HandleGrantLevel writes GetPackGUID(), so this is packed.
        if (packet.hasRemaining(1)) {
            uint64_t mentorGuid = packet.readPackedGuid();
            std::string mentorName;
            auto ent = entityController_->getEntityManager().getEntity(mentorGuid);
            if (auto* unit = dynamic_cast<Unit*>(ent.get())) mentorName = unit->getName();
            if (mentorName.empty()) mentorName = lookupName(mentorGuid);
            // LEVEL_GRANT_PROPOSED and its popup, no chat line (Wow.exe
            // 0x513FD0).
            if (!mentorName.empty()) fireAddonEvent("LEVEL_GRANT_PROPOSED", {mentorName});
            else queryPlayerName(mentorGuid);
        }
        packet.skipAll();
    };
    // SMSG_REFER_A_FRIEND_EXPIRED - moved to SocialHandler::registerOpcodes
    // SMSG_REFER_A_FRIEND_FAILURE - moved to SocialHandler::registerOpcodes
    // SMSG_REPORT_PVP_AFK_RESULT - moved to SocialHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_RESPOND_INSPECT_ACHIEVEMENTS] = [this](network::Packet& packet) {
        loadAchievementNameCache();
        if (!packet.hasRemaining(1)) return;
        uint64_t inspectedGuid = packet.readPackedGuid();
        if (inspectedGuid == 0) { packet.skipAll(); return; }
        std::unordered_map<uint32_t, uint32_t> achievements;
        while (packet.hasRemaining(4)) {
            uint32_t id = packet.readUInt32();
            if (id == 0xFFFFFFFF) break;
            if (!packet.hasRemaining(4)) break;
            // Kept rather than skipped: the comparison tab prints the date each
            // row was earned beside it.
            achievements[id] = packet.readUInt32();
        }
        while (packet.hasRemaining(4)) {
            uint32_t id = packet.readUInt32();
            if (id == 0xFFFFFFFF) break;
            if (!packet.hasRemaining(16)) break;
            packet.readUInt64(); packet.readUInt32(); packet.readUInt32();
        }
        inspectedPlayerAchievements_[inspectedGuid] = std::move(achievements);
        LOG_INFO("SMSG_RESPOND_INSPECT_ACHIEVEMENTS: guid=0x", std::hex, inspectedGuid, std::dec,
                 " achievements=", inspectedPlayerAchievements_[inspectedGuid].size());
        // The comparison tab reads its totals on this and nothing else, so
        // without it the panel sat on whatever it had when it opened.
        fireAddonEvent("INSPECT_ACHIEVEMENT_READY", {});
    };
    // AzerothCore sends this to a player who becomes a vehicle. It is no
    // seat change of the player's; the seat follows the movement.
    dispatchTable_[Opcode::SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA] = [](network::Packet& packet) {
        packet.skipAll();
    };
    // uint32 type (0=normal, 1=heavy, 2=tired/restricted) + uint32 minutes played
    dispatchTable_[Opcode::SMSG_PLAY_TIME_WARNING] = [this](network::Packet& packet) {
        // uint32 type (0=normal, 1=heavy, 2=tired/restricted) + uint32 minutes played
        if (packet.hasRemaining(4)) {
            // Flags and seconds; Wow.exe (0x6CD270) picks the warning by flag
            // and gives the time as "%2d:%02d" (hours, minutes), chat lines.
            const uint32_t flags = packet.readUInt32();
            const int32_t secs = packet.hasRemaining(4) ? static_cast<int32_t>(packet.readUInt32()) : 0;
            char when[32];
            std::snprintf(when, sizeof(when), "%2d:%02d", (secs / 60) / 60, (secs / 60) % 60);
            if (flags & 0x1000) displayGameMessage("ERR_APPROACHING_PARTIAL_PLAY_TIME", "", {when});
            else if (flags & 0x40000000) displayGameMessage("ERR_APPROACHING_PARTIAL_PLAY_TIME_2", "", {when});
            else if (flags & 0x2000) displayGameMessage("ERR_APPROACHING_NO_PLAY_TIME", "", {when});
            else if (flags & 0x20000000) displayGameMessage("ERR_APPROACHING_NO_PLAY_TIME_2", "", {when});
            else if (flags & 0x80000000u) displayGameMessage("ERR_UNHEALTHY_TIME", "");
        }
    };
    // WotLK 3.3.5a format:
    //   uint64 mirrorGuid - GUID of the mirror image unit
    //   uint32 displayId  - display ID to render the image with
    //   uint8  raceId     - race of caster
    //   uint8  genderFlag - gender of caster
    //   uint8  classId    - class of caster
    //   uint64 casterGuid - GUID of the player who cast the spell
    //   Followed by equipped item display IDs (11 × uint32) if casterGuid != 0
    // Purpose: tells client how to render the image (same appearance as caster).
    // We parse the GUIDs so units render correctly via their existing display IDs.
    dispatchTable_[Opcode::SMSG_MIRRORIMAGE_DATA] = [this](network::Packet& packet) {
        // WotLK 3.3.5a format:
        //   uint64 mirrorGuid - GUID of the mirror image unit
        //   uint32 displayId  - display ID to render the image with
        //   uint8  raceId     - race of caster
        //   uint8  genderFlag - gender of caster
        //   uint8  classId    - class of caster
        //   uint64 casterGuid - GUID of the player who cast the spell
        //   Followed by equipped item display IDs (11 × uint32) if casterGuid != 0
        // Purpose: tells client how to render the image (same appearance as caster).
        // We parse the GUIDs so units render correctly via their existing display IDs.
        if (!packet.hasRemaining(8)) return;
        uint64_t mirrorGuid = packet.readUInt64();
        if (!packet.hasRemaining(4)) return;
        uint32_t displayId  = packet.readUInt32();
        if (!packet.hasRemaining(3)) return;
        /*uint8_t raceId   =*/ packet.readUInt8();
        /*uint8_t gender   =*/ packet.readUInt8();
        /*uint8_t classId  =*/ packet.readUInt8();
        // Apply display ID to the mirror image unit so it renders correctly
        if (mirrorGuid != 0 && displayId != 0) {
            auto entity = entityController_->getEntityManager().getEntity(mirrorGuid);
            if (entity) {
                auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
                if (unit && unit->getDisplayId() == 0)
                    unit->setDisplayId(displayId);
            }
        }
        LOG_DEBUG("SMSG_MIRRORIMAGE_DATA: mirrorGuid=0x", std::hex, mirrorGuid,
                  " displayId=", std::dec, displayId);
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 zoneId + uint64 expireUnixTime (seconds)
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_ENTRY_INVITE] = [this](network::Packet& packet) {
        // uint32 battleId + uint32 zoneId + uint32 acceptTime - twelve bytes.
        //
        // This required twenty and read a 64-bit guid and a 64-bit time that
        // the server does not send, so it returned on the length check every
        // time and the handler never ran once. The event was written down as
        // never fired, which was true and was the symptom rather than the
        // cause.
        if (!packet.hasRemaining(12)) {
            packet.skipAll(); return;
        }
        const uint32_t bfBattleId = packet.readUInt32();
        const uint32_t bfZoneId   = packet.readUInt32();
        const uint32_t expireTime = packet.readUInt32();
        bfMgrBattleId_ = bfBattleId;
        // The world PvP queue GetWorldPVPQueueStatus reads (0x549500):
        // "confirm" until the server time the invitation lapses.
        setWorldPvpQueue(bfBattleId, bfZoneId, 2, expireTime);
        // Store the invitation so the UI can show a prompt
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfZoneId;
        // No chat line: the event and its popup are all Wow.exe (0x54B610) says.
        // The prompt to enter is a static popup the interface raises from this.
        // The zone id goes with it as the battle id: every handler in the group
        // opens `local battleID = ...` and passes it back when the player
        // answers, so a bare fire would answer for no battlefield.
        if (addonEventCallback_)
            addonEventCallback_("BATTLEFIELD_MGR_ENTRY_INVITE", {std::to_string(bfBattleId)});
        LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTRY_INVITE: zoneId=", bfZoneId);
    };
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_ENTERED] = [this](network::Packet& packet) {
        // uint32 battleId, then three flags - seven bytes. This read a 64-bit
        // guid the server does not send and guarded eight, so on a seven-byte
        // packet it never ran: entering Wintergrasp announced nothing.
        if (packet.hasRemaining(7)) {
            const uint32_t bfBattleId = packet.readUInt32();
            bfMgrBattleId_ = bfBattleId;
            // Wow.exe (0x5498C0): the third byte clears AFK and says so
            // (CLEARED_AFK, a plain chat line); the event carries the battle
            // id and the first two as booleans. Nothing else is said.
            const uint8_t first = packet.readUInt8();
            const uint8_t isSafe  = packet.readUInt8();
            const uint8_t onQueue = packet.readUInt8();
            bfMgrInvitePending_ = false;
            bfMgrActive_        = true;
            setWorldPvpQueue(bfBattleId, worldPvpQueue_.zoneId, 0, 0);
            if (onQueue) addGlobalStringChatLine("CLEARED_AFK", "You are no longer Away.");
            if (addonEventCallback_)
                addonEventCallback_("BATTLEFIELD_MGR_ENTERED",
                                    {std::to_string(bfBattleId), eventBool(first != 0), eventBool(isSafe != 0)});
            LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTERED: isSafe=", static_cast<int>(isSafe), " onQueue=", static_cast<int>(onQueue));
        }
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 battlefieldId + uint64 expireTime
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_INVITE] = [this](network::Packet& packet) {
        // uint32 battleId + uint8 warmup - five bytes, and this required
        // twenty. Same shape as the entry invite above: never ran, and the
        // event it fires was recorded as one the server never sends.
        if (!packet.hasRemaining(5)) {
            packet.skipAll(); return;
        }
        const uint32_t bfId = packet.readUInt32();
        const bool warmup = packet.readUInt8() != 0;
        bfMgrBattleId_ = bfId;
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfId;
        // The event alone (Wow.exe 0x54B7A0).
        if (addonEventCallback_)
            // The warmup flag picks between two dialogs and was read off the
            // wire and thrown away, so the warmup one could never appear.
            addonEventCallback_("BATTLEFIELD_MGR_QUEUE_INVITE",
                                {std::to_string(bfId), eventBool(warmup)});
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_INVITE: bfId=", bfId);
    };
    // uint32 battlefieldId + uint32 teamId + uint8 accepted + uint8 loggingEnabled + uint8 result
    // result: 0=queued, 1=not_in_group, 2=too_high_level, 3=too_low_level,
    //         4=in_cooldown, 5=queued_other_bf, 6=bf_full
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE] = [this](network::Packet& packet) {
        // uint32 battlefieldId + uint32 teamId + uint8 accepted + uint8 loggingEnabled + uint8 result
        // result: 0=queued, 1=not_in_group, 2=too_high_level, 3=too_low_level,
        //         4=in_cooldown, 5=queued_other_bf, 6=bf_full
        if (!packet.hasRemaining(11)) {
            packet.skipAll(); return;
        }
        // Wow.exe (0x54B680) says nothing in chat; the event carries the
        // battle id, accepted, warmup (third byte == 1), whether the player
        // stands in the battle's zone, and the logged-in byte.
        uint32_t bfId2    = packet.readUInt32();
        const uint32_t zoneId = packet.readUInt32();
        uint8_t accepted  = packet.readUInt8();
        const uint8_t loggedIn = packet.readUInt8();
        uint8_t result    = packet.readUInt8();
        setWorldPvpQueue(bfId2, zoneId, accepted ? 1 : 0, 0);
        if (addonEventCallback_) {
            addonEventCallback_("BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE",
                                {std::to_string(bfId2), eventBool(accepted != 0), eventBool(result == 1),
                                 eventBool(zoneId != 0 && zoneId == worldStateZoneId_), eventBool(loggedIn != 0)});
        }
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE: accepted=", static_cast<int>(accepted),
                 " result=", static_cast<int>(result));
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint8 remove
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_EJECT_PENDING] = [this](network::Packet& packet) {
        // A battle id and nothing else. This read a packed pair of a guid and a
        // flag across nine bytes, so the guard alone kept it from running.
        //
        // AzerothCore declares this opcode and never sends it, so nothing here
        // has been exercised - but a wrong reading is worse than an untested
        // one, and the frame that answers it is live.
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t battleId = packet.readUInt32();
        // The byte after it picks between the two dialogs (Wow.exe 0x54B750);
        // no chat line.
        const bool remote = packet.hasRemaining(1) && packet.readUInt8() != 0;
        packet.skipAll();
        LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECT_PENDING: battle ", battleId);
        fireAddonEvent("BATTLEFIELD_MGR_EJECT_PENDING",
                       {std::to_string(battleId), eventBool(remote)});
    };
    // uint64 battlefieldGuid + uint32 reason + uint32 battleStatus + uint8 relocated
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_EJECTED] = [this](network::Packet& packet) {
        // Seven bytes: a battle id, then three single bytes. SendBfLeaveMessage
        // writes exactly that.
        //
        // This guarded seventeen and read a guid, two thirty-two-bit fields and
        // a flag, so it never ran once - and the reason it treated as a small
        // enum is a bit mask, which would have named the wrong thing if it had.
        if (!packet.hasRemaining(7)) { packet.skipAll(); return; }
        const uint32_t battleId = packet.readUInt32();
        const uint8_t reason = packet.readUInt8();
        const uint8_t battleStatus = packet.readUInt8();
        const uint8_t relocated = packet.readUInt8();
        packet.skipAll();

        constexpr uint8_t kClose = 0x01, kExited = 0x08, kLowLevel = 0x10;
        const bool exited = (reason & kExited) != 0;
        const bool lowLevel = (reason & kLowLevel) != 0;

        // No chat line: Wow.exe (0x5499C0) raises the event, whose dialog
        // says why. Its flags: the reason equal to 8 (exited) or 10 (low
        // level), relocated, and the battle status equal to 2.
        (void)kClose; (void)exited; (void)lowLevel;

        LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECTED: battle ", battleId,
                 " reason 0x", std::hex, static_cast<int>(reason), std::dec,
                 " status ", static_cast<int>(battleStatus),
                 relocated ? " (relocated)" : "");

        bfMgrActive_        = false;
        bfMgrInvitePending_ = false;
        // 0x5499C0: these reasons end the world PvP queue.
        switch (reason) {
            case 1: case 2: case 4: case 5: case 7: case 8: case 9: case 10: case 11:
                setWorldPvpQueue(battleId, worldPvpQueue_.zoneId, 0, 0);
                break;
            default: break;
        }

        // battleID, playerExited, relocated, battleActive, lowLevel. The four
        // flags are booleans in the middle of the list, so they go through
        // eventBool - a zero here would read as true and pick the wrong dialog.
        fireAddonEvent("BATTLEFIELD_MGR_EJECTED",
                       {std::to_string(battleId),
                        eventBool(reason == kExited),
                        eventBool(relocated != 0),
                        eventBool(battleStatus == 2),
                        eventBool(reason == 10)});
    };
    // uint32 oldState + uint32 newState
    // States: 0=Waiting, 1=Starting, 2=InProgress, 3=Ending, 4=Cooldown
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_STATE_CHANGE] = [this](network::Packet& packet) {
        // uint32 oldState + uint32 newState
        // States: 0=Waiting, 1=Starting, 2=InProgress, 3=Ending, 4=Cooldown
        if (packet.hasRemaining(8)) {
            // BATTLEFIELD_MGR_STATE_CHANGE with both numbers, no chat line
            // (Wow.exe 0x54B7F0).
            const uint32_t first = packet.readUInt32();
            uint32_t newState   = packet.readUInt32();
            fireAddonEvent("BATTLEFIELD_MGR_STATE_CHANGE", {std::to_string(first), std::to_string(newState)});
            LOG_INFO("SMSG_BATTLEFIELD_MGR_STATE_CHANGE: newState=", newState);
        }
        packet.skipAll();
    };
    // One event in full: the answer to opening it (sendType GET), or the
    // server telling its creator that an add or a copy went through
    // (CalendarMgr::AddEvent sends CALENDAR_SENDTYPE_ADD / _COPY).
    //
    // Three different things to the interface. Blizzard_Calendar opens its
    // view or edit frame on CALENDAR_OPEN_EVENT(calendarType) - nothing else
    // opens it (CalendarFrame_OnEvent) - and closes the create frame on
    // CALENDAR_NEW_EVENT(isCopy=false). An add or a copy is not an opened
    // event, so it goes onto the list and leaves the current event alone.
    dispatchTable_[Opcode::SMSG_CALENDAR_SEND_EVENT] = [this](network::Packet& packet) {
        CalendarEventDetail parsed;
        if (!parseCalendarSendEvent(packet, parsed)) {
            LOG_WARNING("SMSG_CALENDAR_SEND_EVENT: could not be read whole; "
                        "keeping the previous event rather than a partial one");
            packet.skipAll();
            return;
        }
        packet.skipAll();
        LOG_INFO("SMSG_CALENDAR_SEND_EVENT(", static_cast<int>(parsed.sendType), "): '",
                 parsed.title, "' with ", parsed.invitees.size(), " invitee(s)");
        // Names and classes for the invite list; the rows carry only guids.
        for (const auto& inv : parsed.invitees) {
            if (inv.guid != 0 && lookupName(inv.guid).empty()) queryPlayerName(inv.guid);
        }

        if (parsed.sendType != 0) {
            CalendarEvent ev;
            ev.eventId = parsed.eventId;
            ev.title = parsed.title;
            ev.type = parsed.type;
            ev.eventTimePacked = parsed.eventTimePacked;
            ev.eventTime = parsed.eventTime;
            ev.flags = parsed.flags;
            ev.dungeonId = parsed.dungeonId;
            ev.creatorGuid = parsed.creatorGuid;
            upsertCalendarEvent(ev);
            const bool isCopy = parsed.sendType == 2;
            if (!isCopy && calendarEventIsNew_) closeCalendarEvent();
            fireAddonEvent("CALENDAR_NEW_EVENT",
                           {isCopy ? std::string("1") : std::string(kEventNil)});
            fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
            return;
        }

        const bool reopened = !calendarEventIsNew_ &&
                              calendarEventDetail_.eventId == parsed.eventId;
        calendarEventDetail_ = std::move(parsed);
        calendarEventIsNew_ = false;
        if (!reopened) calendarSelectedInvite_ = 0;
        // The edit form commits the draft, so the draft is the event as the
        // server has it - not whatever the last event or new one left there.
        seedCalendarDraftFromDetail();
        if (reopened) {
            fireAddonEvent("CALENDAR_UPDATE_EVENT", {});
        } else {
            fireAddonEvent("CALENDAR_OPEN_EVENT",
                           {calendarTypeForFlags(calendarEventDetail_.flags)});
        }
    };

    // The whole calendar, in answer to a request for it.
    //
    // Six lists back to back with no length prefix on any of them, so this is
    // parsed in one place with a test rather than read inline - a row taken
    // one field wrong slides everything after it and the next count is read
    // out of the middle of a string, which fails silently and completely.
    // See parseCalendarSendCalendar.
    dispatchTable_[Opcode::SMSG_CALENDAR_SEND_CALENDAR] = [this](network::Packet& packet) {
        CalendarData parsed;
        if (!parseCalendarSendCalendar(packet, parsed)) {
            LOG_WARNING("SMSG_CALENDAR_SEND_CALENDAR: could not be read whole; "
                        "keeping the previous calendar rather than a partial one");
            packet.skipAll();
            return;
        }
        calendarData_ = std::move(parsed);
        LOG_INFO("SMSG_CALENDAR_SEND_CALENDAR: ", calendarData_.events.size(),
                 " event(s), ", calendarData_.invites.size(), " invite(s), ",
                 calendarData_.lockouts.size(), " raid lockout(s), ",
                 calendarData_.holidays.size(), " holiday(s)");
        // The addon reads the whole thing on this event and nothing else
        // announces that it arrived.
        if (addonEventCallback_) addonEventCallback_("CALENDAR_UPDATE_EVENT_LIST", {});
        packet.skipAll();
    };
    // uint32 numPending - number of unacknowledged calendar invites
    dispatchTable_[Opcode::SMSG_CALENDAR_SEND_NUM_PENDING] = [this](network::Packet& packet) {
        // uint32 numPending - number of unacknowledged calendar invites
        if (packet.hasRemaining(4)) {
            uint32_t numPending = packet.readUInt32();
            const bool countChanged = (calendarPendingInvites_ != numPending);
            calendarPendingInvites_ = numPending;
            // The minimap's date button carries the indicator: gametime.lua
            // shows GameTimeCalendarInvitesTexture when the count rises and
            // hides it at zero, and it only ever asks on this event and on
            // PLAYER_ENTERING_WORLD. Firing nothing left it correct at login
            // and stale for the rest of the session.
            if (countChanged && addonEventCallback_)
                addonEventCallback_("CALENDAR_UPDATE_PENDING_INVITES", {});
            // No chat line (Wow.exe 0x5B7CE0): the minimap's invite marker
            // is the announcement.
            LOG_DEBUG("SMSG_CALENDAR_SEND_NUM_PENDING: ", numPending, " pending invites");
        }
    };
    // A calendar request refused. CalendarMgr::SendCalendarCommandResult
    // writes uint32 0, uint8 0, a string (the player named by the three
    // errors that name one, empty otherwise) and then the CalendarError - the
    // code is last, not in the byte that was read as it, which is always 0.
    //
    // Shown the way Blizzard_Calendar shows it: CALENDAR_UPDATE_ERROR(text)
    // puts up its CALENDAR_ERROR popup, and the text is the GlobalStrings.lua
    // string for the code, filled in with the name or the limit.
    dispatchTable_[Opcode::SMSG_CALENDAR_COMMAND_RESULT] = [this](network::Packet& packet) {
        CalendarCommandResult result;
        const bool ok = parseCalendarCommandResult(packet, result);
        packet.skipAll();
        if (!ok) return;
        setCalendarActionPending(false);
        if (result.error == 0) return;
        const char* key = calendarErrorStringName(result.error);
        std::string text = key[0] ? globalString(services_.assetManager, key) : std::string();
        if (text.empty()) text = globalString(services_.assetManager, "CALENDAR_ERROR_INTERNAL");
        // One conversion each: %s is the name, %d the limit the server
        // enforces (CalendarLimits in CalendarMgr.h).
        if (auto at = text.find("%s"); at != std::string::npos) {
            text.replace(at, 2, result.param);
        } else if (auto atD = text.find("%d"); atD != std::string::npos) {
            const char* limit = result.error == 2 ? "30" : "100";
            text.replace(atD, 2, limit);
        }
        LOG_INFO("SMSG_CALENDAR_COMMAND_RESULT: error ", result.error, " '", text, "'");
        fireAddonEvent("CALENDAR_UPDATE_ERROR", {text});
    };
    // An event and this player's invite to it (CalendarMgr::
    // SendCalendarEventInviteAlert): sent to every invitee as they are added,
    // the creator included, and to the whole guild for a guild announcement.
    //
    // The event goes onto the list and the invite beside it, so the day it is
    // on shows it without a fresh SMSG_CALENDAR_SEND_CALENDAR. The pending
    // count only moves for a status that still wants an answer - the
    // creator's own confirmed row is not one, and an announcement's
    // NOT_SIGNED_UP is never stored by the server, so it never counts there
    // either (CalendarMgr::AddInvite, GetPlayerNumPending).
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_INVITE_ALERT] = [this](network::Packet& packet) {
        CalendarInviteAlert alert;
        const bool ok = parseCalendarInviteAlert(packet, alert);
        packet.skipAll();
        if (!ok) {
            LOG_WARNING("SMSG_CALENDAR_EVENT_INVITE_ALERT: could not be read whole");
            return;
        }
        upsertCalendarEvent(alert.event);
        const bool announcement = (alert.event.flags & kCalendarFlagAnnouncement) != 0;
        bool newPending = false;
        if (!announcement) {
            CalendarInvite* mine = nullptr;
            for (auto& inv : calendarData_.invites) {
                if (inv.eventId == alert.event.eventId) { mine = &inv; break; }
            }
            if (!mine) {
                calendarData_.invites.push_back({});
                mine = &calendarData_.invites.back();
                newPending = calendarStatusIsPending(alert.status);
            }
            mine->eventId = alert.event.eventId;
            mine->inviteId = alert.inviteId;
            mine->status = alert.status;
            mine->rank = alert.rank;
            mine->isGuildEvent = (alert.event.flags & kCalendarFlagGuildEvent) != 0;
            mine->creatorGuid = alert.event.creatorGuid;
            if (newPending && calendarData_.serverTimeUnix == 0) {
                // No list to recount from yet: the server's count plus this.
                ++calendarPendingInvites_;
                fireAddonEvent("CALENDAR_UPDATE_PENDING_INVITES", {});
            }
        }
        recountCalendarPending();
        // No chat line for an invite (Wow.exe 0x5C3AB0): the pending-invite
        // marker and the calendar's list carry it.
        fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
        LOG_INFO("SMSG_CALENDAR_EVENT_INVITE_ALERT: '", alert.event.title,
                 "' status ", static_cast<int>(alert.status));
    };
    // Someone's answer to an event changed - an RSVP, or the owner setting a
    // status (CalendarMgr::SendCalendarEventStatus, to everyone the event
    // concerns): packedGuid invitee, eventId, event time, flags, status,
    // rank, status time. The old reading here had an invite id first and a
    // title last, neither of which is sent.
    //
    // The invitee's row on the open event moves (CALENDAR_UPDATE_INVITE_LIST,
    // which is how Blizzard_Calendar redraws the RSVP buttons too), and the
    // player's own invite on the list when it is theirs
    // (CALENDAR_UPDATE_EVENT_LIST, the colour of the event on its day).
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_STATUS] = [this](network::Packet& packet) {
        CalendarEventStatusNotice st;
        const bool ok = parseCalendarEventStatus(packet, st);
        packet.skipAll();
        if (!ok) {
            LOG_WARNING("SMSG_CALENDAR_EVENT_STATUS: could not be read whole");
            return;
        }
        if (st.inviteeGuid == getPlayerGuid()) {
            for (auto& inv : calendarData_.invites) {
                if (inv.eventId != st.eventId) continue;
                inv.status = st.status;
                inv.rank = st.rank;
            }
            recountCalendarPending();
            fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
        }
        if (!calendarEventIsNew_ && calendarEventDetail_.eventId == st.eventId) {
            for (auto& inv : calendarEventDetail_.invitees) {
                if (inv.guid != st.inviteeGuid) continue;
                inv.status = st.status;
                inv.rank = st.rank;
                inv.statusTime = unpackWowPackedTime(st.statusTimePacked);
            }
            fireAddonEvent("CALENDAR_UPDATE_INVITE_LIST", {});
        }
    };
    // One row joining an invite list (CalendarMgr::SendCalendarEventInvite).
    //
    // With event id 0 it answers a pre-invite: the name typed into a new
    // event's form, looked up by the server and handed back as a row for the
    // draft, which is how the draft's list grows - and what the creation
    // packet's invite list is then built from. Otherwise it is a row on a
    // real event: added to that event if it is the open one, and to the
    // player's own invites when the row is theirs, which is the only notice a
    // guild event sign-up gets (no invite alert goes to a guild member who
    // signs up - CalendarMgr::AddInvite).
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_INVITE] = [this](network::Packet& packet) {
        CalendarEventInviteNotice n;
        const bool ok = parseCalendarEventInvite(packet, n);
        packet.skipAll();
        if (!ok) {
            LOG_WARNING("SMSG_CALENDAR_EVENT_INVITE: could not be read whole");
            return;
        }
        if (lookupName(n.inviteeGuid).empty()) queryPlayerName(n.inviteeGuid);
        const bool forDraft = (n.eventId == 0 && calendarEventIsNew_);
        const bool forOpen = (n.eventId != 0 && !calendarEventIsNew_ &&
                              calendarEventDetail_.eventId == n.eventId);
        if (forDraft || forOpen) {
            CalendarEventInvitee* row = nullptr;
            for (auto& inv : calendarEventDetail_.invitees) {
                if (inv.guid == n.inviteeGuid) { row = &inv; break; }
            }
            if (!row) {
                calendarEventDetail_.invitees.push_back({});
                row = &calendarEventDetail_.invitees.back();
                row->guid = n.inviteeGuid;
            }
            row->inviteId = n.inviteId;
            row->level = n.level;
            row->status = n.status;
            if (n.hasStatusTime) row->statusTime = unpackWowPackedTime(n.statusTimePacked);
            fireAddonEvent("CALENDAR_UPDATE_INVITE_LIST", {});
        }
        if (n.eventId != 0 && n.inviteeGuid == getPlayerGuid()) {
            CalendarInvite* mine = nullptr;
            for (auto& inv : calendarData_.invites) {
                if (inv.eventId == n.eventId) { mine = &inv; break; }
            }
            if (!mine) {
                calendarData_.invites.push_back({});
                mine = &calendarData_.invites.back();
                mine->eventId = n.eventId;
                for (const auto& ev : calendarData_.events) {
                    if (ev.eventId != n.eventId) continue;
                    mine->isGuildEvent = (ev.flags & kCalendarFlagGuildEvent) != 0;
                    mine->creatorGuid = ev.creatorGuid;
                }
            }
            mine->inviteId = n.inviteId;
            mine->status = n.status;
            recountCalendarPending();
            fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
        }
    };
    // The answer to a mass invite, guild or arena team: the members to add
    // (Guild::MassInviteToEvent, ArenaTeam::MassInviteToEvent). The server
    // invites no one itself. For a new event they join its draft list as
    // invited, which is what the creation packet then sends; for an open one
    // each is invited by name, the only way an existing event takes invites.
    auto massInviteAnswer = [this](network::Packet& packet) {
        std::vector<uint64_t> guids;
        const bool ok = parseCalendarMassInviteList(packet, guids);
        packet.skipAll();
        if (!ok) return;
        if (calendarEventIsNew_) {
            for (uint64_t guid : guids) {
                bool present = false;
                for (const auto& inv : calendarEventDetail_.invitees) {
                    if (inv.guid == guid) { present = true; break; }
                }
                if (present) continue;
                CalendarEventInvitee inv;
                inv.guid = guid;
                inv.status = kCalendarStatusInvited;
                calendarEventDetail_.invitees.push_back(inv);
                if (lookupName(guid).empty()) queryPlayerName(guid);
            }
            fireAddonEvent("CALENDAR_UPDATE_INVITE_LIST", {});
        } else if (calendarEventDetail_.eventId != 0) {
            const bool guildEvent = (calendarEventDetail_.flags & kCalendarFlagGuildEvent) != 0;
            for (uint64_t guid : guids) {
                const std::string& name = lookupName(guid);
                if (name.empty()) continue;
                inviteToCalendarEvent(calendarEventDetail_.eventId, 0, name, false, guildEvent);
            }
        }
    };
    dispatchTable_[Opcode::SMSG_CALENDAR_FILTER_GUILD] = massInviteAnswer;
    dispatchTable_[Opcode::SMSG_CALENDAR_ARENA_TEAM] = massInviteAnswer;
    // An event was deleted (CalendarMgr::SendCalendarEventRemovedAlert, to
    // everyone it concerned). Off the list, its invite with it, and closed if
    // it is the one open - the view frame closes on CALENDAR_CLOSE_EVENT.
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_REMOVED_ALERT] = [this](network::Packet& packet) {
        uint64_t eventId = 0;
        const bool ok = parseCalendarEventRemovedAlert(packet, eventId);
        packet.skipAll();
        if (!ok) return;
        auto& events = calendarData_.events;
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [eventId](const CalendarEvent& e) { return e.eventId == eventId; }),
                     events.end());
        auto& invites = calendarData_.invites;
        invites.erase(std::remove_if(invites.begin(), invites.end(),
                                     [eventId](const CalendarInvite& i) { return i.eventId == eventId; }),
                      invites.end());
        recountCalendarPending();
        if (!calendarEventIsNew_ && calendarEventDetail_.eventId == eventId) {
            closeCalendarEvent();
            fireAddonEvent("CALENDAR_CLOSE_EVENT", {});
        }
        fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
    };
    // Someone was taken off an invite list (CalendarMgr::
    // SendCalendarEventInviteRemove, to everyone the event concerns).
    // Their row leaves the open event; if it was the player, their invite
    // leaves the list - and the event too, unless it is a guild event, which
    // a guild member sees whether invited or not.
    auto dropOwnInvite = [this](uint64_t eventId, uint32_t flags) {
        auto& invites = calendarData_.invites;
        invites.erase(std::remove_if(invites.begin(), invites.end(),
                                     [eventId](const CalendarInvite& i) { return i.eventId == eventId; }),
                      invites.end());
        if (!(flags & (kCalendarFlagGuildEvent | kCalendarFlagAnnouncement))) {
            auto& events = calendarData_.events;
            events.erase(std::remove_if(events.begin(), events.end(),
                                        [eventId](const CalendarEvent& e) { return e.eventId == eventId; }),
                         events.end());
            if (!calendarEventIsNew_ && calendarEventDetail_.eventId == eventId) {
                closeCalendarEvent();
                fireAddonEvent("CALENDAR_CLOSE_EVENT", {});
            }
        }
        recountCalendarPending();
        fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
    };
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED] =
        [this, dropOwnInvite](network::Packet& packet) {
        CalendarInviteRemovedNotice n;
        const bool ok = parseCalendarInviteRemoved(packet, n);
        packet.skipAll();
        if (!ok) return;
        if (!calendarEventIsNew_ && calendarEventDetail_.eventId == n.eventId) {
            auto& list = calendarEventDetail_.invitees;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&n](const CalendarEventInvitee& i) { return i.guid == n.inviteeGuid; }),
                       list.end());
            calendarSelectedInvite_ = 0;
            fireAddonEvent("CALENDAR_UPDATE_INVITE_LIST", {});
        }
        if (n.inviteeGuid == getPlayerGuid()) dropOwnInvite(n.eventId, n.flags);
    };
    // The same, to the one removed (SendCalendarEventInviteRemoveAlert).
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT] =
        [dropOwnInvite](network::Packet& packet) {
        CalendarInviteRemovedAlert n;
        const bool ok = parseCalendarInviteRemovedAlert(packet, n);
        packet.skipAll();
        if (ok) dropOwnInvite(n.eventId, n.flags);
    };
    // An invitee made or unmade moderator (SendCalendarEventModeratorStatusAlert).
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT] =
        [this](network::Packet& packet) {
        CalendarModeratorNotice n;
        const bool ok = parseCalendarModeratorStatus(packet, n);
        packet.skipAll();
        if (!ok) return;
        if (n.inviteeGuid == getPlayerGuid()) {
            for (auto& inv : calendarData_.invites) {
                if (inv.eventId == n.eventId) inv.rank = n.rank;
            }
            fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
        }
        if (!calendarEventIsNew_ && calendarEventDetail_.eventId == n.eventId) {
            for (auto& inv : calendarEventDetail_.invitees) {
                if (inv.guid == n.inviteeGuid) inv.rank = n.rank;
            }
            fireAddonEvent("CALENDAR_UPDATE_INVITE_LIST", {});
        }
    };
    // An event was edited (CalendarMgr::SendCalendarEventUpdateAlert). The
    // list's copy and the open one both take the new fields; the open one
    // re-seeds the edit form's draft so a later save does not write the old
    // values back over the edit.
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_UPDATED_ALERT] = [this](network::Packet& packet) {
        CalendarEventUpdatedAlert u;
        const bool ok = parseCalendarEventUpdatedAlert(packet, u);
        packet.skipAll();
        if (!ok) return;
        for (auto& ev : calendarData_.events) {
            if (ev.eventId != u.eventId) continue;
            ev.title = u.title;
            ev.type = u.type;
            ev.flags = u.flags;
            ev.dungeonId = u.dungeonId;
            ev.eventTimePacked = u.eventTimePacked;
            ev.eventTime = unpackWowPackedTime(u.eventTimePacked);
        }
        if (!calendarEventIsNew_ && calendarEventDetail_.eventId == u.eventId) {
            auto& d = calendarEventDetail_;
            d.title = u.title;
            d.description = u.description;
            d.type = u.type;
            d.flags = u.flags;
            d.dungeonId = u.dungeonId;
            d.eventTimePacked = u.eventTimePacked;
            d.eventTime = unpackWowPackedTime(u.eventTimePacked);
            d.repeatOption = u.repeatOption;
            d.maxInvites = u.maxInvites;
            seedCalendarDraftFromDetail();
            fireAddonEvent("CALENDAR_UPDATE_EVENT", {});
        }
        fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
    };
    // The answer to an RSVP or a sign-up has landed (CalendarHandler.cpp:642).
    dispatchTable_[Opcode::SMSG_CALENDAR_CLEAR_PENDING_ACTION] = [this](network::Packet& packet) {
        packet.skipAll();
        setCalendarActionPending(false);
    };
    // A raid save, as WorldSession::SendCalendarRaidLockout writes it
    // (CalendarHandler.cpp:819): for ADDED a packed time first, then uint32
    // mapId, difficulty, seconds left, and the instance guid in full. The old
    // reading had an invite id and an event id in front - sixteen bytes that
    // are not sent - so the map and difficulty came from the guid.
    //
    // The calendar's own lockout list takes it too, so the day it expires
    // shows it without a fresh SMSG_CALENDAR_SEND_CALENDAR. The list's
    // seconds count from the server time that calendar carried, so a lockout
    // arriving later is moved onto that clock.
    auto applyLockout = [this](uint32_t mapId, uint32_t difficulty,
                               uint32_t secondsLeft, uint64_t instanceGuid, bool add) {
        auto& locks = calendarData_.lockouts;
        locks.erase(std::remove_if(locks.begin(), locks.end(),
                                   [&](const CalendarRaidLockout& l) {
                                       return l.mapId == mapId && l.difficulty == difficulty;
                                   }),
                    locks.end());
        if (add && calendarData_.serverTimeUnix != 0) {
            const auto now = static_cast<int64_t>(std::time(nullptr));
            const int64_t since = std::max<int64_t>(0, now - calendarData_.serverTimeUnix);
            CalendarRaidLockout l;
            l.mapId = mapId;
            l.difficulty = difficulty;
            l.secondsRemaining = static_cast<uint32_t>(secondsLeft + since);
            l.instanceGuid = instanceGuid;
            locks.push_back(l);
        }
        fireAddonEvent("CALENDAR_UPDATE_EVENT_LIST", {});
    };
    dispatchTable_[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_ADDED] =
        [applyLockout](network::Packet& packet) {
        if (packet.hasRemaining(4 + 4 + 4 + 4 + 8)) {
            /*uint32_t packedNow =*/ packet.readUInt32();
            uint32_t mapId     = packet.readUInt32();
            uint32_t difficulty = packet.readUInt32();
            const uint32_t secondsLeft = packet.readUInt32();
            const uint64_t instanceGuid = packet.readUInt64();
            // Stored, no chat line (Wow.exe 0x5C37D0).
            applyLockout(mapId, difficulty, secondsLeft, instanceGuid, true);
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_ADDED: mapId=", mapId, " difficulty=", difficulty);
        }
        packet.skipAll();
    };
    // The same without the leading time: mapId, difficulty, seconds, guid.
    dispatchTable_[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_REMOVED] =
        [applyLockout](network::Packet& packet) {
        if (packet.hasRemaining(4 + 4)) {
            uint32_t mapId     = packet.readUInt32();
            uint32_t difficulty = packet.readUInt32();
            // Stored, no chat line (Wow.exe 0x5C07E0).
            applyLockout(mapId, difficulty, 0, 0, false);
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_REMOVED: mapId=", mapId,
                      " difficulty=", difficulty);
        }
        packet.skipAll();
    };
    // uint32 unixTime - server's current unix timestamp; use to sync gameTime_
    dispatchTable_[Opcode::SMSG_SERVERTIME] = [](network::Packet& packet) {
        // uint32 unixTime - server's current unix timestamp; use to sync gameTime_
        // A unix timestamp, which is not a time of day. It used to be stored
        // in gameTime_ alongside values in three other units, so whichever
        // packet arrived last decided what the sky thought the hour was.
        // Logged and dropped: nothing here needs the server's wall clock, and
        // the game clock has its own opcodes.
        if (packet.hasRemaining(4)) {
            const uint32_t srvTime = packet.readUInt32();
            LOG_DEBUG("SMSG_SERVERTIME: serverTime=", srvTime);
        }
    };
    // uint64 kickerGuid + uint32 kickReasonType + null-terminated reason string
    // kickReasonType: 0=other, 1=afk, 2=vote kick
    dispatchTable_[Opcode::SMSG_KICK_REASON] = [](network::Packet& packet) {
        // uint64 kickerGuid + uint32 kickReasonType + null-terminated reason string
        // kickReasonType: 0=other, 1=afk, 2=vote kick
        if (!packet.hasRemaining(12)) {
            packet.skipAll();
            return;
        }
        uint64_t kickerGuid   = packet.readUInt64();
        uint32_t reasonType   = packet.readUInt32();
        std::string reason;
        if (packet.hasData())
            reason = packet.readString();
        // Kept for the disconnect screen by Wow.exe (0x4D9500); nothing is
        // said in the world.
        (void)kickerGuid;
        LOG_INFO("SMSG_KICK_REASON: reasonType=", reasonType,
                 " reason='", reason, "'");
    };
    // uint32 throttleMs - rate-limited group action; notify the player
    dispatchTable_[Opcode::SMSG_GROUPACTION_THROTTLED] = [this](network::Packet& packet) {
        // uint32 throttleMs - rate-limited group action; notify the player
        if (packet.hasRemaining(4)) {
            // ERR_GROUP_ACTION_THROTTLED, a chat line (Wow.exe 0x6CBE80).
            uint32_t throttleMs = packet.readUInt32();
            displayGameMessage("ERR_GROUP_ACTION_THROTTLED",
                "You have attempted too many group actions in a short period of time.  Please wait momentarily before attempting further group actions.");
            LOG_DEBUG("SMSG_GROUPACTION_THROTTLED: throttleMs=", throttleMs);
        }
    };
    // WotLK 3.3.5a: uint32 ticketId + string subject + string body + uint32 count
    //   per count: string responseText
    dispatchTable_[Opcode::SMSG_GMRESPONSE_RECEIVED] = [this](network::Packet& packet) {
        // WotLK 3.3.5a: uint32 ticketId + string subject + string body + uint32 count
        //   per count: string responseText
        if (!packet.hasRemaining(4)) {
            packet.skipAll();
            return;
        }
        uint32_t ticketId = packet.readUInt32();
        std::string subject;
        std::string body;
        if (packet.hasData()) subject = packet.readString();
        if (packet.hasData()) body    = packet.readString();
        uint32_t responseCount = 0;
        if (packet.hasRemaining(4))
            responseCount = packet.readUInt32();
        std::string responseText;
        for (uint32_t i = 0; i < responseCount && i < 10; ++i) {
            if (packet.hasData()) {
                std::string t = packet.readString();
                if (i == 0) responseText = t;
            }
        }
        // No chat line (Wow.exe 0x5AD240): the help frame opens on the event.
        (void)ticketId;
        // The help frame takes the ticket's own text and the reply, and opens
        // the panel that lets the answer be accepted or more help asked for.
        // Both strings are parsed right here and were only ever said in chat,
        // so the reply arrived and the frame that deals with it never opened.
        //
        // The description first, then the response - the order it unpacks them
        // - and the body stands in for a description the server did not send,
        // since an empty first argument leaves the panel captionless.
        fireAddonEvent("GMRESPONSE_RECEIVED",
                       {subject.empty() ? body : subject, responseText});
        LOG_INFO("SMSG_GMRESPONSE_RECEIVED: ticketId=", ticketId,
                 " subject='", subject, "'");
    };
    dispatchTable_[Opcode::SMSG_GMRESPONSE_STATUS_UPDATE] = [this](network::Packet& packet) {
        // One byte, and it is not a status: SendGMResponse writes
        // uint8(getSurvey), which asks whether to offer the survey now that the
        // ticket is answered. This read a four-byte ticket id and a status
        // across five bytes, so it never ran once.
        if (packet.hasRemaining(1)) {
            const uint32_t ticketId = 0;
            const uint8_t  status   = packet.readUInt8() ? 2u : 1u;
            // GMSURVEY_DISPLAY for a survey, otherwise UPDATE_TICKET with no
            // ticket; no chat line (Wow.exe 0x5AD240).
            gmTicketActive_ = false;
            gmTicketText_.clear();
            fireAddonEvent(status == 2 ? "GMSURVEY_DISPLAY" : "UPDATE_TICKET", {});
            LOG_DEBUG("SMSG_GMRESPONSE_STATUS_UPDATE: ticketId=", ticketId,
                      " status=", static_cast<int>(status));
        }
    };
    // GM ticket status (new/updated); no ticket UI yet
    registerSkipHandler(Opcode::SMSG_GM_TICKET_STATUS_UPDATE);
    // Broadcast of another player's collision height change - cosmetic only.
    registerSkipHandler(Opcode::MSG_MOVE_SET_COLLISION_HGT);
    // Client uses this outbound; treat inbound variant as no-op for robustness.
    registerSkipHandler(Opcode::MSG_MOVE_WORLDPORT_ACK);
    // Observed custom server packet (8 bytes). Safe-consume for now.
    registerSkipHandler(Opcode::MSG_MOVE_TIME_SKIPPED);
    // loggingOut_ already cleared by cancelLogout(); this is server's confirmation
    // Not skipped: this is the server confirming a logout was called off, and
    // the interface hides its countdown on it. The client already cancels
    // optimistically in cancelLogout(), so the ack changes no state here - it
    // is the only thing that tells the interface the countdown is over.
    // 0x6B0900: LOGOUT_CANCEL only while a logout is still pending - the
    // client's own CancelLogout clears that before it sends, so its ack is
    // silent and only a cancel the server makes on its own hides the popup.
    dispatchTable_[Opcode::SMSG_LOGOUT_CANCEL_ACK] = [this](network::Packet& packet) {
        if (isLoggingOut()) {
            if (socialHandler_) socialHandler_->clearPendingLogout();
            if (addonEventCallback_) addonEventCallback_("LOGOUT_CANCEL", {});
        }
        packet.skipAll();
    };
    // Someone has taken the player's insignia, so the corpse is gone and any
    // resurrect offer against it is void. The body carries a free-repop flag
    // the interface reads only in code Blizzard commented out, so it is not
    // parsed here - the message and the cancelled offers are the whole effect.
    dispatchTable_[Opcode::SMSG_PLAYER_SKINNED] = [this](network::Packet& packet) {
        addUIError("Insignia Taken - You can only resurrect at the graveyard");
        if (addonEventCallback_) addonEventCallback_("PLAYER_SKINNED", {});
        packet.skipAll();
    };
    // These packets are not damage-shield events. Consume them without
    // synthesizing reflected damage entries or misattributing GUIDs.
    registerSkipHandler(Opcode::SMSG_AURACASTLOG);
    // These packets are not damage-shield events. Consume them without
    // synthesizing reflected damage entries or misattributing GUIDs.
    // SMSG_SPELLBREAKLOG: SpellHandler (the combat log's AURA_BROKEN).
    // Consume silently - informational, no UI action needed
    // What an item cost and how long is left to hand it back.
    //
    // Skipped until now, which is why GetContainerItemPurchaseInfo answered
    // nil and the refund lock never appeared. The client was never sent this
    // unasked - it is a reply to CMSG_ITEM_REFUND_INFO, and nothing asked.
    dispatchTable_[Opcode::SMSG_ITEM_REFUND_INFO_RESPONSE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8 + 4 * 3)) return;
        const uint64_t itemGuid = packet.readUInt64();
        ItemRefundInfo info;
        info.money = packet.readUInt32();
        info.honor = packet.readUInt32();
        info.arena = packet.readUInt32();
        // Five cost slots, id and count together - the same interleaving the
        // item query's sockets use, and the same trap if read as two runs.
        for (auto& slot : info.items) {
            if (!packet.hasRemaining(8)) break;
            slot.first  = packet.readUInt32();
            slot.second = packet.readUInt32();
        }
        if (packet.hasRemaining(8)) {
            packet.readUInt32();                       // always zero
            info.playedSincePurchase = packet.readUInt32();
        }
        packet.skipAll();
        itemRefundInfo_[itemGuid] = info;
        fireAddonEvent("UPDATE_INVENTORY_ALERTS", {});
    };
    // Consume silently - informational, no UI action needed
    registerSkipHandler(Opcode::SMSG_LOOT_LIST);
    // Same format as LOCKOUT_ADDED; consume
    registerSkipHandler(Opcode::SMSG_CALENDAR_RAID_LOCKOUT_UPDATED);
    // Consume - remaining server notifications not yet parsed
    for (auto op : {
        Opcode::SMSG_AFK_MONITOR_INFO_RESPONSE,
        Opcode::SMSG_AUCTION_LIST_PENDING_SALES,
        Opcode::SMSG_AVAILABLE_VOICE_CHANNEL,
        // The other calendar notices are handled with the rest of the
        // calendar above; these three AzerothCore never sends.
        Opcode::SMSG_CALENDAR_EVENT_INVITE_NOTES,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_NOTES_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_STATUS_ALERT,
        Opcode::SMSG_CHEAT_DUMP_ITEMS_DEBUG_ONLY_RESPONSE,
        Opcode::SMSG_CHEAT_DUMP_ITEMS_DEBUG_ONLY_RESPONSE_WRITE_FILE,
        Opcode::SMSG_CHEAT_PLAYER_LOOKUP,
        Opcode::SMSG_CHECK_FOR_BOTS,
        Opcode::SMSG_COMMENTATOR_GET_PLAYER_INFO,
        Opcode::SMSG_COMMENTATOR_MAP_INFO,
        Opcode::SMSG_COMMENTATOR_PLAYER_INFO,
        Opcode::SMSG_COMMENTATOR_SKIRMISH_QUEUE_RESULT1,
        Opcode::SMSG_COMMENTATOR_SKIRMISH_QUEUE_RESULT2,
        Opcode::SMSG_COMMENTATOR_STATE_CHANGED,
        Opcode::SMSG_COOLDOWN_CHEAT,
        Opcode::SMSG_DANCE_QUERY_RESPONSE,
        Opcode::SMSG_DBLOOKUP,
        Opcode::SMSG_DEBUGAURAPROC,
        Opcode::SMSG_DEBUG_AISTATE,
        Opcode::SMSG_DEBUG_LIST_TARGETS,
        Opcode::SMSG_DEBUG_SERVER_GEO,
        Opcode::SMSG_DUMP_OBJECTS_DATA,
        Opcode::SMSG_FORCEACTIONSHOW,
        Opcode::SMSG_GM_PLAYER_INFO,
        Opcode::SMSG_GODMODE,
        Opcode::SMSG_IGNORE_DIMINISHING_RETURNS_CHEAT,
        Opcode::SMSG_IGNORE_REQUIREMENTS_CHEAT,
        Opcode::SMSG_INVALIDATE_DANCE,
        Opcode::SMSG_LFG_PENDING_INVITE,
        Opcode::SMSG_LFG_PENDING_MATCH,
        Opcode::SMSG_LFG_PENDING_MATCH_DONE,
        Opcode::SMSG_LFG_UPDATE,
        Opcode::SMSG_LFG_UPDATE_LFG,
        Opcode::SMSG_LFG_UPDATE_LFM,
        Opcode::SMSG_LFG_UPDATE_QUEUED,
        Opcode::SMSG_MOVE_CHARACTER_CHEAT,
        Opcode::SMSG_NOTIFY_DANCE,
        Opcode::SMSG_NOTIFY_DEST_LOC_SPELL_CAST,
        Opcode::SMSG_PETGODMODE,
        Opcode::SMSG_PET_UPDATE_COMBO_POINTS,
        Opcode::SMSG_PLAY_DANCE,
        Opcode::SMSG_PROFILEDATA_RESPONSE,
        Opcode::SMSG_PVP_QUEUE_STATS,
        Opcode::SMSG_QUERY_OBJECT_POSITION,
        Opcode::SMSG_QUERY_OBJECT_ROTATION,
        Opcode::SMSG_REDIRECT_CLIENT,
        Opcode::SMSG_RESET_RANGED_COMBAT_TIMER,
        Opcode::SMSG_SEND_ALL_COMBAT_LOG,
        Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE,
        Opcode::SMSG_SET_PLAYER_DECLINED_NAMES_RESULT,
        Opcode::SMSG_SET_PROJECTILE_POSITION,
        Opcode::SMSG_SPELL_CHANCE_RESIST_PUSHBACK,
        Opcode::SMSG_SPELL_UPDATE_CHAIN_TARGETS,
        Opcode::SMSG_STOP_DANCE,
        Opcode::SMSG_TEST_DROP_RATE_RESULT,
        Opcode::SMSG_UPDATE_ACCOUNT_DATA,
        Opcode::SMSG_UPDATE_ACCOUNT_DATA_COMPLETE,
        Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP,
        Opcode::SMSG_UPDATE_LAST_INSTANCE,
        Opcode::SMSG_VOICESESSION_FULL,
        Opcode::SMSG_VOICE_CHAT_STATUS,
        Opcode::SMSG_VOICE_PARENTAL_CONTROLS,
        Opcode::SMSG_VOICE_SESSION_ADJUST_PRIORITY,
        Opcode::SMSG_VOICE_SESSION_ENABLE,
        Opcode::SMSG_VOICE_SESSION_LEAVE,
        Opcode::SMSG_VOICE_SESSION_ROSTER_UPDATE,
        Opcode::SMSG_VOICE_SET_TALKER_MUTED
    }) { registerSkipHandler(op); }

}

// The domain handlers registering their own.
//
// This used to be described as overriding duplicate entries above, which has
// not been true since those were moved out. The two sets are disjoint - checked
// opcode by opcode, 182 here against 320 there, no overlap - so the order
// between them carries no meaning and a reader need not look for one.
void GameHandler::registerDomainOpcodes() {
    chatHandler_->registerOpcodes(dispatchTable_);
    movementHandler_->registerOpcodes(dispatchTable_);
    combatHandler_->registerOpcodes(dispatchTable_);
    spellHandler_->registerOpcodes(dispatchTable_);
    inventoryHandler_->registerOpcodes(dispatchTable_);
    socialHandler_->registerOpcodes(dispatchTable_);
    questHandler_->registerOpcodes(dispatchTable_);
    wardenHandler_->registerOpcodes(dispatchTable_);
}

void GameHandler::registerOpcodeHandlers() {
    registerCoreOpcodes();
    registerRemainingOpcodes();
    registerDomainOpcodes();
}

void GameHandler::handlePacket(network::Packet& packet) {
    // Do NOT drop packets with an empty body. getSize() is the payload length and the
    // opcode is carried separately, so for the many opcodes that have no payload the
    // opcode *is* the message. Dropping them here silently swallowed
    // SMSG_LOGOUT_COMPLETE - the server logged the character out and moved on while
    // the client waited indefinitely, so the countdown ended and nothing happened.
    uint16_t opcode = packet.getOpcode();

    try {

    const bool allowVanillaAliases = isPreWotlk();

    // Vanilla compatibility aliases:
    // - 0x006B: can be SMSG_COMPRESSED_MOVES on some vanilla-family servers
    //           and SMSG_WEATHER on others
    // - 0x0103: SMSG_PLAY_MUSIC (some vanilla-family servers)
    //
    // We gate these by payload shape so expansion-native mappings remain intact.
    if (allowVanillaAliases && opcode == 0x006B) {
        // Try compressed movement batch first:
        // [u8 subSize][u16 subOpcode][subPayload...] ...
        // where subOpcode is typically SMSG_MONSTER_MOVE / SMSG_MONSTER_MOVE_TRANSPORT.
        const auto& data = packet.getData();
        if (packet.getReadPos() + 3 <= data.size()) {
            size_t pos = packet.getReadPos();
            uint8_t subSize = data[pos];
            if (subSize >= 2 && pos + 1 + subSize <= data.size()) {
                uint16_t subOpcode = static_cast<uint16_t>(data[pos + 1]) |
                                     (static_cast<uint16_t>(data[pos + 2]) << 8);
                uint16_t monsterMoveWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE);
                uint16_t monsterMoveTransportWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT);
                if ((monsterMoveWire != 0xFFFF && subOpcode == monsterMoveWire) ||
                    (monsterMoveTransportWire != 0xFFFF && subOpcode == monsterMoveTransportWire)) {
                    LOG_INFO("Opcode 0x006B interpreted as SMSG_COMPRESSED_MOVES (subOpcode=0x",
                             std::hex, subOpcode, std::dec, ")");
                    if (movementHandler_) movementHandler_->handleCompressedMoves(packet);
                    return;
                }
            }
        }

        // Expected weather payload: uint32 weatherType, float intensity, uint8 abrupt
        if (packet.hasRemaining(9)) {
            uint32_t wType = packet.readUInt32();
            float wIntensity = packet.readFloat();
            uint8_t abrupt = packet.readUInt8();
            bool plausibleWeather =
                (wType == 0 || weatherKind(wType) != WeatherKind::NONE) &&
                std::isfinite(wIntensity) &&
                (wIntensity >= 0.0f && wIntensity <= 1.5f) &&
                (abrupt <= 1);
            if (plausibleWeather) {
                LOG_INFO("Opcode 0x006B interpreted as SMSG_WEATHER");
                applyWeather(wType, wIntensity);
                return;
            }
            // Not weather-shaped: rewind and fall through to normal opcode table handling.
            packet.setReadPos(0);
        }
    } else if (allowVanillaAliases && opcode == 0x0103) {
        // Expected play-music payload: uint32 sound/music id
        if (packet.getRemainingSize() == 4) {
            uint32_t soundId = packet.readUInt32();
            LOG_INFO("SMSG_PLAY_MUSIC (0x0103 alias): soundId=", soundId);
            if (playMusicCallback_) playMusicCallback_(soundId);
            return;
        }
    }

    auto preLogicalOp = opcodeTable_.fromWire(opcode);
    if (preLogicalOp && isAuthCharPipelineOpcode(*preLogicalOp)) {
        LOG_DEBUG("AUTH/CHAR RX opcode=0x", std::hex, opcode, std::dec,
                 " logical=", static_cast<uint32_t>(*preLogicalOp),
                 " state=", worldStateName(state),
                 " size=", packet.getSize());
    }

    LOG_DEBUG("Received world packet: opcode=0x", std::hex, opcode, std::dec,
              " size=", packet.getSize(), " bytes");

    auto logicalOp = preLogicalOp;

    if (!logicalOp) {
        // Login-critical opcodes share the same wire values across all expansions.
        // Fall back to hardcoded mapping so the auth/char pipeline works even when
        // the expansion opcode table failed to load (wrong CWD, missing Data/, etc.).
        switch (opcode) {
            case 0x1EC: logicalOp = Opcode::SMSG_AUTH_CHALLENGE; break;
            case 0x1EE: logicalOp = Opcode::SMSG_AUTH_RESPONSE;  break;
            case 0x03B: logicalOp = Opcode::SMSG_CHAR_ENUM;      break;
            case 0x03A: logicalOp = Opcode::SMSG_CHAR_CREATE;     break;
            case 0x03C: logicalOp = Opcode::SMSG_CHAR_DELETE;     break;
            case 0x2E6: logicalOp = Opcode::SMSG_WARDEN_DATA;     break;
            default: break;
        }
        if (logicalOp) {
            static bool loggedFallback = false;
            if (!loggedFallback) {
                loggedFallback = true;
                LOG_WARNING("Opcode table lookup failed for login-critical opcode 0x",
                            std::hex, opcode, std::dec,
                            " (table has ", opcodeTable_.size(), " entries). "
                            "Using hardcoded fallback. Check that Data/expansions/<id>/opcodes.json is loadable.");
            }
        } else {
            static std::unordered_set<uint16_t> loggedUnknownWireOpcodes;
            if (loggedUnknownWireOpcodes.insert(opcode).second) {
                LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec,
                            " state=", static_cast<int>(state),
                            " size=", packet.getSize());
            }
            return;
        }
    }

    // An NPC's window opening is the answer to a hello. See pendingNpcHello_.
    switch (*logicalOp) {
        case Opcode::SMSG_GOSSIP_MESSAGE:
        case Opcode::SMSG_GOSSIP_COMPLETE:
        case Opcode::SMSG_NPC_WONT_TALK:
        case Opcode::SMSG_QUESTGIVER_QUEST_LIST:
        case Opcode::SMSG_QUESTGIVER_QUEST_DETAILS:
        case Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS:
        case Opcode::SMSG_QUESTGIVER_OFFER_REWARD:
        case Opcode::SMSG_QUESTGIVER_QUEST_INVALID:
        case Opcode::SMSG_LIST_INVENTORY:
        case Opcode::SMSG_TRAINER_LIST:
        case Opcode::SMSG_SHOWTAXINODES:
        case Opcode::SMSG_BINDER_CONFIRM:
        case Opcode::SMSG_SHOW_BANK:
        case Opcode::SMSG_PETITION_SHOWLIST:
        case Opcode::SMSG_SPIRIT_HEALER_CONFIRM:
        case Opcode::SMSG_AREA_SPIRIT_HEALER_TIME:
        case Opcode::MSG_AUCTION_HELLO:
        case Opcode::MSG_TABARDVENDOR_ACTIVATE:
        case Opcode::MSG_LIST_STABLED_PETS:
            noteNpcAnswered();
            break;
        default:
            break;
    }

    // Dispatch via the opcode handler table
    auto it = dispatchTable_.find(*logicalOp);
    if (it != dispatchTable_.end()) {
        it->second(packet);
    } else {
        // In pre-world states we need full visibility (char create/login handshakes).
        // In-world we keep de-duplication to avoid heavy log I/O in busy areas.
        if (state != WorldState::IN_WORLD) {
            static std::unordered_set<uint32_t> loggedUnhandledByState;
            const uint32_t key = (static_cast<uint32_t>(static_cast<uint8_t>(state)) << 16) |
                                 static_cast<uint32_t>(opcode);
            if (loggedUnhandledByState.insert(key).second) {
                LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec,
                            " state=", static_cast<int>(state),
                            " size=", packet.getSize());
                const auto& data = packet.getData();
                std::string hex;
                size_t limit = std::min<size_t>(data.size(), 48);
                hex.reserve(limit * 3);
                for (size_t i = 0; i < limit; ++i) {
                    char b[4];
                    snprintf(b, sizeof(b), "%02x ", data[i]);
                    hex += b;
                }
                LOG_INFO("Unhandled opcode payload hex (first ", limit, " bytes): ", hex);
            }
        } else {
            static std::unordered_set<uint16_t> loggedUnhandledOpcodes;
            if (loggedUnhandledOpcodes.insert(static_cast<uint16_t>(opcode)).second) {
                LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec);
            }
        }
    }
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("OOM while handling world opcode=0x", std::hex, opcode, std::dec,
                  " state=", worldStateName(state),
                  " size=", packet.getSize(),
                  " readPos=", packet.getReadPos(),
                  " what=", e.what());
        if (socket && state == WorldState::IN_WORLD) {
            disconnect();
            fail("Out of memory while parsing world packet");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception while handling world opcode=0x", std::hex, opcode, std::dec,
                  " state=", worldStateName(state),
                  " size=", packet.getSize(),
                  " readPos=", packet.getReadPos(),
                  " what=", e.what());
    }
}

void GameHandler::enqueueIncomingPacket(const network::Packet& packet) {
    if (pendingIncomingPackets_.size() >= kMaxQueuedInboundPackets) {
        LOG_ERROR("Inbound packet queue overflow (", pendingIncomingPackets_.size(),
                  " packets); dropping oldest packet to preserve responsiveness");
        pendingIncomingPackets_.pop_front();
    }
    pendingIncomingPackets_.push_back(packet);
    lastRxTime_ = std::chrono::steady_clock::now();
    rxSilenceLogged_ = false;
    rxSilence15sLogged_ = false;
}

void GameHandler::enqueueIncomingPacketFront(network::Packet&& packet) {
    if (pendingIncomingPackets_.size() >= kMaxQueuedInboundPackets) {
        LOG_ERROR("Inbound packet queue overflow while prepending (", pendingIncomingPackets_.size(),
                  " packets); dropping newest queued packet to preserve ordering");
        pendingIncomingPackets_.pop_back();
    }
    pendingIncomingPackets_.emplace_front(std::move(packet));
}

// enqueueUpdateObjectWork and processPendingUpdateObjectWork moved to EntityController

void GameHandler::processQueuedIncomingPackets() {
    if (pendingIncomingPackets_.empty() && !entityController_->hasPendingUpdateObjectWork()) {
        return;
    }

    const int maxPacketsThisUpdate = incomingPacketsBudgetPerUpdate(state);
    const float budgetMs = incomingPacketBudgetMs(state);
    const auto start = std::chrono::steady_clock::now();
    int processed = 0;

    while (processed < maxPacketsThisUpdate) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= budgetMs) {
            break;
        }

        if (entityController_->hasPendingUpdateObjectWork()) {
            entityController_->processPendingUpdateObjectWork(start, budgetMs);
            if (entityController_->hasPendingUpdateObjectWork()) {
                break;
            }
            continue;
        }

        if (pendingIncomingPackets_.empty()) {
            break;
        }

        network::Packet packet = std::move(pendingIncomingPackets_.front());
        pendingIncomingPackets_.pop_front();
        const uint16_t wireOp = packet.getOpcode();
        const auto logicalOp = opcodeTable_.fromWire(wireOp);
        auto packetHandleStart = std::chrono::steady_clock::now();
        handlePacket(packet);
        float packetMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - packetHandleStart).count();
        if (packetMs > slowPacketLogThresholdMs()) {
            const char* logicalName = logicalOp
                ? OpcodeTable::logicalToName(*logicalOp)
                : "UNKNOWN";
            LOG_WARNING("SLOW packet handler: ", packetMs,
                        "ms wire=0x", std::hex, wireOp, std::dec,
                        " logical=", logicalName,
                        " size=", packet.getSize(),
                        " state=", worldStateName(state));
        }
        ++processed;
    }

    if (entityController_->hasPendingUpdateObjectWork()) {
        return;
    }

    if (!pendingIncomingPackets_.empty()) {
        LOG_DEBUG("GameHandler packet budget reached (processed=", processed,
                  ", remaining=", pendingIncomingPackets_.size(),
                  ", state=", worldStateName(state), ")");
    }
}


} // namespace game
} // namespace wowee
