#include "game/game_handler.hpp"
#include "game/reputation_standing.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/spell_description_eval.hpp"
#include "game/gather_spells.hpp"
#include "game/interaction_rules.hpp"
#include "game/global_strings.hpp"
#include "game/game_error_table.hpp"
#include "game/quest_giver_status.hpp"
#include "game/packed_time.hpp"
#include "game/inventory_slots.hpp"
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
#include "ui/framexml_takeover.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/spell_visual_system.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/player_voice_manager.hpp"
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
#include <vector>
#include <random>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <cstring>
#include <bit>
#include <optional>
#include <limits>
#include <openssl/sha.h>
#include <openssl/hmac.h>


namespace wowee {
namespace game {

namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool containsAnyTerm(const std::string& haystack, const char* const* terms, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (haystack.find(terms[i]) != std::string::npos) return true;
    }
    return false;
}

// GAMEOBJECT_TYPE_FISHINGHOLE. A fishing school is not a container: retail
// gives it no interact cursor at all, and the only way to take from it is to
// fish in it. Clicking one here sent CMSG_GAMEOBJ_USE and, while its metadata
// was still pending, a CMSG_LOOT as well - which the server answers with the
// hole's loot, harvesting the school in one right-click.
constexpr uint32_t kGoTypeFishingHole = 25;

// The Lock.dbc row a game object is opened through. Most types keep it in
// data[0]; doors and buttons in data[1]. The same on every expansion.
uint32_t gameObjectLockId(const GameObjectQueryResponseData* info) {
    if (!info || !info->hasData) return 0;
    switch (info->type) {
        case 0:  // DOOR
        case 1:  // BUTTON
            return info->data[1];
        case 2:  // QUESTGIVER
        case 3:  // CHEST
        case 6:  // TRAP
        case 10: // GOOBER
        case 12: // AREADAMAGE
        case 13: // CAMERA
        case 24: // FLAGSTAND
        case 26: // FLAGDROP
            return info->data[0];
        default:
            return 0;
    }
}

// How the local player should open a game object, decided from its Lock.dbc row.
enum class LockOpenMethod {
    UseDirect,  // CMSG_GAMEOBJ_USE. Opens unlocked/quest containers and, crucially,
                // key-item chests: the server credits the key (from a bag) and opens
                // it. keyItemId names the key so the caller can ensure it's in a bag.
    CastSpell,  // cast a KNOWN player OPEN_LOCK spell at the GO (rogue Pick Lock).
                // Generic Opening spells the player does not know are rejected by
                // the server (BAD_TARGETS), so we never fabricate a cast.
};

struct LockOpenPlan {
    LockOpenMethod method = LockOpenMethod::UseDirect;
    uint32_t spellId = 0;
    uint32_t keyItemId = 0;  // required key item (LOCK_KEY_ITEM), 0 if none
};

// Decide how to open a game object from its Lock.dbc row. Only a known player
// OPEN_LOCK spell yields CastSpell; every other case (including key-item locks)
// is UseDirect, with keyItemId set so the caller can move the key into a bag.
LockOpenPlan planGameObjectOpen(pipeline::AssetManager* assets,
                                const GameObjectQueryResponseData* info,
                                const std::unordered_set<uint32_t>& knownSpells,
                                uint32_t playerLevel, uint32_t goLevel) {
    LockOpenPlan plan;
    const uint32_t lockId = gameObjectLockId(info);
    if (!assets || lockId == 0) return plan;

    auto lockDbc = assets->loadDBC("Lock.dbc");
    auto spellDbc = assets->loadDBC("Spell.dbc");
    // Which columns hold the spell's effects and their misc values, from the
    // layout. They were 71 and 110, the WotLK positions, behind a demand that
    // Spell.dbc have 234 fields - which is the WotLK file and nothing else, so
    // on Classic (173) and TBC (216) this walked away and every locked chest
    // went to the server as a bare USE. The effect block starts at 61 on
    // vanilla and 65 on TBC because both carry an EffectBaseDice this one does
    // not; the misc values are at 106 there rather than 110.
    const auto* spellL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
    const uint32_t effect0Field = spellL ? spellL->field("Effect0") : 0xFFFFFFFF;
    const uint32_t misc0Field   = spellL ? spellL->field("EffectMiscValue0") : 0xFFFFFFFF;
    // The 3.3.5a client (CGGameObject_C::IsLocked) only takes a spell whose
    // minimum points reach the lock's Skill, or 5 x the object's level when the
    // lock has none. The player's skill never enters it: Pick Lock's points grow
    // 5 per level and stand in for it. Only the WotLK layout maps these columns.
    // Before 3.1 Lockpicking was a trained skill the server added to the
    // points, so the check would turn away picks those servers accept.
    const uint32_t base0Field     = spellL ? spellL->field("EffectBasePoints0") : 0xFFFFFFFF;
    const uint32_t perLevel0Field = spellL ? spellL->field("EffectRealPointsPerLevel0") : 0xFFFFFFFF;
    const uint32_t maxLevelField  = spellL ? spellL->field("MaxLevel") : 0xFFFFFFFF;
    const uint32_t baseLevelField = spellL ? spellL->field("BaseLevel") : 0xFFFFFFFF;
    const uint32_t spellLevelField = spellL ? spellL->field("SpellLevel") : 0xFFFFFFFF;
    const bool checkPoints =
        base0Field != 0xFFFFFFFF && perLevel0Field != 0xFFFFFFFF &&
        maxLevelField != 0xFFFFFFFF && baseLevelField != 0xFFFFFFFF &&
        spellLevelField != 0xFFFFFFFF &&
        base0Field + 3 <= spellDbc->getFieldCount() &&
        perLevel0Field + 3 <= spellDbc->getFieldCount();
    // basePoints + 1 + perLevel x (caster level, clamped to the spell's level
    // range, minus SpellLevel); Spell_C_GetMinMaxPoints without modifiers.
    const auto minPoints = [&](uint32_t spellRow, uint32_t effect) {
        int32_t level = static_cast<int32_t>(playerLevel);
        const int32_t maxLevel = spellDbc->getInt32(spellRow, maxLevelField);
        const int32_t baseLevel = spellDbc->getInt32(spellRow, baseLevelField);
        if (maxLevel > 0 && level > maxLevel) level = maxLevel;
        else if (level < baseLevel) level = baseLevel;
        level -= spellDbc->getInt32(spellRow, spellLevelField);
        return spellDbc->getInt32(spellRow, base0Field + effect) + 1 +
               static_cast<int32_t>(spellDbc->getFloat(spellRow, perLevel0Field + effect) *
                                    static_cast<float>(level));
    };
    if (!lockDbc || !spellDbc || !lockDbc->isLoaded() || !spellDbc->isLoaded() ||
        lockDbc->getFieldCount() < 33 ||
        effect0Field == 0xFFFFFFFF || misc0Field == 0xFFFFFFFF ||
        effect0Field + 3 > spellDbc->getFieldCount() ||
        misc0Field + 3 > spellDbc->getFieldCount()) {
        return plan; // Can't inspect the lock - let the server adjudicate a USE.
    }

    for (uint32_t row = 0; row < lockDbc->getRecordCount(); ++row) {
        if (lockDbc->getUInt32(row, 0) != lockId) continue;

        for (uint32_t slot = 0; slot < 8; ++slot) {
            // Lock.dbc: Type[8] at cols 1-8, Index[8] at cols 9-16.
            constexpr uint32_t kLockKeyItem = 1;   // requires a key item
            constexpr uint32_t kLockKeySkill = 2;  // requires a skill (LockType id)
            const uint32_t keyType = lockDbc->getUInt32(row, 1 + slot);
            const uint32_t keyIndex = lockDbc->getUInt32(row, 9 + slot);
            if (keyIndex == 0) continue;
            // Skill[8] at cols 17-24.
            const uint32_t lockSkill = lockDbc->getUInt32(row, 17 + slot);
            const int32_t requiredPoints = static_cast<int32_t>(lockSkill ? lockSkill : 5 * goLevel);

            if (keyType == kLockKeyItem) {
                plan.keyItemId = keyIndex; // remember the key so we can locate it
                continue;
            }
            if (keyType != kLockKeySkill) continue;

            // A known player spell whose OPEN_LOCK effect matches this LockType
            // lets us open it directly (Lockpicking, etc.).
            for (uint32_t spellRow = 0; spellRow < spellDbc->getRecordCount(); ++spellRow) {
                const uint32_t spellId = spellDbc->getUInt32(spellRow, 0);
                if (knownSpells.count(spellId) == 0) continue;
                for (uint32_t effect = 0; effect < 3; ++effect) {
                    constexpr uint32_t kSpellEffectOpenLock = 33;
                    if (spellDbc->getUInt32(spellRow, effect0Field + effect) == kSpellEffectOpenLock &&
                        spellDbc->getUInt32(spellRow, misc0Field + effect) == keyIndex) {
                        if (checkPoints && minPoints(spellRow, effect) < requiredPoints) {
                            LOG_INFO("GO open-lock spell ", spellId, " too weak: points ",
                                     minPoints(spellRow, effect), " < ", requiredPoints,
                                     " (lockId=", lockId, ")");
                            continue;
                        }
                        plan.method = LockOpenMethod::CastSpell;
                        plan.spellId = spellId;
                        return plan;
                    }
                }
            }
        }
        break;
    }
    return plan;
}


// Whether a point is close enough to use a game object, as the 3.3.5a client's
// CGGameObject_C_TypeBase::CheckRange (0x0070FA70) decides it: the display's
// GeoBox, scaled, grown by the reach on every side and tested in the object's
// own frame, so a big object is usable from anywhere along its length and a
// small one only from close by. The reach is 5 yards, or the open spell's range
// when one opens it; every OPEN_LOCK and gather spell in Spell.dbc has the
// 5-yard Combat Range, so the two agree. Fishing nodes reach 100 and spell
// focuses test a sphere. The client uses the door's animation bounds for type
// 0; the display box stands in for them, as it does on the server. nullopt
// when the layout has no box (only WotLK's maps one) or the display has no row.
std::optional<bool> withinGameObjectReach(pipeline::AssetManager* assets,
                                          const GameObjectQueryResponseData* info,
                                          const Entity& go, const glm::vec3& point) {
    if (!assets || !info) return std::nullopt;
    constexpr uint32_t kGoTypeSpellFocus = 8;
    // The type handler's own reach: five yards for most, 5.56 for a quest
    // giver or a sign, ten for a binder, three for a chair, a hundred for the
    // bobber (interaction_rules.hpp).
    const float reach = interaction::gameObjectUseRange(info->type);
    const glm::vec3 offset = point - glm::vec3(go.getX(), go.getY(), go.getZ());
    if (info->type == kGoTypeSpellFocus) return glm::length(offset) <= reach;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
    const uint32_t minField = layout ? layout->field("GeoBoxMinX") : 0xFFFFFFFF;
    const uint32_t maxField = layout ? layout->field("GeoBoxMaxX") : 0xFFFFFFFF;
    if (minField == 0xFFFFFFFF || maxField == 0xFFFFFFFF) return std::nullopt;
    auto displayDbc = assets->loadDBC("GameObjectDisplayInfo.dbc");
    if (!displayDbc || !displayDbc->isLoaded() ||
        minField + 3 > displayDbc->getFieldCount() ||
        maxField + 3 > displayDbc->getFieldCount()) {
        return std::nullopt;
    }
    const int32_t row = displayDbc->findRecordById(info->displayId);
    if (row < 0) return std::nullopt;

    float scale = 1.0f;
    const uint16_t scaleField = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
    if (scaleField != 0xFFFF && go.getField(scaleField) != 0) {
        scale = std::bit_cast<float>(go.getField(scaleField));
        if (scale <= 0.01f || scale > 100.0f) scale = 1.0f;
    }
    // Into the model's frame: +X along its facing, +Y to its left. Canonical
    // yaw c faces (cos c, -sin c) in (north, west).
    const float c = go.getOrientation();
    const glm::vec3 local(offset.x * std::cos(c) - offset.y * std::sin(c),
                          offset.x * std::sin(c) + offset.y * std::cos(c),
                          offset.z);
    for (int axis = 0; axis < 3; ++axis) {
        const float lo = displayDbc->getFloat(static_cast<uint32_t>(row), minField + axis) * scale - reach;
        const float hi = displayDbc->getFloat(static_cast<uint32_t>(row), maxField + axis) * scale + reach;
        if (!(local[axis] > lo && local[axis] < hi)) return false;
    }
    return true;
}

uint32_t gatherSpellForGameObject(const GameObjectQueryResponseData* info, const std::string& name) {
    if (info && info->type != 3) return 0; // GAMEOBJECT_TYPE_CHEST

    const std::string lower = lowerCopy(name);
    static constexpr const char* kMiningTerms[] = {
        "vein", "deposit", "mineral"
    };
    static constexpr const char* kHerbTerms[] = {
        "peacebloom", "silverleaf", "earthroot", "mageroyal", "briarthorn",
        "stranglekelp", "bruiseweed", "steelbloom", "grave moss", "kingsblood",
        "liferoot", "fadeleaf", "goldthorn", "khadgar", "wintersbite",
        "firebloom", "purple lotus", "arthas", "sungrass", "blindweed",
        "ghost mushroom", "gromsblood", "dreamfoil", "silversage",
        "plaguebloom", "icecap", "black lotus", "felweed", "dreaming glory",
        "terocone", "ragveil", "ancient lichen", "netherbloom",
        "nightmare vine", "mana thistle"
    };

    if (containsAnyTerm(lower, kMiningTerms, sizeof(kMiningTerms) / sizeof(kMiningTerms[0]))) return kMiningBaseSpellId;
    if (containsAnyTerm(lower, kHerbTerms, sizeof(kHerbTerms) / sizeof(kHerbTerms[0]))) return kHerbBaseSpellId;
    return 0;
}

uint32_t knownGatherRank(const SpellHandler* spellHandler, uint32_t baseSpellId) {
    if (!spellHandler) return 0;

    size_t count = 0;
    const uint32_t* ranks = gatherRanksForBase(baseSpellId, count);
    if (!ranks) return 0;

    for (size_t i = count; i > 0; --i) {
        const uint32_t spellId = ranks[i - 1];
        if (spellHandler->hasKnownSpell(spellId)) return spellId;
    }
    return 0;
}

} // end anonymous namespace

void GameHandler::handleAuthChallenge(network::Packet& packet) {
    LOG_INFO("Handling SMSG_AUTH_CHALLENGE");

    AuthChallengeData challenge;
    if (!AuthChallengeParser::parse(packet, challenge)) {
        fail("Failed to parse SMSG_AUTH_CHALLENGE");
        return;
    }

    if (!challenge.isValid()) {
        fail("Invalid auth challenge data");
        return;
    }

    // Store server seed
    serverSeed = challenge.serverSeed;
    LOG_DEBUG("Server seed: 0x", std::hex, serverSeed, std::dec);

    setState(WorldState::CHALLENGE_RECEIVED);

    // Send authentication session
    sendAuthSession();
}

void GameHandler::sendAuthSession() {
    LOG_INFO("Sending CMSG_AUTH_SESSION");

    // Build authentication packet
    auto packet = AuthSessionPacket::build(
        build,
        accountName,
        clientSeed,
        sessionKey,
        serverSeed,
        realmId_
    );

    LOG_DEBUG("CMSG_AUTH_SESSION packet size: ", packet.getSize(), " bytes");

    // Send packet (unencrypted - this is the last unencrypted packet)
    socket->send(packet);

    // Enable encryption IMMEDIATELY after sending AUTH_SESSION
    // AzerothCore enables encryption before sending AUTH_RESPONSE,
    // so we need to be ready to decrypt the response
    LOG_INFO("Enabling encryption immediately after AUTH_SESSION");
    socket->initEncryption(sessionKey, build);

    setState(WorldState::AUTH_SENT);
    LOG_INFO("CMSG_AUTH_SESSION sent, encryption enabled, waiting for AUTH_RESPONSE...");
}

void GameHandler::handleAuthResponse(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_AUTH_RESPONSE, size=", packet.getSize());

    AuthResponseData response;
    if (!AuthResponseParser::parse(packet, response)) {
        fail("Failed to parse SMSG_AUTH_RESPONSE");
        return;
    }

    if (!response.isSuccess()) {
        std::string reason = std::string("Authentication failed: ") +
                           getAuthResultString(response.result);
        fail(reason);
        return;
    }

    // Encryption was already enabled after sending AUTH_SESSION
    LOG_INFO("AUTH_RESPONSE OK - world authentication successful");

    setState(WorldState::AUTHENTICATED);

    LOG_INFO("========================================");
    LOG_INFO("   WORLD AUTHENTICATION SUCCESSFUL!");
    LOG_INFO("========================================");
    LOG_INFO("Connected to world server");
    LOG_INFO("Ready for character operations");

    setState(WorldState::READY);

    // Request character list automatically
    requestCharacterList();

    // Call success callback
    if (onSuccess) {
        onSuccess();
    }
}

void GameHandler::requestCharacterList() {
    if (state == WorldState::FAILED || !socket || !socket->isConnected()) {
        return;
    }

    if (state != WorldState::READY && state != WorldState::AUTHENTICATED &&
        state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot request character list in state: ", worldStateName(state));
        return;
    }

    LOG_INFO("Requesting character list from server...");

    // Prevent the UI from showing/selecting stale characters while we wait for the new SMSG_CHAR_ENUM.
    // This matters after character create/delete where the old list can linger for a few frames.
    characters.clear();

    // Build CMSG_CHAR_ENUM packet (no body, just opcode)
    auto packet = CharEnumPacket::build();

    // Send packet
    socket->send(packet);

    setState(WorldState::CHAR_LIST_REQUESTED);
    LOG_INFO("CMSG_CHAR_ENUM sent, waiting for character list...");
}

void GameHandler::handleCharEnum(network::Packet& packet) {
    LOG_INFO("Handling SMSG_CHAR_ENUM");

    CharEnumResponse response;
    // IMPORTANT: Do not infer packet formats from numeric build alone.
    // Turtle WoW uses a "high" build but classic-era world packet formats.
    bool parsed = packetParsers_ ? packetParsers_->parseCharEnum(packet, response)
                                 : CharEnumParser::parse(packet, response);
    if (!parsed) {
        fail("Failed to parse SMSG_CHAR_ENUM");
        return;
    }

    // Store characters
    characters = response.characters;

    setState(WorldState::CHAR_LIST_RECEIVED);

    std::vector<uint32_t> queriedGuildIds;
    for (const auto& character : characters) {
        if (!character.hasGuild()) continue;
        if (std::find(queriedGuildIds.begin(), queriedGuildIds.end(), character.guildId) != queriedGuildIds.end())
            continue;
        queriedGuildIds.push_back(character.guildId);
        queryGuildInfo(character.guildId);
    }
    if (!queriedGuildIds.empty()) {
        LOG_INFO("Queued guild name queries for ", queriedGuildIds.size(), " guild(s) from character list");
    }

    LOG_INFO("========================================");
    LOG_INFO("   CHARACTER LIST RECEIVED");
    LOG_INFO("========================================");
    LOG_INFO("Found ", characters.size(), " character(s)");

    if (characters.empty()) {
        LOG_INFO("No characters on this account");
    } else {
        LOG_INFO("Characters:");
        for (size_t i = 0; i < characters.size(); ++i) {
            const auto& character = characters[i];
            LOG_INFO("  [", i + 1, "] ", character.name);
            LOG_INFO("      GUID: 0x", std::hex, character.guid, std::dec);
            LOG_INFO("      ", getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            LOG_INFO("      Level ", static_cast<int>(character.level));
        }
    }

    LOG_INFO("Ready to select character");
}

void GameHandler::createCharacter(const CharCreateData& data) {

    // Online mode: send packet to server
    if (!socket) {
        LOG_WARNING("Cannot create character: not connected");
        if (charCreateCallback_) {
            charCreateCallback_(false, "Not connected to server");
        }
        return;
    }

    if (state != WorldState::CHAR_LIST_RECEIVED) {
        std::string msg = "Character list not ready yet. Wait for SMSG_CHAR_ENUM.";
        LOG_WARNING("Blocking CMSG_CHAR_CREATE in state=", worldStateName(state),
                    " (awaiting CHAR_LIST_RECEIVED)");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
        return;
    }

    auto packet = CharCreatePacket::build(data);
    socket->send(packet);
    LOG_INFO("CMSG_CHAR_CREATE sent for: ", data.name);
}

void GameHandler::handleCharCreateResponse(network::Packet& packet) {
    CharCreateResponseData data;
    if (!CharCreateResponseParser::parse(packet, data)) {
        LOG_ERROR("Failed to parse SMSG_CHAR_CREATE");
        return;
    }

    if (data.result == CharCreateResult::SUCCESS || data.result == CharCreateResult::IN_PROGRESS) {
        LOG_INFO("Character created successfully (code=", static_cast<int>(data.result), ")");
        requestCharacterList();
        if (charCreateCallback_) {
            charCreateCallback_(true, "Character created!");
        }
    } else {
        std::string msg;
        switch (data.result) {
            case CharCreateResult::CHAR_ERROR: msg = "Server error"; break;
            case CharCreateResult::FAILED: msg = "Creation failed"; break;
            case CharCreateResult::NAME_IN_USE: msg = "Name already in use"; break;
            case CharCreateResult::DISABLED: msg = "Character creation disabled"; break;
            case CharCreateResult::PVP_TEAMS_VIOLATION: msg = "PvP faction violation"; break;
            case CharCreateResult::SERVER_LIMIT: msg = "Server character limit reached"; break;
            case CharCreateResult::ACCOUNT_LIMIT: msg = "Account character limit reached"; break;
            case CharCreateResult::SERVER_QUEUE: msg = "Server is queued"; break;
            case CharCreateResult::ONLY_EXISTING: msg = "Only existing characters allowed"; break;
            case CharCreateResult::EXPANSION: msg = "Expansion required"; break;
            case CharCreateResult::EXPANSION_CLASS: msg = "Expansion required for this class"; break;
            case CharCreateResult::LEVEL_REQUIREMENT: msg = "Level requirement not met"; break;
            case CharCreateResult::UNIQUE_CLASS_LIMIT: msg = "Unique class limit reached"; break;
            case CharCreateResult::RESTRICTED_RACECLASS: msg = "Race/class combination not allowed"; break;
            case CharCreateResult::IN_PROGRESS: msg = "Character creation in progress..."; break;
            case CharCreateResult::CHARACTER_CHOOSE_RACE: msg = "Please choose a different race"; break;
            case CharCreateResult::CHARACTER_ARENA_LEADER: msg = "Arena team leader restriction"; break;
            case CharCreateResult::CHARACTER_DELETE_MAIL: msg = "Character has mail"; break;
            case CharCreateResult::CHARACTER_SWAP_FACTION: msg = "Faction swap restriction"; break;
            case CharCreateResult::CHARACTER_RACE_ONLY: msg = "Race-only restriction"; break;
            case CharCreateResult::CHARACTER_GOLD_LIMIT: msg = "Gold limit reached"; break;
            case CharCreateResult::FORCE_LOGIN: msg = "Force login required"; break;
            case CharCreateResult::CHARACTER_IN_GUILD: msg = "Character is in a guild"; break;
            // Name validation errors
            case CharCreateResult::NAME_FAILURE: msg = "Invalid name"; break;
            case CharCreateResult::NAME_NO_NAME: msg = "Please enter a name"; break;
            case CharCreateResult::NAME_TOO_SHORT: msg = "Name is too short"; break;
            case CharCreateResult::NAME_TOO_LONG: msg = "Name is too long"; break;
            case CharCreateResult::NAME_INVALID_CHARACTER: msg = "Name contains invalid characters"; break;
            case CharCreateResult::NAME_MIXED_LANGUAGES: msg = "Name mixes languages"; break;
            case CharCreateResult::NAME_PROFANE: msg = "Name contains profanity"; break;
            case CharCreateResult::NAME_RESERVED: msg = "Name is reserved"; break;
            case CharCreateResult::NAME_INVALID_APOSTROPHE: msg = "Invalid apostrophe in name"; break;
            case CharCreateResult::NAME_MULTIPLE_APOSTROPHES: msg = "Name has multiple apostrophes"; break;
            case CharCreateResult::NAME_THREE_CONSECUTIVE: msg = "Name has 3+ consecutive same letters"; break;
            case CharCreateResult::NAME_INVALID_SPACE: msg = "Invalid space in name"; break;
            case CharCreateResult::NAME_CONSECUTIVE_SPACES: msg = "Name has consecutive spaces"; break;
            default: msg = "Unknown error (code " + std::to_string(static_cast<int>(data.result)) + ")"; break;
        }
        LOG_WARNING("Character creation failed: ", msg, " (code=", static_cast<int>(data.result), ")");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
    }
}

void GameHandler::deleteCharacter(uint64_t characterGuid) {
    if (!socket) {
        if (charDeleteCallback_) charDeleteCallback_(false, "Delete failed: not connected to server.");
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_CHAR_DELETE));
    packet.writeUInt64(characterGuid);
    socket->send(packet);
    pendingCharDeleteResponse_ = true;
    pendingDeleteGuid_ = characterGuid;
    pendingDeleteTimer_ = 0.0f;
    LOG_INFO("CMSG_CHAR_DELETE sent for GUID: 0x", std::hex, characterGuid, std::dec);
}

const Character* GameHandler::getActiveCharacter() const {
    if (activeCharacterGuid_ == 0) return nullptr;
    for (const auto& ch : characters) {
        if (ch.guid == activeCharacterGuid_) return &ch;
    }
    return nullptr;
}
void GameHandler::playErrorSpeech(audio::PlayerErrorSpeech type) {
    auto* ac = services_.audioCoordinator;
    if (!ac) return;
    auto* voice = ac->getPlayerVoiceManager();
    if (!voice) return;
    const Character* ch = getActiveCharacter();
    if (!ch) return;
    voice->playError(type, static_cast<uint8_t>(ch->race),
                     static_cast<uint8_t>(toServerGender(ch->gender)));
}

void GameHandler::playUiMessageSound(std::string_view messageKey) {
    auto* ac = services_.audioCoordinator;
    if (!ac) return;
    if (auto* sfx = ac->getUiSoundManager()) sfx->playUiMessage(messageKey);
}

void GameHandler::playUiSoundEntry(uint32_t soundEntryId) {
    auto* ac = services_.audioCoordinator;
    if (!ac) return;
    auto* sfx = ac->getUiSoundManager();
    const audio::SoundData* data = ac->getSoundData();
    if (sfx && data) sfx->playEntry(data->sound(soundEntryId));
}

void GameHandler::playUiSoundByName(const char* soundName) {
    auto* ac = services_.audioCoordinator;
    if (!ac || !soundName) return;
    if (auto* sfx = ac->getUiSoundManager()) sfx->playByName(soundName);
}

void GameHandler::handleCharLoginFailed(network::Packet& packet) {
    uint8_t reason = packet.readUInt8();

    static const char* reasonNames[] = {
        "Login failed",          // 0
        "World server is down",  // 1
        "Duplicate character",   // 2 (session still active)
        "No instance servers",   // 3
        "Login disabled",        // 4
        "Character not found",   // 5
        "Locked for transfer",   // 6
        "Locked by billing",     // 7
        "Using remote",          // 8
    };
    const char* msg = (reason < 9) ? reasonNames[reason] : "Unknown reason";

    LOG_ERROR("SMSG_CHARACTER_LOGIN_FAILED: reason=", static_cast<int>(reason), " (", msg, ")");

    // Allow the player to re-select a character
    setState(WorldState::CHAR_LIST_RECEIVED);

    if (charLoginFailCallback_) {
        charLoginFailCallback_(msg);
    }
}

void GameHandler::selectCharacter(uint64_t characterGuid) {
    if (state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot select character in state: ", static_cast<int>(state));
        return;
    }

    // Make the selected character authoritative in GameHandler.
    // This avoids relying on UI/Application ordering for appearance-dependent logic.
    activeCharacterGuid_ = characterGuid;

    // The last character's guild is not this one's.
    //
    // The guild name is kept for the character being played and was cleared
    // only when that character left the guild or it disbanded - so logging out
    // of a guilded character and into a guildless one left the name behind,
    // and everything that asks "am I in a guild" answered yes. The guild vault
    // is where that showed: the click passed the check, the server refused it
    // with error 9, and an empty vault window was already on screen.
    if (socialHandler_) socialHandler_->clearGuildMembership();

    LOG_INFO("========================================");
    LOG_INFO("   ENTERING WORLD");
    LOG_INFO("========================================");
    LOG_INFO("Character GUID: 0x", std::hex, characterGuid, std::dec);

    // Find character name for logging
    for (const auto& character : characters) {
        if (character.guid == characterGuid) {
            LOG_INFO("Character: ", character.name);
            LOG_INFO("Level ", static_cast<int>(character.level), " ",
                     getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            playerRace_ = character.race;
            break;
        }
    }

    // Store player GUID
    playerGuid = characterGuid;

    // Reset per-character state so previous character data doesn't bleed through
    inventory = Inventory();
    onlineItems_.clear();
    itemInfoCache_.clear();
    pendingItemQueries_.clear();
    equipSlotGuids_ = {};
    backpackSlotGuids_ = {};
    keyringSlotGuids_ = {};
    invSlotBase_ = -1;
    packSlotBase_ = -1;
    lastPlayerFields_.clear();
    onlineEquipDirty_ = false;
    playerMoneyCopper_ = 0;
    playerArmorRating_ = 0;
    std::fill(std::begin(playerResistances_), std::end(playerResistances_), 0);
    std::fill(std::begin(playerStats_), std::end(playerStats_), -1);
    playerMeleeAP_ = -1;
    playerRangedAP_ = -1;
    std::fill(std::begin(playerSpellDmgBonus_), std::end(playerSpellDmgBonus_), -1);
    playerHealBonus_ = -1;
    playerDodgePct_ = -1.0f;
    playerParryPct_ = -1.0f;
    playerBlockPct_ = -1.0f;
    playerExpertise_ = 0;
    playerOffhandExpertise_ = 0;
    playerCritPct_  = -1.0f;
    playerRangedCritPct_ = -1.0f;
    std::fill(std::begin(playerSpellCritPct_), std::end(playerSpellCritPct_), -1.0f);
    std::fill(std::begin(playerCombatRatings_), std::end(playerCombatRatings_), -1);
    if (spellHandler_) spellHandler_->resetAllState();
    if (auto* renderer = services_.renderer) {
        if (auto* camera = renderer->getCameraController()) camera->setIntoxication(0.0f);
        if (auto* post = renderer->getPostProcessPipeline()) post->setIntoxication(0.0f);
    }
    spellFlatMods_.clear();
    spellPctMods_.clear();
    actionBar = {};
    petGuid_ = 0;
    stableWindowOpen_  = false;
    stableMasterGuid_  = 0;
    stableNumSlots_    = 0;
    stabledPets_.clear();
    playerXp_ = 0;
    playerNextLevelXp_ = 0;
    serverPlayerLevel_ = 1;
    std::fill(playerExploredZones_.begin(), playerExploredZones_.end(), 0u);
    hasPlayerExploredZones_ = false;
    playerSkills_.clear();
    // The handler's, not this class's same-named members - every reader
    // forwards there, so clearing the copies here cleared nothing anyone
    // looks at and the previous character's quests stayed in the log.
    if (questHandler_) questHandler_->clearQuestStateForCharacterSwitch();
    pendingLoginQuestResync_ = false;
    pendingLoginQuestResyncTimeout_ = 0.0f;
    if (questHandler_) {
        questHandler_->pendingQuestAcceptTimeoutsRef().clear();
        questHandler_->pendingQuestAcceptNpcGuidsRef().clear();
    }
    if (combatHandler_) combatHandler_->resetAllCombatState();
    // resetCastState() already called inside resetAllState() above
    pendingGameObjectInteractGuid_ = 0;
    lastInteractedGoGuid_ = 0;
    playerDead_ = false;
    releasedSpirit_ = false;
    // A character whose flags are all clear sends no PLAYER_FLAGS on login,
    // so nothing else would put down the last character's inn.
    isResting_ = false;
    corpseGuid_ = 0;
    corpsePositionValid_ = false;
    corpseReclaimAvailableMs_ = 0;
    targetGuid = 0;
    focusGuid = 0;
    lastTargetGuid = 0;
    entityController_->clearAll();

    // Build CMSG_PLAYER_LOGIN packet
    auto packet = PlayerLoginPacket::build(characterGuid);

    // Send packet
    socket->send(packet);

    setState(WorldState::ENTERING_WORLD);
    LOG_INFO("CMSG_PLAYER_LOGIN sent, entering world...");
}

// SMSG_WEATHER, and the 0x006B alias some 1.12 servers send it as.
void GameHandler::applyWeather(uint32_t state, float intensity) {
    const WeatherKind kind = weatherKind(state);
    weatherType_ = state;
    weatherIntensity_ = intensity;
    LOG_INFO("Weather changed: state=", state, ", intensity=", intensity);
    // No chat line: the stock client says nothing when the weather changes,
    // it only shows it.
    // Notify addons of weather change
    fireAddonEvent("WEATHER_CHANGED", {std::to_string(state), std::to_string(intensity)});
    // Storm transition: trigger a low-frequency thunder rumble shake
    if (kind == WeatherKind::THUNDERSTORM && intensity > 0.3f && cameraShakeCallback_) {
        float mag = 0.03f + intensity * 0.04f; // 0.03–0.07 units
        cameraShakeCallback_(mag, 6.0f, 0.6f);
    }
}

void GameHandler::handleLoginSetTimeSpeed(network::Packet& packet) {
    // SMSG_LOGIN_SETTIMESPEED (0x042)
    // Structure: PackedTime gameTime, float timeScale
    //
    // gameTime is a packed bitfield, not a count of seconds - Player.cpp writes
    // it with AppendPackedTime. It was being stored raw and then divided by
    // 86400 to get a time of day, which made the sky's clock a number with no
    // relation to the hour the server sent.
    //
    // timeScale: game minutes per real second (0.0166, so the clock keeps realm time)

    if (packet.getSize() < 8) {
        LOG_WARNING("SMSG_LOGIN_SETTIMESPEED: packet too small (", packet.getSize(), " bytes)");
        return;
    }

    const WowDate now = unpackWowPackedTime(packet.readUInt32());
    float timeScale = packet.readFloat();

    // Hours since midnight, which is what GetGameTime already assumed and what
    // the sky wants. One meaning for this field, written down here.
    gameTimeSetAt_ = std::chrono::steady_clock::now();
    gameTime_ = static_cast<float>(now.hour) + static_cast<float>(now.minute) / 60.0f;
    timeSpeed_ = timeScale;

    LOG_INFO("Server time: ", now.hour, ":", now.minute, " (gameTime=", gameTime_,
             "h), timeSpeed=", timeSpeed_);
}

// Every arrival in a world, login or transfer (Wow.exe 0x528010): the
// battleground client is reset (0x54CA50 - scoreboard and battlemaster list
// emptied, CMSG_BATTLEFIELD_STATUS sent) and, on a map whose InstanceType is
// 3 or 4 - an arena as well as a battleground - PLAYER_ENTERING_BATTLEGROUND.
void GameHandler::battlefieldWorldEntered() {
    if (socialHandler_) socialHandler_->onWorldEntered();
    const uint32_t type = getMapInstanceType(getCurrentMapId());
    if (type == 3 || type == 4) fireAddonEvent("PLAYER_ENTERING_BATTLEGROUND", {});
}

void GameHandler::handleLoginVerifyWorld(network::Packet& packet) {
    LOG_INFO("Handling SMSG_LOGIN_VERIFY_WORLD");
    const bool initialWorldEntry = (state == WorldState::ENTERING_WORLD);

    LoginVerifyWorldData data;
    if (!LoginVerifyWorldParser::parse(packet, data)) {
        fail("Failed to parse SMSG_LOGIN_VERIFY_WORLD");
        return;
    }

    if (!data.isValid()) {
        fail("Invalid world entry data");
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
    const bool alreadyInWorld = (state == WorldState::IN_WORLD);
    const bool sameMap = alreadyInWorld && (currentMapId_ == data.mapId);
    const float dxCurrent = movementInfo.x - canonical.x;
    const float dyCurrent = movementInfo.y - canonical.y;
    const float dzCurrent = movementInfo.z - canonical.z;
    const float distSqCurrent = dxCurrent * dxCurrent + dyCurrent * dyCurrent + dzCurrent * dzCurrent;

    // Some realms emit a late duplicate LOGIN_VERIFY_WORLD after the client is already
    // in-world. Re-running full world-entry handling here can trigger an expensive
    // same-map reload/reset path and starve networking for tens of seconds.
    // The distance test alone is not enough while flying. A taxi carries the
    // player a long way from where the packet's position points, so a late
    // duplicate stops looking like a duplicate and is taken for a teleport -
    // and world entry puts the player back at the position it carries, which
    // is where they logged in. Any of these arriving mid-flight is a duplicate
    // whatever the distance says: the server moves a passenger along the
    // spline, not by sending them into the world again.
    const bool flying = isOnTaxiFlight();
    if (!initialWorldEntry && sameMap &&
        (flying || distSqCurrent <= (5.0f * 5.0f))) {
        LOG_INFO("Ignoring duplicate SMSG_LOGIN_VERIFY_WORLD while already in world: mapId=",
                 data.mapId, " dist=", std::sqrt(distSqCurrent),
                 flying ? " (on a taxi flight)" : "");
        return;
    }

    // Said at warning level when it happens in-world, because from here the
    // player is moved to the position this packet carries. A duplicate that
    // slips past the test above is indistinguishable, after the fact, from a
    // teleport nobody asked for - and one arriving in-world is what puts a
    // player back where they logged in.
    if (!initialWorldEntry) {
        LOG_WARNING("SMSG_LOGIN_VERIFY_WORLD in-world is being treated as a "
                    "teleport: mapId=", data.mapId, " sameMap=", sameMap,
                    " dist=", std::sqrt(distSqCurrent),
                    " onTaxi=", flying,
                    " - the player is about to be placed at (", data.x, ", ",
                    data.y, ", ", data.z, ")");
    }

    // Successfully entered the world (or teleported)
    currentMapId_ = data.mapId;
    setState(WorldState::IN_WORLD);
    if (socket) {
        socket->tracePacketsFor(std::chrono::seconds(12), "login_verify_world");
    }

    LOG_INFO("========================================");
    LOG_INFO("   SUCCESSFULLY ENTERED WORLD!");
    LOG_INFO("========================================");
    LOG_INFO("Map ID: ", data.mapId);
    LOG_INFO("Position: (", data.x, ", ", data.y, ", ", data.z, ")");
    LOG_INFO("Orientation: ", data.orientation, " radians");
    LOG_INFO("Player is now in the game world");

    // The player moves itself again: a vehicle it drove stays behind.
    if (movementHandler_) movementHandler_->resetActiveMover();
    // Initialize movement info with world entry position (server → canonical)
    LOG_DEBUG("LOGIN_VERIFY_WORLD: server=(", data.x, ", ", data.y, ", ", data.z,
             ") canonical=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ") mapId=", data.mapId);
    movementInfo.x = canonical.x;
    movementInfo.y = canonical.y;
    movementInfo.z = canonical.z;
    movementInfo.orientation = core::coords::serverToCanonicalYaw(data.orientation);
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    if (movementHandler_) {
        movementHandler_->resetMovementClock();
    }
    movementInfo.time = nextMovementTimestampMs();
    if (movementHandler_) {
        movementHandler_->setFalling(false);
        movementHandler_->setFallStartMs(0);
    }
    movementInfo.fallTime = 0;
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = 0.0f;
    movementInfo.jumpCosAngle = 0.0f;
    movementInfo.jumpXYSpeed = 0.0f;
    resurrectPending_ = false;
    resurrectRequestPending_ = false;
    selfResAvailable_ = false;
    onTaxiFlight_ = false;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    taxiClientActive_ = false;
    taxiClientPath_.clear();
    // taxiRecoverPending_ is NOT cleared here - it must survive the general
    // state reset so the recovery check below can detect a mid-flight reconnect.
    taxiStartGrace_ = 0.0f;
    currentMountDisplayId_ = 0;
    vehicleId_ = 0;
    vehiclePassengers_.clear();
    unitVehicleIds_.clear();
    if (mountCallback_) {
        mountCallback_(0);
    }

    // Clear boss encounter unit slots and raid marks on world transfer
    if (socialHandler_) socialHandler_->resetTransferState();

    // Suppress area triggers on initial login - prevents exit portals from
    // immediately firing when spawning inside a dungeon/instance. Deeprun Tram
    // (map 369) needs a shorter window because exits are close to the spawn.
    const bool deeprunTram = data.mapId == 369;
    activeAreaTriggers_.clear();
    areaTriggerCheckTimer_ = deeprunTram ? -1.0f : -5.0f;
    areaTriggerSuppressFirst_ = true;
    areaTriggerCooldown_ = deeprunTram ? 1.5f : 10.0f;

    // Notify application to load terrain for this map/position (online mode)
    if (worldEntryCallback_) {
        worldEntryCallback_(data.mapId, data.x, data.y, data.z, initialWorldEntry);
    }

    // Send CMSG_SET_ACTIVE_MOVER on initial world entry and world transfers.
    if (playerGuid != 0 && socket) {
        auto activeMoverPacket = SetActiveMoverPacket::build(playerGuid);
        socket->send(activeMoverPacket);
        LOG_INFO("Sent CMSG_SET_ACTIVE_MOVER for player 0x", std::hex, playerGuid, std::dec);
    }

    // Immediately sync our position so the server doesn't keep stale coordinates
    // from a previous session or character. Without this, the server can think
    // the player is elsewhere and issue a corrective teleport.
    if (socket) {
        sendMovement(game::Opcode::MSG_MOVE_STOP);
        sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
    }

    // Kick the first keepalive immediately on world entry. Classic-like realms
    // can close the session before our default 30s ping cadence fires.
    timeSinceLastPing = 0.0f;
    if (socket) {
        LOG_DEBUG("World entry keepalive: sending immediate ping after LOGIN_VERIFY_WORLD");
        sendPing();
    }

    // If we disconnected mid-taxi, attempt to recover to destination after login.
    if (taxiRecoverPending_ && taxiRecoverMapId_ == data.mapId) {
        float dx = movementInfo.x - taxiRecoverPos_.x;
        float dy = movementInfo.y - taxiRecoverPos_.y;
        float dz = movementInfo.z - taxiRecoverPos_.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 5.0f) {
            // Keep pending until player entity exists; update() will apply.
            LOG_INFO("Taxi recovery pending: dist=", dist);
        } else {
            taxiRecoverPending_ = false;
        }
    }

    if (initialWorldEntry) {
        // Clear inspect caches on world entry to avoid showing stale data.
        inspectedPlayerAchievements_.clear();

        // Buyback mirror persists across vendor windows within a session but
        // must not leak into another character's session.
        if (inventoryHandler_) inventoryHandler_->clearBuybackState();

        // Reset talent initialization so the first SMSG_TALENTS_INFO after login
        // correctly sets the active spec (static locals don't reset across logins).
        if (spellHandler_) spellHandler_->resetTalentState();

        // Auto-join default chat channels only on first world entry.
        autoJoinDefaultChannels();

        // Auto-query guild info on login.
        const Character* activeChar = getActiveCharacter();
        if (activeChar && activeChar->hasGuild() && socket) {
            auto gqPacket = GuildQueryPacket::build(activeChar->guildId);
            socket->send(gqPacket);
            auto grPacket = GuildRosterPacket::build();
            socket->send(grPacket);
            LOG_INFO("Auto-queried guild info (guildId=", activeChar->guildId, ")");
        }

        if (questHandler_) {
            questHandler_->pendingQuestAcceptTimeoutsRef().clear();
            questHandler_->pendingQuestAcceptNpcGuidsRef().clear();
        }
        pendingQuestQueryIds_.clear();
        pendingLoginQuestResync_ = true;
        pendingLoginQuestResyncTimeout_ = 10.0f;
        completedQuests_.clear();
        LOG_INFO("Queued quest log resync for login (from server quest slots)");

        // Request completed quest IDs when the expansion supports it. Classic-like
        // opcode tables do not define this packet, and sending 0xFFFF during world
        // entry can desync the early session handshake.
        if (socket) {
            const uint16_t queryCompletedWire = wireOpcode(Opcode::CMSG_QUERY_QUESTS_COMPLETED);
            if (queryCompletedWire != 0xFFFF) {
                network::Packet cqcPkt(queryCompletedWire);
                socket->send(cqcPkt);
                LOG_INFO("Sent CMSG_QUERY_QUESTS_COMPLETED");
            } else {
                LOG_INFO("Skipping CMSG_QUERY_QUESTS_COMPLETED: opcode not mapped for current expansion");
            }
        }

        // Auto-request played time on login so the character Stats tab is
        // populated immediately without requiring /played.
        if (socket) {
            auto ptPkt = RequestPlayedTimePacket::build(false);  // false = don't show in chat
            socket->send(ptPkt);
            LOG_INFO("Auto-requested played time on login");
        }
    }

    // Pre-load DBC name caches during world entry so the first packet that
    // needs spell/title/achievement data doesn't stall mid-gameplay (the
    // Spell.dbc cache alone is ~170ms on a cold load).
    if (initialWorldEntry) {
        preloadDBCCaches();
        // Asked once, read all session: the reply carries how long until the
        // daily quests reset, which is the only place that figure comes from.
        // Silently - the announcement belongs to /time.
        queryServerTime(false);
        // Whether mail is waiting and from whom. The handler for the reply
        // existed and nothing asked, so the minimap envelope only lit when
        // mail arrived mid-session, never for mail sent while offline.
        if (inventoryHandler_) inventoryHandler_->queryNextMailTime();
    }

    // Fire PLAYER_ENTERING_WORLD - THE most important event for addon initialization.
    // Fires on initial login, teleports, instance transitions, and zone changes.
    if (addonEventCallback_) {
        fireAddonEvent("PLAYER_ENTERING_WORLD", {initialWorldEntry ? "1" : "0"});
        // FrameXML arranges itself from this event, so the interface diagnostics
        // wait for it rather than describing a layout nobody ever sees.
        ui::frameXmlNoteWorldEntry();
        // Also fire ZONE_CHANGED_NEW_AREA and UPDATE_WORLD_STATES so map/BG addons refresh
        fireAddonEvent("ZONE_CHANGED_NEW_AREA", {});
        fireAddonEvent("UPDATE_WORLD_STATES", {});
        // Entering a battleground, told apart by the map itself rather than by
        // anything the server says about it - BattlemasterList.dbc names the
        // maps and this client already reads it for the queue list.
        //
        // The battleground frame answers this by filtering chat: a battleground
        // announces every player joining and leaving, and the filter collapses
        // that into one line that counts them. Without the event the filter is
        // never installed and all of it goes to the chat window one at a time.
        battlefieldWorldEntered();
        // The currencies are known now - CurrencyTypes.dbc is read on demand
        // and the amounts are item stacks that have just arrived with the
        // inventory. The main bar builds its token frame on this and had no
        // reason to, so the frame stayed empty however much was held.
        //
        // One event, not two: KNOWN_CURRENCY_TYPES_UPDATE and
        // CURRENCY_DISPLAY_UPDATE share a branch there, and the branch rebuilds
        // the whole row either way.
        fireAddonEvent("KNOWN_CURRENCY_TYPES_UPDATE", {});
        // PLAYER_LOGIN fires only on initial login (not teleports)
        if (initialWorldEntry) {
            fireAddonEvent("PLAYER_LOGIN", {});
        }

        // The player's own values, said again now.
        //
        // Every UNIT_ event this client sends is a change notice: it fires when
        // an update block carries a different number. That is right for keeping
        // a frame current and useless for filling one in, because a frame built
        // before the player existed has missed every one of them and nothing
        // will change health or level just because someone is looking. The
        // original interface fills its unit frames on PLAYER_ENTERING_WORLD for
        // exactly this reason, and it reads the current values rather than the
        // event's - so the event alone is enough, and this is where it belongs.
        for (const char* what : {"UNIT_HEALTH", "UNIT_MAXHEALTH", "UNIT_LEVEL",
                                 "UNIT_DISPLAYPOWER", "UNIT_MANA", "UNIT_MAXMANA",
                                 "UNIT_RAGE", "UNIT_MAXRAGE", "UNIT_ENERGY",
                                 "UNIT_MAXENERGY", "UNIT_FOCUS", "UNIT_MAXFOCUS",
                                 "UNIT_NAME_UPDATE", "UNIT_FACTION"}) {
            fireAddonEvent(what, {"player"});
        }
        fireAddonEvent("PLAYER_XP_UPDATE", {"player"});
        // Dead on arrival, said now.
        //
        // Logging in as a corpse is the one time death is not a transition the
        // client watches happen - it is the state the player already has when
        // the world loads, so no health-drop and no flag-flip fires PLAYER_DEAD
        // and the interface never puts up the release-spirit popup. The player
        // sits dead, unable to act, until the server tires of waiting and
        // pulls them to the graveyard. Same reason every UNIT_ value above is
        // re-sent here: a frame built after the fact has missed the event that
        // would have filled it in.
        if (isPlayerDead()) {
            fireAddonEvent("PLAYER_DEAD", {});
            // And the body lies down. A corpse (not yet a ghost) entering the
            // world never sees health reach zero either, so nothing put the
            // character into its death pose; it stood over its own death.
            // Already down when the spawn saw the death (no second fall).
            if (!isPlayerGhost() && npcDeathCallback_) npcDeathCallback_(playerGuid);
        }
        // The durability warnings hide themselves when nothing is damaged, and
        // this is the event that asks them to look. Without it the frame keeps
        // the state its XML was written with, which is shown.
        fireAddonEvent("UPDATE_INVENTORY_ALERTS", {});
        fireAddonEvent("UPDATE_INVENTORY_DURABILITY", {});
    }
}

void GameHandler::handleClientCacheVersion(network::Packet& packet) {
    if (packet.getSize() < 4) {
        LOG_WARNING("SMSG_CLIENTCACHE_VERSION too short: ", packet.getSize(), " bytes");
        return;
    }

    uint32_t version = packet.readUInt32();
    LOG_INFO("SMSG_CLIENTCACHE_VERSION: ", version);
}

void GameHandler::handleTutorialFlags(network::Packet& packet) {
    if (packet.getSize() < 32) {
        LOG_WARNING("SMSG_TUTORIAL_FLAGS too short: ", packet.getSize(), " bytes");
        return;
    }

    std::array<uint32_t, 8> flags{};
    for (uint32_t& v : flags) {
        v = packet.readUInt32();
    }

    LOG_INFO("SMSG_TUTORIAL_FLAGS: [",
             flags[0], ", ", flags[1], ", ", flags[2], ", ", flags[3], ", ",
             flags[4], ", ", flags[5], ", ", flags[6], ", ", flags[7], "]");
}

void GameHandler::handleAccountDataTimes(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_ACCOUNT_DATA_TIMES");

    AccountDataTimesData data;
    if (!AccountDataTimesParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_ACCOUNT_DATA_TIMES");
        return;
    }

    LOG_DEBUG("Account data times received (server time: ", data.serverTime, ")");
}

void GameHandler::handleMotd(network::Packet& packet) {
    if (chatHandler_) chatHandler_->handleMotd(packet);
}

void GameHandler::handleNotification(network::Packet& packet) {
    // SMSG_NOTIFICATION: single null-terminated string
    // The red error line and the console, not chat (Wow.exe 0x401280).
    // A leading "~" carries flags the stock client reads - ">" not on
    // screen, "#" also in chat, "^N" the chat type - kept as far as Wowee can.
    std::string message = packet.readString();
    if (message.rfind("DEBUGACTION: ", 0) == 0) message = message.substr(13);
    if (message.empty()) return;
    LOG_INFO("Server notification: ", message);
    if (message[0] != '~') { addUIError(message); return; }
    size_t at = 1;
    bool onScreen = true, inChat = false;
    if (at < message.size() && message[at] == '>') { onScreen = false; ++at; }
    if (at < message.size() && message[at] == '#') { inChat = true; ++at; }
    if (at + 1 < message.size() && message[at] == '^') at += 2;
    const std::string text = message.substr(at);
    if (onScreen) addUIError(text);
    if (inChat) addSystemChatMessage(text);
}

void GameHandler::sendPing() {
    if (state != WorldState::IN_WORLD) {
        return;
    }

    // Increment sequence number
    pingSequence++;

    LOG_DEBUG("Sending CMSG_PING: sequence=", pingSequence,
              " latencyHintMs=", lastLatency);

    // Record send time for RTT measurement
    pingTimestamp_ = std::chrono::steady_clock::now();

    // Build and send ping packet
    auto packet = PingPacket::build(pingSequence, lastLatency);
    socket->send(packet);
}

void GameHandler::sendSetSheathed(uint32_t sheathState) {
    // CMSG_SET_SHEATHED: uint32 SheathState (0 unarmed, 1 melee, 2 ranged).
    // The server keeps it in UNIT_FIELD_BYTES_2 byte 0, which is how every
    // other client sees this character draw and put away its weapons.
    if (state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SET_SHEATHED));
    pkt.writeUInt32(sheathState);
    socket->send(pkt);
}

const std::vector<GameHandler::EquipmentSetInfo>& GameHandler::getEquipmentSets() const {
    if (inventoryHandler_) return inventoryHandler_->getEquipmentSets();
    static const std::vector<EquipmentSetInfo> empty;
    return empty;
}

const std::array<uint64_t, 19>* GameHandler::getEquipmentSetItems(uint32_t setId) const {
    return inventoryHandler_ ? inventoryHandler_->getEquipmentSetItems(setId) : nullptr;
}

uint32_t GameHandler::getEquipmentSetIgnoreMask(uint32_t setId) const {
    return inventoryHandler_ ? inventoryHandler_->getEquipmentSetIgnoreMask(setId) : 0u;
}

// Trade state delegation to InventoryHandler (which owns the canonical trade state)
GameHandler::TradeStatus GameHandler::getTradeStatus() const {
    if (inventoryHandler_) return static_cast<TradeStatus>(inventoryHandler_->getTradeStatus());
    return tradeStatus_;
}
bool GameHandler::hasPendingTradeRequest() const {
    return inventoryHandler_ ? inventoryHandler_->hasPendingTradeRequest() : false;
}
bool GameHandler::isTradeOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isTradeOpen() : false;
}
const std::string& GameHandler::getTradePeerName() const {
    if (inventoryHandler_) return inventoryHandler_->getTradePeerName();
    return tradePeerName_;
}
const std::array<GameHandler::TradeSlot, GameHandler::TRADE_SLOT_COUNT>& GameHandler::getMyTradeSlots() const {
    if (inventoryHandler_) {
        // Convert InventoryHandler::TradeSlot → GameHandler::TradeSlot (different struct layouts)
        static std::array<TradeSlot, TRADE_SLOT_COUNT> converted{};
        const auto& src = inventoryHandler_->getMyTradeSlots();
        for (size_t i = 0; i < TRADE_SLOT_COUNT; i++) {
            converted[i].itemId = src[i].itemId;
            converted[i].displayId = src[i].displayId;
            converted[i].stackCount = src[i].stackCount;
            converted[i].itemGuid = src[i].itemGuid;
            converted[i].occupied = (src[i].itemId != 0);
        }
        return converted;
    }
    return myTradeSlots_;
}
const std::array<GameHandler::TradeSlot, GameHandler::TRADE_SLOT_COUNT>& GameHandler::getPeerTradeSlots() const {
    if (inventoryHandler_) {
        static std::array<TradeSlot, TRADE_SLOT_COUNT> converted{};
        const auto& src = inventoryHandler_->getPeerTradeSlots();
        for (size_t i = 0; i < TRADE_SLOT_COUNT; i++) {
            converted[i].itemId = src[i].itemId;
            converted[i].displayId = src[i].displayId;
            converted[i].stackCount = src[i].stackCount;
            converted[i].itemGuid = src[i].itemGuid;
            converted[i].occupied = (src[i].itemId != 0);
        }
        return converted;
    }
    return peerTradeSlots_;
}
uint64_t GameHandler::getMyTradeGold() const {
    return inventoryHandler_ ? inventoryHandler_->getMyTradeGold() : myTradeGold_;
}
uint64_t GameHandler::getPeerTradeGold() const {
    return inventoryHandler_ ? inventoryHandler_->getPeerTradeGold() : peerTradeGold_;
}

bool GameHandler::supportsEquipmentSets() const {
    return inventoryHandler_ && inventoryHandler_->supportsEquipmentSets();
}

void GameHandler::useEquipmentSet(uint32_t setId) {
    if (inventoryHandler_) inventoryHandler_->useEquipmentSet(setId);
}

void GameHandler::saveEquipmentSet(const std::string& name, const std::string& iconName,
                                    uint64_t existingGuid, uint32_t setIndex,
                                    uint32_t ignoreMask) {
    if (inventoryHandler_) {
        inventoryHandler_->saveEquipmentSet(name, iconName, existingGuid, setIndex, ignoreMask);
    }
}

void GameHandler::deleteEquipmentSet(uint64_t setGuid) {
    if (inventoryHandler_) inventoryHandler_->deleteEquipmentSet(setGuid);
}

// --- Inventory state delegation (canonical state lives in InventoryHandler) ---

// Item text
bool GameHandler::isItemTextOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isItemTextOpen() : itemTextOpen_;
}
const std::string& GameHandler::getItemText() const {
    if (inventoryHandler_) return inventoryHandler_->getItemText();
    return itemText_;
}
void GameHandler::closeItemText() {
    if (inventoryHandler_) inventoryHandler_->closeItemText();
    else itemTextOpen_ = false;
}

// Loot
bool GameHandler::isLootWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isLootWindowOpen() : lootWindowOpen;
}
const LootResponseData& GameHandler::getCurrentLoot() const {
    if (inventoryHandler_) return inventoryHandler_->getCurrentLoot();
    return currentLoot;
}
void GameHandler::setAutoLoot(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoLoot(enabled);
    else autoLoot_ = enabled;
}
bool GameHandler::isAutoLoot() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoLoot() : autoLoot_;
}
void GameHandler::setAutoSellGrey(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoSellGrey(enabled);
    else autoSellGrey_ = enabled;
}
bool GameHandler::isAutoSellGrey() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoSellGrey() : autoSellGrey_;
}
void GameHandler::setAutoRepair(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoRepair(enabled);
    else autoRepair_ = enabled;
}
bool GameHandler::isAutoRepair() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoRepair() : autoRepair_;
}
const std::vector<uint64_t>& GameHandler::getMasterLootCandidates() const {
    if (inventoryHandler_) return inventoryHandler_->getMasterLootCandidates();
    return masterLootCandidates_;
}
bool GameHandler::hasMasterLootCandidates() const {
    return inventoryHandler_ ? inventoryHandler_->hasMasterLootCandidates() : !masterLootCandidates_.empty();
}
const LootRollEntry* GameHandler::getLootRoll(int rollId) const {
    return inventoryHandler_ ? inventoryHandler_->getLootRoll(rollId) : nullptr;
}

// Vendor
bool GameHandler::isVendorWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isVendorWindowOpen() : vendorWindowOpen;
}
const ListInventoryData& GameHandler::getVendorItems() const {
    if (inventoryHandler_) return inventoryHandler_->getVendorItems();
    return currentVendorItems;
}
void GameHandler::setVendorCanRepair(bool v) {
    if (inventoryHandler_) inventoryHandler_->setVendorCanRepair(v);
    else currentVendorItems.canRepair = v;
}
const std::deque<GameHandler::BuybackItem>& GameHandler::getBuybackItems() const {
    // One struct now (game::BuybackItem), so no cast between two look-alikes.
    if (inventoryHandler_) return inventoryHandler_->getBuybackItems();
    return buybackItems_;
}
uint64_t GameHandler::getVendorGuid() const {
    if (inventoryHandler_) return inventoryHandler_->getVendorGuid();
    return currentVendorItems.vendorGuid;
}

// Mail
bool GameHandler::isSocketingOpen() const {
    return inventoryHandler_ && inventoryHandler_->getSocketSession().open;
}

uint64_t GameHandler::getSocketItemGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getSocketSession().itemGuid : 0;
}

uint32_t GameHandler::getSocketItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getSocketSession().itemId : 0;
}
uint32_t GameHandler::getSocketPendingGemItemId(int index) const {
    if (!inventoryHandler_ || index < 0 || index > 2) return 0;
    return inventoryHandler_->getSocketSession().newGemItemId[index];
}

void GameHandler::openSocketing(uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->openSocketing(itemGuid);
}

void GameHandler::closeSocketing() {
    if (inventoryHandler_) inventoryHandler_->closeSocketing();
}

bool GameHandler::setSocketGem(int index, uint64_t gemGuid, uint32_t gemItemId) {
    return inventoryHandler_ && inventoryHandler_->setSocketGem(index, gemGuid, gemItemId);
}

void GameHandler::acceptSockets() {
    if (inventoryHandler_) inventoryHandler_->acceptSockets();
}

bool GameHandler::isMailboxOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isMailboxOpen() : mailboxOpen_;
}
const std::vector<MailMessage>& GameHandler::getMailInbox() const {
    if (inventoryHandler_) return inventoryHandler_->getMailInbox();
    return mailInbox_;
}
std::vector<MailMessage>& GameHandler::mailInboxRef() {
    if (inventoryHandler_) return inventoryHandler_->mailInboxRef();
    return mailInbox_;
}
std::string GameHandler::getMailDisplaySubject(const MailMessage& mail) {
    if (mail.messageType != 2) return mail.subject;

    AuctionMailSubject auction;
    if (!parseAuctionMailSubject(mail.subject, auction)) return mail.subject;

    ensureItemInfo(auction.itemEntry);
    const auto* info = getItemInfo(auction.itemEntry);
    const std::string itemName = info && !info->name.empty()
        ? info->name
        : "Item #" + std::to_string(auction.itemEntry);
    return formatAuctionMailSubject(auction, itemName);
}
int GameHandler::getSelectedMailIndex() const {
    return inventoryHandler_ ? inventoryHandler_->getSelectedMailIndex() : selectedMailIndex_;
}
void GameHandler::setSelectedMailIndex(int idx) {
    if (inventoryHandler_) inventoryHandler_->setSelectedMailIndex(idx);
    else selectedMailIndex_ = idx;
}
bool GameHandler::isMailComposeOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isMailComposeOpen() : showMailCompose_;
}
void GameHandler::openMailCompose() {
    if (inventoryHandler_) inventoryHandler_->openMailCompose();
    else { showMailCompose_ = true; clearMailAttachments(); }
}
void GameHandler::closeMailCompose() {
    if (inventoryHandler_) inventoryHandler_->closeMailCompose();
    else { showMailCompose_ = false; clearMailAttachments(); }
}
void GameHandler::setMailComposeShowing(bool showing) {
    if (inventoryHandler_) inventoryHandler_->setMailComposeShowing(showing);
    else showMailCompose_ = showing;
}
bool GameHandler::hasNewMail() const {
    return inventoryHandler_ ? inventoryHandler_->hasNewMail() : hasNewMail_;
}
std::vector<std::string> GameHandler::getNextMailSenderNames() const {
    std::vector<std::string> names;
    if (!inventoryHandler_) return names;
    for (const auto& sender : inventoryHandler_->getNextMailSenders()) {
        names.push_back(getMailSenderName(sender));
    }
    return names;
}
const std::array<GameHandler::MailAttachSlot, 12>& GameHandler::getMailAttachments() const {
    if (inventoryHandler_) {
        // Layout-identical structs (InventoryHandler::MailAttachSlot == GameHandler::MailAttachSlot)
        return reinterpret_cast<const std::array<MailAttachSlot, 12>&>(inventoryHandler_->getMailAttachments());
    }
    return mailAttachments_;
}


// Bank
bool GameHandler::isBankOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isBankOpen() : bankOpen_;
}
uint64_t GameHandler::getBankerGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getBankerGuid() : bankerGuid_;
}
int GameHandler::getEffectiveBankSlots() const {
    return inventoryHandler_ ? inventoryHandler_->getEffectiveBankSlots() : effectiveBankSlots_;
}
int GameHandler::getEffectiveBankBagSlots() const {
    return inventoryHandler_ ? inventoryHandler_->getEffectiveBankBagSlots() : effectiveBankBagSlots_;
}

// Guild Bank
bool GameHandler::isGuildBankOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isGuildBankOpen() : guildBankOpen_;
}
const GuildBankData& GameHandler::getGuildBankData() const {
    if (inventoryHandler_) return inventoryHandler_->getGuildBankData();
    return guildBankData_;
}
void GameHandler::setGuildBankMoney(uint64_t money) {
    if (inventoryHandler_) inventoryHandler_->setGuildBankMoney(money);
    else guildBankData_.money = money;
}
uint8_t GameHandler::getGuildBankActiveTab() const {
    return inventoryHandler_ ? inventoryHandler_->getGuildBankActiveTab() : guildBankActiveTab_;
}
void GameHandler::setGuildBankActiveTab(uint8_t tab) {
    if (inventoryHandler_) inventoryHandler_->setGuildBankActiveTab(tab);
    else guildBankActiveTab_ = tab;
}

// Auction House
bool GameHandler::isAuctionHouseOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isAuctionHouseOpen() : auctionOpen_;
}
uint64_t GameHandler::getAuctioneerGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctioneerGuid() : auctioneerGuid_;
}
const AuctionListResult& GameHandler::getAuctionBrowseResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionBrowseResults();
    return auctionBrowseResults_;
}
AuctionListResult& GameHandler::auctionBrowseResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionBrowseResultsRef();
    return auctionBrowseResults_;
}
const AuctionListResult& GameHandler::getAuctionOwnerResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionOwnerResults();
    return auctionOwnerResults_;
}
AuctionListResult& GameHandler::auctionOwnerResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionOwnerResultsRef();
    return auctionOwnerResults_;
}
const AuctionListResult& GameHandler::getAuctionBidderResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionBidderResults();
    return auctionBidderResults_;
}
AuctionListResult& GameHandler::auctionBidderResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionBidderResultsRef();
    return auctionBidderResults_;
}
int GameHandler::getAuctionActiveTab() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctionActiveTab() : auctionActiveTab_;
}
void GameHandler::setAuctionActiveTab(int tab) {
    if (inventoryHandler_) inventoryHandler_->setAuctionActiveTab(tab);
    else auctionActiveTab_ = tab;
}
float GameHandler::getAuctionSearchDelay() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctionSearchDelay() : 0.0f;
}

// Trainer
bool GameHandler::isTrainerWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isTrainerWindowOpen() : trainerWindowOpen_;
}
const TrainerListData& GameHandler::getTrainerSpells() const {
    if (inventoryHandler_) return inventoryHandler_->getTrainerSpells();
    // inventoryHandler_ is always constructed; this is the empty answer for the
    // theoretical null case, replacing a GameHandler-owned copy nothing wrote.
    static const TrainerListData kEmpty;
    return kEmpty;
}
const std::vector<GameHandler::TrainerTab>& GameHandler::getTrainerTabs() const {
    if (inventoryHandler_) {
        // Layout-identical structs (InventoryHandler::TrainerTab == GameHandler::TrainerTab)
        return reinterpret_cast<const std::vector<TrainerTab>&>(inventoryHandler_->getTrainerTabs());
    }
    static const std::vector<TrainerTab> kEmpty;
    return kEmpty;
}

void GameHandler::sendMinimapPing(float wowX, float wowY) {
    if (socialHandler_) socialHandler_->sendMinimapPing(wowX, wowY);
}

void GameHandler::handlePong(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_PONG");

    PongData data;
    if (!PongParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_PONG");
        return;
    }

    // Verify sequence matches
    if (data.sequence != pingSequence) {
        LOG_WARNING("SMSG_PONG sequence mismatch: expected ", pingSequence,
                    ", got ", data.sequence);
        return;
    }

    // Measure round-trip time
    auto rtt = std::chrono::steady_clock::now() - pingTimestamp_;
    lastLatency = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count());

    LOG_DEBUG("SMSG_PONG acknowledged: sequence=", data.sequence,
              " latencyMs=", lastLatency);
}

bool GameHandler::isServerMovementAllowed() const {
    return movementHandler_ ? movementHandler_->isServerMovementAllowed() : true;
}

uint32_t GameHandler::nextMovementTimestampMs() {
    if (movementHandler_) return movementHandler_->nextMovementTimestampMs();
    return 0;
}

void GameHandler::sendMovement(Opcode opcode) {
    if (movementHandler_) movementHandler_->sendMovement(opcode);
}

void GameHandler::sanitizeMovementForTaxi() {
    if (movementHandler_) movementHandler_->sanitizeMovementForTaxi();
}

void GameHandler::forceClearTaxiAndMovementState() {
    if (movementHandler_) movementHandler_->forceClearTaxiAndMovementState();
}

void GameHandler::setPosition(float x, float y, float z) {
    if (movementHandler_) movementHandler_->setPosition(x, y, z);
}

void GameHandler::endWorldPortHold() {
    if (movementHandler_) movementHandler_->endWorldPortHold();
}

void GameHandler::setOrientation(float orientation) {
    if (movementHandler_) movementHandler_->setOrientation(orientation);
}

// Entity lifecycle methods (handleUpdateObject, processOutOfRangeObjects,
// applyUpdateObjectBlock, finalizeUpdateObjectBatch, handleCompressedUpdateObject,
// handleDestroyObject) moved to EntityController - see entity_controller.cpp

void GameHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (chatHandler_) chatHandler_->sendChatMessage(type, message, target);
}

void GameHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (chatHandler_) chatHandler_->sendAddonMessage(type, message, target);
}

void GameHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (chatHandler_) chatHandler_->sendTextEmote(textEmoteId, targetGuid);
}

void GameHandler::joinChannel(const std::string& channelName, const std::string& password) {
    if (chatHandler_) chatHandler_->joinChannel(channelName, password);
}

void GameHandler::leaveChannel(const std::string& channelName) {
    if (chatHandler_) chatHandler_->leaveChannel(channelName);
}

std::string GameHandler::getChannelByIndex(int index) const {
    return chatHandler_ ? chatHandler_->getChannelByIndex(index) : "";
}

int GameHandler::getChannelIndex(const std::string& channelName) const {
    return chatHandler_ ? chatHandler_->getChannelIndex(channelName) : 0;
}

void GameHandler::autoJoinDefaultChannels() {
    if (chatHandler_) {
        chatHandler_->chatAutoJoin.general = chatAutoJoin.general;
        chatHandler_->chatAutoJoin.trade = chatAutoJoin.trade;
        chatHandler_->chatAutoJoin.localDefense = chatAutoJoin.localDefense;
        chatHandler_->chatAutoJoin.lfg = chatAutoJoin.lfg;
        chatHandler_->chatAutoJoin.local = chatAutoJoin.local;
        chatHandler_->autoJoinDefaultChannels();
    }
}

void GameHandler::setTarget(uint64_t guid) {
    if (combatHandler_) combatHandler_->setTarget(guid);
}

void GameHandler::clearTarget() {
    if (combatHandler_) combatHandler_->clearTarget();
}

std::shared_ptr<Entity> GameHandler::getTarget() const {
    return combatHandler_ ? combatHandler_->getTarget() : nullptr;
}

void GameHandler::setFocus(uint64_t guid) {
    if (combatHandler_) combatHandler_->setFocus(guid);
}

void GameHandler::clearFocus() {
    if (combatHandler_) combatHandler_->clearFocus();
}

void GameHandler::setMouseoverGuid(uint64_t guid) {
    if (combatHandler_) combatHandler_->setMouseoverGuid(guid);
}

std::shared_ptr<Entity> GameHandler::getFocus() const {
    return combatHandler_ ? combatHandler_->getFocus() : nullptr;
}

void GameHandler::targetLastTarget() {
    if (combatHandler_) combatHandler_->targetLastTarget();
}

void GameHandler::targetEnemy(bool reverse) {
    if (combatHandler_) combatHandler_->targetEnemy(reverse);
}

void GameHandler::targetFriend(bool reverse) {
    if (combatHandler_) combatHandler_->targetFriend(reverse);
}
void GameHandler::targetNearestEnemyPlayer(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestEnemyPlayer(reverse);
}
void GameHandler::targetNearestFriendPlayer(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestFriendPlayer(reverse);
}
void GameHandler::targetNearestPartyMember(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestPartyMember(reverse);
}
void GameHandler::targetNearestRaidMember(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestRaidMember(reverse);
}
void GameHandler::targetLastEnemy() {
    if (combatHandler_) combatHandler_->targetLastEnemy();
}
void GameHandler::targetLastFriend() {
    if (combatHandler_) combatHandler_->targetLastFriend();
}

void GameHandler::inspectTarget() {
    if (socialHandler_) socialHandler_->inspectTarget();
}

void GameHandler::inspectUnit(uint64_t guid) {
    if (socialHandler_) socialHandler_->inspectUnit(guid);
}

void GameHandler::requestInspectHonorData(uint64_t guid) {
    if (socialHandler_) socialHandler_->requestInspectHonorData(guid);
}

const GameHandler::InspectResult* GameHandler::getInspectResult() const {
    return socialHandler_ ? socialHandler_->getInspectResult() : nullptr;
}

void GameHandler::queryServerTime(bool announce) {
    if (socialHandler_) socialHandler_->queryServerTime(announce);
}

uint64_t GameHandler::getTalentWipeNpcGuid() const {
    return spellHandler_ ? spellHandler_->getTalentWipeNpcGuid() : 0u;
}

uint32_t GameHandler::getSecondsUntilDailyReset() const {
    return socialHandler_ ? socialHandler_->getSecondsUntilDailyReset() : 0u;
}

void GameHandler::requestPlayedTime() {
    if (socialHandler_) socialHandler_->requestPlayedTime();
}

void GameHandler::queryWho(const std::string& playerName) {
    if (socialHandler_) socialHandler_->queryWho(playerName);
}

void GameHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->addFriend(playerName, note);
}

void GameHandler::removeFriend(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeFriend(playerName);
}

void GameHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->setFriendNote(playerName, note);
}

void GameHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (socialHandler_) socialHandler_->randomRoll(minRoll, maxRoll);
}

void GameHandler::addIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->addIgnore(playerName);
}

void GameHandler::removeIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeIgnore(playerName);
}

void GameHandler::faceCanonicalYaw(float canonicalYaw) {
    movementInfo.orientation = canonicalYaw;

    // One-shot check that the renderer's facing round-trips to the same
    // canonical yaw we just computed from positions. If it does not, the
    // per-frame resync replaces this with a different heading and the server
    // re-checks the arc against that instead.
    if (auto* r = services_.renderer) {
        const float fromRender = core::coords::characterYawDegToCanonical(r->getCharacterYaw());
        const float delta = core::coords::normalizeAngleRad(fromRender - canonicalYaw);
        if (std::abs(delta) > 0.2f) {
            LOG_WARNING("Facing mismatch: computed canonical=", canonicalYaw,
                        " but the character's ", r->getCharacterYaw(),
                        " deg maps to ", fromRender, " (off by ",
                        delta * 57.2957795f, " deg) - the resync will undo this");
        }
    }

    // The renderer owns facing; the game side is downstream of it every frame.
    if (auto* renderer = services_.renderer) {
        const float facingDeg = core::coords::canonicalToCharacterYawDeg(canonicalYaw);
        renderer->setCharacterYaw(facingDeg);
        if (auto* cc = renderer->getCameraController()) {
            cc->setFacingYaw(facingDeg);
        }
    }

    sendMovement(Opcode::MSG_MOVE_SET_FACING);
}

void GameHandler::requestLogout(bool exitAfterLogout) {
    if (socialHandler_) socialHandler_->requestLogout(exitAfterLogout);
}

void GameHandler::cancelLogout() {
    if (socialHandler_) socialHandler_->cancelLogout();
}

void GameHandler::sendSetDifficulty(uint32_t difficulty, bool raid) {
    if (socialHandler_) socialHandler_->sendSetDifficulty(difficulty, raid);
}

void GameHandler::setStandState(uint8_t standState) {
    if (socialHandler_) socialHandler_->setStandState(standState);
}

void GameHandler::toggleHelm() {
    if (socialHandler_) socialHandler_->toggleHelm();
}

void GameHandler::toggleCloak() {
    if (socialHandler_) socialHandler_->toggleCloak();
}

void GameHandler::followTarget() {
    if (movementHandler_) movementHandler_->followTarget();
}

void GameHandler::cancelFollow() {
    if (movementHandler_) movementHandler_->cancelFollow();
}

void GameHandler::assistTarget() {
    if (combatHandler_) combatHandler_->assistTarget();
}

void GameHandler::togglePvp() {
    if (combatHandler_) combatHandler_->togglePvp();
}

void GameHandler::requestGuildInfo() {
    if (socialHandler_) socialHandler_->requestGuildInfo();
}

void GameHandler::requestGuildRoster() {
    if (socialHandler_) socialHandler_->requestGuildRoster();
}

void GameHandler::setGuildMotd(const std::string& motd) {
    if (socialHandler_) socialHandler_->setGuildMotd(motd);
}

void GameHandler::promoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->promoteGuildMember(playerName);
}

void GameHandler::demoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->demoteGuildMember(playerName);
}

void GameHandler::leaveGuild() {
    if (socialHandler_) socialHandler_->leaveGuild();
}

void GameHandler::inviteToGuild(const std::string& playerName) {
    if (socialHandler_) socialHandler_->inviteToGuild(playerName);
}

void GameHandler::initiateReadyCheck() {
    if (socialHandler_) socialHandler_->initiateReadyCheck();
}

void GameHandler::respondToReadyCheck(bool ready) {
    if (socialHandler_) socialHandler_->respondToReadyCheck(ready);
}

void GameHandler::acceptDuel() {
    if (socialHandler_) socialHandler_->acceptDuel();
}

void GameHandler::forfeitDuel() {
    if (socialHandler_) socialHandler_->forfeitDuel();
}

void GameHandler::toggleAfk(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleAfk(message);
}

void GameHandler::toggleDnd(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleDnd(message);
}

void GameHandler::replyToLastWhisper(const std::string& message) {
    if (chatHandler_) chatHandler_->replyToLastWhisper(message);
}

void GameHandler::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (socialHandler_) socialHandler_->setGuildBankTabText(tab, text);
}

void GameHandler::channelModeration(Opcode op, const std::string& channelName,
                                    const std::string& targetName,
                                    bool allowEmptyTarget) {
    if (socialHandler_) socialHandler_->channelModeration(op, channelName, targetName,
                                                         allowEmptyTarget);
}

void GameHandler::promoteToLeader(uint64_t guid) {
    if (socialHandler_) socialHandler_->promoteToLeader(guid);
}

void GameHandler::uninvitePlayer(const std::string& playerName) {
    if (socialHandler_) socialHandler_->uninvitePlayer(playerName);
}

void GameHandler::leaveParty() {
    if (socialHandler_) socialHandler_->leaveParty();
}

void GameHandler::setMainTank(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainTank(targetGuid);
}

void GameHandler::setMainAssist(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainAssist(targetGuid);
}

void GameHandler::clearMainTank() {
    if (socialHandler_) socialHandler_->clearMainTank();
}

void GameHandler::clearMainAssist() {
    if (socialHandler_) socialHandler_->clearMainAssist();
}

void GameHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (socialHandler_) socialHandler_->setRaidMark(guid, icon);
}

// The marks live in SocialHandler - MSG_RAID_TARGET_UPDATE writes them there.
// GameHandler kept a second, identical array that nothing ever wrote, so every
// caller of these (target frame, nameplates, minimap, social panel) read an
// array of zeros and no marker icon was ever drawn.
uint64_t GameHandler::getRaidMarkGuid(uint32_t icon) const {
    return socialHandler_ ? socialHandler_->getRaidMarkGuid(icon) : 0;
}

uint8_t GameHandler::getEntityRaidMark(uint64_t guid) const {
    return socialHandler_ ? socialHandler_->getEntityRaidMark(guid) : 0xFF;
}

void GameHandler::requestRaidInfo() {
    if (socialHandler_) socialHandler_->requestRaidInfo();
}

void GameHandler::setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend) {
    if (socialHandler_) socialHandler_->setSavedInstanceExtend(mapId, difficulty, extend);
}

void GameHandler::proposeDuel(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->proposeDuel(targetGuid);
}

void GameHandler::initiateTrade(uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->initiateTrade(targetGuid);
}

void GameHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (socialHandler_) socialHandler_->reportPlayer(targetGuid, reason);
}

void GameHandler::stopCasting() {
    if (spellHandler_) spellHandler_->stopCasting();
}

void GameHandler::resetCastState() {
    if (spellHandler_) spellHandler_->resetCastState();
}

void GameHandler::clearUnitCaches() {
    if (spellHandler_) spellHandler_->clearUnitCaches();
}

void GameHandler::releaseSpirit() {
    if (combatHandler_) combatHandler_->releaseSpirit();
}

bool GameHandler::canReclaimCorpse() const {
    return combatHandler_ ? combatHandler_->canReclaimCorpse() : false;
}

uint32_t GameHandler::playerFieldValue(UF field, bool* known) const {
    if (known) *known = false;
    const uint16_t idx = fieldIndex(field);
    if (idx == 0xFFFF || playerGuid == 0) return 0;
    auto entity = getEntityManager().getEntity(playerGuid);
    if (!entity) return 0;
    if (known) *known = true;
    return entity->getField(idx);
}

void GameHandler::latchReleaseTimer() const {
    if (releaseTimerLatched_ || !playerDead_ || releasedSpirit_) return;
    releaseTimerLatched_ = true;
    deathTimeMs_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    // Without the field (a layout that does not name it) there is no timer,
    // which is what this client always answered.
    bool known = false;
    const uint32_t bytes = playerFieldValue(UF::PLAYER_FIELD_BYTES, &known);
    releaseDelayMs_ = known && fieldIndex(UF::PLAYER_FIELD_BYTES) != 0xFFFF
        ? stock_death::releaseDelayMs(static_cast<uint8_t>(bytes & 0xFF),
                                      playerFieldValue(UF::PLAYER_FLAGS))
        : -1;
    LOG_INFO("Death: release timer ", releaseDelayMs_ < 0 ? std::string("none")
             : std::to_string(releaseDelayMs_ / 1000) + " s",
             " (PLAYER_FIELD_BYTES byte0=0x", std::hex, (bytes & 0xFF), std::dec, ")");
}

int GameHandler::getReleaseTimeRemaining() const {
    latchReleaseTimer();
    if (!playerDead_ || releasedSpirit_ || !releaseTimerLatched_) return -1;
    const auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return stock_death::releaseSecondsRemaining(releaseDelayMs_, deathTimeMs_, nowMs);
}

bool GameHandler::isPlayerOutOfBounds() const {
    return (playerFieldValue(UF::PLAYER_FLAGS) & stock_death::kPlayerFlagOutOfBounds) != 0;
}

uint32_t GameHandler::computeScreenEffectId() const {
    // 0x4f88b0: the auras last to first, the first SCREEN_EFFECT one wins.
    std::vector<uint32_t> fromAuras;
    loadSpellNameCache();
    for (const auto& aura : getPlayerAuras()) {
        if (aura.isEmpty()) continue;
        auto it = spellNameCache_.find(aura.spellId);
        if (it == spellNameCache_.end()) continue;
        for (int e = 0; e < 3; ++e) {
            if (it->second.effectAuraIds[e] == stock_death::kAuraScreenEffect) {
                fromAuras.push_back(static_cast<uint32_t>(it->second.effectMiscValues[e]));
                break;
            }
        }
    }
    // The ghost flag as this client tracks it: PLAYER_FLAGS, or the login and
    // corpse paths that set releasedSpirit_ before the field arrives.
    uint32_t flags = playerFieldValue(UF::PLAYER_FLAGS);
    if (releasedSpirit_) flags |= stock_death::kPlayerFlagGhost;
    const uint8_t bytes2b3 = static_cast<uint8_t>(playerFieldValue(UF::PLAYER_FIELD_BYTES2) >> 24);
    return stock_death::screenEffectId(fromAuras, flags, bytes2b3);
}

const stock_death::ScreenEffectRow* GameHandler::screenEffectRowFor(uint32_t id) {
    if (!screenEffectRowsLoaded_) {
        screenEffectRowsLoaded_ = true;
        auto* am = services_.assetManager;
        auto dbc = am ? am->loadDBC("ScreenEffect.dbc") : nullptr;
        // ID, Name, Effect, Param[4], LightParamsID, SoundAmbienceID,
        // ZoneMusicID: the 3.3.5a file, ten columns (0x4f7020 reads +0x8,
        // +0x1c, +0x20, +0x24).
        if (dbc && dbc->getFieldCount() >= 10) {
            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                stock_death::ScreenEffectRow row;
                row.id = dbc->getUInt32(r, 0);
                row.effect = dbc->getUInt32(r, 2);
                row.lightParamsSlot = static_cast<int32_t>(dbc->getUInt32(r, 7));
                row.soundAmbienceId = dbc->getUInt32(r, 8);
                row.zoneMusicId = dbc->getUInt32(r, 9);
                screenEffectRows_[row.id] = row;
            }
        } else {
            LOG_WARNING("ScreenEffect.dbc missing or not the 3.3.5a shape - "
                        "the ghost world keeps its look but not its music or ambience");
        }
    }
    auto it = screenEffectRows_.find(id);
    return it != screenEffectRows_.end() ? &it->second : nullptr;
}

void GameHandler::updateDeathState() {
    // 0x6dc070, at the death: the release timer from PLAYER_FIELD_BYTES,
    // taken here on the first frame after the death if nothing asked first.
    if (playerDead_ && !releasedSpirit_) {
        latchReleaseTimer();
        if (!deathSideEffectsDone_) {
            deathSideEffectsDone_ = true;
            // The rest of 0x6dc0f0 at the death: the target is cleared
            // (0x524bf0(0)) and an open loot released (0x523640). The windows
            // are FrameXML's, which closes them on PLAYER_DEAD.
            if (targetGuid != 0) clearTarget();
            if (isLootWindowOpen()) closeLoot();
            // And a death on a flight (UNIT_FIELD_FLAGS 0x100000) releases
            // at once (RepopMe, 0x6d2950).
            if (stock_death::releasesAtDeath(playerFieldValue(UF::UNIT_FIELD_FLAGS))) {
                LOG_INFO("Death: released at once (UNIT_FIELD_FLAGS 0x100000, on a flight)");
                releaseSpirit();
            }
        }
    } else {
        releaseTimerLatched_ = false;
        deathSideEffectsDone_ = false;
    }

    // 0x51f5c0 every frame: within 40 yards of the corpse. The edge is
    // CORPSE_IN_INSTANCE rather than CORPSE_IN_RANGE when the corpse lies on
    // another map than the one its position is given on (0x512c40).
    {
        const bool inRange = releasedSpirit_ && canReclaimCorpse();
        if (inRange != corpseInRangeAnnounced_) {
            corpseInRangeAnnounced_ = inRange;
            const bool inInstance = corpseQueryTravelMapId_ == corpseMapId_ &&
                                    corpseActualMapId_ != corpseMapId_;
            fireAddonEvent(!inRange ? "CORPSE_OUT_OF_RANGE"
                           : inInstance ? "CORPSE_IN_INSTANCE" : "CORPSE_IN_RANGE", {});
        }
    }

    // 0x4f88b0: the screen effect, its light slot, music and ambience.
    const uint32_t effectId = computeScreenEffectId();
    if (effectId != screenEffectRow_.id) {
        const stock_death::ScreenEffectRow* row = effectId ? screenEffectRowFor(effectId) : nullptr;
        screenEffectRow_ = row ? *row : stock_death::ScreenEffectRow{};
        if (effectId && !row) {
            // The ghost's row is known even without the table: Effect 1,
            // the death light, GhostState ambience 462, GhostMusic 36.
            if (effectId == stock_death::kScreenEffectGhost) {
                screenEffectRow_ = {stock_death::kScreenEffectGhost, 1, stock_death::kLightSlotDeath, 462, 36};
            } else {
                screenEffectRow_.id = effectId;
            }
        }
        LOG_INFO("Screen effect: ", screenEffectRow_.id, " (effect ", screenEffectRow_.effect,
                 ", light slot ", screenEffectRow_.lightParamsSlot, ", ambience ",
                 screenEffectRow_.soundAmbienceId, ", music ", screenEffectRow_.zoneMusicId, ")");
        if (auto* ac = services_.audioCoordinator) {
            ac->setScreenEffectSounds(screenEffectRow_.soundAmbienceId, screenEffectRow_.zoneMusicId);
        }
    }
}

float GameHandler::getCorpseReclaimDelaySec() const {
    return combatHandler_ ? combatHandler_->getCorpseReclaimDelaySec() : 0.0f;
}

void GameHandler::reclaimCorpse() {
    if (combatHandler_) combatHandler_->reclaimCorpse();
}

void GameHandler::useSelfRes() {
    if (combatHandler_) combatHandler_->useSelfRes();
}

void GameHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (combatHandler_) combatHandler_->activateSpiritHealer(npcGuid);
}

void GameHandler::acceptResurrect() {
    if (combatHandler_) combatHandler_->acceptResurrect();
}

void GameHandler::declineResurrect() {
    if (combatHandler_) combatHandler_->declineResurrect();
}

void GameHandler::tabTarget(float playerX, float playerY, float playerZ) {
    if (combatHandler_) combatHandler_->tabTarget(playerX, playerY, playerZ);
}

void GameHandler::addLocalChatMessage(const MessageChatData& msg) {
    if (chatHandler_) chatHandler_->addLocalChatMessage(msg);
}

const std::deque<MessageChatData>& GameHandler::getChatHistory() const {
    if (chatHandler_) return chatHandler_->getChatHistory();
    static const std::deque<MessageChatData> kEmpty;
    return kEmpty;
}

void GameHandler::clearChatHistory() {
    if (chatHandler_) chatHandler_->getChatHistory().clear();
}

bool GameHandler::ownsChatChannel(const std::string& name) const {
    return chatHandler_ && chatHandler_->ownsChannel(name);
}

uint64_t GameHandler::chatLineSender(uint32_t lineId) const {
    return chatHandler_ ? chatHandler_->chatLineSender(lineId) : 0;
}

const std::vector<std::string>& GameHandler::getJoinedChannels() const {
    if (chatHandler_) return chatHandler_->getJoinedChannels();
    static const std::vector<std::string> kEmpty;
    return kEmpty;
}

// ============================================================
// Name Queries (delegated to EntityController)
// ============================================================

// Who a piece of mail is from. The wire gives a player GUID for player mail and
// an entry for everything else, and nothing resolved either - the inbox showed
// an empty From line for every message.
//
// Resolved on demand rather than stored, so a name that arrives after the inbox
// was parsed shows up on the next frame instead of staying blank.
std::string GameHandler::getMailSenderName(const MailMessage& mail) const {
    switch (mail.messageType) {
        case 0: {  // from another player
            // The name-query response backfills senderName, and Reply uses that
            // field, so prefer it and fall back to the cache for the window
            // between the inbox arriving and the query returning.
            if (!mail.senderName.empty()) return mail.senderName;
            if (mail.senderGuid == 0) break;
            const std::string& name = lookupName(mail.senderGuid);
            if (!name.empty()) return name;
            return "Unknown";
        }
        case 2:    // the auction house, which has no creature behind it
            return "Auction House";
        case 3: {  // a creature
            std::string name = getCachedCreatureName(mail.senderEntry);
            if (!name.empty()) return name;
            break;
        }
        case 4: {  // a game object
            if (const auto* info = getCachedGameObjectInfo(mail.senderEntry);
                info && !info->name.empty()) {
                return info->name;
            }
            break;
        }
        default:
            break;
    }
    return "Unknown";
}

void GameHandler::queryPlayerName(uint64_t guid) {
    if (entityController_) entityController_->queryPlayerName(guid);
}

void GameHandler::queryCreatureInfo(uint32_t entry, uint64_t guid) {
    if (entityController_) entityController_->queryCreatureInfo(entry, guid);
}

uint32_t GameHandler::getCreatureDisplayIdForEntry(uint32_t entry) {
    if (entry == 0) return 0;
    const auto& cache = getCreatureInfoCache();
    if (auto it = cache.find(entry); it != cache.end()) {
        // The first non-zero of the four. A template may carry several models
        // and the server picks between them per spawn; for a preview any of
        // them is the creature, and the first is what it looks like by default.
        for (uint32_t id : it->second.displayId)
            if (id != 0) return id;
        return 0;
    }
    // Guid zero: this is not about any one spawn. queryCreatureInfo drops a
    // repeat while one is in flight, so calling every frame sends one query.
    queryCreatureInfo(entry, 0);
    return 0;
}

void GameHandler::queryGameObjectInfo(uint32_t entry, uint64_t guid) {
    if (entityController_) entityController_->queryGameObjectInfo(entry, guid);
}

std::string GameHandler::getCachedPlayerName(uint64_t guid) const {
    return entityController_ ? entityController_->getCachedPlayerName(guid) : "";
}

std::string GameHandler::getCachedCreatureName(uint32_t entry) const {
    return entityController_ ? entityController_->getCachedCreatureName(entry) : "";
}

// ============================================================
// Item Query (forwarded to InventoryHandler)
// ============================================================

void GameHandler::queryItemInfo(uint32_t entry, uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->queryItemInfo(entry, guid);
}

uint64_t GameHandler::resolveOnlineItemGuid(uint32_t itemId) const {
    return inventoryHandler_ ? inventoryHandler_->resolveOnlineItemGuid(itemId) : 0;
}

void GameHandler::detectInventorySlotBases(const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->detectInventorySlotBases(fields);
}

bool GameHandler::applyInventoryFields(const FlatFieldMap& fields) {
    return inventoryHandler_ ? inventoryHandler_->applyInventoryFields(fields) : false;
}

uint64_t GameHandler::getInteractNpcGuid() const {
    if (inventoryHandler_) {
        if (const uint64_t peer = inventoryHandler_->getTradePeerGuid()) return peer;
        if (const uint64_t vendor = getVendorGuid()) return vendor;
    }
    // The trainer, which the list above this function has always named and
    // this function never asked.
    //
    // Nothing else answers for it either: SMSG_TRAINER_LIST closes the gossip
    // that led to the trainer, so by the time the panel is up the gossip row
    // below returns 0 as well. With no unit to render, the portrait kept the
    // stand-in SetPortraitTexture stamps when it cannot get a face - which is
    // a class circle, and a tailoring trainer was drawn as a warrior's sword.
    //
    // Above the gossip for the same reason the vendor is: a trainer is a window
    // opened from a gossip and covering it.
    if (isTrainerWindowOpen()) {
        if (const uint64_t trainer = getTrainerSpells().trainerGuid) return trainer;
    }
    // From the movement handler, which owns both. GameHandler kept members of
    // the same two names that the decomposition left behind unwritten, so this
    // read false forever and the flight master had no portrait in the taxi
    // frame.
    if (movementHandler_ && movementHandler_->isTaxiWindowOpen()) {
        if (const uint64_t taxiNpc = movementHandler_->getTaxiNpcGuid()) return taxiNpc;
    }
    if (questHandler_) {
        if (const uint64_t gossip = questHandler_->getCurrentGossip().npcGuid) return gossip;
    }
    return 0;
}

bool GameHandler::getOtherPlayerEquipment(uint64_t guid,
                                          std::array<uint32_t, 19>& displayIds,
                                          std::array<uint8_t, 19>& invTypes) const {
    return inventoryHandler_
        ? inventoryHandler_->resolveOtherPlayerEquipment(guid, displayIds, invTypes)
        : false;
}

void GameHandler::extractContainerFields(uint64_t containerGuid, const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->extractContainerFields(containerGuid, fields);
}

void GameHandler::rebuildOnlineInventory() {
    if (inventoryHandler_) inventoryHandler_->rebuildOnlineInventory();
}

void GameHandler::maybeDetectVisibleItemLayout() {
    if (inventoryHandler_) inventoryHandler_->maybeDetectVisibleItemLayout();
}

void GameHandler::updateOtherPlayerVisibleItems(uint64_t guid, const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->updateOtherPlayerVisibleItems(guid, fields);
}

void GameHandler::cacheInspectedPlayerEquipment(uint64_t guid, const std::array<uint32_t, 19>& itemEntries) {
    if (inventoryHandler_) inventoryHandler_->cacheInspectedPlayerEquipment(guid, itemEntries);
}

void GameHandler::emitOtherPlayerEquipment(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->emitOtherPlayerEquipment(guid);
}

void GameHandler::emitAllOtherPlayerEquipment() {
    if (inventoryHandler_) inventoryHandler_->emitAllOtherPlayerEquipment();
}

// ============================================================
// Combat (delegated to CombatHandler)
// ============================================================

bool GameHandler::canAttackUnit(uint64_t guid) const {
    return combatHandler_ && combatHandler_->canAttackUnit(guid);
}

void GameHandler::startAutoAttack(uint64_t targetGuid) {
    if (combatHandler_) combatHandler_->startAutoAttack(targetGuid);
}

void GameHandler::stopAutoAttack() {
    if (combatHandler_) combatHandler_->stopAutoAttack();
}

void GameHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                uint64_t srcGuid, uint64_t dstGuid) {
    if (combatHandler_) combatHandler_->addCombatText(type, amount, spellId, isPlayerSource, powerType, srcGuid, dstGuid);
}

bool GameHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    return combatHandler_ ? combatHandler_->shouldLogSpellstealAura(casterGuid, victimGuid, spellId) : false;
}

void GameHandler::updateCombatText(float deltaTime) {
    if (combatHandler_) combatHandler_->updateCombatText(deltaTime);
}

bool GameHandler::isAutoAttacking() const {
    return combatHandler_ ? combatHandler_->isAutoAttacking() : false;
}

bool GameHandler::hasAutoAttackIntent() const {
    return combatHandler_ ? combatHandler_->hasAutoAttackIntent() : false;
}

bool GameHandler::isInCombat() const {
    return combatHandler_ ? combatHandler_->isInCombat() : false;
}

bool GameHandler::isInCombatWith(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isInCombatWith(guid) : false;
}

uint64_t GameHandler::getAutoAttackTargetGuid() const {
    return combatHandler_ ? combatHandler_->getAutoAttackTargetGuid() : 0;
}

bool GameHandler::isAggressiveTowardPlayer(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isAggressiveTowardPlayer(guid) : false;
}

uint64_t GameHandler::getLastMeleeSwingMs() const {
    return combatHandler_ ? combatHandler_->getLastMeleeSwingMs() : 0;
}

const std::vector<CombatTextEntry>& GameHandler::getCombatText() const {
    static const std::vector<CombatTextEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatText() : empty;
}

const std::deque<CombatLogEntry>& GameHandler::getCombatLog() const {
    static const std::deque<CombatLogEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatLog() : empty;
}

uint64_t GameHandler::getCombatLogAppended() const {
    return combatHandler_ ? combatHandler_->getCombatLogAppended() : 0;
}


void GameHandler::clearCombatText() {
    if (combatHandler_) combatHandler_->clearCombatText();
}

void GameHandler::clearHostileAttackers() {
    if (combatHandler_) combatHandler_->clearHostileAttackers();
}

const std::vector<GameHandler::ThreatEntry>* GameHandler::getThreatList(uint64_t unitGuid) const {
    return combatHandler_ ? combatHandler_->getThreatList(unitGuid) : nullptr;
}

const std::unordered_map<uint64_t, std::vector<GameHandler::ThreatEntry>>*
GameHandler::getThreatLists() const {
    return combatHandler_ ? &combatHandler_->getThreatLists() : nullptr;
}

bool GameHandler::isHostileAttacker(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isHostileAttacker(guid) : false;
}

void GameHandler::confirmBinder() {
    if (!isInWorld() || !socket || binderGuid_ == 0) return;
    auto pkt = BinderActivatePacket::build(binderGuid_);
    socket->send(pkt);
    LOG_INFO("Sent CMSG_BINDER_ACTIVATE (confirmed) for npc=0x",
             std::hex, binderGuid_, std::dec);
}

void GameHandler::setFlightAirborne(bool airborne) {
    if (movementHandler_) movementHandler_->setFlightAirborne(airborne);
}

void GameHandler::dismount() {
    if (movementHandler_) movementHandler_->dismount();
}

// ============================================================
// Arena / Battleground Handlers
// ============================================================

void GameHandler::declineBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->declineBattlefield(queueSlot);
}

bool GameHandler::hasPendingBgInvite() const {
    return socialHandler_ && socialHandler_->hasPendingBgInvite();
}

// Seeds the staging copy from the rank as it stands, so a field the panel never
// touches goes back to the server unchanged instead of as a zero.
void GameHandler::setSelectedGuildRank(int index) {
    selectedGuildRank_ = index;
    pendingGuildRank_ = PendingGuildRank{};
    const auto& roster = getGuildRoster();
    if (index < 1 || index > static_cast<int>(roster.ranks.size())) return;
    const auto& r = roster.ranks[static_cast<size_t>(index) - 1];
    pendingGuildRank_.rights    = r.rights;
    pendingGuildRank_.goldLimit = r.goldLimit;
    pendingGuildRank_.tabRights = r.bankTabRights;
    pendingGuildRank_.tabSlots  = r.bankTabSlotsPerDay;
}

void GameHandler::saveGuildRank(const std::string& rankName) {
    if (!socialHandler_) return;
    if (selectedGuildRank_ < 1) return;
    const auto& p = pendingGuildRank_;
    // Ranks are zero-based on the wire and one-based in the panel.
    socialHandler_->saveGuildRank(static_cast<uint32_t>(selectedGuildRank_ - 1),
                                  p.rights, rankName, p.goldLimit,
                                  p.tabRights.data(), p.tabSlots.data());
}

void GameHandler::setGuildInfoText(const std::string& text) {
    if (socialHandler_) socialHandler_->setGuildInfoText(text);
}

void GameHandler::takeInboxTextItem(uint32_t mailId) {
    if (socialHandler_) socialHandler_->takeInboxTextItem(mailId);
}

uint32_t GameHandler::getPlayerGuildRankRights() const {
    return socialHandler_ ? socialHandler_->getPlayerGuildRankRights() : 0u;
}

void GameHandler::delGuildRank() {
    if (socialHandler_) socialHandler_->delGuildRank();
}

void GameHandler::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (socialHandler_) socialHandler_->setLootMethod(method, masterGuid, threshold);
}

void GameHandler::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (socialHandler_) socialHandler_->setPartyAssignment(assignment, guid, apply);
}

void GameHandler::setRaidSubgroup(const std::string& playerName, uint8_t group) {
    if (socialHandler_) socialHandler_->setRaidSubgroup(playerName, group);
}

void GameHandler::swapRaidSubgroup(const std::string& a, const std::string& b) {
    if (socialHandler_) socialHandler_->swapRaidSubgroup(a, b);
}

void GameHandler::joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                  uint32_t instanceId, bool asGroup) {
    if (socialHandler_) socialHandler_->joinBattlefield(battlemasterGuid, bgTypeId, instanceId, asGroup);
}

void GameHandler::leaveBattlefield() {
    if (socialHandler_) socialHandler_->leaveBattlefield();
}

void GameHandler::requestBattlefieldList(uint32_t bgTypeId) {
    if (socialHandler_) socialHandler_->requestBattlefieldList(bgTypeId);
}

void GameHandler::reportPvpAfk(uint64_t playerGuid) {
    if (socialHandler_) socialHandler_->reportPvpAfk(playerGuid);
}

void GameHandler::acceptBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->acceptBattlefield(queueSlot);
}

// ---------------------------------------------------------------------------
// LFG / Dungeon Finder handlers (WotLK 3.3.5a)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LFG outgoing packets
// ---------------------------------------------------------------------------

void GameHandler::lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgJoin(dungeonIds, roles);
}

void GameHandler::lfgLeave() {
    if (socialHandler_) socialHandler_->lfgLeave();
}

void GameHandler::requestLfgPlayerLockInfo() {
    if (state != WorldState::IN_WORLD || !socket) return;
    // Payload-free requests. The dungeon finder's ready popup asks for both in
    // its OnShow, so the rewards it is about to draw are the current ones.
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PLAYER_LOCK_INFO_REQUEST));
    socket->send(pkt);
}

void GameHandler::requestLfgPartyLockInfo() {
    if (state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PARTY_LOCK_INFO_REQUEST));
    socket->send(pkt);
}

void GameHandler::lfgSetRoles(uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgSetRoles(roles);
}

void GameHandler::lfgCompleteRoleCheck(bool accept) {
    if (socialHandler_) socialHandler_->lfgCompleteRoleCheck(accept);
}

void GameHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (socialHandler_) socialHandler_->lfgAcceptProposal(proposalId, accept);
}

void GameHandler::lfgTeleport(bool out) {
    if (socialHandler_) socialHandler_->lfgTeleport(out);
}

void GameHandler::lfgSetBootVote(bool vote) {
    if (socialHandler_) socialHandler_->lfgSetBootVote(vote);
}

void GameHandler::loadAreaTriggerDbc() {
    if (movementHandler_) movementHandler_->loadAreaTriggerDbc();
}

void GameHandler::checkAreaTriggers() {
    if (movementHandler_) movementHandler_->checkAreaTriggers();
}

void GameHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (socialHandler_) socialHandler_->requestArenaTeamRoster(teamId);
}

void GameHandler::requestPvpLog() {
    if (socialHandler_) socialHandler_->requestPvpLog();
}

// ============================================================
// Spells
// ============================================================

void GameHandler::castSpell(uint32_t spellId, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->castSpell(spellId, targetGuid);
}

void GameHandler::cancelCast() {
    if (spellHandler_) spellHandler_->cancelCast();
}

void GameHandler::startCraftQueue(uint32_t spellId, int count) {
    if (spellHandler_) spellHandler_->startCraftQueue(spellId, count);
}

void GameHandler::cancelCraftQueue() {
    if (spellHandler_) spellHandler_->cancelCraftQueue();
}

void GameHandler::cancelAura(uint32_t spellId) {
    if (spellHandler_) spellHandler_->cancelAura(spellId);
}

uint32_t GameHandler::getTempEnchantRemainingMs(uint32_t slot) const {
    return inventoryHandler_ ? inventoryHandler_->getTempEnchantRemainingMs(slot) : 0u;
}

void GameHandler::handlePetSpells(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handlePetSpells(packet);
}

void GameHandler::sendPetAction(uint32_t action, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->sendPetAction(action, targetGuid);
}

void GameHandler::dismissCritter() {
    if (activeCritterGuid_ == 0 || !getSocket() || getState() != WorldState::IN_WORLD) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_DISMISS_CRITTER);
    if (wire == 0xFFFF) return;   // expansion has no such opcode
    auto pkt = DismissCritterPacket::build(activeCritterGuid_);
    getSocket()->send(pkt);
    LOG_INFO("Dismissing companion: guid=0x", std::hex, activeCritterGuid_, std::dec,
             " summonedBy=", activeCritterSpellId_);
    // The server confirms by clearing the critter field; leave the local state
    // alone so a refused dismiss does not leave the client thinking it worked.
}

void GameHandler::dismissPet() {
    if (spellHandler_) spellHandler_->dismissPet();
}

void GameHandler::togglePetSpellAutocast(uint32_t spellId) {
    if (spellHandler_) spellHandler_->togglePetSpellAutocast(spellId);
}

void GameHandler::renamePet(const std::string& newName) {
    if (spellHandler_) spellHandler_->renamePet(newName);
}

void GameHandler::requestStabledPetList() {
    if (spellHandler_) spellHandler_->requestStabledPetList();
}

void GameHandler::stablePet(uint8_t slot) {
    if (spellHandler_) spellHandler_->stablePet(slot);
}

void GameHandler::swapStablePet(uint32_t petNumber) {
    if (spellHandler_) spellHandler_->swapStablePet(petNumber);
}

void GameHandler::buyStableSlot() {
    if (spellHandler_) spellHandler_->buyStableSlot();
}

void GameHandler::abandonPet() {
    if (spellHandler_) spellHandler_->abandonPet();
}

// CMSG_UNLEARN_SKILL carries the skill line and nothing else.
// HandleUnlearnSkillOpcode drops anything that is not a primary profession, so
// a mistaken call costs nothing, but the confirmation dialog in front of it is
// there because forgetting one is expensive to undo.
void GameHandler::unlearnSkill(uint32_t skillId) {
    if (skillId == 0 || getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_UNLEARN_SKILL));
    pkt.writeUInt32(skillId);
    getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_UNLEARN_SKILL: skill=", skillId);
}

// An empty message. The server resets everything the player is saved to, or
// everything the group is if they lead one, and reads no body at all.
void GameHandler::resetInstances() {
    if (getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_RESET_INSTANCES));
    getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_RESET_INSTANCES");
}

void GameHandler::unstablePet(uint32_t petNumber) {
    if (spellHandler_) spellHandler_->unstablePet(petNumber);
}

void GameHandler::handleListStabledPets(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handleListStabledPets(packet);
}

void GameHandler::setActionBarSlot(int slot, ActionBarSlot::Type type, uint32_t id) {
    if (slot < 0 || slot >= ACTION_BAR_SLOTS) return;
    actionBar[slot].type = type;
    actionBar[slot].id = id;
    // Pre-query item information so action bar displays item name instead of "Item" placeholder
    if (type == ActionBarSlot::ITEM && id != 0) {
        queryItemInfo(id, 0);
    }
    saveCharacterConfig();
    // Notify Lua addons that the action bar changed
        fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(slot + 1)});
        fireAddonEvent("ACTIONBAR_UPDATE_STATE", {});
    // Notify the server so the action bar persists across relogs.
    if (isInWorld()) {
        const bool classic = isClassicLikeExpansion();
        auto pkt = SetActionButtonPacket::build(
            static_cast<uint8_t>(slot),
            static_cast<uint8_t>(type),
            id,
            classic);
        socket->send(pkt);
    }
}

void GameHandler::setGroupAssistant(uint64_t guid, bool apply) {
    if (socialHandler_) socialHandler_->setGroupAssistant(guid, apply);
}

void GameHandler::requestBattlefieldPositions() {
    if (socialHandler_) socialHandler_->requestBattlefieldPositions();
}

void GameHandler::requestContactList() {
    if (socialHandler_) socialHandler_->requestContactList();
}

uint32_t GameHandler::getPlayerGuildRankIndex() const {
    return socialHandler_ ? socialHandler_->getPlayerGuildRankIndex() : 0xFFFFFFFFu;
}

float GameHandler::getSpellCooldown(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellCooldown(spellId);
    return 0;
}

float GameHandler::getSpellCooldownTotal(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellCooldownTotal(spellId);
    return 0;
}

// ============================================================
// Talents
// ============================================================

void GameHandler::learnTalent(uint32_t talentId, uint32_t requestedRank) {
    if (spellHandler_) spellHandler_->learnTalent(talentId, requestedRank);
}

void GameHandler::learnPreviewTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talents) {
    if (spellHandler_) spellHandler_->learnPreviewTalents(talents);
}

void GameHandler::switchTalentSpec(uint8_t newSpec) {
    if (spellHandler_) spellHandler_->switchTalentSpec(newSpec);
}

void GameHandler::confirmPetUnlearn() {
    if (spellHandler_) spellHandler_->confirmPetUnlearn();
}

void GameHandler::confirmTalentWipe() {
    if (spellHandler_) spellHandler_->confirmTalentWipe();
}

void GameHandler::sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                                      uint32_t facialHairEntry, uint32_t skinColorEntry) {
    if (socialHandler_) {
        socialHandler_->sendAlterAppearance(hairStyleEntry, hairColor,
                                            facialHairEntry, skinColorEntry);
    }
}

// ============================================================
// Group/Party
// ============================================================

void GameHandler::inviteToGroup(const std::string& playerName) {
    if (socialHandler_) socialHandler_->inviteToGroup(playerName);
}

void GameHandler::acceptGroupInvite() {
    if (socialHandler_) socialHandler_->acceptGroupInvite();
}

void GameHandler::declineGroupInvite() {
    if (socialHandler_) socialHandler_->declineGroupInvite();
}

void GameHandler::leaveGroup() {
    if (socialHandler_) socialHandler_->leaveGroup();
}

void GameHandler::convertToRaid() {
    if (socialHandler_) socialHandler_->convertToRaid();
}

void GameHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (socialHandler_) socialHandler_->sendSetLootMethod(method, threshold, masterLooterGuid);
}

// ============================================================
// Guild Handlers
// ============================================================

void GameHandler::kickGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->kickGuildMember(playerName);
}

void GameHandler::disbandGuild() {
    if (socialHandler_) socialHandler_->disbandGuild();
}

void GameHandler::setGuildLeader(const std::string& name) {
    if (socialHandler_) socialHandler_->setGuildLeader(name);
}

void GameHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildPublicNote(name, note);
}

void GameHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildOfficerNote(name, note);
}

void GameHandler::acceptGuildInvite() {
    if (socialHandler_) socialHandler_->acceptGuildInvite();
}

void GameHandler::declineGuildInvite() {
    if (socialHandler_) socialHandler_->declineGuildInvite();
}

void GameHandler::submitGmTicket(const std::string& text) {
    if (chatHandler_) chatHandler_->submitGmTicket(text);
}

void GameHandler::updateGmTicket(const std::string& text) {
    if (chatHandler_) chatHandler_->updateGmTicket(text);
}

void GameHandler::destroyTotem(int slot) {
    // CMSG_TOTEM_DESTROYED carries the slot and nothing else on these
    // expansions; the guid form arrived a expansion later than any of them.
    if (slot < 0 || slot > 3 || !isInWorld() || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_TOTEM_DESTROYED));
    pkt.writeUInt8(static_cast<uint8_t>(slot));
    getSocket()->send(pkt);
}

void GameHandler::deleteGmTicket() {
    if (socialHandler_) socialHandler_->deleteGmTicket();
}

void GameHandler::requestGmTicket() {
    if (socialHandler_) socialHandler_->requestGmTicket();
}

void GameHandler::requestGmSystemStatus() {
    if (socialHandler_) socialHandler_->requestGmSystemStatus();
}

void GameHandler::requestGuildEventLog() {
    if (socialHandler_) socialHandler_->requestGuildEventLog();
}

const std::vector<GuildEventLogEntry>& GameHandler::getGuildEventLog() const {
    static const std::vector<GuildEventLogEntry> empty;
    return socialHandler_ ? socialHandler_->getGuildEventLog() : empty;
}

void GameHandler::queryGuildInfo(uint32_t guildId) {
    if (socialHandler_) socialHandler_->queryGuildInfo(guildId);
}

const std::string& GameHandler::lookupGuildName(uint32_t guildId) {
    static const std::string kEmpty;
    if (socialHandler_) return socialHandler_->lookupGuildName(guildId);
    return kEmpty;
}

uint32_t GameHandler::getEntityGuildId(uint64_t guid) const {
    if (socialHandler_) return socialHandler_->getEntityGuildId(guid);
    return 0;
}

void GameHandler::createGuild(const std::string& guildName) {
    if (socialHandler_) socialHandler_->createGuild(guildName);
}

void GameHandler::addGuildRank(const std::string& rankName) {
    if (socialHandler_) socialHandler_->addGuildRank(rankName);
}

void GameHandler::deleteGuildRank() {
    if (socialHandler_) socialHandler_->deleteGuildRank();
}

void GameHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (socialHandler_) socialHandler_->requestPetitionShowlist(npcGuid);
}

void GameHandler::buyPetition(uint64_t npcGuid, const std::string& guildName,
                              uint32_t clientIndex) {
    if (socialHandler_) socialHandler_->buyPetition(npcGuid, guildName, clientIndex);
}

void GameHandler::signPetition(uint64_t petitionGuid) {
    if (socialHandler_) socialHandler_->signPetition(petitionGuid);
}

void GameHandler::turnInPetition(uint64_t petitionGuid) {
    if (socialHandler_) socialHandler_->turnInPetition(petitionGuid);
}

void GameHandler::offerPetition(uint64_t petitionGuid, uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->offerPetition(petitionGuid, targetGuid);
}

// ============================================================
// Loot, Gossip, Vendor
// ============================================================

void GameHandler::latchAutoLootForClick(bool autoLootModifier) {
    if (inventoryHandler_) inventoryHandler_->latchAutoLoot(isAutoLoot() != autoLootModifier);
}

void GameHandler::lootTarget(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->lootTarget(guid);
}

// The corpse half of CGUnit_C::OnRightClick (0x731260), with the loot request
// itself (0x6DAD90) and the skinning cast (0x6D6F40).
void GameHandler::rightClickCorpse(uint64_t guid, bool autoLootModifier) {
    if (!isInWorld() || !socket) return;
    // A dead player who has not released does nothing with a click.
    if (isPlayerDead() && !isPlayerGhost()) return;
    auto entity = getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::UNIT) return;
    auto unit = std::static_pointer_cast<Unit>(entity);
    if (!isUnitCorpseState(unit->getHealth(), unit->getMaxHealth(), unit->getDynamicFlags())) return;

    // autoLootDefault, flipped while the auto-loot key is held (0x513700).
    const bool autoLoot = isAutoLoot() != autoLootModifier;

    float myReach = 1.5f;
    if (auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid))) {
        if (me->getCombatReach() > 0.0f) myReach = me->getCombatReach();
    }
    const float dx = movementInfo.x - unit->getX();
    const float dy = movementInfo.y - unit->getY();
    const float dz = movementInfo.z - unit->getZ();
    const float dist2 = dx * dx + dy * dy + dz * dz;

    // Lootable once it is down (0x717b60): a body still falling is not yet
    // a loot, and the click is judged as if it had none.
    if (unit->isLootableNow()) {
        // Sitting: stand up when autoStand allows, else say so.
        if (standState_ != 0) {
            if (addons::storedCVarValue("autoStand", "1") == "0") {
                raiseGameError("ERR_LOOT_NOTSTANDING", "You need to be standing up to loot something!");
                return;
            }
            setStandState(0);
        }
        // 0x6D5A60: in melee reach of the corpse, no other loot open, not
        // falling. Refused in silence - the cursor already said "unable".
        const float reach = interaction::meleeReach(myReach, unit->getCombatReach());
        if (dist2 > reach * reach) return;
        if (isLootWindowOpen()) return;
        if (movementInfo.flags & static_cast<uint32_t>(MovementFlags::FALLING)) return;
        // Not while moving, turning, pitching or flying up or down (0xC010FF).
        constexpr uint32_t kLootBlockingMoves = 0x00C010FFu;
        if (movementInfo.flags & kLootBlockingMoves) return;
        // Mounted: down with autoDismount, else refused.
        if (isMounted() && !isTaxiMountActive()) {
            if (addons::storedCVarValue("autoDismount", "1") == "0") {
                raiseGameError("ERR_NOT_WHILE_MOUNTED", "You can't do that while mounted.");
                return;
            }
            dismount();
        }
        if (inventoryHandler_) inventoryHandler_->latchAutoLoot(autoLoot);
        lootTarget(guid);
        // Loot (50) as the request goes out, before any answer (0x6DAD90).
        if (lootWindowCallback_) lootWindowCallback_(true);
        return;
    }

    if (unit->getUnitFlags() & interaction::kUnitFlagSkinnable) {
        // The skill the corpse is gathered with, by its creature type flags
        // (0x715E50): herbs, ore (mining or engineering), else hide.
        uint32_t typeFlags = 0;
        const auto& cache = getCreatureInfoCache();
        if (auto it = cache.find(unit->getEntry()); it != cache.end()) typeFlags = it->second.typeFlags;
        const uint32_t* ranks = kSkinningRanks;
        size_t count = sizeof(kSkinningRanks) / sizeof(kSkinningRanks[0]);
        const char* skillName = "Skinning";
        if (typeFlags & 0x100) {
            ranks = kHerbRanks; count = kHerbRankCount; skillName = "Herbalism";
        } else if (typeFlags & (0x200 | 0x8000)) {
            ranks = kMiningRanks; count = kMiningRankCount; skillName = "Mining";
        }
        uint32_t spellId = 0;
        const auto& known = getKnownSpells();
        for (size_t i = count; i > 0 && spellId == 0; --i) {
            if (known.count(ranks[i - 1])) spellId = ranks[i - 1];
        }
        if (spellId == 0) {
            addUIError(std::string("Requires ") + skillName);
            return;
        }
        // The cast's own range and skill checks decide the rest; the stock
        // client casts it from wherever the player stands (0x6D6F40).
        if (inventoryHandler_) inventoryHandler_->latchAutoLoot(autoLoot);
        castSpell(spellId, guid);
    }
}

void GameHandler::lootItem(uint8_t slotIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->lootItem(slotIndex, confirmed);
}

void GameHandler::confirmPendingLoot() {
    if (inventoryHandler_) inventoryHandler_->confirmPendingLoot();
}

void GameHandler::confirmBindOnUse() {
    if (inventoryHandler_) inventoryHandler_->confirmBindOnUse();
}

void GameHandler::lootMoney() {
    if (inventoryHandler_) inventoryHandler_->lootMoney();
}

void GameHandler::cancelTempEnchantment(uint8_t handIndex) {
    if (inventoryHandler_) inventoryHandler_->cancelTempEnchantment(handIndex);
}

void GameHandler::closeLoot() {
    if (inventoryHandler_) inventoryHandler_->closeLoot();
}

void GameHandler::scheduleGameObjectLootOpen(uint64_t guid, float delaySeconds, uint8_t attempts) {
    if (guid == 0) return;
    clearPendingGameObjectLootOpen(guid);
    PendingLootOpen pending;
    pending.guid = guid;
    pending.timer = std::max(0.0f, delaySeconds);
    pending.remainingAttempts = std::max<uint8_t>(attempts, 1);
    pendingGameObjectLootOpens_.push_back(pending);
}

bool GameHandler::isFishingHoleGameObject(uint64_t guid) const {
    if (guid == 0 || !entityController_) return false;
    auto entity = entityController_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return false;
    const auto* info = getCachedGameObjectInfo(
        std::static_pointer_cast<GameObject>(entity)->getEntry());
    return info && info->type == kGoTypeFishingHole;
}

void GameHandler::clearPendingGameObjectLootOpen(uint64_t guid) {
    pendingGameObjectLootOpens_.erase(
        std::remove_if(pendingGameObjectLootOpens_.begin(), pendingGameObjectLootOpens_.end(),
                       [guid](const PendingLootOpen& pending) {
                           return guid == 0 || pending.guid == guid;
                       }),
        pendingGameObjectLootOpens_.end());
}

bool GameHandler::hasPendingGameObjectLootOpen(uint64_t guid) const {
    if (guid == 0) return false;
    return std::any_of(pendingGameObjectLootOpens_.begin(), pendingGameObjectLootOpens_.end(),
                       [guid](const PendingLootOpen& pending) {
                           return pending.guid == guid;
                       });
}

bool GameHandler::isGatherGameObject(uint64_t guid) const {
    if (guid == 0 || !entityController_) return false;
    auto entity = entityController_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return false;

    auto go = std::static_pointer_cast<GameObject>(entity);
    const GameObjectQueryResponseData* goInfo = getCachedGameObjectInfo(go->getEntry());
    return gatherSpellForGameObject(goInfo, go->getName()) != 0;
}

void GameHandler::despawnCreatureLocally(uint64_t guid) {
    if (guid == 0 || !entityController_) return;

    auto& entityManager = entityController_->getEntityManager();
    auto entity = entityManager.getEntity(guid);
    if (!entity || entity->getType() != ObjectType::UNIT) return;

    if (creatureDespawnCallback_) creatureDespawnCallback_(guid);
    entityManager.removeEntity(guid);

    if (combatHandler_ && guid == combatHandler_->getAutoAttackTargetGuid()) stopAutoAttack();
    if (getTargetGuid() == guid) setTargetGuidRaw(0);

    LOG_INFO("Locally despawned looted corpse: 0x", std::hex, guid, std::dec);
}

void GameHandler::despawnGameObjectLocally(uint64_t guid) {
    if (guid == 0 || !entityController_) return;

    auto& entityManager = entityController_->getEntityManager();
    auto entity = entityManager.getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return;

    if (gameObjectDespawnCallback_) gameObjectDespawnCallback_(guid);
    entityManager.removeEntity(guid);

    clearPendingGameObjectLootOpen(guid);
    if (lastInteractedGoGuid_ == guid) lastInteractedGoGuid_ = 0;
    if (pendingGameObjectInteractGuid_ == guid) pendingGameObjectInteractGuid_ = 0;
    if (getTargetGuid() == guid) setTargetGuidRaw(0);

    LOG_INFO("Locally despawned game object: 0x", std::hex, guid, std::dec);
}

void GameHandler::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->lootMasterGive(lootSlot, targetGuid);
}

void GameHandler::raiseGameError(const char* errorName, const char* fallback) {
    // CGGameUI::DisplayError (0x5216F0) shows the GameError's GlobalStrings
    // text; the English is the fallback when the interface files are absent.
    displayGameMessage(errorName, fallback);
}

void GameHandler::displayGameMessage(const char* key, const std::string& fallback,
                                     const std::vector<std::string>& args) {
    const std::string format = globalString(services_.assetManager, key);
    const std::string text = format.empty() ? fallback : formatGlobalString(format, args);
    if (text.empty()) return;
    switch (gameMessageKind(key)) {
        case GameMessageKind::System:  addSystemChatMessage(text); break;
        case GameMessageKind::Info:    addUIInfoMessage(text); break;
        case GameMessageKind::Error:   addUIError(text); break;
        case GameMessageKind::Console: LOG_INFO("[console] ", text); break;
    }
    playUiMessageSound(key);
}

void GameHandler::addGlobalStringChatLine(const char* key, const std::string& fallback,
                                          const std::vector<std::string>& args) {
    // With no args the text goes out as it stands, as Wow.exe hands it to
    // the chat frame unformatted: DURABILITYDAMAGE_DEATH's "10% durability"
    // would otherwise lose "% d" to a printf conversion.
    std::string text = globalString(services_.assetManager, key);
    if (text.empty()) text = fallback;
    if (!args.empty()) text = formatGlobalString(text, args);
    if (!text.empty()) addSystemChatMessage(text);
}

void GameHandler::displayGameMessageNaming(uint64_t guid, const char* key, const std::string& fallback,
                                           std::vector<std::string> args, bool plain) {
    if (guid == 0) return;
    const std::string name = lookupName(guid);
    if (name.empty()) {
        auto& held = messagesAwaitingName_[guid];
        // A name that never answers must not hold an unbounded queue.
        if (held.size() < 16) held.push_back({key, fallback, std::move(args), plain});
        queryPlayerName(guid);
        return;
    }
    for (auto& a : args)
        if (a == kPlayerNameArg) a = name;
    if (plain) addGlobalStringChatLine(key, fallback, args);
    else displayGameMessage(key, formatGlobalString(fallback, args), args);
}

std::string GameHandler::formatMoneyText(uint32_t copper) const {
    struct Part { uint32_t amount; const char* key; const char* fallback; };
    const Part parts[] = {
        {copper / 10000, "GOLD_AMOUNT", "%d Gold"},
        {(copper / 100) % 100, "SILVER_AMOUNT", "%d Silver"},
        {copper % 100, "COPPER_AMOUNT", "%d Copper"},
    };
    std::string out;
    for (const auto& p : parts) {
        if (p.amount == 0) continue;
        std::string format = globalString(services_.assetManager, p.key);
        if (format.empty()) format = p.fallback;
        if (!out.empty()) out += ", ";
        out += formatGlobalString(format, {std::to_string(p.amount)});
    }
    return out;
}

void GameHandler::flushMessagesAwaitingName(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->onPlayerNameKnown(guid);
    auto it = messagesAwaitingName_.find(guid);
    if (it == messagesAwaitingName_.end()) return;
    auto held = std::move(it->second);
    messagesAwaitingName_.erase(it);
    if (lookupName(guid).empty()) return;
    for (auto& m : held) displayGameMessageNaming(guid, m.key.c_str(), m.fallback, std::move(m.args), m.plain);
}

// A right-click on a friendly NPC, as CGUnit_C::OnRightClick (0x731260) and
// CGPlayer_C::OnNpcInteract (0x6DDBB0) do it. The client measures the reach
// itself and says so when it is too far; it sends exactly one packet, picked
// by the NPC's flags; and it neither stops, turns nor dismounts the player.
void GameHandler::interactWithNpc(uint64_t guid) {
    if (!isInWorld()) return;
    auto entity = getEntityManager().getEntity(guid);
    auto* unit = dynamic_cast<Unit*>(entity.get());
    // A dead player at an area spirit healer is a different conversation. The
    // battleground ones do not answer a gossip hello at all - they answer
    // CMSG_AREA_SPIRIT_HEALER_QUERY with the seconds to the next mass
    // resurrection, and the client has to ask (0x6DB180).
    if (unit && (isPlayerDead() || isPlayerGhost())) {
        constexpr uint32_t kSpiritFlags = NPC_FLAG_SPIRIT_GUIDE | NPC_FLAG_SPIRIT_HEALER;
        if (unit->getNpcFlags() & kSpiritFlags) {
            queryAreaSpiritHealer(guid);
        }
    }
    // UNIT_NPC_FLAGS zero is a creature with nothing to say, and an update
    // block leaves zero fields out, so "not arrived" means the same thing.
    if (!unit || !unit->isInteractable()) {
        LOG_DEBUG("interactWithNpc: 0x", std::hex, guid, std::dec, " has no NPC flags; nothing sent");
        return;
    }
    // Reach: the NPC's combat reach plus four yards, centre to centre in 3D.
    // Nothing is sent from further out; the player is told instead.
    {
        const float dx = movementInfo.x - unit->getX();
        const float dy = movementInfo.y - unit->getY();
        const float dz = movementInfo.z - unit->getZ();
        const float reach = interaction::npcInteractRange(unit->getCombatReach());
        if (dx * dx + dy * dy + dz * dz > reach * reach) {
            raiseGameError("ERR_TOO_FAR_TO_INTERACT",
                           "You need to be closer to interact with that target.");
            return;
        }
    }
    // Already the one whose window is open: the second click does nothing.
    if (guid == interactTargetGuid_) return;

    // Sent anyway - the server is the judge - but said, because the server
    // says nothing: an NPC whose faction regards the player as Unfriendly or
    // worse is refused the conversation in silence.
    if (const int reaction = unitReactionToPlayer(*unit); reaction <= 3) {
        static std::unordered_set<uint64_t> said;
        if (said.insert(guid).second) {
            LOG_WARNING("interactWithNpc: ", unit->getName(), " (faction template ",
                        unit->getFactionTemplate(), ") regards you as ",
                        kReputationStandings[std::max(reaction, 1) - 1].name,
                        " - the server will not talk to you below Neutral");
        }
    }

    // One packet, by the first flag in the stock order. Every one of these is
    // the NPC's guid and nothing else.
    const auto status = static_cast<uint8_t>(getQuestGiverStatus(guid));
    const interaction::NpcHello hello = interaction::npcHelloFor(unit->getNpcFlags(), status);
    auto sendGuidPacket = [&](Opcode op) {
        const auto* table = getActiveOpcodeTable();
        if (table && !table->hasOpcode(op)) return false;
        network::Packet packet(wireOpcode(op));
        packet.writeUInt64(guid);
        socket->send(packet);
        return true;
    };
    bool sent = true;
    switch (hello) {
        case interaction::NpcHello::Gossip:
            sent = sendGuidPacket(Opcode::CMSG_GOSSIP_HELLO); break;
        case interaction::NpcHello::QuestGiver:
            sent = sendGuidPacket(Opcode::CMSG_QUESTGIVER_HELLO); break;
        case interaction::NpcHello::Taxi:
            sent = sendGuidPacket(Opcode::CMSG_TAXIQUERYAVAILABLENODES); break;
        case interaction::NpcHello::Vendor:
            sent = sendGuidPacket(Opcode::CMSG_LIST_INVENTORY); break;
        case interaction::NpcHello::Trainer:
            sent = sendGuidPacket(Opcode::CMSG_TRAINER_LIST); break;
        case interaction::NpcHello::SpiritHealer:
        case interaction::NpcHello::Innkeeper:
            // Both ask before they act: the stock client raises CONFIRM_XP_LOSS
            // or CONFIRM_BINDER itself and sends the activate on the answer.
            // CMSG_SPIRIT_HEALER_ACTIVATE or CMSG_BINDER_ACTIVATE sent now
            // would resurrect or bind with no question asked, so the hello is
            // sent instead and the server's own confirmation is the question.
            sent = sendGuidPacket(Opcode::CMSG_GOSSIP_HELLO); break;
        case interaction::NpcHello::SpiritGuide:
            queryAreaSpiritHealer(guid); break;
        case interaction::NpcHello::Banker:
            sent = sendGuidPacket(Opcode::CMSG_BANKER_ACTIVATE); break;
        case interaction::NpcHello::Petitioner:
            requestPetitionShowlist(guid); break;
        case interaction::NpcHello::TabardDesigner:
            sent = sendGuidPacket(Opcode::MSG_TABARDVENDOR_ACTIVATE); break;
        case interaction::NpcHello::Battlemaster:
            sent = sendGuidPacket(Opcode::CMSG_BATTLEMASTER_HELLO); break;
        case interaction::NpcHello::Auctioneer:
            // The window opens on the server's MSG_AUCTION_HELLO answer.
            sent = sendGuidPacket(Opcode::MSG_AUCTION_HELLO); break;
        case interaction::NpcHello::StableMaster:
            stableMasterGuid_ = guid;
            requestStabledPetList(); break;
        case interaction::NpcHello::GuildBanker:
            openGuildBank(guid); break;
        case interaction::NpcHello::SpellClick:
            sent = sendGuidPacket(Opcode::CMSG_SPELLCLICK); break;
        case interaction::NpcHello::Mailbox:
            openMailbox(guid); break;
        case interaction::NpcHello::None:
            return;
    }
    // A table without the specific opcode still has the gossip hello, which
    // AzerothCore answers for any NPC flag.
    if (!sent) sendGuidPacket(Opcode::CMSG_GOSSIP_HELLO);

    pendingNpcHello_ = PendingNpcHello{};
    pendingNpcHello_.guid = guid;
    pendingNpcHello_.sentAt = std::chrono::steady_clock::now();
    {
        const float dx = movementInfo.x - unit->getX();
        const float dy = movementInfo.y - unit->getY();
        const float dz = movementInfo.z - unit->getZ();
        pendingNpcHello_.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        pendingNpcHello_.name = unit->getName();
        pendingNpcHello_.reaction = unitReactionToPlayer(*unit);
        pendingNpcHello_.npcFlags = unit->getNpcFlags();
    }
    LOG_DEBUG("interactWithNpc: hello ", static_cast<int>(hello), " to ", pendingNpcHello_.name,
              " at ", pendingNpcHello_.distance, " yards");
}

bool GameHandler::interactTargetFacesPlayer(uint64_t guid) const {
    if (guid == 0 || guid != interactTargetGuid_) return false;
    auto entity = entityController_ ? entityController_->getEntityManager().getEntity(guid) : nullptr;
    if (!entity || entity->getType() != ObjectType::UNIT) return false;
    auto unit = std::static_pointer_cast<Unit>(entity);
    // 0x735F60 turns a unit to the active player only when it has no target
    // or channel object of its own, stands (stand state 0), is not stunned
    // (UNIT_FLAG_STUNNED 0x40000) or possessed (0x01000000), and is alive.
    // An NPC emote state whose Emotes.dbc row lacks flag 0x2000 keeps the NPC
    // at its work instead; with the flag, the emote stops while it talks
    // (0x7171C0). Only the no-emote case is taken here, so an NPC at work is
    // never turned while still working.
    constexpr uint32_t kNoTurnFlags = 0x00040000u | 0x01000000u;
    return unit->getHealth() > 0 && unit->getStandState() == 0 &&
           (unit->getUnitFlags() & kNoTurnFlags) == 0 && unit->getNpcEmoteState() == 0 &&
           unitTargetGuid(*unit) == 0;
}

std::optional<bool> GameHandler::gameObjectInUseReach(uint64_t guid) const {
    auto entity = entityController_ ? entityController_->getEntityManager().getEntity(guid) : nullptr;
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return std::nullopt;
    const auto* info = getCachedGameObjectInfo(std::static_pointer_cast<GameObject>(entity)->getEntry());
    return withinGameObjectReach(services_.assetManager, info, *entity,
                                 glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
}

// The interact target: whoever's window is open. The stock client keeps one
// (CGGameUI 0xBD07A8), set when a window opens (0x51FCE0), and every frame
// (0x51FDE0, from CGWorldFrame 0x4FA040) gives it up when the player is out of
// reach of it, it is gone, or it is no longer something to talk to. Giving it
// up closes every window bound to it (0x512E60) - gossip, quest, vendor,
// trainer, taxi, bank, guild bank, auction, mail, stable, petition, trade - and
// a unit says its NPCSounds greeting when it becomes the target and its
// farewell when it stops being one (0x720860).
void GameHandler::updateInteractTarget() {
    struct Bound {
        bool open;
        uint64_t guid;
        std::function<void()> close;
    };
    auto* inv = inventoryHandler_.get();
    const Bound windows[] = {
        {inv && inv->isTradeOpen(), inv ? inv->getTradePeerGuid() : 0, [this] { cancelTrade(); }},
        {isVendorWindowOpen(), getVendorGuid(), [this] { closeVendor(); }},
        {isTrainerWindowOpen(), getTrainerSpells().trainerGuid, [this] { closeTrainer(); }},
        {isTaxiWindowOpen(), movementHandler_ ? movementHandler_->getTaxiNpcGuid() : 0,
         [this] { closeTaxi(); }},
        {isQuestDetailsOpen(), getQuestDetails().npcGuid, [this] { declineQuest(true); }},
        {isQuestRequestItemsOpen(), getQuestRequestItems().npcGuid,
         [this] { closeQuestRequestItems(true); }},
        {isQuestOfferRewardOpen(), getQuestOfferReward().npcGuid,
         [this] { closeQuestOfferReward(true); }},
        {isGossipWindowOpen(), getCurrentGossip().npcGuid, [this] { closeGossip(); }},
        {isBankOpen(), getBankerGuid(), [this] { closeBank(); }},
        {isGuildBankOpen(), inv ? inv->getGuildBankerGuid() : 0, [this] { closeGuildBank(); }},
        {isAuctionHouseOpen(), getAuctioneerGuid(), [this] { closeAuctionHouse(); }},
        {isMailboxOpen(), inv ? inv->getMailboxGuid() : 0, [this] { closeMailbox(); }},
        {isStableWindowOpen(), getStableMasterGuid(), [this] { closeStableWindow(); }},
        {hasPetitionShowlist(), getPetitionNpcGuid(), [this] { closePetitionVendor(); }},
    };

    uint64_t current = 0;
    for (const auto& w : windows) {
        if (w.open && w.guid != 0) { current = w.guid; break; }
    }

    // Out of reach, gone, or no longer an NPC: let go of it.
    auto stillInReach = [&](uint64_t guid) -> bool {
        auto entity = getEntityManager().getEntity(guid);
        if (!entity) {
            // An item is a target with no place in the world (a quest offered
            // from a bag): never out of reach. Anything else that has left
            // view has gone.
            return guid == playerGuid || (guid >> 48) == 0x4000;
        }
        const glm::vec3 me(movementInfo.x, movementInfo.y, movementInfo.z);
        const glm::vec3 it(entity->getX(), entity->getY(), entity->getZ());
        const float d2 = glm::dot(me - it, me - it);
        if (entity->getType() == ObjectType::PLAYER) {
            return d2 <= interaction::kPlayerInteractRangeSq;
        }
        if (entity->getType() == ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<Unit>(entity);
            if (unit->getNpcFlags() == 0) return false;
            // A creature always has a combat reach; zero is one whose field
            // has not been read, and the default 1.5 stands in for it.
            const float reachField = unit->getCombatReach();
            const float reach = interaction::npcInteractRange(reachField > 0.0f ? reachField : 1.5f);
            return d2 <= reach * reach;
        }
        if (entity->getType() == ObjectType::GAMEOBJECT) {
            if (auto inReach = gameObjectInUseReach(guid)) return *inReach;
            // No model box to grow: its type's reach plus a few yards of body.
            const auto* info = getCachedGameObjectInfo(
                std::static_pointer_cast<GameObject>(entity)->getEntry());
            const float reach = interaction::gameObjectUseRange(info ? info->type : 0) + 5.0f;
            return d2 <= reach * reach;
        }
        return true;
    };

    if (current != 0 && !stillInReach(current)) {
        LOG_INFO("Interact target 0x", std::hex, current, std::dec,
                 " out of reach: closing its windows");
        for (const auto& w : windows) {
            if (w.open && w.guid == current) w.close();
        }
        current = 0;
    }

    if (current == interactTargetGuid_) return;
    const uint64_t previous = interactTargetGuid_;
    interactTargetGuid_ = current;
    auto voice = [&](uint64_t guid, auto& callback) {
        if (!callback) return;
        auto entity = getEntityManager().getEntity(guid);
        if (!entity || entity->getType() != ObjectType::UNIT) return;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (unit->getHealth() == 0 || isPlayerDead()) return;
        callback(guid, glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
    };
    // Farewell only when the target is let go of, not when another replaces
    // it (0x518D50 passes "replaced" to 0x720860).
    if (previous != 0 && current == 0) voice(previous, npcFarewellCallback_);
    if (current != 0) voice(current, npcGreetingCallback_);
}

void GameHandler::queryAreaSpiritHealer(uint64_t guid) {
    if (!isInWorld() || !socket || !guid) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY));
    packet.writeUInt64(guid);
    socket->send(packet);
    LOG_INFO("CMSG_AREA_SPIRIT_HEALER_QUERY: 0x", std::hex, guid, std::dec);
}

// 0x523EB0: a different spirit guide (or none). Leaving one whose wave was
// counting cancels the queue's aura; a new one is asked for its time.
void GameHandler::setAreaSpiritHealer(uint64_t guid) {
    if (guid == areaSpiritHealerGuid_) return;
    areaSpiritHealerGuid_ = guid;
    if (areaSpiritHealerExpiresMs_ != 0) cancelAreaSpiritHeal();
    areaSpiritHealerExpiresMs_ = 0;
    if (guid) queryAreaSpiritHealer(guid);
}

void GameHandler::updateAreaSpiritHealer() {
    if (!isInWorld() || !isPlayerGhost()) { setAreaSpiritHealer(0); return; }
    auto dist2 = [this](const Entity& e) {
        const float dx = movementInfo.x - e.getX(), dy = movementInfo.y - e.getY(),
                    dz = movementInfo.z - e.getZ();
        return dx * dx + dy * dy + dz * dz;
    };
    if (areaSpiritHealerGuid_) {
        auto e = getEntityManager().getEntity(areaSpiritHealerGuid_);
        if (!e || dist2(*e) > 484.0f) setAreaSpiritHealer(0);   // 22 yards
        return;
    }
    for (const auto& [guid, e] : getEntityManager().getEntities()) {
        if (!e || (e->getType() != ObjectType::UNIT)) continue;
        const auto* unit = static_cast<const Unit*>(e.get());
        if (!(unit->getNpcFlags() & NPC_FLAG_SPIRIT_GUIDE)) continue;
        if (dist2(*e) <= 400.0f) { setAreaSpiritHealer(guid); return; }   // 20 yards
    }
}

int GameHandler::getAreaSpiritHealerTime() const {
    if (areaSpiritHealerExpiresMs_ == 0) return 0;
    const int64_t left = areaSpiritHealerExpiresMs_ - bf::clockMs();
    return left > 0 ? static_cast<int>(left / 1000) : 0;
}

// AcceptAreaSpiritHeal() (0x5262D0 -> 0x524B60): CMSG_AREA_SPIRIT_HEALER_QUEUE
// to the spirit guide in reach.
void GameHandler::queueAreaSpiritHeal() {
    updateAreaSpiritHealer();
    if (!isInWorld() || !socket || !areaSpiritHealerGuid_) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE));
    packet.writeUInt64(areaSpiritHealerGuid_);
    socket->send(packet);
    LOG_INFO("CMSG_AREA_SPIRIT_HEALER_QUEUE: 0x", std::hex, areaSpiritHealerGuid_, std::dec);
}

void GameHandler::interactWithGameObject(uint64_t guid) {
    LOG_DEBUG("[GO-DIAG] interactWithGameObject called: guid=0x", std::hex, guid, std::dec);
    if (guid == 0) { LOG_DEBUG("[GO-DIAG] BLOCKED: guid==0"); return; }
    if (!isInWorld()) { LOG_DEBUG("[GO-DIAG] BLOCKED: not in world"); return; }
    // Do not overlap an actual spell cast. Reeling an owned fishing bobber is
    // the deliberate exception: the fishing spell remains channelled while the
    // bobber is in the water, and using the hooked bobber is what ends it.
    const bool reelingHookedFishingBobber = guid == hookedFishingBobberGuid_;
    if (!reelingHookedFishingBobber && spellHandler_ && spellHandler_->isCasting() &&
        spellHandler_->getCurrentCastSpellId() != 0) {
        LOG_DEBUG("[GO-DIAG] BLOCKED: already casting spellId=", spellHandler_->getCurrentCastSpellId());
        return;
    }
    // In the air, a click is not an order to get off. A node or a chest
    // clicked by accident from a flying mount dismounted the player there and
    // then, and they fell out of the sky. The same rule casting follows: with
    // Auto Dismount in Flight off - the default - it is refused, and the
    // player stays up.
    if (isMounted() && !isTaxiMountActive() && isPlayerFlying() &&
        addons::storedCVarValue("autoDismountFlying", "0") == "0") {
        addUIError("You can't do that while flying.");
        return;
    }
    // An open loot is released first (0x711140 -> 0x523640). An auto-attack
    // is left running: the stock client does not stop it to use an object.
    if (isLootWindowOpen()) closeLoot();
    // Getting down (0x711470, 0x7116C0). A mailbox is used from the saddle,
    // and so is anything whose template allows it (0x710390); for the rest
    // autoDismount takes the player down, and with it off the use is refused.
    // Only within reach: the stock client checks the reach before it gets
    // anyone off a mount, so a click across the valley costs nothing.
    if (isMounted() && !isTaxiMountActive()) {
        const GameObjectQueryResponseData* goInfo = nullptr;
        if (auto entity = getEntityManager().getEntity(guid);
            entity && entity->getType() == ObjectType::GAMEOBJECT) {
            goInfo = getCachedGameObjectInfo(std::static_pointer_cast<GameObject>(entity)->getEntry());
        }
        const bool allowsMounted = goInfo &&
            interaction::gameObjectAllowsMounted(goInfo->type, goInfo->hasData ? goInfo->data : nullptr);
        if (!allowsMounted && gameObjectInUseReach(guid).value_or(true)) {
            if (addons::storedCVarValue("autoDismount", "1") == "0") {
                raiseGameError("ERR_NOT_WHILE_MOUNTED", "You can't do that while mounted.");
                return;
            }
            LOG_DEBUG("[GO-DIAG] dismounting before interacting");
            dismount();
        }
    }
    // Set the pending GO guid so that:
    // 1. cancelCast() won't send CMSG_CANCEL_CAST for GO-triggered casts
    //    (e.g., "Opening" on a quest chest) - without this, any movement
    //    during the cast cancels it server-side and quest credit is lost.
    // 2. The cast-completion fallback in update() can call
    //    performGameObjectInteractionNow after the cast timer expires.
    // 3. isGameObjectInteractionCasting() returns true during GO casts.
    pendingGameObjectInteractGuid_ = guid;
    performGameObjectInteractionNow(guid);
}

// The original client can't use an object it has no template for, and neither
// can this: what to send depends on the type and the lock. A click that beats
// the query response used to go out as a bare USE plus LOOT probes, which
// AzerothCore ignores for a chest, and the click was lost. Hold it instead.
bool GameHandler::deferGameObjectClickUntilQueried(uint64_t guid) {
    if (deferredGameObjectClick_.guid == guid && deferredGameObjectClick_.released) {
        deferredGameObjectClick_ = {};
        return false;
    }
    if (!entityController_) return false;
    auto entity = entityController_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return false;
    const uint32_t entry = std::static_pointer_cast<GameObject>(entity)->getEntry();
    if (entry == 0 || getCachedGameObjectInfo(entry)) return false;

    queryGameObjectInfo(entry, guid);
    deferredGameObjectClick_ = {guid, entry, 1.5f, false};
    LOG_INFO("GO click held for query response: guid=0x", std::hex, guid, std::dec,
             " entry=", entry);
    return true;
}

void GameHandler::updateDeferredGameObjectClick(float deltaTime) {
    auto& click = deferredGameObjectClick_;
    if (click.guid == 0 || click.released) return;
    click.timer -= deltaTime;
    const bool ready = getCachedGameObjectInfo(click.entry) != nullptr;
    if (!ready && click.timer > 0.0f) return;
    if (!isInWorld() || !getEntityManager().getEntity(click.guid)) {
        click = {};
        return;
    }
    if (!ready) {
        // Dispatched anyway, the way it was before clicks were held.
        LOG_WARNING("GO query response never arrived for entry=", click.entry,
                    " - using guid=0x", std::hex, click.guid, std::dec, " without it");
    }
    click.released = true;
    performGameObjectInteractionNow(click.guid);
}

void GameHandler::performGameObjectInteractionNow(uint64_t guid) {
    if (guid == 0) return;
    if (!isInWorld()) return;
    if (deferGameObjectClickUntilQueried(guid)) return;
    // Rate-limit to prevent spamming the server
    static uint64_t lastInteractGuid = 0;
    static std::chrono::steady_clock::time_point lastInteractTime{};
    auto now = std::chrono::steady_clock::now();
    // Keep duplicate suppression, but allow quick retry clicks.
    constexpr int64_t minRepeatMs = 150;
    if (guid == lastInteractGuid &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInteractTime).count() < minRepeatMs) {
        return;
    }
    lastInteractGuid = guid;
    lastInteractTime = now;

    auto entity = entityController_->getEntityManager().getEntity(guid);
    uint32_t goEntry = 0;
    uint32_t goType = 0;
    std::string goName;
    const GameObjectQueryResponseData* goInfo = nullptr;
    float interactionDistance = -1.0f;

    if (entity) {
        if (entity->getType() == ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<GameObject>(entity);
            goEntry = go->getEntry();
            goName = go->getName();
            goInfo = getCachedGameObjectInfo(goEntry);
            if (goInfo) goType = goInfo->type;
            if (goType == kGoTypeFishingHole) {
                // Nothing sent, and nothing scheduled: the school is fished, not
                // opened. Silent, because retail does not respond to the click
                // either.
                LOG_INFO("GO fishing hole click ignored: guid=0x", std::hex, guid,
                         std::dec, " entry=", goEntry, " name='", goName, "'");
                return;
            }
            // A generic object's name is not said on a click; the stock
            // client shows it on the tooltip and nowhere else.
        }
        // Face object and send heartbeat before use so strict servers don't require
        // a nudge movement to accept interaction.
        float dx = entity->getX() - movementInfo.x;
        float dy = entity->getY() - movementInfo.y;
        float dz = entity->getZ() - movementInfo.z;
        float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
        interactionDistance = dist3d;
        // Fishing bobbers are intentionally cast beyond normal GO-use range.
        // Only the owned bobber whose bite we observed gets the extended range.
        // Anything else is measured against its display box when there is one,
        // and against a 10-yard sphere when there is not.
        const std::optional<bool> inReach = guid == hookedFishingBobberGuid_
            ? std::nullopt
            : withinGameObjectReach(services_.assetManager, goInfo, *entity,
                                    glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
        const float maxInteractDistance = (guid == hookedFishingBobberGuid_)
            ? 30.0f : interaction::gameObjectUseRange(goType) + 5.0f;
        if (inReach ? !*inReach : dist3d > maxInteractDistance) {
            raiseGameError("ERR_USE_TOO_FAR", "You are too far away.");
            return;
        }
        // A chest that says "not in combat" stays shut in a fight (0x710340).
        if (goInfo && isInCombat() &&
            !interaction::gameObjectUsableInCombat(goInfo->type, goInfo->hasData ? goInfo->data : nullptr)) {
            raiseGameError("ERR_NOT_IN_COMBAT", "You can't do that while in combat");
            return;
        }
        // The stock client neither stops nor turns the player to use an object
        // (CGGameObject_C::OnRightClick 0x711140, Use 0x70F680): a gather
        // started on the move is the cast's to refuse. Only the position is
        // brought up to date, which nothing on screen shows.
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    // Determine GO type for interaction strategy
    bool isMailbox = false;
    bool isGuildBank = false;
    bool isReadable = false;
    bool chestLike = false;
    bool metadataPending = false;
    if (entity && entity->getType() == ObjectType::GAMEOBJECT) {
        auto go = std::static_pointer_cast<GameObject>(entity);
        if (!goInfo) goInfo = getCachedGameObjectInfo(go->getEntry());
        metadataPending = (goInfo == nullptr);
        if (goInfo && goInfo->type == 19) {
            isMailbox = true;
        } else if (goInfo && goInfo->type == 34) { // GAMEOBJECT_TYPE_GUILD_BANK
            isGuildBank = true;
        } else if (goInfo && goInfo->type == 3) {
            chestLike = true;
        } else if (goInfo && (goInfo->type == 9 ||
                              (goInfo->type == 10 && goInfo->hasData && goInfo->data[7] != 0))) {
            // 9 is TEXT - a sign - and a GOOBER carries a page of its own in
            // data[7] when it has one to show.
            isReadable = true;
        }
    }
    LOG_INFO("GO interaction: guid=0x", std::hex, guid, std::dec,
             " entry=", goEntry, " type=", goType,
             " name='", goName, "' chestLike=", chestLike,
             " metadataPending=", metadataPending, " isMailbox=", isMailbox,
             " lockId=", gameObjectLockId(goInfo));

    // The original client sends CMSG_GAMEOBJ_REPORT_USE after every use, whether
    // it went out as a USE, an open-lock cast, a key or a gather. The server runs
    // the object's report-use scripts (SmartAI quest credit) and the use-object
    // achievement criteria from it. Only WotLK's table has the opcode.
    const auto sendReportUse = [this, guid]() {
        const auto* table = getActiveOpcodeTable();
        if (table && table->hasOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE)) {
            network::Packet reportUse(wireOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE));
            reportUse.writeUInt64(guid);
            socket->send(reportUse);
        }
    };

    const uint32_t gatherBaseSpellId = gatherSpellForGameObject(goInfo, goName);
    if (gatherBaseSpellId != 0) {
        const uint32_t gatherSpellId = knownGatherRank(spellHandler_.get(), gatherBaseSpellId);
        if (gatherSpellId == 0) {
            // ERR_USE_LOCKED_WITH_SPELL_S naming the lock's skill, the red
            // error (Wow.exe 0x7116C0, lock type 2 with the skill unknown).
            {
                const std::string skill = gatherBaseSpellId == kMiningBaseSpellId ? "Mining" : "Herbalism";
                displayGameMessage("ERR_USE_LOCKED_WITH_SPELL_S", "Requires " + skill, {skill});
            }
            LOG_INFO("GO gather skipped: no known rank for base spell=", gatherBaseSpellId,
                     " guid=0x", std::hex, guid, std::dec, " name='", goName, "'");
            return;
        }
        auto castPacket = getPacketParsers()
            ? getPacketParsers()->buildCastGameObjectSpell(gatherSpellId, guid, 0)
            : CastSpellPacket::buildGameObjectTarget(gatherSpellId, guid, 0);
        socket->send(castPacket);
        lastInteractedGoGuid_ = guid;
        scheduleGameObjectLootOpen(guid, 0.50f, 8);
        sendReportUse();
        LOG_INFO("GO gather cast: spell=", gatherSpellId, " guid=0x",
                 std::hex, guid, std::dec, " name='", goName, "'");
        return;
    }

    if (gameObjectLockId(goInfo) != 0) {
        // A locked object is opened by casting an OPEN_LOCK spell at it (a known
        // Pick Lock, or the generic Opening), on every expansion; the server then
        // validates the lock. A plain CMSG_GAMEOBJ_USE only opens unlocked /
        // server-gated quest containers (e.g. the Legend of Stalvan Sealed Crate).
        const uint16_t goLevelField = fieldIndex(UF::GAMEOBJECT_LEVEL);
        const uint32_t goLevel = (entity && goLevelField != 0xFFFF)
            ? entity->getField(goLevelField) : 0;
        const LockOpenPlan plan = planGameObjectOpen(
            services_.assetManager, goInfo, spellHandler_->getKnownSpells(),
            getPlayerLevel(), goLevel);
        if (plan.method == LockOpenMethod::CastSpell) {
            auto castPacket = getPacketParsers()->buildCastGameObjectSpell(
                plan.spellId, guid, 0);
            socket->send(castPacket);
            lastInteractedGoGuid_ = guid;
            scheduleGameObjectLootOpen(guid, 0.60f, 8);
            sendReportUse();
            LOG_INFO("GO open-lock cast: spell=", plan.spellId,
                     " guid=0x", std::hex, guid, std::dec,
                     " entry=", goEntry, " name='", goName, "'");
            return;
        }
        // Key-item lock: a warrior knows no open-lock spell. Open it by USING the
        // key item on the chest (CMSG_USE_ITEM targeting the GO) - the server casts
        // the key's on-use OPEN_LOCK spell because the player holds the key. A plain
        // USE does not open a locked type-3 chest, and casting the item's spell via
        // CMSG_CAST_SPELL is rejected (the player doesn't "know" it).
        if (plan.keyItemId != 0) {
            const ItemQueryResponseData* keyInfo = getItemInfo(plan.keyItemId);
            uint32_t keyUseSpell = 0;
            if (keyInfo) {
                for (const auto& sp : keyInfo->spells)
                    if (sp.spellTrigger == 0 && sp.spellId != 0 && keyUseSpell == 0)
                        keyUseSpell = sp.spellId;
            } else {
                ensureItemInfo(plan.keyItemId); // not cached yet - request for next click
            }

            // Locate the key: wire (bag, slot) + item GUID. Keyring wire slots
            // start at 86; backpack at 23 in bag 0xFF; equipped bags are 19..22.
            const Inventory& inv = getInventory();
            uint8_t keyBag = 0xFF, keySlot = 0; uint64_t keyGuid = 0; bool keyFound = false;
            for (int i = 0; i < inv.getBackpackSize() && !keyFound; ++i)
                if (inv.getBackpackSlot(i).item.itemId == plan.keyItemId) {
                    keyBag = 0xFF; keySlot = static_cast<uint8_t>(game::slots::backpackWireSlot(i));
                    keyGuid = inv.getBackpackSlot(i).item.guid; keyFound = true;
                }
            for (int b = 0; b < Inventory::NUM_BAG_SLOTS && !keyFound; ++b)
                for (int s = 0; s < inv.getBagSize(b) && !keyFound; ++s)
                    if (inv.getBagSlot(b, s).item.itemId == plan.keyItemId) {
                        keyBag = static_cast<uint8_t>(game::slots::wornBagContainer(b)); keySlot = static_cast<uint8_t>(s);
                        keyGuid = inv.getBagSlot(b, s).item.guid; keyFound = true;
                    }
            for (int i = 0; i < inv.getKeyringSize() && !keyFound; ++i)
                if (inv.getKeyringSlot(i).item.itemId == plan.keyItemId) {
                    keyBag = 0xFF; keySlot = static_cast<uint8_t>(game::slots::keyringWireSlot(i));
                    keyGuid = inv.getKeyringSlot(i).item.guid; keyFound = true;
                }

            if (keyFound && keyUseSpell != 0) {
                auto usePacket = getPacketParsers()->buildUseItem(keyBag, keySlot, keyGuid,
                                                                  keyUseSpell, 0, 0, guid);
                socket->send(usePacket);
                lastInteractedGoGuid_ = guid;
                scheduleGameObjectLootOpen(guid, 0.60f, 8);
                sendReportUse();
                LOG_INFO("GO key-item use: keyItem=", plan.keyItemId,
                         " spell=", keyUseSpell, " keySlot(bag=", (int)keyBag,
                         " slot=", (int)keySlot, ") guid=0x", std::hex, guid, std::dec,
                         " entry=", goEntry, " name='", goName, "'");
                return;
            }
        }
        // LockOpenMethod::UseDirect - fall through to CMSG_GAMEOBJ_USE.
        LOG_INFO("GO opens via direct USE: lockId=", gameObjectLockId(goInfo),
                 " keyItem=", plan.keyItemId,
                 " guid=0x", std::hex, guid, std::dec,
                 " entry=", goEntry, " name='", goName, "'");
    }

    // CMSG_GAMEOBJ_USE is the generic type handler's use (0x7116C0). A
    // mailbox (0x70C9D0), a guild vault (0x70ECB0) and a sign (0x70C850) have
    // their own, which open their window here and send no USE; only the
    // CMSG_GAMEOBJ_REPORT_USE after it goes out for them too (0x70F680).
    const bool ownUse = isMailbox || isGuildBank || (goInfo && goInfo->type == 9);
    if (!ownUse) {
        auto usePacket = GameObjectUsePacket::build(guid);
        socket->send(usePacket);
    }
    lastInteractedGoGuid_ = guid;
    if (guid == hookedFishingBobberGuid_) {
        LOG_WARNING("Fishing bobber reel sent: guid=0x", std::hex, guid, std::dec,
                    " distance=", interactionDistance);
        hookedFishingBobberGuid_ = 0;
    }

    if (chestLike || metadataPending) {
        // Don't send CMSG_LOOT immediately - the server may start a timed cast
        // (e.g., "Opening") and the GO isn't lootable until the cast finishes.
        // Sending LOOT prematurely gets an empty response or is silently dropped,
        // which can interfere with the server's loot state machine.
        // Queue a delayed open: if a server-side gather cast starts, update()
        // defers this until the cast is over; if no cast packet arrives, retry
        // a few times so resource nodes do not fail after one early CMSG_LOOT.
        // Unknown metadata is common immediately after a GO spawn. A delayed
        // loot probe is harmless for non-loot objects and prevents the first
        // click on quest containers from being lost while their query is pending.
        scheduleGameObjectLootOpen(guid, 0.35f, 8);
    } else if (isMailbox) {
        openMailbox(guid);
    } else if (isGuildBank) {
        // Guild vault: CMSG_GAMEOBJ_USE above is a no-op on the server; the bank
        // opens via CMSG_GUILD_BANKER_ACTIVATE, which openGuildBank() sends.
        openGuildBank(guid);
    } else if (isReadable) {
        // A sign, a plaque, a gravestone: the client reads it.
        //
        // Nothing happened when one was clicked because this waited for
        // SMSG_GAMEOBJECT_PAGETEXT, which is the server offering a page - and
        // for a type 9 the server has nothing to offer, the page id being in
        // the object's own data where the real client reads it from. Stormwind
        // is full of signs that answered a click with silence.
        if (entityController_) entityController_->showGameObjectPageText(guid);
    }

    sendReportUse();
}

void GameHandler::selectGossipOption(uint32_t optionId, const std::string& code) {
    if (questHandler_) questHandler_->selectGossipOption(optionId, code);
}

void GameHandler::selectGossipQuest(uint32_t questId) {
    if (questHandler_) questHandler_->selectGossipQuest(questId);
}

bool GameHandler::requestQuestQuery(uint32_t questId, bool force) {
    return questHandler_ && questHandler_->requestQuestQuery(questId, force);
}

bool GameHandler::hasQuestInLog(uint32_t questId) const {
    return questHandler_ && questHandler_->hasQuestInLog(questId);
}

Unit* GameHandler::getUnitByGuid(uint64_t guid) {
    auto entity = entityController_->getEntityManager().getEntity(guid);
    // Use the type tag to skip RTTI - both UNIT and PLAYER object types derive from Unit.
    if (!entity || !entity->isUnit()) return nullptr;
    return static_cast<Unit*>(entity.get());
}

std::string GameHandler::guidToUnitId(uint64_t guid) const {
    if (guid == playerGuid)      return "player";
    if (guid == targetGuid)      return "target";
    if (guid == focusGuid)       return "focus";
    if (guid == petGuid_)        return "pet";
    return {};
}
std::vector<std::string> GameHandler::unitTokensForGuid(uint64_t guid) const {
    std::vector<std::string> out;
    if (guid == 0) return out;
    if (guid == playerGuid) out.emplace_back("player");
    if (guid == petGuid_) out.emplace_back("pet");
    const auto& pd = getPartyData();
    const bool raid = groupIsRaid(pd.groupType);
    int n = 0;
    for (const auto& m : pd.members) {
        if (m.guid == playerGuid) continue;
        if (raid && m.subGroup != pd.subGroup) continue;
        if (++n > 4) break;
        if (m.guid == guid) out.push_back("party" + std::to_string(n));
    }
    if (raid) {
        const std::string r = groupUnitToken(pd, playerGuid, guid);
        if (r.rfind("raid", 0) == 0) out.push_back(r);
    }
    for (size_t i = 0; i < 5; ++i) {
        if (arenaOpponentGuid(i, false) == guid) out.push_back("arena" + std::to_string(i + 1));
        else if (arenaOpponentGuid(i, true) == guid) out.push_back("arenapet" + std::to_string(i + 1));
    }
    if (guid == targetGuid) out.emplace_back("target");
    if (guid == focusGuid) out.emplace_back("focus");
    if (guid == mouseoverGuid_) out.emplace_back("mouseover");
    return out;
}
int GameHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questHandler_) return questHandler_->findQuestLogSlotIndexFromServer(questId);
    return 0;
}

void GameHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questHandler_) questHandler_->addQuestToLocalLogIfMissing(questId, title, objectives);
}

bool GameHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    return questHandler_ && questHandler_->resyncQuestLogFromServerSlots(forceQueryMetadata);
}

// Apply quest completion state from player update fields to already-tracked local quests.
// Called from VALUES update handler so quests that complete mid-session (or that were
// complete on login) get quest.complete=true without waiting for SMSG_QUESTUPDATE_COMPLETE.
void GameHandler::applyQuestStateFromFields(const FlatFieldMap& fields) {
    if (questHandler_) questHandler_->applyQuestStateFromFields(fields);
}

// Extract expansion-specific kill/objective counters from quest-log update fields
// and populate quest.killCounts using the structured objectives obtained from a prior
// SMSG_QUEST_QUERY_RESPONSE. Silently does nothing if objectives are absent.
void GameHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (questHandler_) questHandler_->applyPackedKillCountsFromFields(quest);
}

void GameHandler::clearPendingQuestAccept(uint32_t questId) {
    if (questHandler_) questHandler_->clearPendingQuestAccept(questId);
}

void GameHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questHandler_) questHandler_->triggerQuestAcceptResync(questId, npcGuid, reason);
}

void GameHandler::acceptQuest() {
    if (questHandler_) questHandler_->acceptQuest();
}

void GameHandler::declineQuest(bool announce) {
    if (questHandler_) questHandler_->declineQuest(announce);
}

void GameHandler::abandonQuest(uint32_t questId) {
    if (questHandler_) questHandler_->abandonQuest(questId);
    setQuestTracked(questId, false);
}

void GameHandler::shareQuestWithParty(uint32_t questId) {
    if (questHandler_) questHandler_->shareQuestWithParty(questId);
}

void GameHandler::completeQuest() {
    if (questHandler_) questHandler_->completeQuest();
}

void GameHandler::closeQuestRequestItems(bool announce) {
    if (questHandler_) questHandler_->closeQuestRequestItems(announce);
}

void GameHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (questHandler_) questHandler_->chooseQuestReward(rewardIndex);
}

void GameHandler::closeQuestOfferReward(bool announce) {
    if (questHandler_) questHandler_->closeQuestOfferReward(announce);
}

void GameHandler::closeGossip() {
    if (questHandler_) questHandler_->closeGossip();
}

void GameHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (questHandler_) questHandler_->offerQuestFromItem(itemGuid, questId);
}

uint64_t GameHandler::getBagItemGuid(int bagIndex, int slotIndex) const {
    if (bagIndex < 0 || bagIndex >= inventory.NUM_BAG_SLOTS) return 0;
    if (slotIndex < 0) return 0;
    uint64_t bagGuid = equipSlotGuids_[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid == 0) return 0;
    auto it = containerContents_.find(bagGuid);
    if (it == containerContents_.end()) return 0;
    if (slotIndex >= static_cast<int>(it->second.numSlots)) return 0;
    return it->second.slotGuids[slotIndex];
}

void GameHandler::openVendor(uint64_t npcGuid) {
    if (inventoryHandler_) inventoryHandler_->openVendor(npcGuid);
}

void GameHandler::closeVendor() {
    if (inventoryHandler_) inventoryHandler_->closeVendor();
}

void GameHandler::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->buyItem(vendorGuid, itemId, slot, count);
}

void GameHandler::buyBackItem(uint32_t buybackSlot) {
    if (inventoryHandler_) inventoryHandler_->buyBackItem(buybackSlot);
}

void GameHandler::repairItem(uint64_t vendorGuid, uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->repairItem(vendorGuid, itemGuid);
}

void GameHandler::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (inventoryHandler_) inventoryHandler_->repairAll(vendorGuid, useGuildBank);
}

uint32_t GameHandler::estimateRepairAllCost() const {
    if (inventoryHandler_) return inventoryHandler_->estimateRepairAllCost();
    return 0;
}

uint32_t GameHandler::estimateItemRepairCost(uint64_t itemGuid) const {
    if (inventoryHandler_) return inventoryHandler_->estimateItemRepairCost(itemGuid);
    return 0;
}

void GameHandler::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->sellItem(vendorGuid, itemGuid, count);
}

void GameHandler::sellItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemBySlot(backpackIndex);
}

void GameHandler::equipItemToSlot(uint64_t itemGuid, uint8_t equipSlot) {
    if (inventoryHandler_) inventoryHandler_->equipItemToSlot(itemGuid, equipSlot);
}

void GameHandler::autoEquipItemBySlot(int backpackIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemBySlot(backpackIndex, confirmed);
}

bool GameHandler::equipWouldBindFromBackpack(int backpackIndex) const {
    return inventoryHandler_ && inventoryHandler_->equipWouldBindFromBackpack(backpackIndex);
}

bool GameHandler::equipWouldBindFromBag(int bagIndex, int slotIndex) const {
    return inventoryHandler_ && inventoryHandler_->equipWouldBindFromBag(bagIndex, slotIndex);
}

void GameHandler::equipPendingItem() {
    if (inventoryHandler_) inventoryHandler_->equipPendingItem();
}

void GameHandler::cancelPendingEquip() {
    if (inventoryHandler_) inventoryHandler_->cancelPendingEquip();
}

void GameHandler::autoEquipItemInBag(int bagIndex, int slotIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemInBag(bagIndex, slotIndex, confirmed);
}

void GameHandler::sellItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemInBag(bagIndex, slotIndex);
}

void GameHandler::unequipToBackpack(EquipSlot equipSlot) {
    if (inventoryHandler_) inventoryHandler_->unequipToBackpack(equipSlot);
}

void GameHandler::swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag,
                                     uint8_t dstSlot, bool applyLocally) {
    if (inventoryHandler_) {
        inventoryHandler_->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot, applyLocally);
    }
}

void GameHandler::notifyBagsChanged() {
    if (inventoryHandler_) inventoryHandler_->fireBagUpdates();
}

void GameHandler::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (inventoryHandler_) inventoryHandler_->swapBagSlots(srcBagIndex, dstBagIndex);
}

void GameHandler::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->destroyItem(bag, slot, count);
}

void GameHandler::splitItem(uint8_t srcBag, uint8_t srcSlot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->splitItem(srcBag, srcSlot, count);
}
void GameHandler::splitItemTo(uint8_t srcBag, uint8_t srcSlot,
                              uint8_t dstBag, uint8_t dstSlot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->splitItemTo(srcBag, srcSlot, dstBag, dstSlot, count);
}
void GameHandler::autostoreBagItem(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag) {
    if (inventoryHandler_) inventoryHandler_->autostoreBagItem(srcBag, srcSlot, dstBag);
}

void GameHandler::useItemBySlot(int backpackIndex, bool confirmed, uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemBySlot(backpackIndex, confirmed, unitTarget);
}

void GameHandler::useKeyringItem(int index, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->useKeyringItem(index, confirmed);
}

void GameHandler::placeGlyphFromBag(uint8_t wireBag, uint8_t wireSlot, uint32_t socketIndex) {
    if (inventoryHandler_) inventoryHandler_->placeGlyphFromBag(wireBag, wireSlot, socketIndex);
}

bool GameHandler::placePendingGlyph(uint32_t socketIndex) {
    return inventoryHandler_ && inventoryHandler_->placePendingGlyph(socketIndex);
}

uint32_t GameHandler::getPendingGlyphItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingGlyphItemId() : 0;
}

void GameHandler::useItemInBag(int bagIndex, int slotIndex, bool confirmed,
                               uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemInBag(bagIndex, slotIndex, confirmed,
                                                           unitTarget);
}

bool GameHandler::isAwaitingItemTarget() const {
    return inventoryHandler_ && inventoryHandler_->isAwaitingItemTarget();
}

bool GameHandler::isAwaitingUnitTarget() const {
    return inventoryHandler_ && inventoryHandler_->isAwaitingUnitTarget();
}

uint32_t GameHandler::getPendingUnitTargetSourceItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingUnitTargetSourceItemId() : 0;
}

void GameHandler::cancelUnitTargeting() {
    if (inventoryHandler_) inventoryHandler_->cancelUnitTargeting();
}

void GameHandler::completeItemUseOnUnit(uint64_t targetUnitGuid) {
    if (inventoryHandler_) inventoryHandler_->completeItemUseOnUnit(targetUnitGuid);
}

void GameHandler::beginSpellItemTargeting(uint32_t spellId, const std::string& spellName) {
    if (inventoryHandler_) inventoryHandler_->beginSpellItemTargeting(spellId, spellName);
}

uint32_t GameHandler::getPendingItemTargetSourceItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingItemTargetSourceItemId() : 0;
}

void GameHandler::cancelItemTargeting() {
    if (inventoryHandler_) inventoryHandler_->cancelItemTargeting();
}

void GameHandler::completeItemUseOnItem(uint64_t targetItemGuid, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->completeItemUseOnItem(targetItemGuid, confirmed);
}

int GameHandler::getInstanceBootTimeRemaining() const {
    if (!instanceBootDeadline_) return 0;
    const auto left = *instanceBootDeadline_ - std::chrono::steady_clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(left).count();
    return secs > 0 ? static_cast<int>(secs) : 0;
}

void GameHandler::replaceEnchant() {
    if (inventoryHandler_) inventoryHandler_->replaceEnchant();
}

void GameHandler::openItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemBySlot(backpackIndex);
}

void GameHandler::openItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemInBag(bagIndex, slotIndex);
}

void GameHandler::readItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->readItemBySlot(backpackIndex);
}

void GameHandler::readItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->readItemInBag(bagIndex, slotIndex);
}

void GameHandler::useItemById(uint32_t itemId, uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemById(itemId, unitTarget);
}

uint32_t GameHandler::getItemIdForSpell(uint32_t spellId) const {
    if (spellId == 0) return 0;
    // Search backpack and bags for an item whose on-use spell matches
    for (int i = 0; i < inventory.getBackpackSize(); i++) {
        const auto& slot = inventory.getBackpackSlot(i);
        if (slot.empty()) continue;
        auto* info = getItemInfo(slot.item.itemId);
        if (!info || !info->valid) continue;
        for (const auto& sp : info->spells) {
            if (sp.spellId == spellId && (sp.spellTrigger == 0 || sp.spellTrigger == 5))
                return slot.item.itemId;
        }
    }
    for (int bag = 0; bag < inventory.NUM_BAG_SLOTS; bag++) {
        for (int s = 0; s < inventory.getBagSize(bag); s++) {
            const auto& slot = inventory.getBagSlot(bag, s);
            if (slot.empty()) continue;
            auto* info = getItemInfo(slot.item.itemId);
            if (!info || !info->valid) continue;
            for (const auto& sp : info->spells) {
                if (sp.spellId == spellId && (sp.spellTrigger == 0 || sp.spellTrigger == 5))
                    return slot.item.itemId;
            }
        }
    }
    return 0;
}

void GameHandler::unstuck() {
    if (unstuckCallback_) {
        unstuckCallback_();
        addSystemChatMessage("Unstuck: snapped upward. Use /unstuckgy for full teleport.");
    }
}

void GameHandler::unstuckGy() {
    if (unstuckGyCallback_) {
        unstuckGyCallback_();
        addSystemChatMessage("Unstuck: teleported to safe location.");
    }
}

void GameHandler::unstuckHearth() {
    if (unstuckHearthCallback_) {
        unstuckHearthCallback_();
        addSystemChatMessage("Unstuck: teleported to hearthstone location.");
    } else {
        addSystemChatMessage("No hearthstone bind point set.");
    }
}

// ============================================================
// Trainer
// ============================================================

void GameHandler::trainSpell(uint32_t spellId) {
    if (inventoryHandler_) inventoryHandler_->trainSpell(spellId);
}

void GameHandler::closeTrainer() {
    if (inventoryHandler_) inventoryHandler_->closeTrainer();
}

void GameHandler::preloadDBCCaches() const {
    LOG_INFO("Pre-loading DBC caches during world entry...");
    auto t0 = std::chrono::steady_clock::now();

    loadSpellNameCache();   // Spell.dbc - largest, ~170ms cold
    loadTitleNameCache();   // CharTitles.dbc
    loadFactionNameCache(); // Faction.dbc
    loadAreaNameCache();    // WorldMapArea.dbc
    loadMapNameCache();     // Map.dbc
    loadLfgDungeonDbc();    // LFGDungeons.dbc

    // Validate animation constants against AnimationData.dbc
    if (auto* am = services_.assetManager) {
        auto animDbc = am->loadDBC("AnimationData.dbc");
        rendering::anim::validateAgainstDBC(animDbc);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("DBC cache pre-load complete in ", elapsed, " ms");
}

void GameHandler::loadSpellNameCache() const {
    if (spellHandler_) spellHandler_->loadSpellNameCache();
}

void GameHandler::loadSkillLineAbilityDbc() {
    if (spellHandler_) spellHandler_->loadSkillLineAbilityDbc();
}

const std::vector<GameHandler::SpellBookTab>& GameHandler::getSpellBookTabs() {
    static const std::vector<SpellBookTab> kEmpty;
    if (spellHandler_) return spellHandler_->getSpellBookTabs();
    return kEmpty;
}


void GameHandler::loadTalentDbc() {
    if (spellHandler_) spellHandler_->loadTalentDbc();
}

static const std::string EMPTY_STRING;

const int32_t* GameHandler::getSpellEffectBasePoints(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellEffectBasePoints(spellId);
    return nullptr;
}

float GameHandler::getSpellDuration(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDuration(spellId);
    return 0.0f;
}

const std::string& GameHandler::getSpellName(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellName(spellId);
    return EMPTY_STRING;
}

uint32_t GameHandler::getSpellTargetFlags(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellTargetFlags(spellId) : 0;
}

uint32_t GameHandler::getSpellImplicitTargetA(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellImplicitTargetA(spellId) : 0;
}

bool GameHandler::isSpellKnownToClient(uint32_t spellId) const {
    return spellHandler_ && spellHandler_->isSpellKnownToClient(spellId);
}

uint32_t GameHandler::getSpellTargetAuraState(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellTargetAuraState(spellId) : 0;
}

const std::string& GameHandler::getSpellRank(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellRank(spellId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getSpellDescription(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDescription(spellId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getSpellAuraDescription(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellAuraDescription(spellId);
    return EMPTY_STRING;
}

namespace {
// Format a duration in seconds the way WoW tooltips do: "12 sec", "5 min", "1 hour".
std::string formatSpellDurationSec(float sec) {
    if (sec <= 0.0f) return {};
    auto trim = [](float v) {
        char buf[32];
        if (v == static_cast<float>(static_cast<long>(v)))
            snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
        else
            snprintf(buf, sizeof(buf), "%.1f", v);
        return std::string(buf);
    };
    if (sec >= 3600.0f && std::fmod(sec, 3600.0f) == 0.0f) {
        float h = sec / 3600.0f;
        return trim(h) + (h == 1.0f ? " hour" : " hours");
    }
    if (sec >= 60.0f) return trim(sec / 60.0f) + " min";
    return trim(sec) + " sec";
}
} // namespace

/// The evaluator's window onto this client: what a magnitude is, and which
/// spells the player knows.
SpellDescriptionContext GameHandler::descriptionContext(
        uint32_t spellId, const std::string* declarations) const {
    SpellDescriptionContext ctx;
    ctx.declarations = declarations;
    ctx.magnitude = [this, spellId](int index, double& out) {
        const int32_t* bp = getSpellEffectBasePoints(spellId);
        if (!bp || index < 0 || index > 2) return false;
        out = std::abs(static_cast<double>(bp[index]) + 1.0);
        return true;
    };
    ctx.knowsSpell = [this](uint32_t id) { return isSpellKnownToClient(id); };
    return ctx;
}

std::string GameHandler::formatSpellDescription(uint32_t selfSpellId,
                                                const std::string& raw) const {
    if (raw.empty()) return raw;

    std::string out;
    out.reserve(raw.size());
    const size_t n = raw.size();
    long lastValue = -1;  // drives $l plural selection

    // Resolve a base-point magnitude ($s/$o/$m/$M) for a spell + effect index.
    auto basePoints = [&](uint32_t sid, int idx) -> long {
        const int32_t* bp = getSpellEffectBasePoints(sid);
        if (!bp || idx < 0 || idx > 2) return LONG_MIN;
        return std::labs(static_cast<long>(bp[idx]) + 1);  // stored as value-1
    };

    for (size_t i = 0; i < n; ) {
        char c = raw[i];
        if (c != '$') { out += c; ++i; continue; }
        if (i + 1 >= n) { out += c; ++i; continue; }

        char next = raw[i + 1];

        // Plural "$lsingular:plural;" / gender "$gmale:female;" - emit one branch.
        if (next == 'l' || next == 'L' || next == 'g' || next == 'G') {
            size_t semi = raw.find(';', i + 2);
            if (semi != std::string::npos) {
                std::string body = raw.substr(i + 2, semi - (i + 2));
                size_t colon = body.find(':');
                std::string a = (colon == std::string::npos) ? body : body.substr(0, colon);
                std::string b = (colon == std::string::npos) ? body : body.substr(colon + 1);
                bool plural = (next == 'l' || next == 'L') ? (lastValue != 1) : false;
                out += plural ? b : a;
                i = semi + 1;
                continue;
            }
        }

        // A named variable "$<percent>", declared in this spell's row of
        // SpellDescriptionVariables.dbc. Nothing read that table, and the
        // parser had no branch for '<' - so it dropped the "$<" as an unknown
        // token and left "percent>" sitting in the sentence.
        if (next == '<') {
            const size_t close = raw.find('>', i + 2);
            if (close != std::string::npos) {
                const std::string name = raw.substr(i + 2, close - (i + 2));
                i = close + 1;

                const auto& cache = const_cast<GameHandler*>(this)->spellNameCacheRef();
                const auto entry = cache.find(selfSpellId);
                const std::string* decls = nullptr;
                if (entry != cache.end() && entry->second.descriptionVariableId != 0) {
                    auto& table = const_cast<GameHandler*>(this)->spellDescriptionVariablesRef();
                    const auto row = table.find(entry->second.descriptionVariableId);
                    if (row != table.end()) decls = &row->second;
                }

                const SpellDescriptionContext ctx = descriptionContext(selfSpellId, decls);
                std::string decl;
                double v = 0.0;
                if (decls && spellDeclarationOf(ctx, name, decl) &&
                    evaluateSpellExpression(decl, ctx, v)) {
                    const long rounded = std::lround(v);
                    out += std::to_string(rounded);
                    lastValue = rounded;
                } else if (i < n && raw[i] == '%') {
                    // Unresolvable: drop the token and the percent sign it
                    // qualified, rather than leave a bare "%" behind.
                    ++i;
                }
                continue;
            }
        }

        // Bracketed math expression "${...}" - evaluate where it can be, strip
        // it (and a trailing %) where it cannot.
        if (next == '{') {
            size_t close = raw.find('}', i + 2);
            if (close != std::string::npos) {
                const auto& cache = const_cast<GameHandler*>(this)->spellNameCacheRef();
                const auto entry = cache.find(selfSpellId);
                const std::string* decls = nullptr;
                if (entry != cache.end() && entry->second.descriptionVariableId != 0) {
                    auto& table = const_cast<GameHandler*>(this)->spellDescriptionVariablesRef();
                    const auto row = table.find(entry->second.descriptionVariableId);
                    if (row != table.end()) decls = &row->second;
                }
                const SpellDescriptionContext ctx = descriptionContext(selfSpellId, decls);
                double v = 0.0;
                if (evaluateSpellExpression(raw.substr(i + 2, close - (i + 2)), ctx, v)) {
                    const long rounded = std::lround(v);
                    out += std::to_string(rounded);
                    lastValue = rounded;
                    i = close + 1;
                    continue;
                }
                i = close + 1;
                if (i < n && raw[i] == '%') ++i;
                continue;
            }
        }

        // Division token "$/N;<valueToken>" - e.g. "$/5;s1" is "s1 divided by 5"
        // (food eat spells: "Restores $/5;s1 health per second").
        if (next == '/') {
            size_t k = i + 2;
            long divisor = 0;
            while (k < n && raw[k] >= '0' && raw[k] <= '9') { divisor = divisor * 10 + (raw[k] - '0'); ++k; }
            if (k < n && raw[k] == ';' && divisor != 0) {
                ++k;  // past ';'
                uint32_t rid = 0; bool hr = false;
                while (k < n && raw[k] >= '0' && raw[k] <= '9') { rid = rid * 10 + (raw[k] - '0'); hr = true; ++k; }
                if (k < n) {
                    char cd = raw[k++];
                    int ix = 0;
                    if (k < n && raw[k] >= '1' && raw[k] <= '3') { ix = raw[k] - '1'; ++k; }
                    if (cd == 's' || cd == 'S' || cd == 'o' || cd == 'O' || cd == 'm' || cd == 'M') {
                        long v = basePoints(hr ? rid : selfSpellId, ix);
                        if (v != LONG_MIN) {
                            long dv = v / divisor;
                            out += std::to_string(dv);
                            lastValue = dv;
                            i = k;
                            continue;
                        }
                    }
                }
            }
            // Unparseable division token - strip through the terminating ';' if present.
            size_t semi = raw.find(';', i + 2);
            i = (semi != std::string::npos) ? semi + 1 : i + 2;
            if (i < n && raw[i] == '%') ++i;
            continue;
        }

        // General token: $ [spellId] letter [effectIndex]
        size_t j = i + 1;
        uint32_t refId = 0;
        bool hasRef = false;
        while (j < n && raw[j] >= '0' && raw[j] <= '9') {
            refId = refId * 10 + static_cast<uint32_t>(raw[j] - '0');
            hasRef = true;
            ++j;
        }
        if (j >= n) { out += c; ++i; continue; }
        char code = raw[j++];
        int idx = 0;
        if (j < n && raw[j] >= '1' && raw[j] <= '3') { idx = raw[j] - '1'; ++j; }
        uint32_t target = hasRef ? refId : selfSpellId;

        bool resolved = false;
        switch (code) {
            case 's': case 'S': case 'o': case 'O':
            case 'm': case 'M': {
                long v = basePoints(target, idx);
                if (v != LONG_MIN) { out += std::to_string(v); lastValue = v; resolved = true; }
                break;
            }
            case 'd': {
                std::string dur = formatSpellDurationSec(getSpellDuration(target));
                if (!dur.empty()) { out += dur; resolved = true; }
                break;
            }
            case 'z': {
                // Where the hearthstone sends you. Spell.dbc says "Returns you
                // to $z.", and with the token dropped the tooltip read
                // "Returns you to ." - which is the one fact the line exists
                // to carry. The bind zone comes from SMSG_BINDPOINTUPDATE; the
                // continent is a poor second but beats a blank.
                std::string home = getAreaName(getHomeBindZoneId());
                if (home.empty()) {
                    uint32_t bindMap = 0;
                    glm::vec3 bindPos(0.0f);
                    if (getHomeBind(bindMap, bindPos)) home = getMapName(bindMap);
                }
                if (!home.empty()) { out += home; resolved = true; }
                break;
            }
            default: break;  // $h proc chance, $t period, $a radius, ... - not resolvable here
        }

        if (resolved) {
            i = j;
        } else {
            // Drop the whole token; a value slot immediately followed by '%' drops that too,
            // so we don't leave a dangling "%".
            i = j;
            if (i < n && raw[i] == '%') ++i;
        }
    }

    // Collapse whitespace left behind by stripped tokens ("a  chance" -> "a chance").
    std::string cleaned;
    cleaned.reserve(out.size());
    bool prevSpace = false;
    for (char c : out) {
        bool isSpace = (c == ' ');
        if (isSpace && prevSpace) continue;
        // A token that could not be resolved leaves the space in front of it
        // sitting against the punctuation that followed it - "Returns you to ."
        // Drop that space too, so an unresolved token reads as a gap in the
        // sentence rather than as a typo.
        if (!cleaned.empty() && prevSpace &&
            (c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ')')) {
            cleaned.pop_back();
        }
        cleaned += c;
        prevSpace = isSpace;
    }
    return cleaned;
}

const std::string& GameHandler::getSpellFocusName(uint32_t focusId) const {
    if (spellHandler_) return spellHandler_->getSpellFocusName(focusId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getTotemCategoryName(uint32_t categoryId) const {
    if (spellHandler_) return spellHandler_->getTotemCategoryName(categoryId);
    return EMPTY_STRING;
}

std::string GameHandler::getEnchantName(uint32_t enchantId) const {
    if (spellHandler_) return spellHandler_->getEnchantName(enchantId);
    return {};
}

uint32_t GameHandler::getEnchantGemItem(uint32_t enchantId) const {
    if (spellHandler_) return spellHandler_->getEnchantGemItem(enchantId);
    return 0;
}

uint8_t GameHandler::getSpellDispelType(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDispelType(spellId);
    return 0;
}

bool GameHandler::isSpellInterruptible(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->isSpellInterruptible(spellId);
    return true;
}

bool GameHandler::isSpellPassive(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->isSpellPassive(spellId);
    return false;
}

uint32_t GameHandler::getSpellSchoolMask(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellSchoolMask(spellId);
    return 0;
}

const std::string& GameHandler::getSkillLineName(uint32_t skillLineId) const {
    if (spellHandler_) return spellHandler_->getSkillLineName(skillLineId);
    return EMPTY_STRING;
}


} // namespace game
} // namespace wowee
