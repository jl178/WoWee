#include "game/group_defines.hpp"
#include "core/local_time.hpp"
#include "game/item_text.hpp"
#include "game/global_strings.hpp"
#include "game/combat_log_event.hpp"
#include "game/stock_combat_log.hpp"
#include "game/social_handler.hpp"
#include "game/chat_social_rules.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/guild_roster_throttle.hpp"
#include "game/spell_handler.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "audio/audio_coordinator.hpp"
#include "core/coordinates.hpp"
#include "audio/ui_sound_manager.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>


namespace wowee {
namespace game {

namespace {

std::filesystem::path guildNameCachePath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "wowee" / "guild_names.tsv";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".wowee" / "guild_names.tsv";
    }
#endif
    return std::filesystem::path("guild_names.tsv");
}

} // namespace




// LFG join result codes from LFGJoinResult enum (WotLK 3.3.5a).
// Case 0 = success (no error message needed), returns nullptr so the caller
// knows not to display an error string.
// A uint32 on the wire, so a uint32 here - narrowing it to a byte would
// wrap a large value into a case that means something else.
static const char* lfgJoinResultString(uint32_t result) {
    switch (result) {
        case 0:  return nullptr;  // LFG_JOIN_OK
        case 1:  return "Role check failed.";
        case 2:  return "No LFG slots available for your group.";
        case 3:  return "No LFG object found.";
        case 4:  return "No slots available (player).";
        case 5:  return "No slots available (party).";
        case 6:  return "Dungeon requirements not met by all members.";
        case 7:  return "Party members are from different realms.";
        case 8:  return "Not all members are present.";
        case 9:  return "Get info timeout.";
        case 10: return "Invalid dungeon slot.";
        case 11: return "You are marked as a deserter.";
        case 12: return "A party member is marked as a deserter.";
        case 13: return "You are on a random dungeon cooldown.";
        case 14: return "A party member is on a random dungeon cooldown.";
        case 15: return "Cannot join dungeon finder.";  // LFG_JOIN_INTERNAL_ERROR
        case 16: return "No spec/role available.";
        default: return "Cannot join dungeon finder.";
    }
}

static bool parseInspectEquipmentPayload(network::Packet& packet,
                                         uint64_t& outGuid,
                                         std::array<uint32_t, 19>& outItems,
                                         std::array<uint16_t, 19>& outEnchants) {
    const size_t start = packet.getReadPos();
    constexpr size_t kGearBytes = 19 * sizeof(uint32_t);
    constexpr uint32_t kEquipmentSlotMask = (1u << 19) - 1u;

    auto reset = [&]() {
        packet.setReadPos(start);
        outGuid = 0;
        outItems.fill(0);
        outEnchants.fill(0);
    };

    auto countSlots = [](uint32_t mask) {
        int count = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if (mask & (1u << slot)) ++count;
        }
        return count;
    };

    auto parseMaskedItems = [&](const char* guidEncoding) -> bool {
        if (!packet.hasRemaining(sizeof(uint32_t))) return false;
        const uint32_t slotMask = packet.readUInt32();
        if ((slotMask & ~kEquipmentSlotMask) != 0) return false;

        const int slotCount = countSlots(slotMask);
        if (slotCount <= 0) return false;

        const size_t remaining = packet.getRemainingSize();
        const size_t candidateRecordSizes[] = {24, 20, 16, 12, 8, 4};
        size_t recordSize = 0;
        for (size_t candidate : candidateRecordSizes) {
            if (remaining == static_cast<size_t>(slotCount) * candidate) {
                recordSize = candidate;
                break;
            }
        }
        if (recordSize == 0) {
            // Some cores append a tiny trailer. Prefer the largest plausible
            // record that fits cleanly, but do not consume packets that are
            // clearly not the masked inspect format.
            for (size_t candidate : candidateRecordSizes) {
                const size_t needed = static_cast<size_t>(slotCount) * candidate;
                if (remaining >= needed && remaining - needed <= 8) {
                    recordSize = candidate;
                    break;
                }
            }
        }
        if (recordSize < sizeof(uint32_t)) return false;

        int nonZero = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if ((slotMask & (1u << slot)) == 0) continue;
            if (!packet.hasRemaining(recordSize)) return false;

            const size_t recordStart = packet.getReadPos();
            const uint32_t itemEntry = packet.readUInt32();
            uint16_t enchantId = 0;
            if (recordSize >= sizeof(uint32_t) + sizeof(uint16_t) &&
                packet.hasRemaining(sizeof(uint16_t))) {
                enchantId = packet.readUInt16();
            }
            packet.setReadPos(recordStart + recordSize);

            outItems[slot] = itemEntry;
            outEnchants[slot] = enchantId;
            if (itemEntry != 0) ++nonZero;
        }

        if (nonZero == 0) return false;
        LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE masked gear: guidEncoding=", guidEncoding,
                 " mask=0x", std::hex, slotMask, std::dec,
                 " slots=", slotCount, " recordSize=", recordSize,
                 " nonZero=", nonZero);
        return true;
    };

    auto readItems = [&]() -> bool {
        for (uint32_t& item : outItems) {
            if (!packet.hasRemaining(sizeof(uint32_t))) return false;
            item = packet.readUInt32();
        }
        return true;
    };

    reset();
    if (packet.hasRemaining(sizeof(uint64_t))) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && parseMaskedItems("uint64")) {
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && parseMaskedItems("packed")) {
            return true;
        }
    }

    reset();
    if (packet.getRemainingSize() >= sizeof(uint64_t) + kGearBytes) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=uint64 slots=19");
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && packet.getRemainingSize() >= kGearBytes && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=packed slots=19");
            return true;
        }
    }

    reset();
    return false;
}

static bool isTbcInspectTalentBitSet(const std::vector<uint8_t>& bitfield, uint32_t bitIndex) {
    const uint32_t slot = bitIndex / 7u;
    const uint32_t offset = bitIndex % 7u;
    return slot < bitfield.size() && (bitfield[slot] & (1u << offset)) != 0;
}

static bool decodeTbcInspectTalentBitfield(GameHandler& owner,
                                           uint8_t classId,
                                           const std::vector<uint8_t>& bitfield,
                                           std::array<uint32_t, 3>& outTreePoints,
                                           uint32_t& outSpentTalents) {
    outTreePoints.fill(0);
    outSpentTalents = 0;
    if (classId == 0 || bitfield.empty()) return false;

    owner.loadTalentDbc();

    const uint32_t classMask = 1u << (classId - 1u);
    std::vector<const TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : owner.getAllTalentTabs()) {
        if (tab.classMask & classMask) {
            classTabs.push_back(&tab);
        }
    }
    std::sort(classTabs.begin(), classTabs.end(),
              [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (classTabs.empty()) return false;

    uint32_t tabBitStart = 0;
    for (size_t tabIndex = 0; tabIndex < classTabs.size() && tabIndex < outTreePoints.size(); ++tabIndex) {
        std::vector<const TalentEntry*> talents;
        for (const auto& [talentId, talent] : owner.getAllTalents()) {
            if (talent.tabId == classTabs[tabIndex]->tabId && talent.maxRank > 0) {
                talents.push_back(&talent);
            }
        }
        std::sort(talents.begin(), talents.end(), [](const auto* a, const auto* b) {
            if (a->row != b->row) return a->row < b->row;
            if (a->column != b->column) return a->column < b->column;
            return a->talentId < b->talentId;
        });

        uint32_t tabBits = 0;
        for (const auto* talent : talents) {
            uint8_t rank = 0;
            for (uint8_t r = 1; r <= talent->maxRank; ++r) {
                if (isTbcInspectTalentBitSet(bitfield, tabBitStart + tabBits + (r - 1u))) {
                    rank = r;
                }
            }
            outTreePoints[tabIndex] += rank;
            outSpentTalents += rank;
            tabBits += talent->maxRank;
        }
        tabBitStart += tabBits;
    }

    return true;
}

static bool parseTbcInspectTalentPayload(network::Packet& packet,
                                         GameHandler& owner,
                                         uint8_t classId,
                                         uint32_t& outUnspentTalents,
                                         uint32_t& outSpentTalents,
                                         std::array<uint32_t, 3>& outTreePoints,
                                         bool& outHasTreePoints) {
    const size_t start = packet.getReadPos();
    outUnspentTalents = 0;
    outSpentTalents = 0;
    outTreePoints.fill(0);
    outHasTreePoints = false;

    const size_t payloadSize = packet.getRemainingSize();
    if (payloadSize < sizeof(uint32_t)) {
        packet.setReadPos(start);
        return false;
    }

    auto parseTalentRecords = [](network::Packet& p, size_t count) {
        uint32_t spent = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t talentId = p.readUInt32();
            const uint8_t rank = p.readUInt8();
            if (talentId == 0) continue;
            spent += static_cast<uint32_t>(rank) + 1u;
        }
        return spent;
    };

    const uint32_t firstValue = packet.readUInt32();

    // TBC/CMaNGOS sends SMSG_INSPECT_TALENT as:
    //   uint32 byteCount, byte[byteCount] compact talent bitfield.
    // Older attempts treated byteCount (0x3d) as talent points; keep the
    // fallback list-style parser below for other 2.x cores, but prefer this
    // server-authored bitfield when the size lines up exactly.
    if (firstValue > 0 && firstValue <= 256 && packet.getRemainingSize() == firstValue) {
        std::vector<uint8_t> bitfield;
        bitfield.reserve(firstValue);
        for (uint32_t i = 0; i < firstValue; ++i) {
            bitfield.push_back(packet.readUInt8());
        }

        outHasTreePoints = decodeTbcInspectTalentBitfield(owner, classId, bitfield, outTreePoints, outSpentTalents);
        if (!outHasTreePoints) {
            outSpentTalents = 0;
        }
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos bitfield bytes=", firstValue,
                 " spent=", outSpentTalents,
                 " trees=", outTreePoints[0], "/", outTreePoints[1], "/", outTreePoints[2],
                 " class=", static_cast<int>(classId),
                 " decoded=", (outHasTreePoints ? "yes" : "no"),
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    // The regular talents-info packet uses uint32 unspent + uint8 count.
    // Accept that shape too so different 2.x cores still render something useful.
    if (packet.hasRemaining(1)) {
        const uint8_t nextByte = packet.readUInt8();
        const size_t afterHeaderBytes = packet.getRemainingSize();

        if (afterHeaderBytes > 0 && (afterHeaderBytes % 5u) == 0u) {
            const size_t recordCount = afterHeaderBytes / 5u;
            const uint32_t rankSpent = parseTalentRecords(packet, recordCount);
            outUnspentTalents = 0;
            outSpentTalents = firstValue != 0 ? firstValue : rankSpent;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos talents records=", recordCount,
                     " spent=", outSpentTalents, " rankSpent=", rankSpent,
                     " unk=", static_cast<int>(nextByte),
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }

        const uint8_t talentCount = nextByte;
        if (packet.hasRemaining(static_cast<size_t>(talentCount) * 5u)) {
            const uint32_t spentTalents = parseTalentRecords(packet, talentCount);
            outUnspentTalents = firstValue;
            outSpentTalents = spentTalents;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed talents count=", static_cast<int>(talentCount),
                     " spent=", spentTalents, " unspent=", firstValue,
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }
    }

    if ((payloadSize % 5u) == 0u) {
        packet.setReadPos(start);
        const size_t recordCount = payloadSize / 5u;
        outSpentTalents = parseTalentRecords(packet, recordCount);
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed bare talents records=", recordCount,
                 " spent=", outSpentTalents,
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    packet.setReadPos(start);
    return false;
}

static const std::string kEmptyString;

SocialHandler::SocialHandler(GameHandler& owner)
    : owner_(owner) {
    loadGuildNameCache();
}

void SocialHandler::loadGuildNameCache() {
    std::ifstream in(guildNameCachePath());
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('\t');
        if (separator == std::string::npos) continue;

        try {
            const uint32_t guildId = static_cast<uint32_t>(std::stoul(line.substr(0, separator)));
            std::string guildName = line.substr(separator + 1);
            if (guildId != 0 && !guildName.empty()) {
                guildNameCache_[guildId] = std::move(guildName);
            }
        } catch (...) {
            // Ignore stale or hand-edited cache lines.
        }
    }
}

void SocialHandler::saveGuildNameCache() const {
    const std::filesystem::path path = guildNameCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save guild name cache to ", path.string());
        return;
    }

    for (const auto& [guildId, guildName] : guildNameCache_) {
        if (guildId != 0 && !guildName.empty()) {
            out << guildId << '\t' << guildName << '\n';
        }
    }
}

void SocialHandler::rememberGuildName(uint32_t guildId, const std::string& guildName) {
    if (guildId == 0 || guildName.empty()) return;

    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end() && it->second == guildName) return;

    guildNameCache_[guildId] = guildName;
    saveGuildNameCache();
}

// ============================================================
// registerOpcodes
// ============================================================

void SocialHandler::registerOpcodes(DispatchTable& table) {
    // ---- Player info queries / social ----
    table[Opcode::SMSG_QUERY_TIME_RESPONSE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleQueryTimeResponse(packet);
    };
    table[Opcode::SMSG_PLAYED_TIME] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handlePlayedTime(packet);
    };
    table[Opcode::SMSG_WHO] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleWho(packet);
    };
    table[Opcode::SMSG_WHOIS] = [](network::Packet& packet) {
        // The GM whois answer goes to the console, not the chat frame:
        // Wow.exe's handler (0x6B3280) hands the text to the console writer
        // (0x765270) and nothing else.
        if (packet.getReadPos() < packet.getSize()) {
            const std::string whoisText = packet.readString();
            if (!whoisText.empty()) LOG_INFO("[console] SMSG_WHOIS: ", whoisText);
        }
    };
    table[Opcode::SMSG_FRIEND_STATUS] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleFriendStatus(packet);
    };
    table[Opcode::SMSG_CONTACT_LIST] = [this](network::Packet& packet) { handleContactList(packet); };
    table[Opcode::SMSG_FRIEND_LIST] = [this](network::Packet& packet) { handleFriendList(packet); };
    table[Opcode::SMSG_IGNORE_LIST] = [this](network::Packet& packet) {
        // Format: uint8 count + count × uint64 guid (no name strings in packet).
        // Names are resolved via SMSG_NAME_QUERY_RESPONSE after the list arrives.
        if (!packet.hasRemaining(1)) return;
        uint8_t ignCount = packet.readUInt8();
        owner_.ignoreListGuidsRef().clear();
        for (uint8_t i = 0; i < ignCount; ++i) {
            if (!packet.hasRemaining(8)) break;
            uint64_t ignGuid = packet.readUInt64();
            if (ignGuid != 0) {
                owner_.ignoreListGuidsRef().insert(ignGuid);
                // Query name so UI can display it later
                owner_.queryPlayerName(ignGuid);
            }
        }
        LOG_DEBUG("SMSG_IGNORE_LIST: loaded ", (int)ignCount, " ignored players");
    };
    table[Opcode::MSG_RANDOM_ROLL] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleRandomRoll(packet);
    };

    // ---- Logout ----
    table[Opcode::SMSG_LOGOUT_RESPONSE] = [this](network::Packet& packet) { handleLogoutResponse(packet); };
    table[Opcode::SMSG_LOGOUT_COMPLETE] = [this](network::Packet& packet) { handleLogoutComplete(packet); };

    // ---- Inspect ----
    table[Opcode::SMSG_INSPECT_TALENT] = [this](network::Packet& packet) { handleInspectResults(packet); };
    // The honour tab's own answer, on the same opcode it was asked with.
    // AzerothCore replies with the target's guid, the honour byte, the packed
    // kill counter and three totals - MiscHandler.cpp, HandleInspectHonorStats.
    table[Opcode::MSG_INSPECT_HONOR_STATS] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8 + 1 + 16)) return;
        const uint64_t guid = packet.readUInt64();
        // Only for whoever is being inspected. The window reads one result and
        // a stale answer for someone else would be attributed to them.
        if (inspectResult_.guid != 0 && guid != inspectResult_.guid) return;
        inspectResult_.honorRank = packet.readUInt8();
        // One field holds both days: PAIR32_LOPART is today and the high half
        // is yesterday, which is how the server rolls the counter over -
        // MAKE_PAIR32(0, kills_today) at midnight.
        const uint32_t kills = packet.readUInt32();
        inspectResult_.honorTodayKills     = kills & 0xFFFFu;
        inspectResult_.honorYesterdayKills = kills >> 16;
        inspectResult_.honorTodayContribution     = packet.readUInt32();
        inspectResult_.honorYesterdayContribution = packet.readUInt32();
        inspectResult_.honorLifetimeKills         = packet.readUInt32();
        inspectResult_.hasHonorData = true;
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("INSPECT_HONOR_UPDATE", {});
    };
    table[Opcode::SMSG_INSPECT_RESULTS_UPDATE] = [this](network::Packet& packet) { handleInspectResults(packet); };
    table[Opcode::MSG_INSPECT_ARENA_TEAMS] = [this](network::Packet& packet) { handleInspectArenaTeams(packet); };
    table[Opcode::SMSG_ARENA_UNIT_DESTROYED] = [this](network::Packet& packet) { handleArenaUnitDestroyed(packet); };

    // ---- Group ----
    table[Opcode::SMSG_GROUP_INVITE] = [this](network::Packet& packet) { handleGroupInvite(packet); };
    table[Opcode::SMSG_GROUP_DECLINE] = [this](network::Packet& packet) { handleGroupDecline(packet); };
    table[Opcode::SMSG_GROUP_LIST] = [this](network::Packet& packet) { handleGroupList(packet); };
    table[Opcode::SMSG_GROUP_DESTROYED] = [this](network::Packet& /*packet*/) {
        const bool wasRaid = !partyData.isEmpty() && groupIsRaid(partyData.groupType);
        // All of it, the group type included: a raid's type left behind kept
        // groupIsRaid answering yes for a group that no longer exists.
        partyData = GroupListData{};
        // ERR_GROUP_DISBANDED, once, as the system line it is.
        owner_.displayGameMessage("ERR_GROUP_DISBANDED", "Your group has been disbanded.");
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
            owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
            if (wasRaid) owner_.addonEventCallbackRef()("RAID_ROSTER_UPDATE", {});
        }
    };
    // The invite this player was holding was withdrawn. AzerothCore declares
    // the opcode and never sends it; a core that does means the invite is
    // gone. uiparent.lua hides the PARTY_INVITE popup on PARTY_INVITE_CANCEL;
    // without it the popup stayed up offering to accept an invite the server
    // had already forgotten.
    table[Opcode::SMSG_GROUP_CANCEL] = [this](network::Packet& /*packet*/) {
        pendingGroupInvite = false;
        pendingInviterName.clear();
        // The event alone (Wow.exe 0x6CBD10 -> 0x512560): no line.
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PARTY_INVITE_CANCEL", {});
    };
    table[Opcode::SMSG_GROUP_UNINVITE] = [this](network::Packet& packet) { handleGroupUninvite(packet); };
    table[Opcode::SMSG_PARTY_COMMAND_RESULT] = [this](network::Packet& packet) { handlePartyCommandResult(packet); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS] = [this](network::Packet& packet) { handlePartyMemberStats(packet, false); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS_FULL] = [this](network::Packet& packet) { handlePartyMemberStats(packet, true); };

    // ---- Ready check ----
    table[Opcode::MSG_RAID_READY_CHECK] = [this](network::Packet& packet) {
        pendingReadyCheck_ = true;
        readyCheckActive_ = true;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckInitiator_.clear();
        readyCheckResults_.clear();
        uint64_t initiatorGuid = 0;
        if (packet.hasRemaining(8)) {
            initiatorGuid = packet.readUInt64();
            auto entity = owner_.getEntityManager().getEntity(initiatorGuid);
            if (auto* unit = dynamic_cast<Unit*>(entity.get()))
                readyCheckInitiator_ = unit->getName();
            if (readyCheckInitiator_.empty()) {
                for (const auto& member : partyData.members) {
                    if (member.guid == initiatorGuid) { readyCheckInitiator_ = member.name; break; }
                }
            }
        }
        if (readyCheckInitiator_.empty() && partyData.leaderGuid != 0) {
            for (const auto& member : partyData.members) {
                if (member.guid == partyData.leaderGuid) { readyCheckInitiator_ = member.name; break; }
            }
        }
        // HandleRaidReadyCheckOpcode broadcasts the check to the whole group,
        // whoever started it included. The one who started it is not asked:
        // ShowReadyCheck hides the listener when UnitIsUnit("player",
        // initiator), and they count as ready - nothing else could answer for
        // them, and the check would otherwise wait on them and mark them AFK.
        //
        // Stock (Wow.exe 0x574d50, 0x5747d0, 0x574180): the check runs thirty
        // seconds from this packet. The initiator sends no answer - their own
        // client counts them ready (GetReadyCheckStatus answers "ready" for
        // the initiator's guid) and plays READY_CHECK_START; everyone else
        // gets ERR_RAID_LEADER_READY_CHECK_START_S in the info frame. Then
        // READY_CHECK(initiator, secondsLeft).
        const bool selfStarted = initiatorGuid != 0 && initiatorGuid == owner_.getPlayerGuid();
        readyCheckInitiatorGuid_ = initiatorGuid;
        readyCheckSecondsLeft_ = 30.0f;
        if (selfStarted) {
            pendingReadyCheck_ = false;
            ReadyCheckResult self{ .name = std::string(), .ready = true };
            self.guid = initiatorGuid;
            readyCheckResults_.push_back(std::move(self));
        } else if (!readyCheckInitiator_.empty()) {
            owner_.displayGameMessage("ERR_RAID_LEADER_READY_CHECK_START_S",
                readyCheckInitiator_ + " has initiated a ready check.", {readyCheckInitiator_});
        }
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("READY_CHECK", {readyCheckInitiator_, "30"});
    };
    table[Opcode::MSG_RAID_READY_CHECK_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        uint64_t respGuid = packet.readUInt64();
        uint8_t  isReady  = packet.readUInt8();
        if (isReady) ++readyCheckReadyCount_; else ++readyCheckNotReadyCount_;
        auto nit = owner_.getPlayerNameCache().find(respGuid);
        std::string rname;
        if (nit != owner_.getPlayerNameCache().end()) rname = nit->second;
        else {
            // Only cast to Unit if the entity actually is one - a raw
            // static_pointer_cast on a GameObject would be undefined behavior.
            auto ent = owner_.getEntityManager().getEntity(respGuid);
            if (ent && (ent->getType() == ObjectType::UNIT || ent->getType() == ObjectType::PLAYER))
                rname = std::static_pointer_cast<game::Unit>(ent)->getName();
        }
        {
            bool found = false;
            for (auto& r : readyCheckResults_) {
                if (r.guid == respGuid) { r.ready = (isReady != 0); found = true; break; }
            }
            if (!found) {
                ReadyCheckResult r{ .name = rname, .ready = isReady != 0 };
                r.guid = respGuid;
                readyCheckResults_.push_back(std::move(r));
            }
        }
        // READY_CHECK_CONFIRM(unit, ready): the responder's unit token and 1
        // for ready, nothing for not - ReadyCheck_Confirm tests `ready == 1`.
        // It was a guid string, which no frame could match to a unit.
        if (owner_.addonEventCallbackRef()) {
            const std::string unit = groupUnitToken(partyData, owner_.getPlayerGuid(), respGuid);
            if (!unit.empty()) {
                if (isReady) owner_.addonEventCallbackRef()("READY_CHECK_CONFIRM", {unit, "1"});
                else         owner_.addonEventCallbackRef()("READY_CHECK_CONFIRM", {unit});
            }
        }
    };
    table[Opcode::MSG_RAID_READY_CHECK_FINISHED] = [this](network::Packet& /*packet*/) {
        pendingReadyCheck_ = false;
        readyCheckSecondsLeft_ = 0.0f;
        readyCheckInitiatorGuid_ = 0;
        readyCheckActive_ = false;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckResults_.clear();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("READY_CHECK_FINISHED", {});
    };
    table[Opcode::SMSG_RAID_INSTANCE_INFO] = [this](network::Packet& packet) { handleRaidInstanceInfo(packet); };

    // ---- Duels ----
    table[Opcode::SMSG_DUEL_REQUESTED] = [this](network::Packet& packet) { handleDuelRequested(packet); };
    table[Opcode::SMSG_DUEL_COMPLETE] = [this](network::Packet& packet) { handleDuelComplete(packet); };
    table[Opcode::SMSG_DUEL_WINNER] = [this](network::Packet& packet) { handleDuelWinner(packet); };
    // 0x5CF910 / 0x5CF930: the events alone. UIParent shows and hides the
    // DUEL_OUTOFBOUNDS popup, whose ten-second timer is the forfeit warning;
    // the client itself says nothing.
    table[Opcode::SMSG_DUEL_OUTOFBOUNDS] = [this](network::Packet& /*packet*/) {
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_OUTOFBOUNDS", {});
    };
    table[Opcode::SMSG_DUEL_INBOUNDS] = [this](network::Packet& /*packet*/) {
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_INBOUNDS", {});
    };
    // 0x5CFA50: the whole seconds, counted down in chat by 0x5CF870
    // ("Duel starting: 3", "2", "1", a second apart). No banner of its own.
    table[Opcode::SMSG_DUEL_COUNTDOWN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        duelCountdownCarryMs_ = 0.0f;
        sayDuelCountdown(duelCountdown_.start(packet.readUInt32()));
    };
    table[Opcode::SMSG_PARTYKILLLOG] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        uint64_t killerGuid = packet.readUInt64();
        uint64_t victimGuid = packet.readUInt64();
        auto nameFor = [this](uint64_t g) -> std::string {
            auto nit = owner_.getPlayerNameCache().find(g);
            if (nit != owner_.getPlayerNameCache().end()) return nit->second;
            auto ent = owner_.getEntityManager().getEntity(g);
            if (ent && (ent->getType() == game::ObjectType::UNIT ||
                        ent->getType() == game::ObjectType::PLAYER))
                return std::static_pointer_cast<game::Unit>(ent)->getName();
            return {};
        };
        std::string killerName = nameFor(killerGuid);
        std::string victimName = nameFor(victimGuid);
        // A combat log event, PARTY_KILL, with the killer as source and the
        // victim as destination (0x750C90) - the combat log window and addons
        // read it there. It was printed as a system chat line ("X killed Y."),
        // which the stock client never shows.
        (void)killerName; (void)victimName;
        if (auto* combat = owner_.getCombatHandler(); combat && combat->stockCombatLogActive()) {
            stock_combat_log::Record r;
            r.event = stock_combat_log::PARTY_KILL;
            r.src = killerGuid;
            r.dst = victimGuid;
            combat->logCombatEvent(r);
        } else if (owner_.addonEventCallbackRef() && !isClassicLikeExpansion()) {
            using namespace combat_log;
            const uint64_t me = owner_.getPlayerGuid(), pet = owner_.getPetGuid();
            const std::vector<std::string> args = {
                std::to_string(static_cast<int64_t>(std::time(nullptr))), "PARTY_KILL",
                guidText(killerGuid), textOrNil(killerName),
                std::to_string(unitFlags(killerGuid, me, pet)),
                guidText(victimGuid), textOrNil(victimName),
                std::to_string(unitFlags(victimGuid, me, pet)),
            };
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", args);
        }
    };

    // ---- Guild ----
    table[Opcode::SMSG_GUILD_INFO] = [this](network::Packet& packet) { handleGuildInfo(packet); };
    table[Opcode::SMSG_GUILD_ROSTER] = [this](network::Packet& packet) { handleGuildRoster(packet); };
    table[Opcode::SMSG_GUILD_QUERY_RESPONSE] = [this](network::Packet& packet) { handleGuildQueryResponse(packet); };
    table[Opcode::SMSG_GUILD_EVENT] = [this](network::Packet& packet) { handleGuildEvent(packet); };
    table[Opcode::SMSG_GUILD_INVITE] = [this](network::Packet& packet) { handleGuildInvite(packet); };
    table[Opcode::SMSG_GUILD_COMMAND_RESULT] = [this](network::Packet& packet) { handleGuildCommandResult(packet); };
    table[Opcode::SMSG_PETITION_SHOWLIST] = [this](network::Packet& packet) { handlePetitionShowlist(packet); };
    table[Opcode::SMSG_TURN_IN_PETITION_RESULTS] = [this](network::Packet& packet) { handleTurnInPetitionResults(packet); };
    table[Opcode::SMSG_OFFER_PETITION_ERROR] = [this](network::Packet& packet) {
        // Wow.exe (0x5CEA00) reads a guid and names it in
        // ERR_ALREADY_IN_ARENA_TEAM_S, whatever the server meant by it.
        if (packet.hasRemaining(8)) {
            const uint64_t who = packet.readUInt64();
            const std::string name = owner_.lookupName(who);
            if (!name.empty())
                owner_.displayGameMessage("ERR_ALREADY_IN_ARENA_TEAM_S",
                                          name + " is already in an arena team of that size.", {name});
            else
                owner_.queryPlayerName(who);
        }
        packet.skipAll();
    };
    table[Opcode::MSG_GUILD_EVENT_LOG_QUERY] = [this](network::Packet& packet) { handleGuildEventLog(packet); };
    table[Opcode::MSG_GUILD_BANK_LOG_QUERY] = [this](network::Packet& packet) { handleGuildBankLog(packet); };
    table[Opcode::SMSG_PETITION_QUERY_RESPONSE] = [this](network::Packet& packet) { handlePetitionQueryResponse(packet); };
    table[Opcode::SMSG_PETITION_SHOW_SIGNATURES] = [this](network::Packet& packet) { handlePetitionShowSignatures(packet); };
    table[Opcode::SMSG_PETITION_SIGN_RESULTS] = [this](network::Packet& packet) { handlePetitionSignResults(packet); };
    // To the charter's owner: who declined to sign it (uint64 guid). Wow.exe
    // (0x6cf140) says ERR_PETITION_DECLINED_S, which plays igPlayerInviteDecline.
    table[Opcode::MSG_PETITION_DECLINE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
        const uint64_t who = packet.readUInt64();
        auto nit = owner_.getPlayerNameCache().find(who);
        const std::string name = nit != owner_.getPlayerNameCache().end() ? nit->second : std::string();
        if (name.empty()) { owner_.queryPlayerName(who); return; }
        owner_.displayGameMessage("ERR_PETITION_DECLINED_S",
                                  name + " has declined to sign your petition.", {name});
    };

    // ---- Battlefield / BG ----
    table[Opcode::SMSG_BATTLEFIELD_STATUS] = [this](network::Packet& packet) { handleBattlefieldStatus(packet); };
    table[Opcode::SMSG_BATTLEFIELD_LIST] = [this](network::Packet& packet) { handleBattlefieldList(packet); };
    // Wow.exe (0x54D4B0): ERR_PLAYER_DEAD, the only reason it knows.
    table[Opcode::SMSG_BATTLEFIELD_PORT_DENIED] = [this](network::Packet& /*packet*/) {
        owner_.displayGameMessage("ERR_PLAYER_DEAD", "You can't do that when you're dead.");
    };
    table[Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS] = [this](network::Packet& packet) {
        bgPlayerPositions_.clear();
        for (int grp = 0; grp < 2; ++grp) {
            if (!packet.hasRemaining(4)) break;
            uint32_t count = packet.readUInt32();
            for (uint32_t i = 0; i < count && packet.getRemainingSize() >= 16; ++i) {
                BgPlayerPosition pos;
                pos.guid = packet.readUInt64();
                pos.wowX = packet.readFloat();
                pos.wowY = packet.readFloat();
                pos.group = grp;
                bgPlayerPositions_.push_back(pos);
            }
        }
    };
    // The reason, as Wow.exe (0x526530 -> 0x522180) words it; other values
    // say nothing.
    table[Opcode::SMSG_REMOVED_FROM_PVP_QUEUE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        switch (packet.readUInt32()) {
            case 0: owner_.displayGameMessage("ERR_REMOVE_FROM_PVP_QUEUE_GRANT_LEVEL",
                        "You have been removed from a PVP queue because another player has granted you a level"); break;
            case 1: owner_.displayGameMessage("ERR_REMOVE_FROM_PVP_QUEUE_XP_GAIN",
                        "You have been removed from a PVP queue because you have changed your XP gain settings"); break;
            case 2: owner_.displayGameMessage("ERR_REMOVE_FROM_PVP_QUEUE_FACTION_CHANGE",
                        "You have been removed from a PvP Queue because you changed your faction."); break;
            default: break;
        }
        packet.skipAll();
    };
    // The answer to a join, and most often a refusal. A positive value is the
    // battleground joined; the negative ones are the reasons a queue was
    // refused. It was read as "joined" whatever it said, so a Deserter or a
    // third queue was reported as success and nothing appeared on the minimap.
    table[Opcode::SMSG_GROUP_JOINED_BATTLEGROUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        bgJoinPendingType_ = 0;  // answered
        // Each answer is the GameError Wow.exe's handler (0x54B1C0) raises
        // for it; -1 says nothing.
        const int32_t result = static_cast<int32_t>(packet.readUInt32());
        struct Refusal { int32_t code; const char* key; const char* text; };
        static constexpr Refusal kRefusals[] = {
            {-2,  "ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS", "You cannot join the battleground yet because you or one of your party members is flagged as a Deserter."},
            {-3,  "ERR_ARENA_TEAM_PARTY_SIZE", "Incorrect party size for this arena."},
            {-4,  "ERR_BATTLEGROUND_TOO_MANY_QUEUES", "You can only be queued for 2 battles at once"},
            {-5,  "ERR_BATTLEGROUND_CANNOT_QUEUE_FOR_RATED", "You cannot queue for a rated match while queued for other battles"},
            {-6,  "ERR_BATTLEDGROUND_QUEUED_FOR_RATED", "You cannot queue for another battle while queued for a rated arena match"},
            {-7,  "ERR_BATTLEGROUND_TEAM_LEFT_QUEUE", "Your team has left the arena queue"},
            {-8,  "ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND", "You can't do that in a battleground."},
            {-9,  "ERR_BATTLEGROUND_JOIN_XP_GAIN", "You can't join a battleground with your experience gain turned off."},
            {-10, "ERR_BATTLEGROUND_JOIN_RANGE_INDEX", "Cannot join the queue unless all members of your party are in the same battleground level range."},
            {-13, "ERR_LFG_CANT_USE_BATTLEGROUND", "You cannot queue for a battleground or arena while using the dungeon system."},
            {-14, "ERR_IN_RANDOM_BG", "Can't do that while in a Random Battleground queue."},
            {-15, "ERR_IN_NON_RANDOM_BG", "Can't queue for Random Battleground while in another Battleground queue."},
        };
        if (result == -1) return;
        for (const auto& r : kRefusals) {
            if (r.code == result) {
                LOG_WARNING("Battleground join refused: ", result);
                owner_.displayGameMessage(r.key, r.text);
                return;
            }
        }
        if (result == -11 || result == -12) {
            // A member who did not answer, by name when the client knows it.
            const uint64_t who = packet.hasRemaining(8) ? packet.readUInt64() : 0;
            const std::string name = who ? owner_.lookupName(who) : std::string();
            if (name.empty())
                owner_.displayGameMessage("ERR_BATTLEGROUND_JOIN_FAILED", "Join as a group failed");
            else
                owner_.displayGameMessage("ERR_BATTLEGROUND_JOIN_TIMED_OUT",
                                          name + " was unavailable to join the queue.", {name});
            return;
        }
        if (result > 0) {
            // The BattlemasterList row's name (0x54B1C0).
            std::string bgName;
            if (const auto* row = owner_.getBattlemasterInfo(static_cast<uint32_t>(result))) bgName = row->name;
            if (!bgName.empty()) {
                owner_.displayGameMessage("ERR_GROUP_JOIN_BATTLEGROUND_S",
                                          "Your group has joined the queue for " + bgName, {bgName});
                return;
            }
        }
        owner_.displayGameMessage("ERR_GROUP_JOIN_BATTLEGROUND_FAIL",
                                  "Your group has joined a battleground queue, but you are not eligible");
    };
    // No handler in Wow.exe at all: the queue status update that follows is
    // the announcement.
    table[Opcode::SMSG_JOINED_BATTLEGROUND_QUEUE] = [](network::Packet& packet) { packet.skipAll(); };
    // ERR_BG_PLAYER_JOINED_SS / ERR_BG_PLAYER_LEFT_S, chat lines (Wow.exe
    // 0x54B510), once the name is known.
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                owner_.displayGameMessage("ERR_BG_PLAYER_JOINED_SS",
                                          name + " has joined the battle", {name, name});
        }
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                owner_.displayGameMessage("ERR_BG_PLAYER_LEFT_S", name + " has left the battle", {name});
        }
    };

    // ---- Instance ----
    table[Opcode::SMSG_INSTANCE_DIFFICULTY] = [this](network::Packet& packet) {
        handleInstanceDifficulty(packet, false);
    };
    table[Opcode::MSG_SET_DUNGEON_DIFFICULTY] = [this](network::Packet& packet) {
        handleInstanceDifficulty(packet, true);
    };
    // 0x526530 case 0x4EB -> 0x5255A0. There was no handler, so a raid
    // difficulty change was never stored or said.
    table[Opcode::MSG_SET_RAID_DIFFICULTY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t diff = packet.readUInt32();
        const uint32_t personal = packet.hasRemaining(4) ? packet.readUInt32() : 1;
        const uint32_t group = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        applyRaidDifficulty(diff, personal != 0, group != 0);
    };

    // ---- Guild / RAF / PvP AFK (moved from GameHandler) ----
    table[Opcode::SMSG_GUILD_DECLINE] = [this](network::Packet& packet) {
        // ERR_GUILD_DECLINE_S, a chat line (Wow.exe 0x6CC410).
        if (packet.hasData()) {
            const std::string name = packet.readString();
            owner_.displayGameMessage("ERR_GUILD_DECLINE_S",
                                      name + " declines your guild invitation.", {name});
        }
    };
    // Nothing said: Wow.exe (0x526530) drops the link from the friends list
    // and fires FRIENDLIST_UPDATE.
    table[Opcode::SMSG_REFER_A_FRIEND_EXPIRED] = [this](network::Packet& packet) {
        packet.skipAll();
        owner_.fireAddonEvent("FRIENDLIST_UPDATE", {});
    };
    // The reason as Wow.exe (0x5219E0) words it. Only reason 9 carries a
    // name; 0 and anything past 13 say nothing.
    table[Opcode::SMSG_REFER_A_FRIEND_FAILURE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t reason = packet.readUInt32();
        std::string name;
        if (reason == 9 && packet.hasData()) name = packet.readString();
        packet.skipAll();
        switch (reason) {
            case 1:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_NOT_REFERRED_BY", "You were not referred by that player"); break;
            case 2:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_TARGET_TOO_HIGH", "That player's level is too high"); break;
            case 3:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_INSUFFICIENT_GRANTABLE_LEVELS", "You have not earned enough levels to grant any more levels"); break;
            case 4:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_TOO_FAR", "That player is too far away"); break;
            case 5:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_DIFFERENT_FACTION", "You cannot grant levels to a character of the opposite faction"); break;
            case 6:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_NOT_NOW", "You cannot grant that player a level right now."); break;
            case 7:  owner_.displayGameMessage("ERR_REFER_A_FRIEND_GRANT_LEVEL_MAX_I", "You cannot grant levels to players level 60 or higher", {"60"}); break;
            case 8:  owner_.displayGameMessage("ERR_GENERIC_NO_TARGET", "You have no target."); break;
            case 9:  owner_.displayGameMessage("ERR_TARGET_NOT_IN_GROUP_S", name + " is not in your party.", {name}); break;
            case 10: owner_.displayGameMessage("ERR_REFER_A_FRIEND_SUMMON_LEVEL_MAX_I", "You cannot summon players above level 60", {"60"}); break;
            case 11: owner_.displayGameMessage("ERR_REFER_A_FRIEND_SUMMON_COOLDOWN", "You can only summon your friend once per hour"); break;
            case 12: owner_.displayGameMessage("ERR_REFER_A_FRIEND_INSUF_EXPAN_LVL", "That player does not have the required expansion to access this area"); break;
            case 13: owner_.displayGameMessage("ERR_REFER_A_FRIEND_SUMMON_OFFLINE_S", " is offline and cannot be summoned", {name}); break;
            default: break;
        }
    };
    // Wow.exe (0x6E2E90) speaks only for results 5 and 6, the AFK-report
    // system being switched on or off; every other result is silent.
    table[Opcode::SMSG_REPORT_PVP_AFK_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            const uint8_t result = packet.readUInt8();
            if (result == 5)
                owner_.addGlobalStringChatLine("PVP_REPORT_AFK_SYSTEM_ENABLED", "The PvP AFK reporting system has been enabled.");
            else if (result == 6)
                owner_.addGlobalStringChatLine("PVP_REPORT_AFK_SYSTEM_DISABLED", "The PvP AFK reporting system has been disabled.");
        }
        packet.skipAll();
    };
    // INSTANCE_SAVED, a plain chat line, for a zero (Wow.exe 0x52BB10).
    table[Opcode::SMSG_INSTANCE_SAVE_CREATED] = [this](network::Packet& packet) {
        // 0x52BB10: a zero is the save itself - INSTANCE_SAVED, and the bind
        // prompt is over (0x513B20(0,0,0): INSTANCE_LOCK_STOP), so a save the
        // server makes on its own, a boss kill, takes the dialog down.
        const uint32_t debug = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        if (debug == 0) {
            owner_.addGlobalStringChatLine("INSTANCE_SAVED", "You are now saved to this instance");
            setInstanceLock(0, 0, false);
        }
    };
    // AzerothCore's RaidInstanceMessage types, as Wow.exe (0x50CA80) says
    // them: 1-3 the lock warnings in hours or minutes, 4 the welcome (an
    // event uiparent.lua words), 5 the lock expiring. Time in seconds.
    table[Opcode::SMSG_RAID_INSTANCE_MESSAGE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
        const uint32_t msgType = packet.readUInt32();
        const uint32_t mapId   = packet.readUInt32();
        packet.readUInt32(); // difficulty
        const uint32_t timeLeft = packet.readUInt32();
        std::string mapLabel = owner_.getMapName(mapId);
        switch (msgType) {
            case 1:
                owner_.addGlobalStringChatLine("RAID_INSTANCE_WARNING_HOURS",
                    "Your instance lock for %s will expire in %d |4hour:hours;.",
                    {mapLabel, std::to_string(timeLeft / 3600)});
                break;
            case 2:
                owner_.addGlobalStringChatLine("RAID_INSTANCE_WARNING_MIN",
                    "Your instance lock for %s will expire in %d |4minute:minutes;!",
                    {mapLabel, std::to_string(timeLeft / 60)});
                break;
            case 3:
                owner_.addGlobalStringChatLine("RAID_INSTANCE_WARNING_MIN_SOON",
                    "Your instance lock for %s will expire in %d |4minute:minutes;.",
                    {mapLabel, std::to_string(timeLeft / 60)});
                break;
            case 4: {
                const bool locked = packet.hasRemaining(1) && packet.readUInt8() != 0;
                const bool extended = packet.hasRemaining(1) && packet.readUInt8() != 0;
                owner_.fireAddonEvent("RAID_INSTANCE_WELCOME",
                                      {mapLabel, std::to_string(timeLeft),
                                       locked ? "1" : "0", extended ? "1" : "0"});
                break;
            }
            case 5:
                owner_.addGlobalStringChatLine("RAID_INSTANCE_EXPIRED",
                    "Your instance lock for %s has expired.", {mapLabel});
                break;
            default: break;
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_INSTANCE_RESET] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t mapId = packet.readUInt32();
        auto it = std::remove_if(instanceLockouts_.begin(), instanceLockouts_.end(),
            [mapId](const InstanceLockout& lo){ return lo.mapId == mapId; });
        instanceLockouts_.erase(it, instanceLockouts_.end());
        // INSTANCE_RESET_SUCCESS, a plain chat line (Wow.exe 0x50CCD0); the
        // map id itself when Map.dbc has no name for it.
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = std::to_string(mapId);
        owner_.addGlobalStringChatLine("INSTANCE_RESET_SUCCESS", "%s has been reset.", {mapLabel});
    };
    // The reason first, then the map (AzerothCore InstanceResetFailed::Write;
    // Wow.exe 0x50CDA0 reads them in that order). Each reason is a plain
    // chat line; any other prints nothing.
    table[Opcode::SMSG_INSTANCE_RESET_FAILED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        const uint32_t reason = packet.readUInt32();
        const uint32_t mapId  = packet.readUInt32();
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = std::to_string(mapId);
        switch (reason) {
            case 0: owner_.addGlobalStringChatLine("INSTANCE_RESET_FAILED",
                        "Cannot reset %s.  There are players still inside the instance.", {mapLabel}); break;
            case 1: owner_.addGlobalStringChatLine("INSTANCE_RESET_FAILED_OFFLINE",
                        "Cannot reset %s.  There are players offline in your party.", {mapLabel}); break;
            case 2: owner_.addGlobalStringChatLine("INSTANCE_RESET_FAILED_ZONING",
                        "Cannot reset %s.  There are players in your party attempting to zone into an instance.", {mapLabel}); break;
            default: break;
        }
    };
    table[Opcode::SMSG_INSTANCE_LOCK_WARNING_QUERY] = [this](network::Packet& packet) {
        // Nine bytes: a countdown in milliseconds, the mask of encounters
        // already completed in this instance, and whether the player was
        // previously saved to it.
        //
        // What was here read a map id, a difficulty, a time and a flag across
        // seventeen bytes, so the guard alone kept the handler from ever
        // running against the nine the server sends. It also answered on the
        // player's behalf - always accept - which is the one thing this packet
        // is asking them.
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        const uint32_t timerMs = packet.readUInt32();
        const uint32_t completedMask = packet.readUInt32();
        const uint8_t previouslySaved = packet.readUInt8();

        LOG_INFO("SMSG_INSTANCE_LOCK_WARNING_QUERY: ", timerMs / 1000,
                 "s to decide, completed mask 0x", std::hex, completedMask, std::dec,
                 previouslySaved ? ", previously saved" : "");
        setInstanceLock(timerMs, completedMask, previouslySaved != 0);
    };

    // ---- LFG ----
    table[Opcode::SMSG_LFG_JOIN_RESULT] = [this](network::Packet& packet) { handleLfgJoinResult(packet); };
    table[Opcode::SMSG_LFG_QUEUE_STATUS] = [this](network::Packet& packet) { handleLfgQueueStatus(packet); };
    table[Opcode::SMSG_LFG_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_ROLE_CHECK_UPDATE] = [this](network::Packet& packet) { handleLfgRoleCheckUpdate(packet); };
    // Two opcodes, two bodies. They were sharing a handler, and the party one
    // carries a join flag and three more bytes of padding the player one does
    // not - so every party update read the wrong byte as the queue flag.
    table[Opcode::SMSG_LFG_UPDATE_PLAYER] = [this](network::Packet& packet) { handleLfgUpdatePlayer(packet); };
    table[Opcode::SMSG_LFG_UPDATE_PARTY]  = [this](network::Packet& packet) { handleLfgUpdateParty(packet); };
    table[Opcode::SMSG_LFG_PLAYER_REWARD] = [this](network::Packet& packet) { handleLfgPlayerReward(packet); };
    table[Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgBootProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_TELEPORT_DENIED] = [this](network::Packet& packet) { handleLfgTeleportDenied(packet); };
    // Wow.exe (0x55BDC0): the queue state is wiped, LFG_UPDATE, and
    // ERR_SYSTEM_DISABLED.
    table[Opcode::SMSG_LFG_DISABLED] = [this](network::Packet& /*packet*/) {
        lfgState_ = LfgState::None;
        owner_.fireAddonEvent("LFG_UPDATE", {});
        owner_.displayGameMessage("ERR_SYSTEM_DISABLED", "This system is currently disabled.");
    };
    table[Opcode::SMSG_LFG_OFFER_CONTINUE] = [this](network::Packet& packet) {
        // uint32 dungeon entry, which the handler used to skip entirely - and
        // then fired the event with nothing. lfgframe.lua opens with
        // NORMAL_FONT_COLOR_CODE..displayName.."|r", and concatenating nil
        // raises, so the offer took its own handler down every time.
        //
        // The entry packs both halves the way LFGDungeonData::Entry builds it:
        // id in the low twenty-four bits, type in the top eight.
        uint32_t dungeonId = 0, typeId = 0;
        if (packet.hasRemaining(4)) {
            const uint32_t entry = packet.readUInt32();
            dungeonId = entry & 0x00FFFFFFu;
            typeId    = (entry >> 24) & 0xFFu;
        }
        // The event alone (Wow.exe 0x55BDC0); the frame words the offer.
        // An entry LFGDungeons.dbc does not know raises nothing.
        std::string name = owner_.getLfgDungeonName(dungeonId);
        if (name.empty()) return;
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("LFG_OFFER_CONTINUE",
                                           {name, std::to_string(dungeonId),
                                            std::to_string(typeId)});
        }
    };
    table[Opcode::SMSG_LFG_ROLE_CHOSEN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(13)) { packet.skipAll(); return; }
        uint64_t roleGuid = packet.readUInt64();
        uint8_t  ready    = packet.readUInt8();
        uint32_t roles    = packet.readUInt32();
        packet.skipAll();
        // Wow.exe (0x55BDC0): a member who declined is
        // ERR_LFG_PLAYER_DECLINED_ROLE_CHECK; one who chose is the event the
        // role-check window lists them from, and no chat line. Both only for
        // a member whose name is known.
        const std::string pName = owner_.lookupName(roleGuid);
        if (pName.empty()) { owner_.queryPlayerName(roleGuid); return; }
        if (!ready) {
            owner_.displayGameMessage("ERR_LFG_PLAYER_DECLINED_ROLE_CHECK",
                                      pName + " has not selected any roles.", {pName});
            return;
        }
        // Booleans through eventBool: "0" is a number and true in Lua, so a
        // false argument in front of a true one cannot be spelled as text.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("LFG_ROLE_CHECK_ROLE_CHOSEN", {
                pName,
                eventBool((roles & 0x02) != 0),
                eventBool((roles & 0x04) != 0),
                eventBool((roles & 0x08) != 0)
            });
        }
        packet.skipAll();
    };
    // The one that carries the locks. Skipped wholesale until now, which is
    // why the dungeon finder listed every dungeon as queueable and left the
    // server to refuse.
    table[Opcode::SMSG_LFG_PLAYER_INFO] = [this](network::Packet& packet) {
        handleLfgPlayerInfo(packet);
    };
    for (auto op : { Opcode::SMSG_LFG_UPDATE_SEARCH, Opcode::SMSG_UPDATE_LFG_LIST,
                     Opcode::SMSG_LFG_PARTY_INFO }) {
        table[op] = [](network::Packet& packet) { packet.skipAll(); };
    }
    table[Opcode::SMSG_OPEN_LFG_DUNGEON_FINDER] = [this](network::Packet& packet) {
        packet.skipAll();
        if (owner_.openLfgCallbackRef()) owner_.openLfgCallbackRef()();
    };

    // ---- Arena ----
    table[Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT] = [this](network::Packet& packet) { handleArenaTeamCommandResult(packet); };
    table[Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE] = [this](network::Packet& packet) { handleArenaTeamQueryResponse(packet); };
    table[Opcode::SMSG_ARENA_TEAM_ROSTER] = [this](network::Packet& packet) { handleArenaTeamRoster(packet); };
    table[Opcode::SMSG_ARENA_TEAM_INVITE] = [this](network::Packet& packet) { handleArenaTeamInvite(packet); };
    table[Opcode::SMSG_ARENA_TEAM_EVENT] = [this](network::Packet& packet) { handleArenaTeamEvent(packet); };
    table[Opcode::SMSG_ARENA_TEAM_STATS] = [this](network::Packet& packet) { handleArenaTeamStats(packet); };
    table[Opcode::SMSG_ARENA_ERROR] = [this](network::Packet& packet) { handleArenaError(packet); };
    table[Opcode::MSG_PVP_LOG_DATA] = [this](network::Packet& packet) { handlePvpLogData(packet); };

    // ---- Factions / group leader ----
    table[Opcode::SMSG_INITIALIZE_FACTIONS] = [this](network::Packet& p) { handleInitializeFactions(p); };
    table[Opcode::SMSG_SET_FACTION_STANDING] = [this](network::Packet& p) { handleSetFactionStanding(p); };
    table[Opcode::SMSG_SET_FACTION_ATWAR] = [this](network::Packet& p) { handleSetFactionAtWar(p); };
    table[Opcode::SMSG_SET_FACTION_VISIBLE] = [this](network::Packet& p) { handleSetFactionVisible(p); };
    table[Opcode::SMSG_GROUP_SET_LEADER] = [this](network::Packet& p) { handleGroupSetLeader(p); };
}

// ============================================================
// Non-inline accessors requiring GameHandler
// ============================================================

std::string SocialHandler::getWhoAreaName(uint32_t zoneId) const {
    return owner_.getAreaName(zoneId);
}

std::string SocialHandler::getCurrentLfgDungeonName() const {
    return owner_.getLfgDungeonName(lfgDungeonId_);
}

uint32_t SocialHandler::ownGuildId() const {
    // The player's own field first: it is what the server keeps current, and
    // the character list's copy was taken at login - before any guild joined
    // or left since.
    if (const uint32_t live = getEntityGuildId(owner_.getPlayerGuid())) return live;
    const Character* ch = owner_.getActiveCharacter();
    return ch ? ch->guildId : 0;
}

bool SocialHandler::isInGuild() const {
    // Three records, and the newest wins: a guild joined this session is in
    // none of the others until the next login.
    if (inGuild_) return true;
    if (!guildName_.empty()) return true;
    return ownGuildId() != 0;
}

uint32_t SocialHandler::getEntityGuildId(uint64_t guid) const {
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return 0;
    const uint16_t ufUnitEnd = fieldIndex(UF::UNIT_END);
    if (ufUnitEnd == 0xFFFF) return 0;
    return entity->getField(ufUnitEnd + 3);
}

const std::string& SocialHandler::lookupGuildName(uint32_t guildId) {
    if (guildId == 0) return kEmptyString;
    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end()) return it->second;

    const auto now = std::chrono::steady_clock::now();
    auto pendingIt = pendingGuildNameQueries_.find(guildId);
    const bool shouldQuery =
        pendingIt == pendingGuildNameQueries_.end() ||
        std::chrono::duration_cast<std::chrono::seconds>(now - pendingIt->second).count() >= 2;
    if (shouldQuery) {
        pendingGuildNameQueries_[guildId] = now;
        queryGuildInfo(guildId);
    }
    return kEmptyString;
}

// ============================================================
// Inspection
// ============================================================

void SocialHandler::inspectTarget() { inspectUnit(owner_.getTargetGuid()); }

// Any player, not only the targeted one: FrameXML's unit menus name the unit
// the menu was opened on, and on a party frame that is not the target.
void SocialHandler::requestInspectHonorData(uint64_t guid) {
    if (guid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // RequestInspectHonorData (0x5EAC90 -> 0x5E9140): MSG_INSPECT_HONOR_STATS
    // and MSG_INSPECT_ARENA_TEAMS for the inspected player, once, and not
    // when the honour figures are already in.
    if (guid == inspectResult_.guid && (inspectResult_.hasHonorData || honorDataRequested_)) return;
    for (Opcode op : {Opcode::MSG_INSPECT_HONOR_STATS, Opcode::MSG_INSPECT_ARENA_TEAMS}) {
        const uint32_t wire = wireOpcode(op);
        if (wire == 0xFFFF) continue;
        network::Packet pkt(static_cast<uint16_t>(wire));
        pkt.writeUInt64(guid);
        owner_.getSocket()->send(pkt);
    }
    if (guid == inspectResult_.guid) honorDataRequested_ = true;
}

// MSG_INSPECT_ARENA_TEAMS (0x5E7B00): one reply a team, for the inspected
// player only, stored by its slot; INSPECT_HONOR_UPDATE.
void SocialHandler::handleInspectArenaTeams(network::Packet& packet) {
    uint64_t guid = 0;
    arena::InspectTeam t;
    const bool ok = arena::parseInspectTeam(packet, guid, t);
    packet.skipAll();
    if (!ok || guid == 0 || guid != inspectResult_.guid || t.slot >= inspectResult_.arenaTeams.size())
        return;
    auto& slot = inspectResult_.arenaTeams[t.slot];
    slot.teamId = t.teamId;
    slot.rating = t.rating;
    slot.seasonGames = t.seasonGames;
    slot.seasonWins = t.seasonWins;
    slot.playerSeasonGames = t.playerSeasonGames;
    slot.personalRating = t.personalRating;
    owner_.fireAddonEvent("INSPECT_HONOR_UPDATE", {});
}

void SocialHandler::inspectUnit(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        LOG_WARNING("Cannot inspect: not in world or not connected");
        return;
    }
    if (guid == 0) {
        owner_.raiseUiError("You must target a player to inspect.");
        return;
    }
    auto entity = owner_.getEntityManager().getEntity(guid);
    // Silent, as NotifyInspect is for anything CanInspect refuses: the stock
    // menus never offer inspecting a creature.
    if (!entity || entity->getType() != ObjectType::PLAYER) return;
    auto packet = InspectPacket::build(guid);
    owner_.getSocket()->send(packet);
    userInspectGuid_ = guid;
    if (isActiveExpansion("wotlk")) {
        auto achPkt = QueryInspectAchievementsPacket::build(guid);
        owner_.getSocket()->send(achPkt);
    }
    auto player = std::static_pointer_cast<Player>(entity);
    std::string name = player->getName().empty() ? "Target" : player->getName();
    // No chat line: the stock client prints nothing here, and the inspect
    // window calls NotifyInspect on every open and every unit change, so a
    // line per request filled chat with "Inspecting ..." echoes.
    LOG_INFO("Sent inspect request for player: ", name, " (GUID: 0x", std::hex, guid, std::dec, ")");
}

void SocialHandler::handleInspectResults(network::Packet& packet) {
    if (isActiveExpansion("tbc") && packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_RESULTS_UPDATE)) {
        uint64_t guid = 0;
        std::array<uint32_t, 19> items{};
        std::array<uint16_t, 19> enchantIds{};
        if (parseInspectEquipmentPayload(packet, guid, items, enchantIds)) {
            owner_.cacheInspectedPlayerEquipment(guid, items);

            auto entity = owner_.getEntityManager().getEntity(guid);
            std::string playerName = "Target";
            if (entity) {
                auto player = std::dynamic_pointer_cast<Player>(entity);
                if (player && !player->getName().empty()) playerName = player->getName();
            }

            // TBC servers send inspected gear separately from talent data. Make the
            // gear-only response visible to the Inspect window immediately while
            // preserving talents if a matching talent response already arrived.
            if (inspectResult_.guid != guid) {
                inspectResult_ = InspectResult{};
                inspectResult_.guid = guid;
            }
            inspectResult_.playerName = playerName;
            inspectResult_.itemEntries = items;
            inspectResult_.enchantIds = enchantIds;

            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): ", playerName, " has gear in ",
                     std::count_if(items.begin(), items.end(),
                                   [](uint32_t e) { return e != 0; }), "/19 slots");
            if (owner_.addonEventCallbackRef()) {
                char guidBuf[32];
                snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
                owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
                // INSPECT_READY is a later expansion's name and nothing in this
                // interface listens for it. The inspect paperdoll refreshes on
                // UNIT_INVENTORY_CHANGED for the unit it is showing, so the gear
                // arrived and the window was never told - it kept whatever it had
                // when it opened. Fired for the unit token this guid answers to,
                // and only when it answers to one: an inspected player who is not
                // the target has no token for the interface to match against.
                const std::string inspectUnit = owner_.guidToUnitId(guid);
                if (!inspectUnit.empty()) {
                    owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
                }
            }
            return;
        }
        LOG_DEBUG("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): unrecognized payload size=",
                  packet.getSize());
    }

    if (isActiveExpansion("tbc") &&
        packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_TALENT)) {
        if (!packet.hasFullPackedGuid()) return;
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0) return;

        auto entity = owner_.getEntityManager().getEntity(guid);
        std::string playerName = "Target";
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) playerName = player->getName();
        }

        if (inspectResult_.guid != guid) {
            inspectResult_ = InspectResult{};
            inspectResult_.guid = guid;
        }
        inspectResult_.playerName = playerName;

        uint32_t unspentTalents = 0;
        uint32_t spentTalents = 0;
        std::array<uint32_t, 3> treePoints{};
        bool hasTreePoints = false;
        const uint8_t classId = owner_.lookupPlayerClass(guid);
        const size_t talentPayloadStart = packet.getReadPos();
        if (parseTbcInspectTalentPayload(packet, owner_, classId, unspentTalents, spentTalents,
                                         treePoints, hasTreePoints)) {
            inspectResult_.totalTalents = spentTalents;
            inspectResult_.unspentTalents = unspentTalents;
            inspectResult_.hasTalentData = true;
            inspectResult_.hasTalentTreePoints = hasTreePoints;
            inspectResult_.talentTreePoints = treePoints;
            inspectResult_.talentGroups = 1;
            inspectResult_.activeTalentGroup = 0;
        } else {
            packet.setReadPos(talentPayloadStart);
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                     " unparsed payload bytes=", packet.getRemainingSize());
        }

        auto gearIt = owner_.inspectedPlayerItemEntriesRef().find(guid);
        if (gearIt != owner_.inspectedPlayerItemEntriesRef().end()) {
            inspectResult_.itemEntries = gearIt->second;
        }

        LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                 " gearCached=", (gearIt != owner_.inspectedPlayerItemEntriesRef().end() ? "yes" : "no"),
                 " payload bytes remaining=", packet.getRemainingSize());
        if (owner_.addonEventCallbackRef()) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
            owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
            const std::string inspectUnit = owner_.guidToUnitId(guid);
            if (!inspectUnit.empty()) {
                owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
            }
        }
        return;
    }

    // SMSG_INSPECT_TALENT (WotLK) is a packed guid, then
    // BuildPlayerTalentsInfoData and BuildEnchantmentsInfoData. There is no
    // leading discriminator, and reading one ate the packed guid's mask byte
    // and put everything after it a byte out.
    //
    // The branch that used to sit behind that byte re-parsed the player's own
    // talents into a second set of members on GameHandler. Nothing read them -
    // SpellHandler keeps the ones the talent frame draws from - and this
    // opcode never carries them anyway.
    const bool talentTbc = isPreWotlk();
    if (packet.getRemainingSize() < (talentTbc ? 8u : 2u)) return;
    uint64_t guid = talentTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (guid == 0) return;

    size_t bytesLeft = packet.getRemainingSize();
    if (bytesLeft < 6) {
        // Before WotLK the guid is the whole answer - a 1.12 realm sends eight
        // bytes and nothing after them, which is the shape and not a
        // truncation. Reported as one, every inspect on turtle wrote a warning
        // into a log that is read for faults.
        if (talentTbc) {
            LOG_INFO("Inspect: this realm answers with the guid alone, so there "
                     "are no talents to show");
        } else {
            LOG_WARNING("SMSG_TALENTS_INFO: too short after guid, ", bytesLeft, " bytes");
        }
        // Nothing to the player: the stock client has no line for an
        // answer without talents.
        return;
    }

    uint32_t unspentTalents = packet.readUInt32();
    uint8_t talentGroupCount = packet.readUInt8();
    uint8_t activeTalentGroup = packet.readUInt8();

    auto entity = owner_.getEntityManager().getEntity(guid);
    std::string playerName = "Target";
    if (entity) {
        auto player = std::dynamic_pointer_cast<Player>(entity);
        if (player && !player->getName().empty()) playerName = player->getName();
    }

    // Every spec is sent. Counting all of them reported a dual-spec character
    // as having spent roughly twice what they have.
    uint32_t totalTalents = 0;
    std::unordered_map<uint32_t, uint8_t> inspectTalents;
    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t talentCount = packet.readUInt8();
        for (uint8_t t = 0; t < talentCount; ++t) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 5) break;
            const uint32_t talentId = packet.readUInt32();
            const uint8_t rank = packet.readUInt8();
            if (g == activeTalentGroup) {
                // Rank is zero-based on the wire, as it is for the player's own
                // talents. The inspect talent tab was counting these and
                // throwing them away, so it drew the viewer's own tree with the
                // target's name over it.
                inspectTalents[talentId] = static_cast<uint8_t>(rank + 1u);
                totalTalents++;
            }
        }
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount; ++gl) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 2) break;
            packet.readUInt16();
        }
    }

    // BuildEnchantmentsInfoData: a mask of which of the nineteen equipment
    // slots are filled, then per filled slot the item's entry, a mask of which
    // of its twelve enchantment slots are used, and an id per set bit.
    //
    // What was here read the slot mask and then one bare uint16 per set bit,
    // so it took the low half of the first item's entry as that slot's enchant
    // and drifted further out of step with every item after it.
    constexpr int kEquipmentSlots = 19;
    constexpr int kEnchantmentSlots = 12;
    std::array<uint32_t, 19> itemEntries{};
    std::array<uint16_t, 19> enchantIds{};
    bool sawGear = false;
    if (packet.hasRemaining(4)) {
        const uint32_t slotMask = packet.readUInt32();
        for (int slot = 0; slot < kEquipmentSlots; ++slot) {
            if (!(slotMask & (1u << slot))) continue;
            if (!packet.hasRemaining(6)) break;
            itemEntries[slot] = packet.readUInt32();
            sawGear = true;
            const uint16_t enchantMask = packet.readUInt16();
            for (int e = 0; e < kEnchantmentSlots; ++e) {
                if (!(enchantMask & (1u << e))) continue;
                if (!packet.hasRemaining(2)) break;
                const uint16_t enchId = packet.readUInt16();
                // Slot zero is the permanent enchant, which is the one the
                // paperdoll names.
                if (e == 0) enchantIds[slot] = enchId;
            }
            // Each item record then closes with its random property id
            // (int16), its creator as a packed guid, and its suffix factor
            // (uint32) - Player::BuildEnchantmentsInfoData writes all three.
            // Skipping none of them read the second item's entry out of the
            // first item's tail, so only the head slot ever came out right and
            // every other square of the inspect paperdoll was garbage or empty.
            if (!packet.hasRemaining(2)) break;
            packet.readUInt16();
            if (!packet.hasFullPackedGuid()) break;
            packet.readPackedGuid();
            if (!packet.hasRemaining(4)) break;
            packet.readUInt32();
        }
    }

    // This client also inspects nearby players on its own, for their gear's
    // look (GameHandler's auto-inspect queue). The stock client inspects only
    // when the interface asks (NotifyInspect / InspectUnit), so only that
    // answer is the inspect frame's: an automatic one keeps its gear in the
    // cache and stops here. It used to replace the inspected player's data and
    // fire INSPECT_TALENT_READY every couple of seconds in a crowd.
    if (guid != userInspectGuid_) {
        if (sawGear) owner_.cacheInspectedPlayerEquipment(guid, itemEntries);
        return;
    }

    // A different player means the honour figures in hand are somebody else's.
    // The fields here are assigned one at a time rather than as a whole struct,
    // so anything not named survives - and the honour tab reads
    // HasInspectHonorData to decide whether to ask, so a stale true would show
    // the last player's kills against this one's name and never correct itself.
    if (inspectResult_.guid != guid) {
        inspectResult_.hasHonorData = false;
        inspectResult_.honorTodayKills = 0;
        inspectResult_.honorYesterdayKills = 0;
        inspectResult_.honorTodayContribution = 0;
        inspectResult_.honorYesterdayContribution = 0;
        inspectResult_.honorLifetimeKills = 0;
        inspectResult_.honorRank = 0;
        inspectResult_.arenaTeams = {};
        honorDataRequested_ = false;
    }
    inspectResult_.guid              = guid;
    inspectResult_.playerName        = playerName;
    inspectResult_.totalTalents      = totalTalents;
    inspectResult_.unspentTalents    = unspentTalents;
    inspectResult_.hasTalentData     = true;
    inspectResult_.hasTalentTreePoints = false;
    inspectResult_.talentTreePoints  = {};
    inspectResult_.talentGroups      = talentGroupCount;
    inspectResult_.activeTalentGroup = activeTalentGroup;
    inspectResult_.enchantIds        = enchantIds;
    inspectResult_.talentRanks       = std::move(inspectTalents);
    inspectResult_.classId           = owner_.lookupPlayerClass(guid);

    // The gear is in this packet. It used to be looked up from the cache that
    // Classic's SMSG_INSPECT and TBC's SMSG_INSPECT_RESULTS_UPDATE fill, and
    // nothing filled it on WotLK, so inspecting anyone showed an empty
    // paperdoll however well equipped they were.
    if (sawGear) {
        inspectResult_.itemEntries = itemEntries;
        owner_.cacheInspectedPlayerEquipment(guid, itemEntries);
    } else {
        auto gearIt = owner_.inspectedPlayerItemEntriesRef().find(guid);
        inspectResult_.itemEntries =
            (gearIt != owner_.inspectedPlayerItemEntriesRef().end()) ? gearIt->second
                                                                     : std::array<uint32_t, 19>{};
    }

    LOG_INFO("Inspect results for ", playerName, ": ", totalTalents, " talents, ",
             unspentTalents, " unspent, ", (int)talentGroupCount, " specs");
    if (owner_.addonEventCallbackRef()) {
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
        owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
        // The same packet carries the talents, and the talent tab of the
        // inspect window redraws on this event alone. Only INSPECT_READY was
        // fired, which is what the gear tab listens for - so inspecting anyone
        // filled the paperdoll and left the talent tab blank, however
        // completely the ranks above had been parsed.
        owner_.addonEventCallbackRef()("INSPECT_TALENT_READY", {});
        const std::string inspectUnit = owner_.guidToUnitId(guid);
        if (!inspectUnit.empty()) {
            owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
        }
    }
}

// ============================================================
// Server Info / Who / Social
// ============================================================

void SocialHandler::queryServerTime(bool announce) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    announceServerTime_ = announce;
    auto packet = QueryTimePacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Requested server time");
}

void SocialHandler::requestPlayedTime() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RequestPlayedTimePacket::build(true);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requested played time");
}

void SocialHandler::queryWho(const std::string& filterText) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // SendWho's filter as Wow.exe reads it (0x6B5630): n- name, g- guild,
    // z- zones, r- races, c- classes, levels, and up to four free words that
    // the server matches against name, guild, race, class and zone. The whole
    // text used to go out as the name, so "/who 70 orc" asked for a player
    // called "70 orc" and found nobody.
    struct WhoRows {
        bool loaded = false;
        std::vector<chat_rules::WhoNamedRow> zones, races, classes;
    };
    static WhoRows rows;
    auto* am = owner_.services().assetManager;
    if (!rows.loaded && am && am->isInitialized()) {
        rows.loaded = true;
        // AreaTable's top-level rows (ParentAreaID 0) in file order; the
        // names are field 11, ChrRaces' field 14, ChrClasses' field 4 (enUS).
        if (auto dbc = am->loadDBC("AreaTable.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() > 11) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                if (dbc->getUInt32(i, 2) == 0)
                    rows.zones.push_back(chat_rules::WhoNamedRow{dbc->getUInt32(i, 0), dbc->getString(i, 11)});
        }
        if (auto dbc = am->loadDBC("ChrRaces.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() > 14) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                rows.races.push_back(chat_rules::WhoNamedRow{dbc->getUInt32(i, 0), dbc->getString(i, 14)});
        }
        if (auto dbc = am->loadDBC("ChrClasses.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() > 4) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                rows.classes.push_back(chat_rules::WhoNamedRow{dbc->getUInt32(i, 0), dbc->getString(i, 4)});
        }
    }
    chat_rules::WhoTags tags;
    auto tag = [&](const char* key, std::string& into) {
        std::string v = globalString(am, key);
        if (!v.empty()) into = std::move(v);
    };
    tag("WHO_TAG_NAME", tags.name);
    tag("WHO_TAG_GUILD", tags.guild);
    tag("WHO_TAG_ZONE", tags.zone);
    tag("WHO_TAG_RACE", tags.race);
    tag("WHO_TAG_CLASS", tags.cls);
    const chat_rules::WhoFilter f =
        chat_rules::parseWhoFilter(filterText, rows.zones, rows.races, rows.classes, tags);
    auto packet = WhoPacket::build(f.minLevel, f.maxLevel, f.name, f.guild, f.raceMask,
                                   f.classMask, f.zones, f.strings);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent WHO query '", filterText, "': levels ", f.minLevel, "-", f.maxLevel,
             " name='", f.name, "' guild='", f.guild, "' zones=", f.zones.size(),
             " strings=", f.strings.size());
}

void SocialHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // Sent without a word (Wow.exe AddFriend 0x6B6AF0 -> 0x6B5400): the
    // server's SMSG_FRIEND_STATUS is what the player hears.
    auto packet = AddFriendPacket::build(playerName, note);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent friend request to: ", playerName);
}

void SocialHandler::removeFriend(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // A name not on the list is ERR_FRIEND_NOT_FOUND (Wow.exe 0x6B6F00);
    // otherwise the request goes without a word.
    auto it = owner_.friendsCacheRef().find(playerName);
    if (it == owner_.friendsCacheRef().end()) {
        owner_.displayGameMessage("ERR_FRIEND_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = DelFriendPacket::build(it->second);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent remove friend request for: ", playerName);
}

void SocialHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // As RemoveFriend (Wow.exe 0x6B6F70).
    auto it = owner_.friendsCacheRef().find(playerName);
    if (it == owner_.friendsCacheRef().end()) {
        owner_.displayGameMessage("ERR_FRIEND_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = SetContactNotesPacket::build(it->second, note);
    owner_.getSocket()->send(packet);
    LOG_INFO("Set friend note for: ", playerName);
}

void SocialHandler::addIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // Someone already ignored is ERR_IGNORE_ALREADY_S, said by the client
    // (Wow.exe 0x6B5F70); otherwise the request goes without a word.
    if (owner_.ignoreCacheRef().count(playerName)) {
        owner_.displayGameMessage("ERR_IGNORE_ALREADY_S", playerName + " is already being ignored.",
                                  {playerName});
        return;
    }
    auto packet = AddIgnorePacket::build(playerName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent ignore request for: ", playerName);
}

void SocialHandler::removeIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // ERR_IGNORE_NOT_FOUND for a name not on the list (Wow.exe 0x6B7040).
    auto it = owner_.ignoreCacheRef().find(playerName);
    if (it == owner_.ignoreCacheRef().end()) {
        owner_.displayGameMessage("ERR_IGNORE_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = DelIgnorePacket::build(it->second);
    owner_.getSocket()->send(packet);
    // Don't erase from ignoreCache here - wait for the server's SMSG_IGNORE_LIST
    // response to confirm. Erasing optimistically desyncs the cache if the server
    // rejects the request. (Compare with removeFriend which also waits for
    // SMSG_FRIEND_STATUS before updating its cache.)
    owner_.ignoreListGuidsRef().erase(it->second);
    LOG_INFO("Sent remove ignore request for: ", playerName);
}

void SocialHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The range is the caller's to check (RandomRoll, 0x516610).
    auto packet = RandomRollPacket::build(minRoll, maxRoll);
    owner_.getSocket()->send(packet);
    LOG_INFO("Rolled ", minRoll, "-", maxRoll);
}

// ============================================================
// Logout
// ============================================================

void SocialHandler::requestLogout(bool exitAfterLogout) {
    // Exit Game has to close the program. Everything below is about leaving the
    // world tidily first, which is worth doing and is not worth being trapped
    // by: each way the request cannot be made is answered by going anyway.
    //
    // With no connection there is nobody to ask. At the login screen, or after
    // a disconnect, the button did nothing whatsoever.
    if (!owner_.getSocket()) {
        if (exitAfterLogout && owner_.logoutCompleteCallbackRef()) {
            owner_.logoutCompleteCallbackRef()(true);
        }
        return;
    }
    if (loggingOut_) {
        // Asked a second time while a logout is already running. For Exit that
        // is the player saying they would rather not wait, which is exactly
        // what ForceQuit is described as doing and what the countdown popup's
        // own button is for.
        if (exitAfterLogout) {
            LOG_WARNING("Exit Game asked for during a logout - closing now");
            if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(true);
            return;
        }
        // A second /logout is ignored in silence (Wow.exe 0x6B1930).
        return;
    }
    auto packet = LogoutRequestPacket::build();
    owner_.getSocket()->send(packet);
    loggingOut_ = true;
    exitAfterLogout_ = exitAfterLogout;
    LOG_WARNING("Sent logout request (exitAfterLogout=", exitAfterLogout ? "yes" : "no", ")");  // temporary
}

void SocialHandler::cancelLogout() {
    if (!owner_.getSocket()) return;
    // CMSG_LOGOUT_CANCEL and nothing said (Wow.exe 0x6B18C0), sent whether or
    // not a logout is pending; the pending mark is cleared on the way, so the
    // server's SMSG_LOGOUT_CANCEL_ACK that answers it fires nothing
    // (0x6B0900 fires LOGOUT_CANCEL only for a logout still pending - one
    // the server called off itself).
    if (owner_.getState() != WorldState::IN_WORLD) return;
    auto packet = LogoutCancelPacket::build();
    owner_.getSocket()->send(packet);
    loggingOut_ = false;
    exitAfterLogout_ = false;
    logoutCountdown_ = 0.0f;
    LOG_INFO("Cancelled logout");
}

void SocialHandler::forceLogout() {
    // The forced form of the request (0x6B1930 with its second argument set):
    // CMSG_PLAYER_LOGOUT, no pending mark, no countdown of the client's own.
    // The server honours it only for an account allowed to leave at once.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_PLAYER_LOGOUT));
    owner_.getSocket()->send(packet);
}

// ============================================================
// Guild
// ============================================================

void SocialHandler::requestGuildInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildInfoPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestGuildRoster() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildRosterPacket::build();
    owner_.getSocket()->send(packet);
    // Said in the log, not to the player. This is asked for far more often
    // than a person would ask for it: eight places here request a roster after
    // a guild change, and the interface asks whenever the mail frame or the
    // friends frame opens with an empty one. Every one of those put a line in
    // chat that the real client never writes.
    LOG_DEBUG("Requested guild roster");
}

// The longer "guild information" text, which is a different field from the
// message of the day and has its own opcode.
void SocialHandler::setGuildInfoText(const std::string& text) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INFO_TEXT));
    packet.writeString(text);
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

// Turn a letter into a readable item, which is what "take" does to a mail whose
// body is the thing being kept.
void SocialHandler::takeInboxTextItem(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM));
    packet.writeUInt64(0);            // mailbox guid - the server uses the open one
    packet.writeUInt32(mailId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildMotd(const std::string& motd) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildMotdPacket::build(motd);
    owner_.getSocket()->send(packet);
}

void SocialHandler::promoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    // The server's guild event announces it.
    auto packet = GuildPromotePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::demoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    auto packet = GuildDemotePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // ERR_GUILD_QUIT_S comes back with the command result.
    auto packet = GuildLeavePacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::inviteToGuild(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name."); return; }
    auto packet = GuildInvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
    // The server answers with SMSG_GUILD_COMMAND_RESULT (ERR_GUILD_INVITE_S).
}

void SocialHandler::kickGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildRemovePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::disbandGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDisbandPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildLeader(const std::string& name) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildLeaderPacket::build(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildSetPublicNotePacket::build(name, note);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildSetOfficerNotePacket::build(name, note);
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGuildInvite_ = false;
    // What the invite called it, until the guild names itself: the roster that
    // confirms the join does not carry a name, and the query that does needs
    // an id the player only has once the server has moved them.
    joinedGuildName_ = pendingGuildInviteGuildName_;
    auto packet = GuildAcceptPacket::build();
    owner_.getSocket()->send(packet);
    // ...and the roster, which is what fills the social window's guild tab.
    // The server sends the join event to the guild rather than a roster to the
    // joiner, so nothing arrived unless something asked.
    requestGuildRoster();
}

void SocialHandler::declineGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGuildInvite_ = false;
    auto packet = GuildDeclineInvitationPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::queryGuildInfo(uint32_t guildId) {
    // Allow guild queries at the character screen too - the socket is
    // connected and the server accepts CMSG_GUILD_QUERY before login.
    if (!owner_.getSocket()) return;
    auto packet = GuildQueryPacket::build(guildId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::createGuild(const std::string& guildName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildCreatePacket::build(guildName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::addGuildRank(const std::string& rankName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildAddRankPacket::build(rankName);
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::deleteGuildRank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDelRankPacket::build();
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = PetitionShowlistPacket::build(npcGuid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::buyPetition(uint64_t npcGuid, const std::string& guildName,
                                uint32_t clientIndex) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = PetitionBuyPacket::build(npcGuid, guildName, clientIndex);
    owner_.getSocket()->send(packet);
}

void SocialHandler::signPetition(uint64_t petitionGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PETITION_SIGN));
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt8(0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::turnInPetition(uint64_t petitionGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_TURN_IN_PETITION));
    pkt.writeUInt64(petitionGuid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::offerPetition(uint64_t petitionGuid, uint64_t targetGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    if (petitionGuid == 0 || targetGuid == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_OFFER_PETITION));
    // The leading dword is not the petition type despite where it sits - the
    // server reads it and throws it away. Every expansion this client speaks
    // has it, so it is written unconditionally rather than gated.
    pkt.writeUInt32(0);
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt64(targetGuid);
    owner_.getSocket()->send(pkt);
}

// ============================================================
// Ready Check
// ============================================================

void SocialHandler::initiateReadyCheck() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (!isInGroup()) { owner_.raiseUiError("You must be in a group to initiate a ready check."); return; }
    // ERR_READY_CHECK_IN_PROGRESS while one runs (Wow.exe 0x5732b0).
    if (readyCheckActive_ && readyCheckSecondsLeft_ > 0.0f) {
        owner_.displayGameMessage("ERR_READY_CHECK_IN_PROGRESS",
                                  "A ready check is already in progress.");
        return;
    }
    auto packet = ReadyCheckPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::respondToReadyCheck(bool ready) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = ReadyCheckConfirmPacket::build(ready);
    owner_.getSocket()->send(packet);
    // Group::BroadcastReadyCheck sends answers to the leader and assistants
    // only, so a plain member never hears their own back. The player frame's
    // check icon reads GetReadyCheckStatus("player") on READY_CHECK_CONFIRM.
    if (readyCheckActive_) {
        const uint64_t self = owner_.getPlayerGuid();
        bool found = false;
        for (auto& r : readyCheckResults_) {
            if (r.guid == self) { r.ready = ready; found = true; break; }
        }
        if (!found) {
            ReadyCheckResult r{ .name = std::string(), .ready = ready };
            r.guid = self;
            readyCheckResults_.push_back(std::move(r));
        }
        if (owner_.addonEventCallbackRef()) {
            if (ready) owner_.addonEventCallbackRef()("READY_CHECK_CONFIRM", {"player", "1"});
            else       owner_.addonEventCallbackRef()("READY_CHECK_CONFIRM", {"player"});
        }
    }
}

const char* SocialHandler::readyCheckStatusFor(uint64_t guid) const {
    if (!readyCheckActive_ || guid == 0) return nullptr;
    for (const auto& r : readyCheckResults_)
        if (r.guid == guid) return r.ready ? "ready" : "notready";
    // Waiting only for someone in the group. Answers reach the leader and
    // assistants alone (Group::BroadcastReadyCheck), so anyone else can only
    // ever know their own.
    const uint64_t self = owner_.getPlayerGuid();
    if (guid == self) return "waiting";
    const bool seesAnswers = partyData.leaderGuid == self || (partyData.flags & 0x01) != 0;
    if (!seesAnswers) return nullptr;
    for (const auto& m : partyData.members)
        if (m.guid == guid) return "waiting";
    return nullptr;
}

// ============================================================
// Duel
// ============================================================

void SocialHandler::acceptDuel() {
    if (!pendingDuelRequest_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingDuelRequest_ = false;
    // CMSG_DUEL_ACCEPTED and nothing said (Wow.exe 0x5CFBD0).
    auto pkt = DuelAcceptPacket::build(duelArbiterGuid_);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::forfeitDuel() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingDuelRequest_ = false;
    // CMSG_DUEL_CANCELLED and nothing said (Wow.exe 0x5CFC50); the server's
    // SMSG_DUEL_COMPLETE / SMSG_DUEL_WINNER are the announcement.
    auto packet = DuelCancelPacket::build(duelArbiterGuid_);
    owner_.getSocket()->send(packet);
}

void SocialHandler::proposeDuel(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (targetGuid == 0) { owner_.raiseUiError("You must target a player to challenge to a duel."); return; }
    // The server's SMSG_DUEL_REQUESTED, which comes back to the challenger
    // too, is where ERR_DUEL_REQUESTED is said.
    auto packet = DuelProposedPacket::build(targetGuid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (targetGuid == 0) {
        owner_.raiseUiError("You must target a player to report.");
        return;
    }
    // SMSG_COMPLAIN_RESULT answers it (COMPLAINT_ADDED).
    auto packet = ComplainPacket::build(targetGuid, reason);
    owner_.getSocket()->send(packet);
    LOG_INFO("Reported player: 0x", std::hex, targetGuid, std::dec, " reason=", reason);
}

void SocialHandler::handleDuelRequested(network::Packet& packet) {
    if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
    // The flag first, then the challenger. Read the other way round, the
    // challenger was the flag - a game object no name lookup could find, so
    // the challenge came from a hex number - and the flag, which the accept
    // and the refusal both have to name, was thrown away.
    duelArbiterGuid_ = packet.readUInt64();
    duelChallengerGuid_ = packet.readUInt64();
    // The server sends this to both players. The one who issued the challenge
    // keeps the flag, to be able to call it off, and is not asked to accept it.
    // Wow.exe (0x5cfcd0) tells that player ERR_DUEL_REQUESTED, which plays
    // LEVELUP.
    // It also answers the server on that player's behalf with
    // CMSG_DUEL_ACCEPTED (0x5CFBD0), which the server ignores from the
    // initiator.
    if (duelChallengerGuid_ == owner_.getPlayerGuid()) {
        pendingDuelRequest_ = false;
        owner_.displayGameMessage("ERR_DUEL_REQUESTED", "You have requested a duel.");
        if (owner_.getSocket()) {
            auto accept = DuelAcceptPacket::build(duelArbiterGuid_);
            owner_.getSocket()->send(accept);
        }
        return;
    }
    // A challenge from someone ignored is refused at once, unseen
    // (0x6B52E0, then CMSG_DUEL_CANCELLED).
    if (owner_.ignoreListGuidsRef().count(duelChallengerGuid_)) {
        if (owner_.getSocket()) {
            auto refuse = DuelCancelPacket::build(duelArbiterGuid_);
            owner_.getSocket()->send(refuse);
        }
        duelArbiterGuid_ = 0;
        return;
    }
    // 0x5CFCD0 asks DUEL_REQUESTED with the challenger's unit name, and only
    // when the challenger is an object it has; the request stands either way.
    duelChallengerName_.clear();
    pendingDuelRequest_ = true;
    auto entity = owner_.getEntityManager().getEntity(duelChallengerGuid_);
    auto* challenger = dynamic_cast<Unit*>(entity.get());
    if (!challenger) return;
    duelChallengerName_ = challenger->getName();
    if (duelChallengerName_.empty()) {
        auto nit = owner_.getPlayerNameCache().find(duelChallengerGuid_);
        if (nit != owner_.getPlayerNameCache().end()) duelChallengerName_ = nit->second;
    }
    // The popup is the whole announcement; no chat line.
    // No sound of ours: DUEL_REQUESTED raises StaticPopupDialogs.DUEL_REQUESTED,
    // whose sound = "igPlayerInvite" plays as it shows. The target-select click
    // played here as well was neither.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_REQUESTED", {duelChallengerName_});
}

void SocialHandler::handleDuelComplete(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t started = packet.readUInt8();
    pendingDuelRequest_ = false;
    // Wow.exe 0x5CFA90: with a flag known, ERR_DUEL_CANCELLED (the yellow
    // info text) for a duel that never began and DUEL_FINISHED; the
    // countdown stops whatever.
    const bool hadArbiter = duelArbiterGuid_ != 0;
    duelArbiterGuid_ = 0;
    duelChallengerGuid_ = 0;
    duelCountdown_.stop();
    if (!hadArbiter) return;
    if (!started) owner_.displayGameMessage("ERR_DUEL_CANCELLED", "Duel cancelled.");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_FINISHED", {});
}

void SocialHandler::handleDuelWinner(network::Packet& packet) {
    if (!packet.hasRemaining(3)) return;
    uint8_t duelType = packet.readUInt8();
    const std::string first  = packet.readString();
    const std::string second = packet.readString();
    // DUEL_WINNER_KNOCKOUT ("%1$s has defeated %2$s in a duel") or, for a
    // flight, DUEL_WINNER_RETREAT ("%2$s has fled from %1$s in a duel"),
    // a plain chat line (Wow.exe 0x5CFB20).
    if (duelType == 0)
        owner_.addGlobalStringChatLine("DUEL_WINNER_KNOCKOUT", "%1$s has defeated %2$s in a duel", {first, second});
    else
        owner_.addGlobalStringChatLine("DUEL_WINNER_RETREAT", "%2$s has fled from %1$s in a duel", {first, second});
}

// ============================================================
// Party / Raid
// ============================================================

void SocialHandler::inviteToGroup(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // 0x6D4300: the player's own name (any case) is ERR_INVITE_SELF and goes
    // no further; anything else goes to the server as typed.
    std::string me;
    if (const Character* active = owner_.getActiveCharacter()) me = active->name;
    if (me.empty()) me = owner_.lookupName(owner_.getPlayerGuid());
    if (!me.empty() && chat_rules::equalsNoCase(playerName, me)) {
        owner_.displayGameMessage("ERR_INVITE_SELF", "You can't invite yourself to a group.");
        return;
    }
    LOG_INFO("Sending CMSG_GROUP_INVITE to '", playerName, "'");
    auto packet = GroupInvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    LOG_WARNING(">>> Sending CMSG_GROUP_ACCEPT");
    pendingGroupInvite = false;
    auto packet = GroupAcceptPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::declineGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGroupInvite = false;
    auto packet = GroupDeclinePacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptArenaTeamInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_ACCEPT));
    owner_.getSocket()->send(packet);
}

void SocialHandler::declineArenaTeamInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DECLINE));
    owner_.getSocket()->send(packet);
}

// The four arena team commands that had no sender. Every one is the team id
// and, for three of them, a player name - the shape AzerothCore reads in
// ArenaTeamHandler.cpp: `recvData >> arenaTeamId >> name`. Disband and the two
// invite answers below were already built; these four were named by the
// interface's own popups and slash commands and reached bindings with nothing
// behind them, so the buttons closed and nothing happened.
void SocialHandler::arenaTeamInvite(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_INVITE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamLeave(uint32_t teamId) {
    if (teamId == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEAVE));
    packet.writeUInt32(teamId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamRemove(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_REMOVE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamSetLeader(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEADER));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::disbandArenaTeam(uint32_t teamId) {
    if (teamId == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DISBAND));
    packet.writeUInt32(teamId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::reportMailSpam(uint64_t senderGuid, uint32_t mailId) {
    if (senderGuid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_COMPLAIN));
    packet.writeUInt8(0);              // 0 = mail, 1 = chat
    packet.writeUInt64(senderGuid);
    packet.writeUInt32(0);
    packet.writeUInt32(mailId);
    packet.writeUInt32(0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveGroup() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GroupDisbandPacket::build();
    owner_.getSocket()->send(packet);
    partyData = GroupListData{};
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
    }
}

void SocialHandler::convertToRaid() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (!isInGroup()) {
        owner_.raiseUiError("You are not in a group.");
        return;
    }
    if (partyData.leaderGuid != owner_.getPlayerGuid()) {
        owner_.raiseUiError("You must be the party leader to convert to raid.");
        return;
    }
    if (groupIsRaid(partyData.groupType)) {
        owner_.raiseUiError("You are already in a raid group.");
        return;
    }
    auto packet = GroupRaidConvertPacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_GROUP_RAID_CONVERT");
}

void SocialHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = SetLootMethodPacket::build(method, threshold, masterLooterGuid);
    owner_.getSocket()->send(packet);
    LOG_INFO("sendSetLootMethod: method=", method, " threshold=", threshold);
}

// The server checks that this player leads the group and that the target is in
// it, so there is nothing to validate here beyond having a guid to send.
void SocialHandler::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildBankSetTabTextPacket::build(tab, text);
    owner_.getSocket()->send(packet);
}

void SocialHandler::channelModeration(Opcode op, const std::string& channelName,
                                     const std::string& targetName,
                                     bool allowEmptyTarget) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // An empty second string is meaningless for the moderation commands - there
    // is no player called nothing - but it is how a channel password is
    // cleared, so that one caller says it means it.
    if (channelName.empty() || (targetName.empty() && !allowEmptyTarget)) return;
    auto packet = ChannelModerationPacket::build(op, channelName, targetName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::promoteToLeader(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    auto packet = GroupSetLeaderPacket::build(guid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::uninvitePlayer(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) { owner_.raiseUiError("You must specify a player name to uninvite."); return; }
    // Nothing said (Wow.exe UninviteUnit 0x51A7A0): the roster update is
    // the answer.
    auto packet = GroupUninvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

// CMSG_LOOT_METHOD carries all three settings together, so changing one means
// resending the other two as they stand. The master looter is only meaningful
// for method 2 and is sent as an empty guid otherwise.
// Removes the lowest rank, which is what the server does with this - it takes
// no index, so there is nothing to get wrong about which one goes.
void SocialHandler::delGuildRank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDelRankPacket::build();
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT_METHOD));
    packet.writeUInt32(method);
    packet.writeUInt64(method == 2 ? masterGuid : 0);
    packet.writeUInt32(threshold);
    owner_.getSocket()->send(packet);
}

// MSG_PARTY_ASSIGNMENT: which assignment (0 main tank, 1 main assist), whether
// it is being given or taken away, and who.
void SocialHandler::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    network::Packet packet(wireOpcode(Opcode::MSG_PARTY_ASSIGNMENT));
    packet.writeUInt8(assignment);
    packet.writeUInt8(apply ? 1 : 0);
    packet.writeUInt64(guid);
    owner_.getSocket()->send(packet);
}

// Move a raid member into a different group of eight. The server takes the
// player's name and the destination group as a zero-based index, while the raid
// UI counts its groups from one.
void SocialHandler::setRaidSubgroup(const std::string& playerName, uint8_t group) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty() || group < 1 || group > 8) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP));
    packet.writeString(playerName);
    packet.writeUInt8(static_cast<uint8_t>(group - 1));
    owner_.getSocket()->send(packet);
}

// Exchange two raid members' places. Used when the destination group is already
// full, which is why it is a separate packet rather than two moves.
void SocialHandler::swapRaidSubgroup(const std::string& firstName, const std::string& secondName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (firstName.empty() || secondName.empty()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_SWAP_SUB_GROUP));
    packet.writeString(firstName);
    packet.writeString(secondName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveParty() {
    // Delegates to leaveGroup which handles both the packet send AND local
    // state cleanup (clearing partyData, firing addon events). Previously
    // this method only sent the packet, leaving the client thinking it was
    // still in a group until the server pushed a group list update.
    leaveGroup();
}

// /maintank and /mainassist are party assignments (SetPartyAssignment,
// MSG_PARTY_ASSIGNMENT), not raid target icons: these sent
// MSG_RAID_TARGET_UPDATE with icon 0 or 1, which put a star or a circle on the
// target and assigned nothing. The roster update announces the change; there
// is no line of the client's own.
void SocialHandler::setMainTank(uint64_t targetGuid) {
    if (targetGuid == 0) { owner_.raiseUiError("You must have a target selected."); return; }
    setPartyAssignment(0, targetGuid, true);
}

void SocialHandler::setMainAssist(uint64_t targetGuid) {
    if (targetGuid == 0) { owner_.raiseUiError("You must have a target selected."); return; }
    setPartyAssignment(1, targetGuid, true);
}

namespace {
// GroupMemberFlags: MEMBER_FLAG_MAINTANK 0x02, MEMBER_FLAG_MAINASSIST 0x04.
uint64_t memberWithFlag(const GroupListData& group, uint8_t flag) {
    for (const auto& m : group.members)
        if (m.flags & flag) return m.guid;
    return 0;
}
}  // namespace

void SocialHandler::clearMainTank() {
    setPartyAssignment(0, memberWithFlag(partyData, 0x02), false);
}

void SocialHandler::clearMainAssist() {
    setPartyAssignment(1, memberWithFlag(partyData, 0x04), false);
}

void SocialHandler::setRaidMarkLocally(uint64_t guid, uint8_t icon) {
    // Mirrors what the server does for a group: an icon is unique, and a unit
    // carries at most one mark, so placing one clears any previous holder.
    if (icon == 0xFF) {
        for (uint32_t i = 0; i < kRaidMarkCount; ++i) {
            if (raidTargetGuids_[i] == guid) raidTargetGuids_[i] = 0;
        }
    } else if (icon < kRaidMarkCount) {
        for (uint32_t i = 0; i < kRaidMarkCount; ++i) {
            if (raidTargetGuids_[i] == guid) raidTargetGuids_[i] = 0;
        }
        raidTargetGuids_[icon] = guid;
    } else {
        return;
    }
    owner_.fireAddonEvent("RAID_TARGET_UPDATE", {});
}

void SocialHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Raid marks are group state on the server: it drops MSG_RAID_TARGET_UPDATE
    // from an ungrouped player and broadcasts nothing back, so a solo mark used
    // to do nothing at all. Apply it locally instead - a deliberate divergence
    // from Blizzlike behaviour, since a solo player marking their own targets
    // harms nobody and the alternative is a feature that cannot be used, or
    // tested, without a second player. Joining a group later is safe: the
    // server's full list is authoritative and replaces these marks wholesale.
    if (!isInGroup()) {
        setRaidMarkLocally(guid, icon);
        return;
    }
    if (icon == 0xFF) {
        for (int i = 0; i < 8; ++i) {
            if (raidTargetGuids_[i] == guid) {
                auto packet = RaidTargetUpdatePacket::build(static_cast<uint8_t>(i), 0);
                owner_.getSocket()->send(packet);
                break;
            }
        }
    } else if (icon < 8) {
        auto packet = RaidTargetUpdatePacket::build(icon, guid);
        owner_.getSocket()->send(packet);
    }
}

void SocialHandler::setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // uint32 mapId, uint32 difficulty, uint8 toggle - HandleSetSavedInstanceExtend.
    // The server ignores it unless the bind is permanent and the flag actually
    // changes, so there is nothing to check here that it does not check better.
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_SAVED_INSTANCE_EXTEND));
    packet.writeUInt32(mapId);
    packet.writeUInt32(difficulty);
    packet.writeUInt8(extend ? 1 : 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestRaidInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RequestRaidInfoPacket::build();
    owner_.getSocket()->send(packet);
    // Not said in chat. The interface asks for this at every login - it is what
    // fills the Raid Info panel - so the line was three words of noise in the
    // first frame of every session, describing a request nobody made. The
    // player-facing answer belongs to whoever asked: /raidinfo prints what came
    // back, and the panel draws it.
    LOG_INFO("CMSG_REQUEST_RAID_INFO: asked for saved instance lockouts");
}

// ============================================================
// Group Handlers
// ============================================================

void SocialHandler::handleGroupInvite(network::Packet& packet) {
    GroupInviteResponseData data;
    const bool hasCanAccept = !isPreWotlk();
    if (!GroupInviteResponseParser::parse(packet, data, hasCanAccept)) return;

    // The leading byte is not decoration: AzerothCore sends this same opcode
    // with a zero in it to say "you were invited and it could not be offered,
    // because you are already in a group" - its own comment beside the write,
    // and the interface carries Blizzard's sentence for exactly that case
    // (ERR_INVITED_ALREADY_IN_GROUP_SS).
    //
    // It was read off the wire and then ignored, so that refusal raised the
    // accept/decline popup, played the invitation sound and left an invite
    // pending that the server would never honour. Answering it is a refusal,
    // not an offer.
    if (!data.canAccept) {
        owner_.displayGameMessage("ERR_INVITED_ALREADY_IN_GROUP_SS",
            "|Hplayer:" + data.inviterName + "|h[" + data.inviterName +
            "]|h has invited you to join a group, but you cannot accept because you are already in a group.",
            {data.inviterName, data.inviterName});
        return;
    }

    pendingGroupInvite = true;
    pendingInviterName = data.inviterName;
    // The popup and a chat line: Wow.exe (0x6DF1A0) raises
    // PARTY_INVITE_REQUEST and then ERR_INVITED_TO_GROUP_SS.
    // The invite sound belongs to the popup: uiparent.lua answers this event
    // with StaticPopup_Show("PARTY_INVITE"), whose entry carries
    // sound = "igPlayerInvite". Playing the quest-accept sound here as well
    // put a second, wrong sound under it.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("PARTY_INVITE_REQUEST", {data.inviterName});
    owner_.displayGameMessage("ERR_INVITED_TO_GROUP_SS",
        "|Hplayer:" + data.inviterName + "|h[" + data.inviterName + "]|h has invited you to join a group.",
        {data.inviterName, data.inviterName});
}

void SocialHandler::handleGroupDecline(network::Packet& packet) {
    GroupDeclineData data;
    if (!GroupDeclineResponseParser::parse(packet, data)) return;
    // ERR_DECLINE_GROUP_S, a chat line with igPlayerInviteDecline (0x6CBD40).
    owner_.displayGameMessage("ERR_DECLINE_GROUP_S",
                              data.playerName + " declines your group invitation.", {data.playerName});
}

void SocialHandler::handleGroupList(network::Packet& packet) {
    const bool hasRoles = isActiveExpansion("wotlk");
    const bool hasBattleGroupFlag = isActiveExpansion("tbc");
    const uint8_t prevLootMethod = partyData.lootMethod;
    const uint64_t prevLooter = partyData.looterGuid;
    const uint8_t prevThreshold = partyData.lootThreshold;
    const bool wasInGroup = !partyData.isEmpty();
    const bool wasRaid = wasInGroup && groupIsRaid(partyData.groupType);
    const uint8_t prevSubGroup = partyData.subGroup;
    std::vector<uint64_t> prevMembers;
    prevMembers.reserve(partyData.members.size());
    for (const auto& m : partyData.members) prevMembers.push_back(m.guid);
    // The roster names the members; their health, level, zone and so on come
    // from SMSG_PARTY_MEMBER_STATS, which the server sends when they change
    // and not again with every roster. Kept across the reset below, or each
    // roster - a subgroup move, a loot change - blanked every member's stats
    // until their next stat packet.
    std::vector<GroupMember> prevStats = std::move(partyData.members);
    partyData = GroupListData{};
    if (!GroupListParser::parse(packet, partyData, hasRoles, hasBattleGroupFlag)) return;
    for (auto& m : partyData.members) {
        bool known = false;
        for (const auto& old : prevStats) {
            if (old.guid != m.guid) continue;
            m.curHealth = old.curHealth;   m.maxHealth = old.maxHealth;
            m.powerType = old.powerType;   m.curPower = old.curPower;
            m.maxPower = old.maxPower;     m.level = old.level;
            m.zoneId = old.zoneId;         m.posX = old.posX;   m.posY = old.posY;
            m.hasPartyStats = old.hasPartyStats;
            known = true;
            break;
        }
        if (known || m.guid == 0 || m.guid == owner_.getPlayerGuid()) continue;
        // A member new to the roster (0x52D410, 0x574F10): health and power
        // start at 1 of 1, so the frame shows a full bar rather than a dead
        // one, and a member out of view is asked for with
        // CMSG_REQUEST_PARTY_MEMBER_STATS. AzerothCore sends a member's stats
        // to the others only when they change, so without the request a bot
        // standing still stayed at nothing - "Dead" on the party frame.
        m.curHealth = 1; m.maxHealth = 1;
        m.curPower = 1;  m.maxPower = 1;
        if (!owner_.getEntityManager().hasEntity(m.guid)) requestPartyMemberStats(m.guid);
    }

    // igPlayerInviteAccept, which Wow.exe's roster handler (0x6d8870) plays
    // when someone in the player's own subgroup arrives: anyone at all when
    // the player was in no group, and in a party (not a raid) anyone who was
    // not on the last roster. Once per roster, however many arrived.
    {
        const bool raid = groupIsRaid(partyData.groupType);
        bool arrived = false;
        for (const auto& m : partyData.members) {
            if (m.guid == 0 || m.guid == owner_.getPlayerGuid() || m.subGroup != partyData.subGroup)
                continue;
            if (!wasInGroup ||
                (!raid && std::find(prevMembers.begin(), prevMembers.end(), m.guid) == prevMembers.end())) {
                arrived = true;
                break;
            }
        }
        if (arrived) owner_.playUiSoundByName("igPlayerInviteAccept");
    }

    // Who came and went, as Wow.exe says it. In a party (0x6D8870): a member
    // new to the player's subgroup "joins the party" (ERR_JOINED_GROUP_S) -
    // or, for a leader whose party has just formed, the last one listed - and
    // one who is gone "leaves the party" (ERR_LEFT_GROUP_S). In a raid
    // (0x574F10): ERR_RAID_YOU_JOINED on becoming one, then
    // ERR_RAID_MEMBER_ADDED_S / _REMOVED_S; ERR_RAID_YOU_LEFT when a raid
    // becomes a party (0x572F90). Names that are not known are not said.
    {
        const uint64_t me = owner_.getPlayerGuid();
        const bool nowRaid = groupIsRaid(partyData.groupType);
        const bool nowInGroupHere = !partyData.isEmpty();
        auto nameOf = [this](uint64_t guid, const std::string& listed) {
            return listed.empty() ? owner_.lookupName(guid) : listed;
        };
        auto wasMember = [&](uint64_t guid, bool sameSubgroupOnly) {
            for (const auto& p : prevStats)
                if (p.guid == guid && (!sameSubgroupOnly || p.subGroup == prevSubGroup)) return true;
            return false;
        };
        if (!nowRaid) {
            if (wasInGroup && !wasRaid) {
                for (const auto& m : partyData.members) {
                    if (m.guid == 0 || m.guid == me || m.subGroup != partyData.subGroup) continue;
                    if (wasMember(m.guid, true)) continue;
                    const std::string n = nameOf(m.guid, m.name);
                    if (!n.empty()) owner_.displayGameMessage("ERR_JOINED_GROUP_S", n + " joins the party.", {n});
                }
            } else if (!wasInGroup && nowInGroupHere && partyData.leaderGuid == me &&
                       !partyData.members.empty()) {
                const auto& last = partyData.members.back();
                const std::string n = nameOf(last.guid, last.name);
                if (!n.empty()) owner_.displayGameMessage("ERR_JOINED_GROUP_S", n + " joins the party.", {n});
            }
            // Not when the list is empty: that is the player leaving, said
            // by the leave or the uninvite.
            if (wasInGroup && !wasRaid && nowInGroupHere) {
                for (const auto& p : prevStats) {
                    if (p.guid == 0 || p.guid == me || p.subGroup != prevSubGroup) continue;
                    bool still = false;
                    for (const auto& m : partyData.members)
                        if (m.guid == p.guid && m.subGroup == partyData.subGroup) { still = true; break; }
                    if (still) continue;
                    const std::string n = nameOf(p.guid, p.name);
                    if (!n.empty()) owner_.displayGameMessage("ERR_LEFT_GROUP_S", n + " leaves the party.", {n});
                }
            }
            if (wasRaid) owner_.displayGameMessage("ERR_RAID_YOU_LEFT", "You have left the raid group.");
        } else {
            if (!wasRaid) {
                owner_.displayGameMessage("ERR_RAID_YOU_JOINED", "You have joined a raid group.");
            } else {
                for (const auto& m : partyData.members) {
                    if (m.guid == 0 || m.guid == me || wasMember(m.guid, false)) continue;
                    const std::string n = nameOf(m.guid, m.name);
                    if (!n.empty()) owner_.displayGameMessage("ERR_RAID_MEMBER_ADDED_S", n + " has joined the raid group.", {n});
                }
                for (const auto& p : prevStats) {
                    if (p.guid == 0 || p.guid == me) continue;
                    bool still = false;
                    for (const auto& m : partyData.members) if (m.guid == p.guid) { still = true; break; }
                    if (still) continue;
                    const std::string n = nameOf(p.guid, p.name);
                    if (!n.empty()) owner_.displayGameMessage("ERR_RAID_MEMBER_REMOVED_S", n + " has left the raid group.", {n});
                }
            }
        }
    }

    // The roster carries online state in isOnline; the per-member stat updates
    // carry it in the onlineStatus bitmask. Seed the bitmask from the roster so
    // both readers agree until the first SMSG_PARTY_MEMBER_STATS arrives -
    // partyData was just reset, so every onlineStatus is otherwise zero and
    // every member would read as offline.
    for (auto& m : partyData.members) {
        if (m.isOnline) m.onlineStatus |= 0x0001;
    }

    const bool nowInGroup = !partyData.isEmpty();
    if (!nowInGroup && wasInGroup) {
        // Said by the packet that ended it, not here: ERR_LEFT_GROUP_YOU with
        // the leave's command result, ERR_UNINVITE_YOU with the uninvite,
        // ERR_GROUP_DISBANDED with the group's end (Wow.exe 0x6CBEC0,
        // 0x6CBE10). Said here as well, each was printed twice.
        LOG_INFO("Left group");
    } else if (nowInGroup && !wasInGroup) {
        // No "joined a group with" line: that is this client's invention.
        LOG_INFO("Joined group with ", partyData.memberCount, " members");
    }
    // Loot method change: ERR_SET_LOOT_* ("Looting changed to Group Loot.").
    if (wasInGroup && nowInGroup && partyData.lootMethod != prevLootMethod) {
        static const char* const kKeys[] = {"ERR_SET_LOOT_FREEFORALL", "ERR_SET_LOOT_ROUNDROBIN",
                                            "ERR_SET_LOOT_MASTER", "ERR_SET_LOOT_GROUP",
                                            "ERR_SET_LOOT_NBG"};
        const char* methodName = lootMethodName(partyData.lootMethod);
        if (partyData.lootMethod < 5)
            owner_.displayGameMessage(kKeys[partyData.lootMethod],
                                      std::string("Looting changed to ") + methodName + ".");
    }
    // The party frames read the method, the master looter and the threshold
    // (GetLootMethod, GetLootThreshold) to draw the master-looter icon and
    // the loot menu, and reread them on this. A new master looter under the
    // same method, or a new threshold, is as much a change as a new method.
    if (wasInGroup && nowInGroup &&
        (partyData.lootMethod != prevLootMethod || partyData.looterGuid != prevLooter ||
         partyData.lootThreshold != prevThreshold)) {
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PARTY_LOOT_METHOD_CHANGED", {});
    }
    const bool nowRaid = nowInGroup && groupIsRaid(partyData.groupType);
    if (wasInGroup && !wasRaid && nowRaid && owner_.addonEventCallbackRef()) {
        // Not listened for by the stock FrameXML, which redraws on the
        // RAID_ROSTER_UPDATE below; fired for addons that watch for it.
        owner_.addonEventCallbackRef()("PARTY_CONVERTED_TO_RAID", {});
    }
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
        // The roles came with this roster and UnitGroupRolesAssigned reads them
        // straight out of it. The player frame draws its tank or healer badge
        // on this event and nothing else, so the roles were parsed, readable,
        // and never shown.
        owner_.addonEventCallbackRef()("PLAYER_ROLES_ASSIGNED", {});
        // Leaving a raid is a raid roster change too: RaidFrame_OnEvent
        // rebuilds (and empties) the raid tab and its pullouts on this, and
        // the player frame drops its "Group N" badge - fired only while the
        // roster was still a raid, both stayed up after the raid was gone.
        if (nowRaid || wasRaid)
            owner_.addonEventCallbackRef()("RAID_ROSTER_UPDATE", {});
    }
}

void SocialHandler::handleGroupUninvite(network::Packet& packet) {
    (void)packet;
    partyData = GroupListData{};
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
        owner_.addonEventCallbackRef()("RAID_ROSTER_UPDATE", {});
    }
    // ERR_UNINVITE_YOU, a chat line, and a pending summon is withdrawn
    // (Wow.exe 0x6CBE10).
    owner_.displayGameMessage("ERR_UNINVITE_YOU", "You have been removed from the group.");
    owner_.fireAddonEvent("CANCEL_SUMMON", {});
}

void SocialHandler::handlePartyCommandResult(network::Packet& packet) {
    PartyCommandResultData data;
    if (!PartyCommandResultParser::parse(packet, data)) return;
    // Every answer as Wow.exe's handler (0x6CBEC0) gives it: a success is
    // ERR_INVITE_PLAYER_S for an invite and ERR_LEFT_GROUP_YOU for a leave,
    // each failure its GameError, unknown codes nothing.
    const std::string& name = data.name;
    const uint32_t command = static_cast<uint32_t>(data.command);
    const uint32_t result = static_cast<uint32_t>(data.result);
    if (result == 0) {
        if (command == 0 && !name.empty()) {
            owner_.displayGameMessage("ERR_INVITE_PLAYER_S", "You have invited " + name + " to join your group.", {name});
        } else if (command == 2) {
            owner_.displayGameMessage("ERR_LEFT_GROUP_YOU", "You leave the group.");
            owner_.fireAddonEvent("CANCEL_SUMMON", {});
        }
        return;
    }
    struct Answer { uint32_t code; const char* key; const char* text; };
    static constexpr Answer kAnswers[] = {
        {1,  "ERR_BAD_PLAYER_NAME_S", "Cannot find player '%s'."},
        {2,  "ERR_TARGET_NOT_IN_GROUP_S", "%s is not in your party."},
        {3,  "ERR_TARGET_NOT_IN_INSTANCE_S", "%s is not in your instance."},
        {4,  "ERR_GROUP_FULL", "Your party is full."},
        {5,  "ERR_ALREADY_IN_GROUP_S", "%s is already in a group."},
        {6,  "ERR_NOT_IN_GROUP", "You aren't in a party."},
        {7,  "ERR_NOT_LEADER", "You are not the party leader."},
        {8,  "ERR_PLAYER_WRONG_FACTION", "Target is unfriendly."},
        {9,  "ERR_IGNORING_YOU_S", "%s is ignoring you."},
        {12, "ERR_LFG_PENDING", "You cannot do that while in a LFG queue."},
        {13, "ERR_INVITE_RESTRICTED", "Trial accounts cannot invite characters into groups."},
        {15, "ERR_INVITE_UNKNOWN_REALM", "You cannot invite players from that realm."},
        {16, "ERR_INVITE_NO_PARTY_SERVER", "Unable to contact party server."},
        {17, "ERR_INVITE_PARTY_BUSY", "Cannot do that now - the party server is busy."},
        {18, "ERR_PARTY_TARGET_AMBIGUOUS", "Target name is ambiguous."},
        {19, "ERR_PARTY_LFG_INVITE_RAID_LOCKED", "%s is locked to a different instance."},
        {20, "ERR_PARTY_LFG_BOOT_LIMIT", "You cannot vote to kick any more players in this group."},
        {22, "ERR_PARTY_LFG_BOOT_IN_PROGRESS", "A vote to kick is already in progress."},
        {23, "ERR_PARTY_LFG_BOOT_TOO_FEW_PLAYERS", "There are too few players in the group to vote to kick."},
        {25, "ERR_RAID_DISALLOWED_BY_LEVEL", "You cannot form a raid with players below level 10."},
        {26, "ERR_PARTY_LFG_BOOT_IN_COMBAT", "You cannot vote to kick while in combat."},
        {28, "ERR_PARTY_LFG_BOOT_DUNGEON_COMPLETE", "You cannot vote to kick once the dungeon is complete."},
        {29, "ERR_PARTY_LFG_BOOT_LOOT_ROLLS", "You cannot vote to kick with loot rolls pending."},
        {30, "ERR_PARTY_LFG_TELEPORT_IN_COMBAT", "You cannot teleport while in combat."},
    };
    if (result == 14) {
        // A swap of subgroups that failed, or an invite in combat.
        if (command == 4) {
            owner_.displayGameMessage("ERR_GROUP_SWAP_FAILED", "Your party could not be swapped.");
            owner_.fireAddonEvent("RAID_ROSTER_UPDATE", {});
        } else {
            owner_.displayGameMessage("ERR_INVITE_IN_COMBAT", "You can't invite players while in combat.");
        }
        return;
    }
    if (result == 27) {
        owner_.fireAddonEvent("VOTE_KICK_REASON_NEEDED", {name});
        return;
    }
    for (const auto& a : kAnswers) {
        if (a.code != result) continue;
        owner_.displayGameMessage(a.key, formatGlobalString(a.text, {name}), {name});
        return;
    }
}

void SocialHandler::handlePartyMemberStats(network::Packet& packet, bool isFull) {
    auto remaining = [&]() { return packet.getRemainingSize(); };
    const bool isWotLK = isActiveExpansion("wotlk");

    // SMSG_PARTY_MEMBER_STATS_FULL opens with a byte that marks an arena
    // opponent's stats (0x6CF9B0): the guid takes an opponent slot (0x54D100)
    // and a pet named in the packet takes the pet slot with it (0x54D1C0).
    // AzerothCore always writes zero.
    bool arenaOpponent = false;
    if (isFull) { if (remaining() < 1) return; arenaOpponent = packet.readUInt8() != 0; }

    const bool pmsTbc = isActiveExpansion("tbc");
    if (remaining() < (pmsTbc ? 8u : 1u)) return;
    uint64_t memberGuid = pmsTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < 4) return;
    uint32_t updateFlags = packet.readUInt32();

    if (arenaOpponent) {
        auto present = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };
        fireArenaOpponentEvents(arenaOpponents_.playerSeen(memberGuid, present));
    }

    game::GroupMember* member = nullptr;
    for (auto& m : partyData.members) {
        if (m.guid == memberGuid) { member = &m; break; }
    }
    if (!member) {
        // Only the pet guid is wanted from an opponent's stats.
        if (arenaOpponent && (updateFlags & 0x0400)) {
            uint64_t petGuid = 0;
            const uint32_t before = updateFlags & 0x03FF;
            const bool wotlk = isActiveExpansion("wotlk");
            if (before & 0x0001) packet.readUInt16();
            if (before & 0x0002) wotlk ? (void)packet.readUInt32() : (void)packet.readUInt16();
            if (before & 0x0004) wotlk ? (void)packet.readUInt32() : (void)packet.readUInt16();
            if (before & 0x0008) packet.readUInt8();
            for (uint32_t bit : {0x0010u, 0x0020u, 0x0040u, 0x0080u})
                if (before & bit) packet.readUInt16();
            if (before & 0x0100) { packet.readUInt16(); packet.readUInt16(); }
            if (before & 0x0200) {
                const uint64_t mask = packet.hasRemaining(8) ? packet.readUInt64() : 0;
                for (int i = 0; i < 64; ++i)
                    if (mask & (uint64_t(1) << i)) {
                        if (wotlk) { packet.readUInt32(); packet.readUInt8(); } else { packet.readUInt16(); }
                    }
            }
            if (packet.hasRemaining(8)) petGuid = packet.readUInt64();
            auto present = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };
            if (petGuid != 0) fireArenaOpponentEvents(arenaOpponents_.petSeen(petGuid, memberGuid, present));
        }
        packet.skipAll();
        return;
    }

    // Track the online transition so it can be announced once the packet is
    // fully read. isOnline is what the Lua bindings read; onlineStatus is what
    // this client's own panels read. Both describe the same fact, so keep them
    // in step here - otherwise UnitIsConnected answers with the stale roster
    // snapshot for as long as the group lasts.
    const bool wasOnline = (member->onlineStatus & 0x0001) != 0;
    bool onlineChanged = false;
    if (updateFlags & 0x0001) {
        if (remaining() >= 2) {
            member->onlineStatus = packet.readUInt16();
            const bool nowOnline = (member->onlineStatus & 0x0001) != 0;
            member->isOnline = nowOnline ? 1 : 0;
            onlineChanged = (nowOnline != wasOnline);
        }
    }
    if (updateFlags & 0x0002) {
        if (isWotLK) { if (remaining() >= 4) member->curHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->curHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0004) {
        if (isWotLK) { if (remaining() >= 4) member->maxHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->maxHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0008) { if (remaining() >= 1) member->powerType = packet.readUInt8(); }
    if (updateFlags & 0x0010) { if (remaining() >= 2) member->curPower = packet.readUInt16(); }
    if (updateFlags & 0x0020) { if (remaining() >= 2) member->maxPower = packet.readUInt16(); }
    if (updateFlags & 0x0040) { if (remaining() >= 2) member->level = packet.readUInt16(); }
    if (updateFlags & 0x0080) { if (remaining() >= 2) member->zoneId = packet.readUInt16(); }
    if (updateFlags & 0x0100) {
        if (remaining() >= 4) {
            member->posX = static_cast<int16_t>(packet.readUInt16());
            member->posY = static_cast<int16_t>(packet.readUInt16());
        }
    }
    if (updateFlags & 0x0200) {
        if (remaining() >= 8) {
            uint64_t auraMask = packet.readUInt64();
            std::vector<AuraSlot> newAuras;
            for (int i = 0; i < 64; ++i) {
                if (auraMask & (uint64_t(1) << i)) {
                    AuraSlot a;
                    a.level = static_cast<uint8_t>(i);
                    if (isWotLK) {
                        if (remaining() < 5) break;
                        a.spellId = packet.readUInt32();
                        a.flags   = packet.readUInt8();
                    } else {
                        if (remaining() < 2) break;
                        a.spellId = packet.readUInt16();
                        uint8_t dt = owner_.getSpellDispelType(a.spellId);
                        if (dt > 0) a.flags = 0x80;
                    }
                    if (a.spellId != 0) newAuras.push_back(a);
                }
            }
            if (memberGuid != 0 && memberGuid != owner_.getPlayerGuid() && memberGuid != owner_.getTargetGuid()) {
                owner_.unitAurasCacheRef()[memberGuid] = std::move(newAuras);
            }
        }
    }
    // Skip pet fields and vehicle seat
    if (updateFlags & 0x0400) { if (remaining() >= 8) packet.readUInt64(); }
    if (updateFlags & 0x0800) { if (remaining() > 0) packet.readString(); }
    if (updateFlags & 0x1000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x2000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x4000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x8000) { if (remaining() >= 1) packet.readUInt8(); }
    if (updateFlags & 0x10000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x20000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x40000) {
        if (remaining() >= 8) {
            uint64_t petAuraMask = packet.readUInt64();
            for (int i = 0; i < 64; ++i) {
                if (petAuraMask & (uint64_t(1) << i)) {
                    if (isWotLK) { if (remaining() < 5) break; packet.readUInt32(); packet.readUInt8(); }
                    else { if (remaining() < 2) break; packet.readUInt16(); }
                }
            }
        }
    }
    if (isWotLK && (updateFlags & 0x80000)) { if (remaining() >= 4) packet.readUInt32(); }

    member->hasPartyStats = true;

    if (owner_.addonEventCallbackRef()) {
        // Every token that names the member (0x6CF740 -> 0x60BF10): in a raid
        // both "raidN", in the raid list's order, and "partyN" for the
        // player's own subgroup. Only "raidN" was fired, and at the member's
        // place in the packet rather than in the raid list, so the party
        // frames of a battleground raid never heard of a change.
        std::vector<std::string> unitIds;
        const uint64_t me = owner_.getPlayerGuid();
        const bool raid = groupIsRaid(partyData.groupType);
        if (raid) {
            const std::string r = groupUnitToken(partyData, me, memberGuid);
            if (!r.empty()) unitIds.push_back(r);
        }
        {
            int found = 0;
            for (const auto& m : partyData.members) {
                if (m.guid == me) continue;
                if (raid && m.subGroup != partyData.subGroup) continue;
                ++found;
                if (m.guid == memberGuid) {
                    if (found <= 4) unitIds.push_back("party" + std::to_string(found));
                    break;
                }
            }
        }
        for (const auto& unitId : unitIds) {
            if (updateFlags & (0x0002 | 0x0004)) owner_.addonEventCallbackRef()("UNIT_HEALTH", {unitId});
            if (updateFlags & (0x0010 | 0x0020)) owner_.addonEventCallbackRef()("UNIT_POWER", {unitId});
            if (updateFlags & 0x0040) owner_.addonEventCallbackRef()("UNIT_LEVEL", {unitId});
            if (updateFlags & 0x0200) owner_.addonEventCallbackRef()("UNIT_AURA", {unitId});
        }
        // Fired without arguments, as the retail client does - the interface
        // re-reads the whole roster rather than acting on one member. Not
        // gated on unitId: the player's own stats resolve to no unit token,
        // and a member dropping is worth announcing either way.
        if (onlineChanged) {
            owner_.addonEventCallbackRef()(
                member->isOnline ? "PARTY_MEMBER_ENABLE" : "PARTY_MEMBER_DISABLE", {});
        }
    }
}

// ============================================================
// Guild Handlers
// ============================================================

void SocialHandler::handleGuildInfo(network::Packet& packet) {
    GuildInfoData data;
    if (!GuildInfoParser::parse(packet, data)) return;
    if (!data.guildName.empty()) {
        if (const Character* ch = owner_.getActiveCharacter(); ch && ch->hasGuild()) {
            rememberGuildName(ch->guildId, data.guildName);
            if (guildName_.empty()) guildName_ = data.guildName;
        }
    }
    // The answer to /guildinfo (GuildInfo(), CMSG_GUILD_INFO), which the
    // server sends only when asked. Wow.exe (0x6CC440) prints two plain chat
    // lines every time: GUILD_NAME_TEMPLATE and GUILD_INFO_TEMPLATE, the
    // date as month, day and year from the packed time.
    guildInfoData_ = data;
    const uint32_t t = data.createdPacked;
    const uint32_t day = (t >> 14) & 0x3F, month = (t >> 20) & 0x0F, year = (t >> 24) & 0x1F;
    owner_.addGlobalStringChatLine("GUILD_NAME_TEMPLATE", "Guild: %s", {data.guildName});
    owner_.addGlobalStringChatLine("GUILD_INFO_TEMPLATE",
        "Guild created %1$d-%2$d-%3$d, %4$d players, %5$d accounts",
        {std::to_string(month + 1), std::to_string(day + 1), std::to_string(year + 2000),
         std::to_string(data.numMembers), std::to_string(data.numAccounts)});
}

void SocialHandler::handleGuildRoster(network::Packet& packet) {
    GuildRosterData data;
    if (!owner_.getPacketParsers()->parseGuildRoster(packet, data)) return;
    guildRoster_ = std::move(data);
    hasGuildRoster_ = true;

    // A roster is only sent to a member, so this is the server saying so.
    //
    // Joining mid-session left every record of membership untouched: the
    // character list's guild id was taken at login and the guild name is only
    // written for a character that already had one. So the social window's
    // guild tab stayed empty for a player who had just accepted an invite, and
    // stayed empty until the next login.
    const bool newlyIn = !inGuild_;
    inGuild_ = true;
    if (guildName_.empty() && !joinedGuildName_.empty()) {
        guildName_ = joinedGuildName_;
    }
    if (guildName_.empty()) {
        // The name comes from the guild's own query, which needs the id - and
        // the id is on the player now that the server has moved them.
        if (const uint32_t id = ownGuildId()) queryGuildInfo(id);
    }
    if (newlyIn && owner_.addonEventCallbackRef()) {
        // What the social window listens for to rebuild its guild tab. Without
        // it the frame has a roster and no idea it is meant to draw one.
        owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
    }
    // False: the roster in hand is the fresh one, so nothing should ask again.
    // The argument is "you may request a roster", and every reader answers it
    // with `if arg1 then GuildRoster() end` - friendsframe, the guild bank and
    // the calendar all do exactly that. Sending true here would have each of
    // them request a roster, whose reply would fire this again, forever.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE", {eventBool(false)});
}

// CMSG_GUILD_RANK rewrites the rank whole: id, rights, name, gold per day, then
// six pairs of bank tab rights and slots. Everything it carries has to be sent,
// which is why the caller stages from the rank's current values rather than
// from nothing - a field left at zero here is not "unchanged", it is revoked.
void SocialHandler::saveGuildRank(uint32_t rankId, uint32_t rights, const std::string& rankName,
                                  uint32_t goldLimit, const uint32_t* tabRights,
                                  const uint32_t* tabSlots) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_RANK));
    packet.writeUInt32(rankId);
    packet.writeUInt32(rights);
    packet.writeString(rankName);
    packet.writeUInt32(goldLimit);
    for (int t = 0; t < 6; ++t) {
        packet.writeUInt32(tabRights ? tabRights[t] : 0);
        packet.writeUInt32(tabSlots ? tabSlots[t] : 0);
    }
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

uint32_t SocialHandler::getPlayerGuildRankIndex() const {
    if (!hasGuildRoster_) return 0xFFFFFFFFu;
    const uint64_t me = owner_.getPlayerGuid();
    for (const auto& m : guildRoster_.members)
        if (m.guid == me) return m.rankIndex;
    return 0xFFFFFFFFu;
}

uint32_t SocialHandler::getPlayerGuildRankRights() const {
    const uint32_t idx = getPlayerGuildRankIndex();
    if (idx >= guildRoster_.ranks.size()) return 0;
    return guildRoster_.ranks[idx].rights;
}

void SocialHandler::handleGuildQueryResponse(network::Packet& packet) {
    GuildQueryResponseData data;
    if (!owner_.getPacketParsers()->parseGuildQueryResponse(packet, data)) return;
    if (data.guildId != 0) {
        pendingGuildNameQueries_.erase(data.guildId);
        if (!data.guildName.empty()) {
            rememberGuildName(data.guildId, data.guildName);
        }
    }
    // Ours when it names the guild this character is actually in - which is
    // the player's own field once they have joined one, not the character
    // list's copy from login.
    const bool isLocalGuild = (ownGuildId() != 0 && ownGuildId() == data.guildId);
    if (isLocalGuild) {
        const bool wasUnknown = guildName_.empty();
        guildName_ = data.guildName;
        guildQueryData_ = data;
        guildRankNames_.clear();
        for (const auto& rankName : data.rankNames) guildRankNames_.push_back(rankName);
        if (wasUnknown && !guildName_.empty()) {
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
        }
    }
}

// Returns true only when the member's cached online state actually changes (and updates
// it), so callers announce genuine online/offline transitions and silently drop the
// redundant SIGNED_ON flood the server emits at login for already-online members.
bool SocialHandler::guildMemberOnlineTransition(const std::string& name, bool nowOnline) {
    for (auto& m : guildRoster_.members) {
        if (m.name == name) {
            if (m.online == nowOnline) return false;
            m.online = nowOnline;
            m.statusFlags = nowOnline ? 0x01 : 0x00;   // AFK/DND come with the next roster
            return true;
        }
    }
    return true;  // not in the cached roster yet - announce rather than swallow
}

void SocialHandler::handleGuildEvent(network::Packet& packet) {
    GuildEventData data;
    if (!GuildEventParser::parse(packet, data)) return;

    // What the game prints for each, in its own words (ERR_GUILD_* and the
    // friend list's online and offline lines) and as a system line. These went
    // out as guild chat with no sender, and the interface puts the player's own
    // name on a local line without one: "[Guild] [You]: [Guild] Coggs has come
    // online."
    std::string msg;
    bool guildLine = false;  // the MOTD, which is the one that is guild chat
    switch (data.eventType) {
        case GuildEvent::PROMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has promoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::DEMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has demoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::MOTD:
            // The interface writes this one itself, from the GUILD_MOTD event
            // fired further down: chatframe.lua formats GUILD_MOTD_TEMPLATE
            // and adds it. Only the client's own window needs a line here.
            if (data.numStrings >= 1 && !ui::frameXmlOwns(ui::UiElement::Chat)) {
                msg = "Guild Message of the Day: " + data.strings[0];
                guildLine = true;
            }
            break;
        case GuildEvent::JOINED:
            if (data.numStrings >= 1) msg = data.strings[0] + " has joined the guild.";
            break;
        case GuildEvent::LEFT:
            if (data.numStrings >= 1) msg = data.strings[0] + " has left the guild.";
            break;
        case GuildEvent::REMOVED:
            // The member removed, then who removed them - the order the server
            // sends and ERR_GUILD_REMOVE_SS reads them in.
            if (data.numStrings >= 2) msg = data.strings[0] + " has been kicked out of the guild by " + data.strings[1] + ".";
            break;
        case GuildEvent::LEADER_IS:
            if (data.numStrings >= 1) msg = data.strings[0] + " is the leader of your guild.";
            break;
        case GuildEvent::LEADER_CHANGED:
            if (data.numStrings >= 2) msg = data.strings[0] + " has made " + data.strings[1] + " the new Guild Master.";
            break;
        case GuildEvent::DISBANDED:
            msg = "Guild has been disbanded.";
            guildName_.clear();
            guildRankNames_.clear();
            guildRoster_ = GuildRosterData{};
            hasGuildRoster_ = false;
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
            break;
        case GuildEvent::SIGNED_ON:
            // Only announce a real offline→online transition. On login the server floods
            // SIGNED_ON for members the roster already lists as online; suppressing those
            // (state unchanged) matches the real client and stops the guild-chat spam.
            //
            // ...and only when the player wants to hear about it at all. Guild
            // Member Notify is offered by the panel and was read by nothing, so
            // a large guild announced every arrival and departure whatever it
            // said. The transition is still tracked when the message is
            // suppressed - the roster's idea of who is online is not the
            // player's idea of what is worth reading.
            //
            // A member who is also on the friends list is left to the friend
            // list's own line, as Wow.exe does (0x6d92d0 checks the friends
            // list first): the two announced the same arrival twice. The line
            // is ERR_FRIEND_ONLINE_SS and plays its sound, FRIENDJOINGAME.
            if (data.numStrings >= 1 && guildMemberOnlineTransition(data.strings[0], true) &&
                addons::storedCVarValue("guildMemberNotify", "1") != "0" &&
                owner_.friendsCacheRef().count(data.strings[0]) == 0) {
                msg = "|Hplayer:" + data.strings[0] + "|h[" + data.strings[0] + "]|h has come online.";
                owner_.playUiMessageSound("ERR_FRIEND_ONLINE_SS");
            }
            break;
        case GuildEvent::SIGNED_OFF:
            if (data.numStrings >= 1 && guildMemberOnlineTransition(data.strings[0], false) &&
                addons::storedCVarValue("guildMemberNotify", "1") != "0" &&
                owner_.friendsCacheRef().count(data.strings[0]) == 0)
                msg = data.strings[0] + " has gone offline.";
            break;
        // The bank's half of the event list. The server broadcasts these to
        // every member whenever anyone touches the bank, so they are frequent,
        // and not one of them is a chat line - they carry the figures the bank
        // panel reads back through its own API. Unhandled, a guild with an
        // active bank filled guild chat with "Guild event 17: 000000000051451B".
        case GuildEvent::BANK_MONEY_SET:
        case GuildEvent::BANK_TAB_AND_MONEY:
            if (data.numStrings >= 1)
                owner_.setGuildBankMoney(guildEventMoney(data.strings[0]));
            break;
        case GuildEvent::BANK_BAG_SLOTS_CHANGED:
        case GuildEvent::BANK_TAB_PURCHASED:
        case GuildEvent::BANK_TAB_UPDATED:
        case GuildEvent::BANK_TEXT_CHANGED:
        case GuildEvent::TABARD_CHANGED:
        case GuildEvent::RANK_UPDATED:
        case GuildEvent::RANK_DELETED:
            // Silent here; the interface events they mean are fired below.
            break;
        default: {
            // Not a chat line either: an event this client does not know is a
            // gap in this switch, and guild chat is not where that belongs.
            // Once per type - these arrive as often as the bank ones do, and a
            // log that repeats itself answers nothing a single line does not.
            static bool warned[256] = {};
            if (!warned[data.eventType]) {
                warned[data.eventType] = true;
                LOG_WARNING("SMSG_GUILD_EVENT: unhandled type ", static_cast<int>(data.eventType),
                            " strings=", static_cast<int>(data.numStrings),
                            data.numStrings >= 1 ? (" first=" + data.strings[0]) : std::string());
            }
            break;
        }
    }

    if (!msg.empty()) {
        MessageChatData chatMsg;
        chatMsg.type = guildLine ? ChatType::GUILD : ChatType::SYSTEM;
        chatMsg.language = ChatLanguage::UNIVERSAL;
        chatMsg.message = msg;
        owner_.addLocalChatMessage(chatMsg);
    }

    // Whether this client is about to ask for a fresh roster itself, which is
    // the switch at the end of this function. The event's argument is the
    // negation: "nobody has asked, so you may". Working it out once and using
    // it in both places is the point - FrameXML requesting when the client
    // already has costs a second roster for a large guild, and FrameXML not
    // requesting when the client has not leaves every guild panel drawing from
    // a copy that no longer says who is online.
    const bool clientWillRequest = hasGuildRoster_ && (
        data.eventType == GuildEvent::PROMOTION ||
        data.eventType == GuildEvent::DEMOTION ||
        data.eventType == GuildEvent::JOINED ||
        data.eventType == GuildEvent::LEFT ||
        data.eventType == GuildEvent::REMOVED ||
        data.eventType == GuildEvent::LEADER_CHANGED ||
        // A rank added, renamed or deleted only reaches the client in the
        // roster: SMSG_GUILD_ROSTER is what carries the rank list, and every
        // panel that names a rank reads it from there.
        data.eventType == GuildEvent::RANK_UPDATED ||
        data.eventType == GuildEvent::RANK_DELETED);

    if (owner_.addonEventCallbackRef()) {
        switch (data.eventType) {
            case GuildEvent::MOTD:
                // GetGuildRosterMOTD answers from the roster, and the event
                // is the only word of a new MOTD until the next roster: the
                // GUILD_MOTD handler's own GetGuildRosterMOTD() read the old one.
                guildRoster_.motd = data.numStrings >= 1 ? data.strings[0] : "";
                owner_.addonEventCallbackRef()("GUILD_MOTD", {guildRoster_.motd});
                break;
            case GuildEvent::SIGNED_ON: case GuildEvent::SIGNED_OFF:
            case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
            case GuildEvent::JOINED: case GuildEvent::LEFT:
            case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
            case GuildEvent::DISBANDED:
            case GuildEvent::RANK_UPDATED: case GuildEvent::RANK_DELETED:
                // Somebody joined, left, was promoted or signed on, and the
                // roster this client holds no longer says so. The server sends
                // the event and not a new roster, so the request has to come
                // from somewhere. Signing on and off is the case nothing
                // covered: the switch below does not request for those, so a
                // member who had just come online stayed grey until something
                // else asked.
                owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE",
                                               {eventBool(!clientWillRequest)});
                break;
            // The bank panel's own events, which are what these types mean.
            // GuildBankFrame_OnEvent returns early unless the frame is
            // visible, so firing them while it is shut costs nothing.
            case GuildEvent::TABARD_CHANGED:
                owner_.addonEventCallbackRef()("GUILDTABARD_UPDATE", {});
                break;
            case GuildEvent::BANK_BAG_SLOTS_CHANGED:
                owner_.addonEventCallbackRef()("GUILDBANKBAGSLOTS_CHANGED", {});
                break;
            case GuildEvent::BANK_TAB_PURCHASED:
            case GuildEvent::BANK_TAB_UPDATED:
            case GuildEvent::BANK_TAB_AND_MONEY:
                // The money half of 18 went through setGuildBankMoney above,
                // which fires GUILDBANK_UPDATE_MONEY when the figure moved.
                owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_TABS", {});
                break;
            case GuildEvent::BANK_TEXT_CHANGED:
                // The tab, as the server counts them from zero and the panel
                // counts them from one. It answers by querying the new text.
                if (data.numStrings >= 1)
                    owner_.addonEventCallbackRef()(
                        "GUILDBANK_TEXT_CHANGED",
                        {std::to_string(std::strtol(data.strings[0].c_str(), nullptr, 10) + 1)});
                break;
            default: break;
        }
    }

    // A new tabard is in the guild query, not the roster, so it is the query
    // that has to be asked again for the emblem to change.
    if (data.eventType == GuildEvent::TABARD_CHANGED) {
        if (const uint32_t id = ownGuildId()) queryGuildInfo(id);
    }

    if (clientWillRequest) requestGuildRoster();
}

void SocialHandler::handleGuildInvite(network::Packet& packet) {
    GuildInviteResponseData data;
    if (!GuildInviteResponseParser::parse(packet, data)) return;
    pendingGuildInvite_ = true;
    pendingGuildInviterName_ = data.inviterName;
    pendingGuildInviteGuildName_ = data.guildName;
    // ERR_INVITED_TO_GUILD_SSS, a chat line with LEVELUP (Wow.exe 0x6CC3B0).
    owner_.displayGameMessage("ERR_INVITED_TO_GUILD_SSS",
        data.inviterName + " has invited you to join the guild, " + data.guildName + ".",
        {data.inviterName, data.inviterName, data.guildName});
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILD_INVITE_REQUEST", {data.inviterName, data.guildName});
}

void SocialHandler::handleGuildCommandResult(network::Packet& packet) {
    GuildCommandResultData data;
    if (!GuildCommandResultParser::parse(packet, data)) return;
    const std::string& name = data.name;
    // Every answer as Wow.exe's handler (0x6CC590) gives it - all of them
    // GameErrors, most of them chat lines - and nothing for the codes it
    // does not know. Commands and codes are AzerothCore's GuildCommandType
    // and GuildCommandError (Guild.h): a quit is command 3.
    if (data.errorCode == 0) {
        // Whether the player's own membership changed, which is a different
        // question from whether the command worked: inviting someone succeeds
        // without altering anything the panels read.
        bool membershipChanged = false;
        switch (data.command) {
            case 0:
                owner_.displayGameMessage("ERR_GUILD_CREATE_S", name + " created.", {name});
                membershipChanged = true;
                break;
            case 1:
                if (!name.empty())
                    owner_.displayGameMessage("ERR_GUILD_INVITE_S",
                        "You have invited " + name + " to join your guild.", {name});
                break;
            case 3:
                owner_.displayGameMessage("ERR_GUILD_QUIT_S", "You are no longer a member of " + name + ".", {name});
                guildName_.clear(); guildRankNames_.clear();
                guildRoster_ = GuildRosterData{}; hasGuildRoster_ = false;
                membershipChanged = true;
                break;
            case 14:
                owner_.displayGameMessage("ERR_GUILD_FOUNDER_S",
                    "Congratulations, you are a founding member of " + name + "!", {name});
                break;
            case 19:
            case 20:
                // A note changed: the roster is stale, and the stock client
                // clears GuildRoster's throttle and says so with
                // GUILD_ROSTER_UPDATE(true), which the guild frame answers by
                // asking again (0x5CA0F0).
                clearGuildRosterThrottle();
                owner_.fireAddonEvent("GUILD_ROSTER_UPDATE", {eventBool(true)});
                break;
            default: break;
        }
        // Leaving a guild emptied the name, the ranks and the whole roster and
        // told nobody, so the interface's guild tab kept showing the guild the
        // player had just left - every row of it, until something else
        // happened to fire a roster update. PLAYER_GUILD_UPDATE is what
        // friendsframe and the paperdoll re-check IsInGuild on.
        //
        // The roster event carries false, which is the established contract
        // here: every reader answers a true with `if arg1 then GuildRoster()
        // end`, and the reply to that would fire this again without end.
        if (membershipChanged && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
            owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE", {eventBool(false)});
        }
        return;
    }
    struct Answer { uint32_t code; const char* key; const char* text; };
    static constexpr Answer kAnswers[] = {
        {1,  "ERR_GUILD_INTERNAL", "Internal guild error."},
        {2,  "ERR_ALREADY_IN_GUILD", "You are already in a guild."},
        {3,  "ERR_ALREADY_IN_GUILD_S", "%s is already in a guild."},
        {4,  "ERR_INVITED_TO_GUILD", "You have already been invited into a guild."},
        {5,  "ERR_ALREADY_INVITED_TO_GUILD_S", "%s has already been invited into a guild."},
        {7,  "ERR_GUILD_NAME_EXISTS_S", "There is already a guild named \"%s\"."},
        {9,  "ERR_GUILD_PLAYER_NOT_IN_GUILD", "You are not in a guild."},
        {10, "ERR_GUILD_PLAYER_NOT_IN_GUILD_S", "%s is not in your guild."},
        {11, "ERR_GUILD_PLAYER_NOT_FOUND_S", "\"%s\" not found."},
        {12, "ERR_GUILD_NOT_ALLIED", "You cannot invite players from the opposing alliance."},
        {13, "ERR_GUILD_RANK_TOO_HIGH_S", "%s's rank is too high."},
        {14, "ERR_GUILD_RANK_TOO_LOW_S", "%s is already at the lowest rank."},
        {17, "ERR_GUILD_RANKS_LOCKED", "Temporary guild error.  Please try again!"},
        {18, "ERR_GUILD_RANK_IN_USE", "That guild rank is currently in use."},
        {19, "ERR_IGNORING_YOU_S", "%s is ignoring you."},
        {25, "ERR_GUILD_WITHDRAW_LIMIT", "You cannot withdraw that much from the guild bank."},
        {26, "ERR_GUILD_NOT_ENOUGH_MONEY", "The guild bank does not have enough money"},
        {28, "ERR_GUILD_BANK_FULL", "This guild bank tab is full"},
        {29, "ERR_ITEM_NOT_FOUND", "The item was not found."},
    };
    const uint32_t code = data.errorCode;
    if (code == 6) {
        // An invalid guild name, worded by the name-check results (0x5218C0
        // with 1: ERR_NAME_INVALID).
        owner_.displayGameMessage("ERR_NAME_INVALID", "Invalid name");
        return;
    }
    if (code == 8) {
        if (data.command == 3)
            owner_.displayGameMessage("ERR_GUILD_LEADER_LEAVE", "You must promote a new Guild Master using /gleader before leaving the guild.");
        else
            owner_.displayGameMessage("ERR_GUILD_PERMISSIONS", "You don't have permission to do that.");
        return;
    }
    if (code == 20) {
        if (data.command == 5) {
            clearGuildRosterThrottle();
            owner_.fireAddonEvent("GUILD_ROSTER_UPDATE", {eventBool(true)});
        }
        return;
    }
    for (const auto& a : kAnswers) {
        if (a.code != code) continue;
        owner_.displayGameMessage(a.key, formatGlobalString(a.text, {name}), {name});
        return;
    }
    LOG_DEBUG("SMSG_GUILD_COMMAND_RESULT: command ", data.command, " error ", code, " (silent)");
}

void SocialHandler::handlePetitionShowlist(network::Packet& packet) {
    PetitionShowlistData data;
    if (!PetitionShowlistParser::parse(packet, data)) return;
    petitionNpcGuid_ = data.npcGuid;
    petitionCost_ = data.cost;
    // The whole offer, not just the first charter's price. An arena registrar
    // offers three and the panel prices whichever tab is open.
    petitionCharters_ = data.charters;
    showPetitionDialog_ = true;
    petitionIsGuildCharter_ = data.isGuildCharter();
    // The vendor panel opens on this. showPetitionDialog_ is what this
    // client's own dialog reads, and the interface's version was told
    // nothing, so walking up to a guild master or an arena registrar opened
    // one of the two windows and never the other.
    //
    // Two panels, one opcode: the guild registrar and the arena registrar are
    // told apart by which charter is on offer, not by what was sent. Firing
    // the arena event at a tabard designer put the wrong window on screen.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()(
            petitionIsGuildCharter_ ? "GUILD_REGISTRAR_SHOW" : "PETITION_VENDOR_SHOW", {});
    }
}

void SocialHandler::closePetitionVendor() {
    if (!showPetitionDialog_) return;
    showPetitionDialog_ = false;
    petitionNpcGuid_ = 0;
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()(
            petitionIsGuildCharter_ ? "GUILD_REGISTRAR_CLOSED" : "PETITION_VENDOR_CLOSED", {});
    }
}

void SocialHandler::handlePetitionQueryResponse(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 12) return;
    /*uint32_t entry =*/ packet.readUInt32();
    uint64_t petGuid = packet.readUInt64();
    std::string guildName = packet.readString();
    /*std::string body =*/ packet.readString();

    // Three words the reply carries that used to be skipped along with the
    // rest of it. The first is how many signatures the charter needs - nothing
    // else on the wire says, so signaturesRequired sat at its default of nine
    // for every charter, and the counter under the signature list read "x / 9"
    // whatever the realm was configured for. CanSignPetition compares against
    // the same number, so the Sign button stayed live past the real
    // requirement.
    //
    // The third tells a guild charter from an arena one: AzerothCore writes
    // zero for a guild and the team size otherwise. GetPetitionInfo answered
    // "guild" for everything, which is the wrong heading and the wrong
    // confirmation on all three arena charters.
    uint32_t needed = 0, arenaType = 0;
    if (rem() >= 12) {
        needed = packet.readUInt32();
        /*uint32_t neededAgain =*/ packet.readUInt32();
        arenaType = packet.readUInt32();
    }
    packet.skipAll();

    if (petitionInfo_.petitionGuid != petGuid) return;
    petitionInfo_.guildName = guildName;
    if (needed > 0) petitionInfo_.signaturesRequired = needed;
    petitionInfo_.isArena = arenaType != 0;

    // Said again, because the frame was drawn when the signatures arrived and
    // everything above lands after it. Without this the charter keeps the
    // blank name and the placeholder count it opened with - the shape that
    // reads as "only right after reopening it".
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PETITION_SHOW", {});
}

void SocialHandler::handlePetitionShowSignatures(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 21) return;
    petitionInfo_ = PetitionInfo{};
    petitionInfo_.petitionGuid = packet.readUInt64();
    petitionInfo_.ownerGuid    = packet.readUInt64();
    const uint32_t petEntry    = packet.readUInt32();
    uint8_t sigCount           = packet.readUInt8();
    petitionInfo_.signatureCount = sigCount;
    petitionInfo_.signatures.reserve(sigCount);
    for (uint8_t i = 0; i < sigCount; ++i) {
        if (rem() < 12) break;
        PetitionSignature sig;
        sig.playerGuid = packet.readUInt64();
        /*uint32_t unk =*/ packet.readUInt32();
        petitionInfo_.signatures.push_back(sig);
    }
    petitionInfo_.showUI = true;

    // Ask what this charter actually is. Everything the frame needs beyond the
    // signatures - the name, the number of signatures required, and whether it
    // is a guild or an arena charter - comes only from the query reply, and
    // this client never sent the query. The opcode was in the table and the
    // reply had a handler waiting for it; nothing ever asked, so the handler
    // never ran and the frame opened on a nameless charter needing nine
    // signatures whatever it was.
    if (owner_.getSocket()) {
        auto query = PetitionQueryPacket::build(petEntry, petitionInfo_.petitionGuid);
        owner_.getSocket()->send(query);
    }

    // The signature list frame opens on this. The petition is parsed and
    // stored above - showUI is what this client's own window reads - and the
    // interface's version was never told, so a charter could not be signed
    // through it.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PETITION_SHOW", {});
}

void SocialHandler::handlePetitionSignResults(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 20) return;
    uint64_t petGuid    = packet.readUInt64();
    uint64_t playerGuid = packet.readUInt64();
    uint32_t result     = packet.readUInt32();
    if (result == 0 && petitionInfo_.petitionGuid == petGuid) {
        petitionInfo_.signatureCount++;
        PetitionSignature sig; sig.playerGuid = playerGuid;
        petitionInfo_.signatures.push_back(sig);
    }
    // Wow.exe (0x6CEFD0): a result naming someone else is the owner hearing
    // of a signature (0x5CF710, ERR_PETITION_SIGNED_S); the player's own is
    // one GameError per PetitionSigns code, and anything else goes to the
    // console only.
    if (playerGuid != owner_.getPlayerGuid()) {
        const std::string signer = owner_.lookupName(playerGuid);
        if (signer.empty()) { owner_.queryPlayerName(playerGuid); return; }
        owner_.displayGameMessage("ERR_PETITION_SIGNED_S", signer + " has signed your petition.", {signer});
        return;
    }
    switch (result) {
        case 0:
            owner_.displayGameMessage("ERR_PETITION_SIGNED", "Charter signed.");
            owner_.fireAddonEvent("PETITION_CLOSED", {});
            break;
        case 1:  owner_.displayGameMessage("ERR_PETITION_ALREADY_SIGNED", "You have already signed that charter."); break;
        case 2:  owner_.displayGameMessage("ERR_PETITION_IN_GUILD", "That player is already in a guild."); break;
        case 3:  owner_.displayGameMessage("ERR_PETITION_CREATOR", "You can't sign your own charter."); break;
        case 5:  owner_.displayGameMessage("ERR_PETITION_NOT_SAME_SERVER", "That player is not from your server"); break;
        case 8:  owner_.displayGameMessage("ERR_PETITION_FULL", "That charter is full."); break;
        case 10: owner_.displayGameMessage("ERR_PETITION_ALREADY_SIGNED_OTHER", "You've already signed another guild charter."); break;
        case 11: owner_.displayGameMessage("ERR_PETITION_RESTRICTED_ACCOUNT", "Trial accounts cannot sign guild charters."); break;
        default: LOG_INFO("[console] Petition error ", result); break;
    }
}

void SocialHandler::handleTurnInPetitionResults(network::Packet& packet) {
    uint32_t result = 0;
    if (!TurnInPetitionResultsParser::parse(packet, result)) return;
    // PetitionTurns, as AzerothCore (Guild.h) and Wow.exe (0x6CF1A0) number
    // them: 0 done (the registrar closes; the new guild says the rest),
    // 2 already in a guild, 4 more signatures needed, 11 no permission.
    switch (result) {
        case 0: owner_.fireAddonEvent("GUILD_REGISTRAR_CLOSED", {}); break;
        case 2: owner_.displayGameMessage("ERR_PETITION_IN_GUILD", "That player is already in a guild."); break;
        case 4: owner_.displayGameMessage("ERR_PETITION_NOT_ENOUGH_SIGNATURES", "You need more signatures."); break;
        case 11: owner_.displayGameMessage("ERR_GUILD_PERMISSIONS", "You don't have permission to do that."); break;
        default: LOG_INFO("[console] Petition error ", result); break;
    }
}

// ============================================================
// Server Info Handlers
// ============================================================

void SocialHandler::handleQueryTimeResponse(network::Packet& packet) {
    QueryTimeResponseData data;
    if (!QueryTimeResponseParser::parse(packet, data)) return;

    // The second word is how long until the daily quests reset -
    // GetNextDailyQuestsResetTime() minus now, in AzerothCore's own terms. It
    // was parsed and dropped, and GetQuestResetTime answered the time until
    // local midnight instead, with a comment saying this was a realm setting
    // nothing sends. It is sent, in the reply to a packet this client already
    // knew how to ask for.
    dailyResetOffset_ = data.timeOffset;
    dailyResetReceivedAt_ = time(nullptr);

    // Only /time says it out loud. The login-time ask is for the reset figure
    // and would otherwise greet every player with the clock.
    if (!announceServerTime_) return;
    announceServerTime_ = false;
    time_t serverTime = static_cast<time_t>(data.serverTime);
    const std::tm timeInfo = core::localTime(serverTime);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeInfo);
    owner_.addSystemChatMessage("Server time: " + std::string(timeStr));
}

uint32_t SocialHandler::getSecondsUntilDailyReset() const {
    if (dailyResetOffset_ == 0 || dailyResetReceivedAt_ == 0) return 0;
    const auto elapsed = static_cast<int64_t>(time(nullptr) - dailyResetReceivedAt_);
    const int64_t left = static_cast<int64_t>(dailyResetOffset_) - elapsed;
    // Past the reset the answer is stale rather than negative; the next login
    // asks again. Zero reads as "not known", which is the honest answer.
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void SocialHandler::handlePlayedTime(network::Packet& packet) {
    PlayedTimeData data;
    if (!PlayedTimeParser::parse(packet, data)) return;
    totalTimePlayed_ = data.totalTimePlayed;
    levelTimePlayed_ = data.levelTimePlayed;
    if (data.triggerMessage) {
        // TIME_PLAYED_MSG(totalTime, levelTime), in seconds, which is what
        // ChatFrame_SystemEventHandler dispatches to ChatFrame_DisplayTimePlayed
        // - the real client's own answer to /played, down to the localised
        // "days, hours, minutes, seconds" string. The server also sends played
        // time unasked at login, and triggerMessage is what tells the two
        // apart, so this stays inside it.
        owner_.fireAddonEvent("TIME_PLAYED_MSG",
                              {std::to_string(data.totalTimePlayed),
                               std::to_string(data.levelTimePlayed)});
        // ...and the hand-built lines below are what this client says when it
        // is drawing its own chat. With FrameXML drawing it they would be a
        // second, differently worded copy printed beside the real one - the
        // same shape as the gossip binder in quest_handler, where doing the
        // work here as well as letting the interface do it asked twice.
        if (ui::frameXmlOwns(ui::UiElement::Chat)) return;

        uint32_t totalDays = data.totalTimePlayed / 86400;
        uint32_t totalHours = (data.totalTimePlayed % 86400) / 3600;
        uint32_t totalMinutes = (data.totalTimePlayed % 3600) / 60;
        uint32_t levelDays = data.levelTimePlayed / 86400;
        uint32_t levelHours = (data.levelTimePlayed % 86400) / 3600;
        uint32_t levelMinutes = (data.levelTimePlayed % 3600) / 60;
        std::string totalMsg = "Total time played: ";
        if (totalDays > 0) totalMsg += std::to_string(totalDays) + " days, ";
        if (totalHours > 0 || totalDays > 0) totalMsg += std::to_string(totalHours) + " hours, ";
        totalMsg += std::to_string(totalMinutes) + " minutes";
        std::string levelMsg = "Time played this level: ";
        if (levelDays > 0) levelMsg += std::to_string(levelDays) + " days, ";
        if (levelHours > 0 || levelDays > 0) levelMsg += std::to_string(levelHours) + " hours, ";
        levelMsg += std::to_string(levelMinutes) + " minutes";
        owner_.addSystemChatMessage(totalMsg);
        owner_.addSystemChatMessage(levelMsg);
    }
}

void SocialHandler::handleWho(network::Packet& packet) {
    const bool hasGender = isActiveExpansion("wotlk");
    uint32_t displayCount = packet.readUInt32();
    uint32_t onlineCount = packet.readUInt32();
    whoResults_.clear();
    whoOnlineCount_ = onlineCount;
    // No results is not an error: "0 players total" in chat, or an empty
    // list in the panel, like any other answer.
    for (uint32_t i = 0; i < displayCount; ++i) {
        if (packet.getReadPos() >= packet.getSize()) break;
        std::string playerName = packet.readString();
        std::string guildName = packet.readString();
        if (!packet.hasRemaining(12)) break;
        uint32_t level   = packet.readUInt32();
        uint32_t classId = packet.readUInt32();
        uint32_t raceId  = packet.readUInt32();
        uint8_t gender = 0;
        if (hasGender && packet.hasRemaining(1)) gender = packet.readUInt8();
        uint32_t zoneId = 0;
        if (packet.hasRemaining(4)) zoneId = packet.readUInt32();
        // Fifty rows are kept (0x6B8720); the rest are read past.
        if (whoResults_.size() >= 50) continue;
        WhoEntry entry;
        entry.name = playerName; entry.guildName = guildName;
        entry.level = level; entry.classId = classId;
        entry.raceId = raceId; entry.zoneId = zoneId; entry.gender = gender;
        whoResults_.push_back(std::move(entry));
    }
    // To the chat when nobody is showing a panel, which is what a stock client
    // does and what SetWhoToUI is for. FriendsFrame turns that on while its Who
    // tab is up and off again on the way out, so a /who typed into chat with
    // the panel closed printed nothing at all here: the rows were parsed,
    // stored, and kept.
    //
    // The formats are WoW's own, from globalstrings - and they are the
    // client's to print rather than the interface's, which is why no FrameXML
    // file formats them. The count line carries a |4 escape, which resolves on
    // the way to the screen.
    // Wow.exe (0x6B8720, 0x6B5030): with the panel not asking (SetWhoToUI)
    // and no more than three rows, WHO_LIST_FORMAT or, with a guild,
    // WHO_LIST_GUILD_FORMAT per row, then WHO_NUM_RESULTS - plain chat lines.
    // Otherwise the rows go to the panel alone, through WHO_LIST_UPDATE.
    // The packet's count decides, not the rows kept.
    const bool toChat = chat_rules::whoResultsToChat(whoToUI_, displayCount);
    if (toChat) {
        for (const auto& e : whoResults_) {
            const std::string level = std::to_string(e.level);
            const std::string race = getRaceName(static_cast<Race>(e.raceId));
            const std::string cls = getClassName(static_cast<Class>(e.classId));
            const std::string zone = owner_.getWhoAreaName(e.zoneId);
            if (e.guildName.empty())
                owner_.addGlobalStringChatLine("WHO_LIST_FORMAT",
                    "|Hplayer:%s|h[%s]|h: Level %d %s %s - %s",
                    {e.name, e.name, level, race, cls, zone});
            else
                owner_.addGlobalStringChatLine("WHO_LIST_GUILD_FORMAT",
                    "|Hplayer:%s|h[%s]|h: Level %d %s %s <%s> - %s",
                    {e.name, e.name, level, race, cls, e.guildName, zone});
        }
        // The count of rows sent, the packet's first field, not its total.
        owner_.addGlobalStringChatLine("WHO_NUM_RESULTS", "%d |4player:players; total",
                                       {std::to_string(displayCount)});
    }

    // The who panel asked for this and redraws its rows on the answer coming
    // back. Every row was parsed and stored and only this client's own window
    // ever read them, so a /who through the interface filled a list nobody was
    // told about and the panel kept showing the previous search.
    else if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("WHO_LIST_UPDATE", {});
}

// ============================================================
// Social (Friend/Ignore/Random Roll) Handlers
// ============================================================

void SocialHandler::handleFriendList(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return;
    uint8_t count = packet.readUInt8();
    owner_.contactsRef().erase(std::remove_if(owner_.contactsRef().begin(), owner_.contactsRef().end(),
        [](const ContactEntry& e){ return e.isFriend(); }), owner_.contactsRef().end());
    for (uint8_t i = 0; i < count && rem() >= 9; ++i) {
        uint64_t guid   = packet.readUInt64();
        uint8_t  status = packet.readUInt8();
        uint32_t area = 0, level = 0, classId = 0;
        if (status != 0 && rem() >= 12) {
            area    = packet.readUInt32();
            level   = packet.readUInt32();
            classId = packet.readUInt32();
        }
        owner_.friendGuidsRef().insert(guid);
        auto nit = owner_.getPlayerNameCache().find(guid);
        std::string name;
        if (nit != owner_.getPlayerNameCache().end()) {
            name = nit->second;
            owner_.friendsCacheRef()[name] = guid;
        } else {
            owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.name = name; entry.flags = 0x1;
        entry.status = status; entry.areaId = area; entry.level = level; entry.classId = classId;
        owner_.contactsRef().push_back(std::move(entry));
    }
    // The full list arrives in reply to this client asking for it, so it is the
    // "show" case as well as the "changed" case. SHOW additionally re-selects
    // the tab; UPDATE only refreshes the rows. A single friend going online
    // sends the status packet instead, and fires UPDATE alone.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
        owner_.addonEventCallbackRef()("FRIENDLIST_SHOW", {});
    }
}

void SocialHandler::handleContactList(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 8) { packet.skipAll(); return; }
    owner_.lastContactListMaskRef()  = packet.readUInt32();
    owner_.lastContactListCountRef() = packet.readUInt32();
    owner_.contactsRef().clear();
    for (uint32_t i = 0; i < owner_.lastContactListCountRef() && rem() >= 8; ++i) {
        uint64_t guid  = packet.readUInt64();
        if (rem() < 4) break;
        uint32_t flags = packet.readUInt32();
        std::string note = packet.readString();
        uint8_t status = 0; uint32_t areaId = 0, level = 0, classId = 0;
        if (flags & 0x1) {
            if (rem() < 1) break;
            status = packet.readUInt8();
            if (status != 0 && rem() >= 12) {
                areaId = packet.readUInt32(); level = packet.readUInt32(); classId = packet.readUInt32();
            }
            owner_.friendGuidsRef().insert(guid);
            auto nit = owner_.getPlayerNameCache().find(guid);
            if (nit != owner_.getPlayerNameCache().end()) owner_.friendsCacheRef()[nit->second] = guid;
            else owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.flags = flags; entry.note = std::move(note);
        entry.status = status; entry.areaId = areaId; entry.level = level; entry.classId = classId;
        auto nit = owner_.getPlayerNameCache().find(guid);
        if (nit != owner_.getPlayerNameCache().end()) entry.name = nit->second;
        owner_.contactsRef().push_back(std::move(entry));
    }
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
        owner_.addonEventCallbackRef()("FRIENDLIST_SHOW", {});
        if (owner_.lastContactListMaskRef() & 0x2) owner_.addonEventCallbackRef()("IGNORELIST_UPDATE", {});
    }
}

void SocialHandler::handleFriendStatus(network::Packet& packet) {
    FriendStatusData data;
    if (!FriendStatusParser::parse(packet, data)) return;

    // Single lookup - reuse iterator for name resolution and update/erase below
    auto cit = std::find_if(owner_.contactsRef().begin(), owner_.contactsRef().end(),
        [&](const ContactEntry& e){ return e.guid == data.guid; });

    // Look up player name: contacts_ (populated by SMSG_FRIEND_LIST) > playerNameCache
    std::string playerName;
    if (cit != owner_.contactsRef().end() && !cit->name.empty()) {
        playerName = cit->name;
    } else {
        auto it = owner_.getPlayerNameCache().find(data.guid);
        if (it != owner_.getPlayerNameCache().end()) playerName = it->second;
    }

    // Only update friendsCache when we have a resolved name - inserting an empty
    // key creates a phantom entry that masks the real one when the name arrives.
    // FriendsResult numbering (below): online, offline and the two adds keep
    // the name; a removal drops it.
    if (!playerName.empty()) {
        if (data.status == 2 || data.status == 3 || data.status == 6 || data.status == 7)
            owner_.friendsCacheRef()[playerName] = data.guid;
        else if (data.status == 5) owner_.friendsCacheRef().erase(playerName);
    }

    // AzerothCore's FriendsResult, read off SocialMgr.h rather than guessed.
    // The numbering here was five places out: zero is a database error and not
    // a removal, one is a full list and not an addition, and the three results
    // that matter most - removed, added-online, added-offline - are 5, 6 and 7
    // where this had "already in your list", "list is full" and "is ignoring
    // you". So adding a friend announced that the list was full, and a
    // database error deleted them from it.
    enum : uint8_t {
        kDbError = 0, kListFull = 1, kOnline = 2, kOffline = 3, kNotFound = 4,
        kRemoved = 5, kAddedOnline = 6, kAddedOffline = 7, kAlready = 8,
        kSelf = 9, kEnemy = 10,
    };
    if (data.status == kRemoved) {
        if (cit != owner_.contactsRef().end())
            owner_.contactsRef().erase(cit);
    } else if (data.status == kOnline || data.status == kOffline ||
               data.status == kAddedOnline || data.status == kAddedOffline) {
        const bool nowOnline =
            (data.status == kOnline || data.status == kAddedOnline);
        // chatFlag is the FriendStatus bitmask the packet carries, and it is
        // what the friends list reads for away and busy. Offline results carry
        // no body, so the flags are cleared rather than left as they were.
        const uint8_t flags = nowOnline ? data.chatFlag : 0;
        if (cit != owner_.contactsRef().end()) {
            if (!playerName.empty() && playerName != "Unknown") cit->name = playerName;
            cit->status = flags;
            if (nowOnline) {
                cit->areaId = data.areaId;
                cit->level = data.level;
                cit->classId = data.classId;
            }
            if (!data.note.empty()) cit->note = data.note;
        } else {
            ContactEntry entry;
            entry.guid = data.guid; entry.name = playerName; entry.flags = 0x1;
            entry.status = flags;
            entry.note = data.note;
            entry.areaId = data.areaId; entry.level = data.level;
            entry.classId = data.classId;
            owner_.contactsRef().push_back(std::move(entry));
        }
    }
    // What Wow.exe's friend handler (0x6B73A0) says for each result, all of
    // them chat lines; the ones naming a player wait for the name.
    const std::string N = GameHandler::kPlayerNameArg;
    switch (data.status) {
        case kDbError:
        case kNotFound:     owner_.displayGameMessage("ERR_FRIEND_NOT_FOUND", "Player not found."); break;
        case kListFull:     owner_.displayGameMessage("ERR_FRIEND_LIST_FULL", "You don't have room for any more friends."); break;
        case kOnline:
            owner_.displayGameMessageNaming(data.guid, "ERR_FRIEND_ONLINE_SS",
                                            "|Hplayer:%s|h[%s]|h has come online.", {N, N});
            break;
        case kOffline:
            owner_.displayGameMessageNaming(data.guid, "ERR_FRIEND_OFFLINE_S", "%s has gone offline.", {N});
            break;
        case kRemoved:
            // A friend whose character is gone is ERR_FRIEND_DELETED.
            if (playerName.empty()) owner_.displayGameMessage("ERR_FRIEND_DELETED", "Friend removed because the character no longer exists.");
            else owner_.displayGameMessage("ERR_FRIEND_REMOVED_S", playerName + " removed from friends list.", {playerName});
            break;
        case kAddedOnline:
        case kAddedOffline:
            owner_.displayGameMessageNaming(data.guid, "ERR_FRIEND_ADDED_S", "%s added to friends.", {N});
            break;
        case kAlready:
            owner_.displayGameMessageNaming(data.guid, "ERR_FRIEND_ALREADY_S", "%s is already your friend.", {N});
            break;
        case kSelf:         owner_.displayGameMessage("ERR_FRIEND_SELF", "You can't put yourself on your friend list."); break;
        case kEnemy:        owner_.displayGameMessage("ERR_FRIEND_WRONG_FACTION", "Friends must be part of your alliance."); break;
        // The ignore list answers on the same opcode.
        case 11: owner_.displayGameMessage("ERR_IGNORE_FULL", "You can't ignore any more players."); break;
        case 12: owner_.displayGameMessage("ERR_IGNORE_SELF", "You can't ignore yourself."); break;
        case 13: owner_.displayGameMessage("ERR_IGNORE_NOT_FOUND", "Player not found."); break;
        case 14: owner_.displayGameMessageNaming(data.guid, "ERR_IGNORE_ALREADY_S", "%s is already being ignored.", {N}); break;
        case 15: owner_.displayGameMessageNaming(data.guid, "ERR_IGNORE_ADDED_S", "%s is now being ignored.", {N}); break;
        case 16: {
            const std::string ignored = owner_.lookupName(data.guid);
            if (ignored.empty()) owner_.displayGameMessage("ERR_IGNORE_DELETED", "Ignore removed because the character no longer exists.");
            else owner_.displayGameMessage("ERR_IGNORE_REMOVED_S", ignored + " is no longer being ignored.", {ignored});
            break;
        }
        case 17: owner_.displayGameMessage("ERR_IGNORE_AMBIGUOUS", "That name is ambiguous, type more of the player's server name"); break;
        case 18: owner_.displayGameMessage("ERR_VOICE_IGNORE_FULL", "You can't mute any more players."); break;
        case 19: owner_.displayGameMessage("ERR_VOICE_IGNORE_SELF", "You can't mute yourself."); break;
        case 20: owner_.displayGameMessage("ERR_VOICE_IGNORE_NOT_FOUND", "Player not found."); break;
        case 21: owner_.displayGameMessageNaming(data.guid, "ERR_VOICE_IGNORE_ALREADY_S", "%s is already muted.", {N}); break;
        case 22: owner_.displayGameMessageNaming(data.guid, "ERR_VOICE_IGNORE_ADDED_S", "%s is now muted.", {N}); break;
        case 24: owner_.displayGameMessage("ERR_VOICE_IGNORE_AMBIGUOUS", "That name is ambiguous, type more of the player's server name"); break;
        case 25: case 26: case 27: break;   // silent updates (flags, area)
        default: owner_.displayGameMessage("ERR_FRIEND_ERROR", "Unknown friend response from server."); break;
    }
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
}

void SocialHandler::handleRandomRoll(network::Packet& packet) {
    RandomRollData data;
    if (!RandomRollParser::parse(packet, data)) return;
    // RANDOM_ROLL_RESULT ("%s rolls %d (%d-%d)"), a plain chat line for
    // everyone including the player, once the roller's name is known
    // (Wow.exe 0x6E2E90); nothing from someone ignored.
    if (owner_.ignoreListGuidsRef().count(data.rollerGuid)) return;
    owner_.displayGameMessageNaming(data.rollerGuid, "RANDOM_ROLL_RESULT", "%s rolls %d (%d-%d)",
                                    {GameHandler::kPlayerNameArg, std::to_string(data.result),
                                     std::to_string(data.minRoll), std::to_string(data.maxRoll)},
                                    /*plain=*/true);
}

// ============================================================
// Logout Handlers
// ============================================================

void SocialHandler::handleLogoutResponse(network::Packet& packet) {
    LogoutResponseData data;
    if (!LogoutResponseParser::parse(packet, data)) return;
    if (data.result == 0) {
        // No line: the countdown popup is the stock client's only word.
        logoutCountdown_ = data.instant ? 0.0f : 20.0f;
        // Accepted (0x6B08B0): PLAYER_CAMPING, or PLAYER_QUITING for a quit,
        // when there is a countdown; nothing when it is instant - the
        // server's SMSG_LOGOUT_COMPLETE follows at once. PLAYER_LOGOUT is not
        // fired here: FrameXML reads it as the character leaving the world
        // and saves its variables on it, twenty seconds early and for a
        // logout that may yet be cancelled.
        if (owner_.addonEventCallbackRef()) {
            // The timer the server just imposed, announced so the interface can
            // show it. UIParent answers PLAYER_CAMPING with the CAMP popup and
            // PLAYER_QUITING with QUIT, both of which count down and offer to
            // cancel - and it already answers LOGOUT_CANCEL by hiding them, so
            // half of this conversation was wired and the half that starts it
            // was not. Nothing could show the twenty seconds but a line of chat.
            //
            // Which of the two is the difference between /logout and /quit,
            // which this handler already keeps in exitAfterLogout_.
            if (!data.instant) {
                owner_.addonEventCallbackRef()(
                    exitAfterLogout_ ? "PLAYER_QUITING" : "PLAYER_CAMPING", {});
            }
        }
    } else {
        // The server said no, and what that means depends on what was asked.
        //
        // For /logout it is the whole answer: the player stays in the world and
        // is told why. For Exit Game it is not. That button asks the program to
        // close, and a refusal governs whether the character leaves the world
        // tidily rather than whether this process keeps running - so printing
        // the error and staying put left a quit button that says no, which is
        // the one thing a quit button must never do.
        const bool exiting = exitAfterLogout_;
        loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
        if (exiting) {
            LOG_WARNING("Logout refused, but Exit Game was asked for - closing anyway");
            if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(true);
        } else {
            // ERR_LOGOUT_FAILED, "You can't logout now." (0x6B08B0).
            owner_.displayGameMessage("ERR_LOGOUT_FAILED", "You can't logout now.");
        }
    }
}

void SocialHandler::handleLogoutComplete(network::Packet& /*packet*/) {
    // The countdown finishing is not the end of it: the server says when the
    // character is actually out of the world, and only then can the client leave -
    // to character select, or out of the game entirely for /quit and /exit.
    const bool exiting = exitAfterLogout_;
    loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
    LOG_WARNING("Logout complete (exiting=", exiting ? "yes" : "no", ")");  // temporary: visible at default log level
    if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(exiting);
}

// ============================================================
// Battleground Handlers
// ============================================================

void SocialHandler::handleBattlefieldStatus(network::Packet& packet) {
    // Wow.exe 0x54AE40 (CGBattlefieldInfo). Two queue slots; a status for any
    // other slot is dropped. Eight zero bytes after the slot are STATUS_NONE
    // and empty the slot - which AzerothCore sends on leaving a queue, on
    // declining an invitation and on leaving the battleground.
    bf::StatusPacket s;
    if (isActiveExpansion("wotlk")) {
        if (!bf::parseStatus(packet, s)) return;
    } else {
        BattlefieldStatusData old;
        if (!BattlefieldStatusPacket::parse(packet, old, isClassicLikeExpansion(), false)) return;
        s.queueSlot = old.queueSlot;
        s.bgTypeId = old.bgTypeId;
        s.teamSize = old.arenaType;
        s.header = old.statusId ? (uint64_t(old.arenaType) | (uint64_t(old.bgTypeId) << 16)) : 0;
        s.minLevel = old.minLevel; s.maxLevel = old.maxLevel;
        s.instanceId = old.instanceId; s.rated = old.isRated; s.status = old.statusId;
        s.avgWaitMs = old.avgWaitMs; s.timeInQueueMs = old.timeInQueueMs;
        s.mapId = old.mapId; s.inviteTimeoutMs = old.inviteTimeoutMs;
    }
    bgJoinPendingType_ = 0;  // answered
    const bf::StatusEffect fx = bgQueues_.apply(s, bf::clockMs(),
        [this](uint32_t mapId) { return owner_.getMapInstanceType(mapId); });
    if (fx.slot < 0) return;
    LOG_INFO("Battleground queue ", fx.slot + 1, ": status ", s.status, " type ", s.bgTypeId,
             " map ", s.mapId, s.header == 0 ? " (cleared)" : "");
    if (fx.enteredActive) {
        // Entering a battleground's status: the scoreboard's own columns are
        // the map's WorldStateUI rows of type 2, its filter both factions
        // (0x54AE40 -> 0x54AA30).
        bgScoreColumns_.clear();
        for (size_t i : bf::scoreColumns(owner_.worldStateUiRows(), s.mapId))
            bgScoreColumns_.push_back(i);
        bgScoreboard_.factionFilter = -1;
        resortScoreboard();
        // Opponents already in view when the arena's status arrives. Stock
        // judges each player as its object is created; a status that comes
        // after the objects would otherwise leave them unjudged.
        if (bgQueues_.active().instanceType == 4) {
            std::vector<uint64_t> inView;
            for (const auto& [guid, e] : owner_.getEntityManager().getEntities())
                if (e && e->getType() == ObjectType::PLAYER && arenaOpponents_.slotOf(guid) < 0)
                    inView.push_back(guid);
            for (uint64_t g : inView) onObjectAppeared(g);
        }
    }
    // The argument is the queue, counted from one; the interface does the
    // telling (the minimap eye, the "enter battle" popup).
    owner_.fireAddonEvent("UPDATE_BATTLEFIELD_STATUS", {std::to_string(fx.slot + 1)});
}

void SocialHandler::handleBattlefieldList(network::Packet& packet) {
    // 0x54E390: the battlemaster's list, or the PvP frame's answer.
    bf::ListPacket list;
    if (isActiveExpansion("wotlk")) {
        if (!bf::parseList(packet, list)) return;
    } else {
        // Pre-WotLK: no guid or rewards; the list arrives for the frame only.
        if (!packet.hasRemaining(5)) return;
        list.bgTypeId = packet.readUInt32();
        packet.readUInt8();
        if (isActiveExpansion("tbc") && packet.hasRemaining(1)) packet.readUInt8();
        if (packet.hasRemaining(4)) {
            const uint32_t n = std::min(packet.readUInt32(), 256u);
            for (uint32_t i = 0; i < n && packet.hasRemaining(4); ++i)
                list.instanceIds.push_back(packet.readUInt32());
        }
        list.battlemaster = owner_.isGossipWindowOpen() ? owner_.getCurrentGossip().npcGuid : 0;
    }
    bgList_ = list;
    bgListReceived_ = true;
    const std::string event = bf::listEvent(list);
    if (event == "NPC_PVPQUEUE_ANYWHERE" || event == "BATTLEFIELDS_SHOW") {
        battlemasterGuid_ = list.battlemaster;
    } else if (event == "BATTLEFIELDS_CLOSED") {
        battlemasterGuid_ = 0;
    }
    owner_.fireAddonEvent(event, {});
}

bool SocialHandler::hasPendingBgInvite() const {
    return bgQueues_.hasInvite();
}

// AcceptBattlefieldPort(index, accept) (0x54DA10): CMSG_BATTLEFIELD_PORT with
// the slot's eight header bytes as the status brought them, and the answer.
bool SocialHandler::acceptBattlefieldPort(uint32_t index, bool accept) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    const bf::QueueSlot* q = bgQueues_.slot(index);
    if (!q || q->header == 0) return false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    if (isActiveExpansion("wotlk")) {
        pkt.writeUInt64(q->header);
    } else {
        if (!isClassicLikeExpansion()) { pkt.writeUInt8(q->teamSize); pkt.writeUInt8(0); }
        pkt.writeUInt32(q->bgTypeId);
        pkt.writeUInt16(0);
    }
    pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
    // Declining the battleground the battlemaster's frame shows closes it.
    if (!accept && bgListReceived_ && q->bgTypeId == bgList_.bgTypeId)
        owner_.fireAddonEvent("BATTLEFIELDS_CLOSED", {});
    return true;
}

void SocialHandler::acceptBattlefield(uint32_t queueSlot) {
    if (queueSlot == 0xFFFFFFFF) {
        for (uint32_t i = 0; i < bf::kQueueSlots; ++i)
            if (bgQueues_.slot(i)->status == 2) { queueSlot = i; break; }
    }
    if (queueSlot < bf::kQueueSlots) acceptBattlefieldPort(queueSlot, true);
}

void SocialHandler::declineBattlefield(uint32_t queueSlot) {
    if (queueSlot == 0xFFFFFFFF) {
        for (uint32_t i = 0; i < bf::kQueueSlots; ++i)
            if (bgQueues_.slot(i)->status == 2) { queueSlot = i; break; }
    }
    if (queueSlot < bf::kQueueSlots) acceptBattlefieldPort(queueSlot, false);
}

// JoinBattlefield(index, asGroup, isRated) (0x54CC70). The battleground is the
// one the last SMSG_BATTLEFIELD_LIST named; an arena row joins through
// CMSG_BATTLEMASTER_JOIN_ARENA with the index as the team size slot.
void SocialHandler::joinBattlefieldIndex(int index, bool asGroup, bool rated) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t type = bgList_.bgTypeId;
    const auto* row = owner_.getBattlemasterInfo(type);
    const bool arena = row && row->instanceType == 4;
    if (asGroup && row) {
        const auto& pd = owner_.getPartyData();
        const uint32_t members = pd.isEmpty() ? 0 : pd.memberCount;
        if (row->maxGroupSize < members) {
            owner_.displayGameMessage("ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY",
                                      "Your group is too large for this battleground.  Please regroup to join.");
            return;
        }
    }
    if (!arena && owner_.getMapInstanceType(owner_.getCurrentMapId()) == 3) {
        owner_.displayGameMessage("ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND", "You can't do that in a battleground.");
        return;
    }
    if (arena) {
        network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEMASTER_JOIN_ARENA));
        packet.writeUInt64(battlemasterGuid_);
        packet.writeUInt8(static_cast<uint8_t>(index - 1));
        packet.writeUInt8(asGroup ? 1 : 0);
        packet.writeUInt8(rated ? 1 : 0);
        owner_.getSocket()->send(packet);
    } else {
        const uint32_t i = static_cast<uint32_t>(index - 1);
        const uint32_t instanceId = (index >= 1 && i < bgList_.instanceIds.size() && battlemasterGuid_ != 0)
                                        ? bgList_.instanceIds[i] : 0;
        joinBattlefield(battlemasterGuid_, type, instanceId, asGroup);
    }
    LOG_INFO("JoinBattlefield: type ", type, " index ", index, arena ? " (arena)" : "",
             " battlemaster 0x", std::hex, battlemasterGuid_, std::dec);
}

// CMSG_BATTLEMASTER_JOIN: the battlemaster, the battleground, which instance
// of it (zero: the first available) and whether the group comes too.
void SocialHandler::joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                    uint32_t instanceId, bool asGroup) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEMASTER_JOIN));
    packet.writeUInt64(battlemasterGuid);
    packet.writeUInt32(bgTypeId);
    packet.writeUInt32(instanceId);
    packet.writeUInt8(asGroup ? 1 : 0);
    owner_.getSocket()->send(packet);
    bgJoinPendingType_ = bgTypeId;
    bgJoinWaitSec_ = 5.0f;
}

// CMSG_BATTLEFIELD_LIST as the PvP frame sends it (0x54E5B0, from
// RequestBattlegroundInstanceInfo): the type, "from anywhere" (1), and the
// experience-off flag below the level cap. The type becomes the selection
// JoinBattlefield queues for.
void SocialHandler::requestBattlefieldList(uint32_t bgTypeId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEFIELD_LIST));
    packet.writeUInt32(bgTypeId);
    if (!isPreWotlk()) {
        packet.writeUInt8(1);  // from anywhere: the PvP frame
        packet.writeUInt8(0);  // experience gain is on
    }
    owner_.getSocket()->send(packet);
    bgList_.bgTypeId = bgTypeId;
}

// CMSG_REPORT_PVP_AFK: this player is not participating.
void SocialHandler::reportPvpAfk(uint64_t playerGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerGuid == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_REPORT_PVP_AFK));
    pkt.writeUInt64(playerGuid);
    owner_.getSocket()->send(pkt);
}

// LeaveBattlefield() (0x54C250): CMSG_LEAVE_BATTLEFIELD carrying the active
// slot's eight header bytes (zero without one). AzerothCore reads all eight
// before acting, so the empty packet this used to send was dropped and the
// "Leave Battleground" button did nothing.
void SocialHandler::leaveBattlefield() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LEAVE_BATTLEFIELD));
    if (!isClassicLikeExpansion()) {
        const int a = bgQueues_.active().slot;
        const bf::QueueSlot* q = a >= 0 ? bgQueues_.slot(static_cast<uint32_t>(a)) : nullptr;
        pkt.writeUInt64(q ? q->header : 0);
    }
    owner_.getSocket()->send(pkt);
}

// CloseBattlefield() (0x549B40): BATTLEFIELDS_CLOSED, and the battlemaster let go.
void SocialHandler::closeBattlefield() {
    owner_.fireAddonEvent("BATTLEFIELDS_CLOSED", {});
    battlemasterGuid_ = 0;
}

// RequestBattlefieldScoreData() (0x54DCA0 -> 0x54CE30). WorldStateScoreFrame
// calls it from OnUpdate, every frame, so the client throttles it: once per
// five seconds, and after the first not at all once the battle has ended or
// in an arena.
void SocialHandler::requestPvpLog() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const int64_t now = bf::clockMs();
    if (nextPvpLogRequestMs_ != 0) {
        if (bgScoreboard_.log.ended || bgQueues_.active().instanceType == 4) return;
        if (now < nextPvpLogRequestMs_) return;
    }
    network::Packet pkt(wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    owner_.getSocket()->send(pkt);
    nextPvpLogRequestMs_ = now + 5000;
}

// The world was (re)entered (0x528010 -> 0x54CA50): the scoreboard and the
// battlemaster's list are emptied, and the server is asked for every queue's
// status again (CMSG_BATTLEFIELD_STATUS).
void SocialHandler::onWorldEntered() {
    // The arena's opponents go with it: "cleared" for all ten (0x54CA50).
    fireArenaOpponentEvents(arenaOpponents_.clear());
    // And the arena teams' rosters are asked for again (0x528010 -> 0x5A3DC0).
    onArenaTeamInfoChanged(false);
    bgScoreboard_ = bf::Scoreboard{};
    nextPvpLogRequestMs_ = 0;
    bgScoreNamesPending_ = false;
    bgScoreColumns_.clear();
    bgList_ = bf::ListPacket{};
    bgListReceived_ = false;
    battlemasterGuid_ = 0;
    bgQueues_.reset();
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket() && !isPreWotlk()) {
        network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_STATUS));
        owner_.getSocket()->send(pkt);
    }
}

void SocialHandler::setScoreFaction(int faction) {
    if (faction != -1 && (faction < 0 || faction > 1)) return;
    bgScoreboard_.factionFilter = faction;
    resortScoreboard();
}

void SocialHandler::sortScores(const std::string& column) {
    if (bgScoreNamesPending_) return;
    bgScoreboard_.sort.select(column);
    resortScoreboard();
}

// 0x54AA30: once every row's name is known, the faction of each battleground
// row from its race, the rows the filter passes counted, the sort, and
// UPDATE_BATTLEFIELD_SCORE.
void SocialHandler::resortScoreboard() {
    if (bgScoreNamesPending_ || !bgScoreboard_.hasData) return;
    const bool arena = bgQueues_.active().instanceType == 4 || bgScoreboard_.log.isArena;
    for (auto& r : bgScoreboard_.log.rows) {
        if (!arena) r.faction = bf::factionOfRace(r.raceId);
    }
    bgScoreboard_.resort(bgScoreColumns_.size());
    owner_.fireAddonEvent("UPDATE_BATTLEFIELD_SCORE", {});
}

// The names, races and classes a scoreboard row needs, from the name cache.
// True when every row has them.
bool SocialHandler::fillScoreNames() {
    bool all = true;
    const uint64_t me = owner_.getPlayerGuid();
    for (auto& r : bgScoreboard_.log.rows) {
        if (r.guid == me) {
            r.name = owner_.lookupName(me);
            r.raceId = owner_.getPlayerRace();
            r.classId = owner_.getPlayerClass();
        } else {
            r.name = owner_.lookupName(r.guid);
            r.raceId = owner_.lookupPlayerRace(r.guid);
            r.classId = owner_.lookupPlayerClass(r.guid);
        }
        if (r.name.empty() || r.raceId == 0) {
            all = false;
            owner_.queryPlayerName(r.guid);
        }
    }
    return all;
}

// ============================================================
// Instance Handlers
// ============================================================

void SocialHandler::handleRaidInstanceInfo(network::Packet& packet) {
    const bool isTbc = isActiveExpansion("tbc");
    const bool isClassic = isClassicLikeExpansion();
    const bool useTbcFormat = isTbc || isClassic;
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    instanceLockouts_.clear();
    instanceLockouts_.reserve(count);
    // WotLK (Wow.exe 0x501030, AzerothCore Player::SendRaidInfo): map,
    // difficulty, the instance's uint64 id, locked, extended, then the
    // seconds to the reset - the id used to be read as the reset time.
    const size_t kEntrySize = useTbcFormat ? 13 : 22;
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getRemainingSize() < kEntrySize) break;
        InstanceLockout lo;
        lo.mapId = packet.readUInt32(); lo.difficulty = packet.readUInt32();
        if (useTbcFormat) {
            lo.resetSeconds = packet.readUInt32(); lo.locked = packet.readUInt8() != 0; lo.extended = false;
        } else {
            lo.instanceId = packet.readUInt64();
            lo.locked = packet.readUInt8() != 0;
            lo.extended = packet.readUInt8() != 0;
            lo.resetSeconds = packet.readUInt32();
        }
        lo.resetTime = now + lo.resetSeconds;
        instanceLockouts_.push_back(lo);
    }
    owner_.fireAddonEvent("UPDATE_INSTANCE_INFO", {});
}

// The difficulty state as Wow.exe keeps it: the instance's own (0xBD0894)
// and its dynamic heroic flag (0xBD1980) from SMSG_INSTANCE_DIFFICULTY, and a
// personal and a group copy of each setting (0xBD0898/0xBD1978 dungeon,
// 0xBD089C/0xBD197C raid). The dungeon setting in force is the group copy in
// a party, the raid one the group copy in a raid (0x515790, 0x515810).
uint32_t SocialHandler::effectiveDungeonDifficulty() const {
    const auto& pd = owner_.getPartyData();
    const bool party = !pd.isEmpty() && pd.groupType == 0;
    return party ? dungeonDifficultyGroup_ : dungeonDifficultySetting_;
}

uint32_t SocialHandler::effectiveRaidDifficulty() const {
    const auto& pd = owner_.getPartyData();
    const bool raid = !pd.isEmpty() && pd.groupType != 0;
    return raid ? raidDifficultyGroup_ : raidDifficultySetting_;
}

void SocialHandler::sayDungeonDifficulty() {
    // ERR_DUNGEON_DIFFICULTY_CHANGED_S with DUNGEON_DIFFICULTY<n+1> (0x524720).
    const uint32_t d = effectiveDungeonDifficulty();
    const std::string nameKey = "DUNGEON_DIFFICULTY" + std::to_string(d + 1);
    std::string label = globalString(owner_.services().assetManager, nameKey);
    if (label.empty()) label = d == 1 ? "5 Player (Heroic)" : "5 Player";
    owner_.displayGameMessage("ERR_DUNGEON_DIFFICULTY_CHANGED_S",
                              "Dungeon Difficulty set to " + label + ".", {label});
}

void SocialHandler::sayRaidDifficulty() {
    // 0x524780: inside a raid map the MapDifficulty row's own name, otherwise
    // RAID_DIFFICULTY<n+1>. (The dynamic-difficulty raids' two lines, Map
    // flag 0x100, are not modelled.)
    const uint32_t d = effectiveRaidDifficulty();
    std::string label;
    const uint32_t map = owner_.getCurrentMapId();
    if (owner_.getMapInstanceType(map) == 2) {
        const auto md = owner_.getMapDifficulty(map, instanceDifficulty_);
        if (!md.difficultyKey.empty())
            label = globalString(owner_.services().assetManager, md.difficultyKey);
    }
    if (label.empty())
        label = globalString(owner_.services().assetManager, "RAID_DIFFICULTY" + std::to_string(d + 1));
    if (label.empty()) label = "Raid";
    owner_.displayGameMessage("ERR_RAID_DIFFICULTY_CHANGED_S",
                              "Raid Difficulty set to " + label + ".", {label});
}

void SocialHandler::applyDungeonDifficulty(uint32_t diff, bool personal, bool group) {
    const uint32_t before = effectiveDungeonDifficulty();
    if (personal) dungeonDifficultySetting_ = diff;
    if (group) dungeonDifficultyGroup_ = diff;
    if (effectiveDungeonDifficulty() != before) sayDungeonDifficulty();
}

void SocialHandler::applyRaidDifficulty(uint32_t diff, bool personal, bool group) {
    const uint32_t before = effectiveRaidDifficulty();
    if (personal) raidDifficultySetting_ = diff;
    if (group) raidDifficultyGroup_ = diff;
    if (effectiveRaidDifficulty() != before) sayRaidDifficulty();
}

void SocialHandler::handleInstanceDifficulty(network::Packet& packet, bool dungeonSetting) {
    if (!packet.hasRemaining(4)) return;
    const uint32_t first = packet.readUInt32();
    if (dungeonSetting) {
        // MSG_SET_DUNGEON_DIFFICULTY (0x525530): difficulty, "personal" (the
        // server always 1), whether it is the group's.
        const uint32_t personal = packet.hasRemaining(4) ? packet.readUInt32() : 1;
        const uint32_t group = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        applyDungeonDifficulty(first, personal != 0, group != 0);
        return;
    }
    // SMSG_INSTANCE_DIFFICULTY (0x526530 case 0x33B): the instance's
    // difficulty and its dynamic heroic flag, stored in silence. It comes at
    // every login, in the open world too, so it says nothing about being in
    // an instance.
    instanceDifficulty_ = first;
    instanceDynamicHeroic_ = packet.hasRemaining(4) ? packet.readUInt32() : 0;
    instanceIsHeroic_ = instanceDifficulty_ == 1;
}

// Set{Dungeon,Raid}Difficulty(n) (0x526050, 0x5261A0): 1-2 for a dungeon,
// 1-4 for a raid; a group member who does not lead is refused with
// ERR_NOT_LEADER and nothing is sent. Otherwise both copies change at once,
// the line is said, and the request goes out - the server's echo then
// changes nothing and says nothing.
void SocialHandler::setDifficultyFromUi(uint32_t n, bool raid) {
    if (n < 1 || n > (raid ? 4u : 2u)) return;
    const auto& pd = owner_.getPartyData();
    if (!pd.isEmpty() && pd.leaderGuid != owner_.getPlayerGuid()) {
        owner_.displayGameMessage("ERR_NOT_LEADER", "You are not the party leader.");
        return;
    }
    if (raid) applyRaidDifficulty(n - 1, true, true);
    else applyDungeonDifficulty(n - 1, true, true);
    sendSetDifficulty(n - 1, raid);
}

// ============================================================
// LFG Handlers
// ============================================================

void SocialHandler::handleLfgJoinResult(network::Packet& packet) {
    // Two uint32s, not two bytes. Reading a byte each left the result right by
    // accident - every value in that enum fits in one - and the state wrong
    // always: it came from the second byte of the result, which is zero. So a
    // queue joined successfully set the state to None, and the dungeon finder
    // never knew it was in a queue.
    if (!packet.hasRemaining(8)) return;
    const uint32_t result = packet.readUInt32();
    const uint32_t state  = packet.readUInt32();
    if (result == 0) {
        // Nothing said here: SMSG_LFG_UPDATE_PLAYER's JOIN_QUEUE follows and
        // carries ERR_LFG_JOINED_QUEUE ("Joined (no client msg)", LFGMgr.h).
        lfgState_ = static_cast<LfgState>(state);
    } else {
        // The ERR_LFG_* GameError for the result, a chat line, from Wow.exe's
        // own table (0xACDDA8, read by 0x55BDC0): 1 and 18 the failed role
        // check - with a reason of 3 or 4 its timeout or "not viable" form -
        // and 2, 4-17 one each. 3 and anything else say nothing.
        static const char* const kKeys[] = {
            nullptr, "ERR_LFG_ROLE_CHECK_FAILED", "ERR_LFG_GROUP_FULL", nullptr,
            "ERR_LFG_NO_LFG_OBJECT", "ERR_LFG_NO_SLOTS_PLAYER", "ERR_LFG_NO_SLOTS_PARTY",
            "ERR_LFG_MISMATCHED_SLOTS", "ERR_LFG_PARTY_PLAYERS_FROM_DIFFERENT_REALMS",
            "ERR_LFG_MEMBERS_NOT_PRESENT", "ERR_LFG_GET_INFO_TIMEOUT", "ERR_LFG_INVALID_SLOT",
            "ERR_LFG_DESERTER_PLAYER", "ERR_LFG_DESERTER_PARTY", "ERR_LFG_RANDOM_COOLDOWN_PLAYER",
            "ERR_LFG_RANDOM_COOLDOWN_PARTY", "ERR_LFG_TOO_MANY_MEMBERS", "ERR_LFG_CANT_USE_DUNGEONS",
            "ERR_LFG_ROLE_CHECK_FAILED",
        };
        if (result == 1 && state == 3) {
            owner_.displayGameMessage("ERR_LFG_ROLE_CHECK_FAILED_TIMEOUT", "The role check has timed out.");
        } else if (result == 1 && state == 4) {
            owner_.displayGameMessage("ERR_LFG_ROLE_CHECK_FAILED_NOT_VIABLE", "Your group cannot be queued as it is.");
        } else if (const char* key = result < sizeof(kKeys) / sizeof(kKeys[0]) ? kKeys[result] : nullptr) {
            const char* msg = lfgJoinResultString(result);
            owner_.displayGameMessage(key, msg ? msg : "");
        }
    }
    // The queue state moved, either into a queue or back out of the attempt,
    // and LFG_UPDATE is how the panel is told to re-read it - the same event
    // handleLfgUpdate fires for the same reason two handlers down. Without it
    // the state was right in this client and stale in the interface until some
    // other LFG packet happened along.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_UPDATE", {});
}

// SMSG_LFG_PLAYER_INFO - what the random dungeons pay out, and what this
// character is not allowed to queue for.
//
// The reward block has to be walked even though none of it is kept, because
// the locks are behind it and its length depends on how many items each
// random dungeon rewards. Skipping to the end was not an option; the whole
// packet was being skipped instead.
void SocialHandler::handleLfgPlayerInfo(network::Packet& packet) {
    if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
    const uint8_t randomCount = packet.readUInt8();
    lfgRewards_.clear();
    for (uint8_t i = 0; i < randomCount; ++i) {
        if (!packet.hasRemaining(4 + 1 + 4 * 4 + 1)) { packet.skipAll(); return; }
        LfgReward r;
        r.dungeonId  = packet.readUInt32() & 0x00FFFFFFu;
        r.done       = packet.readUInt8() != 0;
        r.money      = packet.readUInt32();
        r.experience = packet.readUInt32();
        packet.readUInt32();          // the variable parts, which the server
        packet.readUInt32();          // sends as zero
        const uint8_t items = packet.readUInt8();
        for (uint8_t j = 0; j < items; ++j) {
            if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
            LfgRewardItem item;
            item.itemId        = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            item.count         = packet.readUInt32();
            r.items.push_back(item);
        }
        lfgRewards_.push_back(std::move(r));
    }

    if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
    const uint32_t lockCount = packet.readUInt32();
    lfgLocks_.clear();
    for (uint32_t i = 0; i < lockCount && packet.hasRemaining(8); ++i) {
        // The entry packs the dungeon and its type; the list is keyed by the
        // dungeon alone, which is what the interface asks with.
        const uint32_t entry = packet.readUInt32();
        const uint32_t status = packet.readUInt32();
        lfgLocks_[entry & 0x00FFFFFFu] = status;
    }
    packet.skipAll();

    LOG_INFO("SMSG_LFG_PLAYER_INFO: ", randomCount, " random dungeons, ",
             lfgLocks_.size(), " locked");
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_LOCK_INFO_RECEIVED", {});
        owner_.addonEventCallbackRef()("LFG_UPDATE_RANDOM_INFO", {});
    }
}

void SocialHandler::handleLfgQueueStatus(network::Packet& packet) {
    // Thirty-one bytes, not thirty-three: dungeon, five wait times, three
    // one-byte "needed" counts, and the time queued. The old length check was
    // longer than the packet the server sends, so this returned before reading
    // anything and the queue status never updated at all - no wait times, no
    // time in queue.
    if (!packet.hasRemaining(31)) return;
    lfgDungeonId_ = packet.readUInt32();
    // Two different numbers, and the panel shows them in two different places:
    // the average across everyone queued for this dungeon, and the estimate for
    // this player in the role they picked. They were being collapsed into one.
    lfgAvgWaitSec_ = static_cast<int32_t>(packet.readUInt32());
    lfgMyWaitSec_  = static_cast<int32_t>(packet.readUInt32());
    lfgWaitTank_   = static_cast<int32_t>(packet.readUInt32());
    lfgWaitHealer_ = static_cast<int32_t>(packet.readUInt32());
    lfgWaitDps_    = static_cast<int32_t>(packet.readUInt32());
    // Three of these, one per role, and only one was being read - so the time
    // in queue came from two bytes of the needed-counts and two of its own.
    lfgNeedTank_   = packet.readUInt8();
    lfgNeedHealer_ = packet.readUInt8();
    lfgNeedDps_    = packet.readUInt8();
    // Seconds, not milliseconds. Every time on this packet is a difference of
    // two time_t values on the server, and all three were being divided by a
    // thousand on the way in - so the queue timer read 0:00 for the whole wait
    // and the average wait time came out as zero, which the interface prints
    // through SecondsToTime as an empty string. A wait estimate that is missing
    // rather than wrong is this division.
    //
    // -1 is the server saying it does not know yet, and it is kept: the panel
    // tests for it and says so, where a zero claims the group is instant.
    lfgQueuedSeconds_ = packet.readUInt32();
    lfgState_ = LfgState::Queued;
    // Read and then kept to itself. The wait time is what the queue window
    // counts up, and LFDSearchStatus_Update is reached from this event and no
    // other - so the numbers above changed every few seconds and the display
    // built on them never moved.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_QUEUE_STATUS_UPDATE", {});
        // The queue state is set two lines above, and LFG_QUEUE_STATUS_UPDATE
        // is not the event that re-reads it: the minimap's dungeon eye is shown
        // by MiniMapLFG_UpdateIsShown, which asks GetLFGMode - which asks
        // GetLFGInfoServer - and is reached from LFG_UPDATE and nowhere else.
        //
        // SMSG_LFG_UPDATE_PLAYER usually arrives alongside this and does fire
        // it, which is why the eye appears at all. The case that has nothing
        // else behind it is a login or a reconnect already in the queue, where
        // this packet is what says so: the state was right and the eye was
        // never shown.
        owner_.addonEventCallbackRef()("LFG_UPDATE", {});
    }
}

namespace {

// 3.3.5 LfgProposalState. Nought is the proposal arriving, not the proposal
// failing: the three were read one place along, so the packet that offers the
// group announced "Group proposal failed" and closed the dialog before it was
// ever shown, and the one that says everyone accepted announced the offer.
// Reported as a group found and then a proposal failed, which is those two
// lines in the order the server sends them.
enum : uint32_t {
    kProposalInitiating = 0,
    kProposalFailed     = 1,
    kProposalSuccess    = 2,
};

// SoundEntries rows Wow.exe's Dungeon Finder handler (0x55bdc0) plays by id.
constexpr uint32_t kSoundLfgDungeonReady = 17318;  // LFG_DungeonReady
constexpr uint32_t kSoundLfgDenied       = 17341;  // LFG_Denied
constexpr uint32_t kSoundPvpEnterQueue   = 8462;   // PVPENTERQUEUE

}  // namespace

void SocialHandler::handleLfgProposalUpdate(network::Packet& packet) {
    // The header is fifteen bytes, and the state is ONE of them.
    //
    // This read three uint32s where the server writes dungeon(4), state(1),
    // id(4), encounters(4), silent(1), size(1) - so the proposal id came out of
    // the state byte plus three bytes of the id, and the state itself out of
    // the id's last byte plus three of the encounter count. Every branch below
    // was taken on a number assembled from the wrong bytes, which is why the
    // pop never behaved: it was not that the state was unhandled, it was never
    // read.
    if (!packet.hasRemaining(15)) return;
    const uint32_t dungeonEntry = packet.readUInt32();
    const uint32_t proposalState = packet.readUInt8();
    const uint32_t proposalId = packet.readUInt32();
    const uint32_t encountersDone = packet.readUInt32();
    // "silent" means do NOT raise the window - the player is already in the
    // group being proposed to, so there is nothing to accept.
    const bool silent = packet.readUInt8() != 0;
    const uint8_t groupSize = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    (void)encountersDone;

    // The entry packs the dungeon and its type, the same way
    // LFGDungeonData::Entry builds it.
    const uint32_t dungeonId = dungeonEntry & 0x00FFFFFFu;

    lfgProposalMembers_.clear();
    for (uint8_t i = 0; i < groupSize && packet.hasRemaining(9); ++i) {
        LfgProposalMember m;
        m.role      = packet.readUInt32();
        m.isSelf    = packet.readUInt8() != 0;
        m.inDungeon = packet.readUInt8() != 0;
        m.sameGroup = packet.readUInt8() != 0;
        m.answered  = packet.readUInt8() != 0;
        m.accepted  = packet.readUInt8() != 0;
        lfgProposalMembers_.push_back(m);
    }
    // LFG_Denied when the player, or someone in the player's own group,
    // answered no, with ERR_LFG_PROPOSAL_DECLINED_SELF or _PARTY (Wow.exe
    // 0x55BDC0; the party's decline wins). A stranger declining is not
    // announced here: the update putting the player back in the queue says
    // ERR_LFG_PROPOSAL_FAILED.
    bool declinedSelf = false, declinedParty = false;
    for (const auto& m : lfgProposalMembers_) {
        if (!m.answered || m.accepted) continue;
        if (m.isSelf) declinedSelf = true;
        else if (m.sameGroup) declinedParty = true;
    }
    if (declinedParty || declinedSelf) {
        owner_.playUiSoundEntry(kSoundLfgDenied);
        if (declinedParty)
            owner_.displayGameMessage("ERR_LFG_PROPOSAL_DECLINED_PARTY",
                "You have been removed from the Dungeon queue because someone in your party did not accept the invitation.");
        else
            owner_.displayGameMessage("ERR_LFG_PROPOSAL_DECLINED_SELF",
                "You have been removed from the Dungeon queue because you did not accept the invitation.");
    }
    lfgProposalFailedUnsaid_ = proposalState == kProposalFailed && !declinedSelf && !declinedParty;

    lfgDungeonId_ = dungeonId; lfgProposalId_ = proposalId;
    switch (proposalState) {
        case kProposalInitiating: {
            // The proposal dialog is what the stock client shows; no line.
            lfgState_ = LfgState::Proposal;
            break;
        }
        case kProposalFailed:
            // ERR_LFG_PROPOSAL_FAILED arrives with the PROPOSAL_FAILED update.
            lfgState_ = LfgState::Queued; lfgProposalId_ = 0;
            break;
        case kProposalSuccess: {
            lfgState_ = LfgState::InDungeon; lfgProposalId_ = 0;
            break;
        }
        default: break;
    }
    // The proposal dialog is built from GetLFGProposal, which already answers
    // from this - it just had no way to know a proposal had arrived.
    if (!owner_.addonEventCallbackRef()) return;
    auto fire = owner_.addonEventCallbackRef();
    fire("LFG_PROPOSAL_UPDATE", {});

    // UPDATE alone only refreshes a dialog already on screen.
    // LFDFrame_OnEvent opens it on SHOW - that branch is the one calling
    // StaticPopupSpecial_Show - and closes it on SUCCEEDED or FAILED. None of
    // the three was fired, so a pop arrived as a line of chat and nothing else,
    // with no way to accept it.
    //
    // SHOW once per proposal rather than on every update: the server resends
    // this as each member answers, and showing again would reset the dialog's
    // countdown and its per-member ticks every time.
    switch (proposalState) {
        case kProposalInitiating:
            // Not when the server says silent: that means the player is
            // already in the group being proposed to and has nothing to answer.
            if (!silent && shownProposalId_ != proposalId) {
                shownProposalId_ = proposalId;
                fire("LFG_PROPOSAL_SHOW", {});
            }
            break;
        case kProposalFailed:
            shownProposalId_ = 0;
            fire("LFG_PROPOSAL_FAILED", {});
            break;
        case kProposalSuccess:
            shownProposalId_ = 0;
            fire("LFG_PROPOSAL_SUCCEEDED", {});
            break;
        default:
            break;
    }
}

void SocialHandler::handleLfgRoleCheckUpdate(network::Packet& packet) {
    // state is the uint32; the uint8 after it says whether the check is only
    // starting. This read the state and threw it away, then took the starting
    // flag for the state - so the branches below ran on 0 or 1 and never saw
    // the values they test for.
    if (!packet.hasRemaining(6)) return;
    const uint32_t roleCheckState = packet.readUInt32();
    const bool initiating = packet.readUInt8() != 0;

    // The dungeons being checked for, and how many are in the group. Both were
    // stopped at: the count was read and the list behind it left on the wire.
    //
    // That is not merely a blank. GetLFGRoleUpdate answered nil for the slot
    // count, and the popup's own tooltip opens with `if ( slots <= 1 )` -
    // comparing nil to a number, which raises. Hovering the role-check popup
    // took the file down.
    //
    // Each dungeon arrives as its LFGDungeons entry, which packs the type into
    // the top byte the same way the queue list does above.
    lfgRoleCheckDungeons_.clear();
    const uint8_t dungeonCount = packet.readUInt8();
    for (uint8_t i = 0; i < dungeonCount && packet.hasRemaining(4); ++i) {
        const uint32_t entry = packet.readUInt32();
        lfgRoleCheckDungeons_.push_back({.dungeonId = entry & 0x00FFFFFFu,
                                         .typeId = static_cast<uint8_t>((entry >> 24) & 0xFFu)});
    }
    lfgRoleCheckMembers_ = packet.hasRemaining(1) ? packet.readUInt8() : 0;

    if (roleCheckState == 1) lfgState_ = LfgState::Queued;
    // Wow.exe (0x55BDC0) says two things here: ERR_LFG_ROLE_CHECK_INITIATED
    // as a check begins and ERR_LFG_ROLE_CHECK_ABORTED when one is aborted
    // (5). A check that fails for a missing role (3) is told by the join
    // result that follows (ERR_LFG_ROLE_CHECK_FAILED).
    else if (roleCheckState == 3) lfgState_ = LfgState::None;
    else if (roleCheckState == 2) lfgState_ = LfgState::RoleCheck;
    // LFG_ROLECHECK_WRONG_ROLES (4), ABORTED (5) and NO_ROLE (6) end the
    // check without a queue, the same as MISSING_ROLE (3): LFGMgr::
    // UpdateRoleCheck sends the join result and removes the group's state.
    // Left at RoleCheck, the finder believed a check was still running and
    // the queue buttons stayed in their "checking roles" state.
    else if (roleCheckState >= 4 && roleCheckState <= 6) lfgState_ = LfgState::None;
    // Wow.exe's Dungeon Finder handler (0x55bdc0) plays LFG_DungeonReady
    // (17318) as a role check begins - the initiating flag, with
    // ERR_LFG_ROLE_CHECK_INITIATED - and LFG_Denied (17341) when one ends
    // aborted (5, someone left; ERR_LFG_ROLE_CHECK_ABORTED) or with someone
    // choosing no role (6).
    if (initiating) {
        owner_.displayGameMessage("ERR_LFG_ROLE_CHECK_INITIATED",
            "A role check has been initiated. Your group will be queued when all members have selected a role.");
        owner_.playUiSoundEntry(kSoundLfgDungeonReady);
    }
    if (roleCheckState == 5 || roleCheckState == 6) owner_.playUiSoundEntry(kSoundLfgDenied);
    if (roleCheckState == 5)
        owner_.displayGameMessage("ERR_LFG_ROLE_CHECK_ABORTED", "Your group leader has cancelled the Role Check.");
    // Likewise GetLFGRoleUpdate, which reports the check in progress.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_ROLE_CHECK_UPDATE", {});
        // The role-check popup shows on one event and hides on another, and
        // neither was ever fired - so the interface's own popup could not
        // appear at all, whichever side is drawing it.
        //
        // State 2 is LFG_ROLECHECK_INITIALITING, where the check begins.
        // Everything else is an ending: finished, missing a role, the wrong
        // roles between them, aborted, or someone picking none.
        constexpr uint32_t kRoleCheckBeginning = 2;
        owner_.addonEventCallbackRef()(
            roleCheckState == kRoleCheckBeginning ? "LFG_ROLE_CHECK_SHOW"
                                                  : "LFG_ROLE_CHECK_HIDE", {});
    }
}

/// SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY.
///
/// Both open with the update type and a flag saying whether the rest of the
/// packet is there. Neither was being read: the flag was *inferred* from the
/// update type by a list of the types thought not to carry a body, and the
/// body itself - which is where the server says whether you are queued - was
/// skipped three bytes at a time.
///
/// An update type once read as "left the queue" was the server's periodic
/// restatement of it, so a player queued for a dungeon was thrown out of the
/// queue by the packet whose whole job is to confirm they are still in it -
/// locally, while the server kept them queued, which is what made it look
/// like being dropped for no reason.
///
/// Reading the flags instead of the type is what stops that returning: the
/// server states the queue in the packet, and the update type only decides
/// what to say about it.
namespace {

// 3.3.5 LfgUpdateType, as the server writes it: AzerothCore's LFG.h, and the
// numbers Wow.exe's own handler (0x55bdc0) tests - 7 and 15 for leaving the
// queue, 10 for a group found, 16 for a party disbanding. This table used to
// run one high from 6 on (15 as UPDATE_STATUS, which is 14), with 6 taken as
// a second spelling of the join.
enum : uint8_t {
    kLfgDefault             = 0,
    kLfgLeaderUnk1          = 1,
    kLfgRoleCheckAborted    = 4,
    kLfgJoinQueue           = 5,
    kLfgRoleCheckFailed     = 6,
    kLfgRemovedFromQueue    = 7,
    kLfgProposalFailed      = 8,
    kLfgProposalDeclined    = 9,
    kLfgGroupFound          = 10,
    kLfgAddedToQueue        = 12,
    kLfgProposalBegin       = 13,
    kLfgUpdateStatus        = 14,
    kLfgGroupMemberOffline  = 15,
    kLfgGroupDisband        = 16,
};

}  // namespace

void SocialHandler::handleLfgUpdatePlayer(network::Packet& packet) {
    //   uint8  updateType
    //   uint8  extraInfo
    //   if extraInfo:
    //     uint8  queued
    //     uint8  0
    //     uint8  0
    //     uint8  dungeonCount
    //     uint32 dungeon x dungeonCount
    //     cstring comment
    if (!packet.hasRemaining(2)) return;
    const uint8_t updateType = packet.readUInt8();
    const bool extraInfo = packet.readUInt8() != 0;
    bool queued = false;
    if (extraInfo && packet.hasRemaining(4)) {
        queued = packet.readUInt8() != 0;
        packet.readUInt8();
        packet.readUInt8();
        readLfgDungeonList(packet);
    }
    // A proposal that was up when a solo player's update shows them back in
    // the queue failed: Wow.exe (0x55BDC0, LFG_UPDATE_PLAYER) clears it and
    // says ERR_LFG_PROPOSAL_FAILED. Checked before applyLfgUpdate moves the
    // state on.
    if (!owner_.isInGroup()) endProposalOnRequeue(updateType, extraInfo, queued);
    applyLfgUpdate(updateType, extraInfo, queued, /*inDungeon=*/false);
    // For the player's own update Wow.exe counts the body's presence as
    // having joined.
    playLfgUpdateSound(lfgPlayerFlagsSeen_, updateType, extraInfo, queued, /*joinCounts=*/true);
}

void SocialHandler::handleLfgUpdateParty(network::Packet& packet) {
    // The same opening, and then a body of its own - a join flag ahead of the
    // queued one and three more bytes before the count. Routed to the player
    // handler before this existed, which read the join flag as "queued" and
    // the dungeon count out of the middle of the padding.
    //
    //   uint8  updateType
    //   uint8  extraInfo
    //   if extraInfo:
    //     uint8  joined
    //     uint8  queued
    //     uint8  0
    //     uint8  0
    //     uint8  0 x3
    //     uint8  dungeonCount
    //     uint32 dungeon x dungeonCount
    //     cstring comment
    if (!packet.hasRemaining(2)) return;
    const uint8_t updateType = packet.readUInt8();
    const bool extraInfo = packet.readUInt8() != 0;
    bool queued = false;
    bool joined = false;
    uint8_t dungeonCount = 0;
    if (extraInfo && packet.hasRemaining(8)) {
        joined = packet.readUInt8() != 0;
        queued = packet.readUInt8() != 0;
        for (int i = 0; i < 5; ++i) packet.readUInt8();
        dungeonCount = readLfgDungeonList(packet);
    }
    // The same for a group, on the party's update (its join flag).
    if (owner_.isInGroup()) endProposalOnRequeue(updateType, joined, queued);
    // The party's join flag covers being in the dungeon as well as being in the
    // queue, so it only means "in a dungeon" when the queue flag is off.
    applyLfgUpdate(updateType, extraInfo, queued, joined && !queued);
    // A party join only counts as entering the queue with dungeons named.
    playLfgUpdateSound(lfgPartyFlagsSeen_, updateType, joined, queued,
                       /*joinCounts=*/dungeonCount != 0);
}

/// The queue sounds of Wow.exe's Dungeon Finder handler (0x55bdc0), from the
/// difference between this update and the last of its kind:
///
///   - joined where the last was not: PVPENTERQUEUE (8462), with
///     ERR_LFG_JOINED_QUEUE or _LIST;
///   - a join, or failing that a queue, the last update held and this one
///     does not, when the update type is REMOVED_FROM_QUEUE (7) or
///     GROUP_MEMBER_OFFLINE (15): LFG_Denied (17341), with ERR_LFG_LEFT_QUEUE
///     or _LIST. Leaving by entering the dungeon is silent.
void SocialHandler::playLfgUpdateSound(LfgFlagsSeen& last, uint8_t updateType, bool joined,
                                       bool queued, bool joinCounts) {
    const LfgFlagsSeen before = last;
    last.joined = joined;
    last.queued = queued;
    if (joined == before.joined && queued == before.queued) return;
    const bool leftType = updateType == kLfgRemovedFromQueue || updateType == kLfgGroupMemberOffline;
    if (joined && !before.joined && joinCounts) {
        owner_.playUiSoundEntry(kSoundPvpEnterQueue);
        owner_.displayGameMessage("ERR_LFG_JOINED_QUEUE", "You are now queued in the Dungeon Finder.");
        return;
    }
    const bool lostJoin = !joined && before.joined;
    const bool lostQueue = !queued && before.queued;
    if ((lostJoin || lostQueue) && leftType && lfgState_ != LfgState::InDungeon) {
        owner_.playUiSoundEntry(kSoundLfgDenied);
        owner_.displayGameMessage("ERR_LFG_LEFT_QUEUE", "You are no longer queued in the Dungeon Finder.");
    }
}

/// ERR_LFG_PROPOSAL_FAILED for a proposal the update shows was given up: one
/// was up, and the update is joined and queued again. Wow.exe tests only the
/// flags; UPDATE_STATUS is left out here because AzerothCore restates the
/// queue with it, queued set, while a proposal is still being answered
/// (see applyLfgUpdate), which the stock client never meets from Blizzard's
/// servers. The proposal's own FAILED state fires LFG_PROPOSAL_FAILED.
void SocialHandler::endProposalOnRequeue(uint8_t updateType, bool joined, bool queued) {
    // AzerothCore sends the proposal's FAILED state before the update, which
    // already ended it here; that is remembered until the update comes.
    const bool proposalUp = (lfgProposalId_ != 0 && lfgState_ == LfgState::Proposal) ||
                            lfgProposalFailedUnsaid_;
    if (!proposalUp || !joined || !queued || updateType == kLfgUpdateStatus) return;
    lfgProposalFailedUnsaid_ = false;
    owner_.displayGameMessage("ERR_LFG_PROPOSAL_FAILED",
        "Someone has declined the dungeon invite. You have been returned to the front of the dungeon queue.");
}

/// The dungeons the update is about. The first is the one the queue window
/// names; the comment after them is the group's listing text, which this
/// client does not offer a way to set.
uint8_t SocialHandler::readLfgDungeonList(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return 0;
    const uint8_t count = packet.readUInt8();
    for (uint8_t i = 0; i < count && packet.hasRemaining(4); ++i) {
        const uint32_t dungeonEntry = packet.readUInt32();
        if (i == 0) lfgDungeonId_ = dungeonEntry & 0x00FFFFFFu;
    }
    return count;
}

/// The state the packet describes, and a word about it if it moved.
void SocialHandler::applyLfgUpdate(uint8_t updateType, bool extraInfo,
                                   bool queued, bool inDungeon) {
    const LfgState previous = lfgState_;

    // A proposal is the one state the flags do not distinguish: the server
    // sends it with the queue flag still set, because a proposal is a queue
    // that has found something.
    if (updateType == kLfgGroupFound || updateType == kLfgProposalBegin) {
        lfgState_ = LfgState::Proposal;
    } else if (!extraInfo) {
        // No body means no state, which is the server's way of saying none -
        // and for the types that never carry one it is not a statement about
        // the queue at all, so those are left alone.
        switch (updateType) {
            case kLfgDefault:
            case kLfgLeaderUnk1:
            case kLfgGroupMemberOffline:
                break;
            case kLfgRemovedFromQueue:
                // AzerothCore takes a matched player out of the queue as the
                // group forms; the proposal's success already put us in the
                // dungeon, and GetLFGMode must go on saying "lfgparty".
                if (previous != LfgState::InDungeon) lfgState_ = LfgState::None;
                break;
            default:
                lfgState_ = LfgState::None;
                break;
        }
    } else if (queued) {
        // A proposal is still a queue on the wire: AzerothCore restates the
        // queue (UPDATE_STATUS, queued set) while the proposal is up, and
        // taking that as "just queued" dropped the open proposal - GetLFGMode
        // went back to "queued" and GetLFGProposal to nil, so the dialog had
        // nothing to accept and the proposal timed out. The proposal ends on
        // its own update (failed or succeeded).
        if (!(previous == LfgState::Proposal && lfgProposalId_ != 0)) lfgState_ = LfgState::Queued;
    } else if (inDungeon) {
        lfgState_ = LfgState::InDungeon;
    }

    // No line by update type: Wow.exe (0x55BDC0) speaks from the flags
    // alone - joined or left (playLfgUpdateSound) and a proposal given up
    // (endProposalOnRequeue). The role check, the proposal's declines and
    // the join result have their own packets and say their own lines.
    if (updateType > kLfgGroupDisband) {
        // An update type this client has no name for. Said once so it can be
        // added, rather than silently deciding the queue is over.
        static std::set<uint8_t> said;
        if (said.insert(updateType).second) {
            LOG_WARNING("SMSG_LFG_UPDATE: unhandled update type ",
                        static_cast<int>(updateType), " (extraInfo=", extraInfo,
                        ", queued=", queued, ") - the queue state is left as it was");
        }
    }

    // Queue state changed - GetLFGInfoServer answers the new one.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_UPDATE", {});
}

void SocialHandler::handleLfgPlayerReward(network::Packet& packet) {
    // SMSG_LFG_PLAYER_REWARD, against WorldSession::SendLfgPlayerReward:
    //
    //   uint32 rdungeonEntry   the random entry queued for
    //   uint32 sdungeonEntry   the dungeon actually finished
    //   uint8  done
    //   uint32 1               a constant the server writes and nothing reads
    //   uint32 money           GetRewOrReqMoney(level), in copper
    //   uint32 xp              XPValue(level)
    //   uint32 0
    //   uint32 0
    //   uint8  itemNum
    //   itemNum x { uint32 itemId, uint32 displayId, uint32 count }
    //
    // This read the constant as the money, the money as the XP and the XP as
    // the item count - so the line it printed was always "1c" followed by the
    // real money labelled XP, and then it looped tens of thousands of times
    // over whatever came next looking for items.
    //
    // packet_layout_check does cover this opcode and did not catch it, for a
    // reason worth keeping: it compares field *widths*, and every field misread
    // here is four bytes wide. Delete one of them and it reports the shape
    // change immediately; shift the meaning of five fields that are all uint32
    // and the two readings line up byte for byte. A same-width misalignment is
    // the one shape that sweep cannot see.
    if (!packet.hasRemaining(4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1)) return;
    LfgCompletionReward reward;
    reward.randomDungeonId = packet.readUInt32();
    reward.dungeonId       = packet.readUInt32();
    reward.done            = packet.readUInt8() != 0;
    // Wow.exe (0x55BDC0) names the rest: strangers, base money, base XP,
    // variable money, variable XP - the variable parts paid per stranger.
    const uint32_t strangers = packet.readUInt32();
    reward.money           = packet.readUInt32();
    reward.xp              = packet.readUInt32();
    const uint32_t varMoney = packet.readUInt32();
    const uint32_t varXp    = packet.readUInt32();
    const uint8_t itemNum = packet.readUInt8();
    for (uint8_t i = 0; i < itemNum && packet.hasRemaining(12); ++i) {
        LfgCompletionReward::Item item;
        item.itemId    = packet.readUInt32();
        item.displayId = packet.readUInt32();
        item.count     = packet.readUInt32();
        reward.items.push_back(item);
    }
    reward.valid = true;
    lfgCompletionReward_ = reward;

    // Chat lines before the toast, as Wow.exe gives them:
    // ERR_QUEST_REWARD_EXP_I for the base XP and again for the per-stranger
    // XP, ERR_QUEST_REWARD_MONEY_S for each money part.
    if (reward.xp > 0)
        owner_.displayGameMessage("ERR_QUEST_REWARD_EXP_I", "Experience gained: " + std::to_string(reward.xp) + ".",
                                  {std::to_string(reward.xp)});
    if (strangers * varXp > 0)
        owner_.displayGameMessage("ERR_QUEST_REWARD_EXP_I", "Experience gained: " + std::to_string(strangers * varXp) + ".",
                                  {std::to_string(strangers * varXp)});
    if (reward.money > 0) {
        const std::string m = owner_.formatMoneyText(reward.money);
        owner_.displayGameMessage("ERR_QUEST_REWARD_MONEY_S", "Received " + m + ".", {m});
    }
    if (strangers * varMoney > 0) {
        const std::string m = owner_.formatMoneyText(strangers * varMoney);
        owner_.displayGameMessage("ERR_QUEST_REWARD_MONEY_S", "Received " + m + ".", {m});
    }
    lfgState_ = LfgState::FinishedDungeon;

    // The reward toast. It carries no arguments - the frame reads the nine
    // values back through GetLFGCompletionReward - so the store above has to
    // be written before this fires.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_COMPLETION_REWARD", {});

}

void SocialHandler::handleLfgBootProposalUpdate(network::Packet& packet) {
    // The victim's GUID sits between the three flags and the counts, and was
    // not being read - so every number after it came from eight bytes earlier
    // than it should have.
    if (!packet.hasRemaining(27)) return;
    bool inProgress = packet.readUInt8() != 0;
    // Both were read and dropped. The vote dialog needs them: didVote decides
    // whether it shows the buttons or the tally, and myVote which of the two
    // is lit.
    lfgBootDidVote_ = packet.readUInt8() != 0;
    lfgBootMyVote_  = packet.readUInt8() != 0;
    lfgBootInProgress_ = inProgress;
    lfgBootVictimGuid_ = packet.readUInt64();
    uint32_t totalVotes = packet.readUInt32();
    uint32_t bootVotes = packet.readUInt32();
    uint32_t timeLeft = packet.readUInt32();
    uint32_t votesNeeded = packet.readUInt32();
    lfgBootVotes_ = bootVotes; lfgBootTotal_ = totalVotes;
    lfgBootTimeLeft_ = timeLeft; lfgBootNeeded_ = votesNeeded;
    if (packet.getReadPos() < packet.getSize()) lfgBootReason_ = packet.readString();
    // The packet carries one string and it is the reason. The victim is the
    // guid above - AzerothCore writes `data << boot.victim` and then the
    // reason, and nothing else - so a second string was being looked for that
    // is never there, and the name it was meant to fill stayed empty. Resolved
    // from the guid instead, which is how every other name here is found.
    lfgBootTargetName_ = owner_.lookupName(lfgBootVictimGuid_);
    if (lfgBootTargetName_.empty() && lfgBootVictimGuid_ != 0) {
        owner_.queryPlayerName(lfgBootVictimGuid_);
    }
    if (inProgress) { lfgState_ = LfgState::Boot; }
    else {
        // ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED or _FAILED naming the victim, a
        // chat line (Wow.exe 0x55BDC0: failed when the votes fell short).
        const bool bootPassed = (bootVotes >= votesNeeded);
        owner_.displayGameMessageNaming(lfgBootVictimGuid_,
            bootPassed ? "ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED" : "ERR_PARTY_LFG_BOOT_VOTE_FAILED",
            bootPassed ? "The vote to kick %s has passed." : "The vote to kick %s has failed.",
            {GameHandler::kPlayerNameArg});
        lfgBootVotes_ = lfgBootTotal_ = lfgBootTimeLeft_ = lfgBootNeeded_ = 0;
        lfgBootTargetName_.clear(); lfgBootReason_.clear();
        lfgBootDidVote_ = lfgBootMyVote_ = lfgBootInProgress_ = false;
        lfgBootVictimGuid_ = 0;
        lfgState_ = LfgState::InDungeon;
    }
    // The vote-to-kick dialog, whose countdown starts on this.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_BOOT_PROPOSAL_UPDATE", {});
}

void SocialHandler::handleLfgTeleportDenied(network::Packet& packet) {
    // A uint32, read as a byte. Little-endian kept it working - every reason
    // is a small number - but the read was the wrong width and the next edit
    // to this handler would have inherited it.
    if (!packet.hasRemaining(4)) return;
    const uint32_t reason = packet.readUInt32();
    // Wow.exe's jump table (0x55D148): 1 dead, 2 falling, 3 locked out,
    // 4 fatigued, 6 an invalid location (then "locked out" as well, its case
    // falling through), past 7 locked out; 0, 5 and 7 say nothing.
    switch (reason) {
        case 1: owner_.displayGameMessage("ERR_PLAYER_DEAD", "You can't do that when you're dead."); break;
        case 2: owner_.displayGameMessage("ERR_NOT_WHILE_FALLING", "You can't do that while jumping or falling"); break;
        case 4: owner_.displayGameMessage("ERR_NOT_WHILE_FATIGUED", "You can't do that while fatigued"); break;
        case 6:
            owner_.displayGameMessage("ERR_INVALID_TELEPORT_LOCATION", "Invalid teleport location");
            [[fallthrough]];
        case 3:
            owner_.displayGameMessage("ERR_CLIENT_LOCKED_OUT", "You can't do that right now.");
            break;
        case 0: case 5: case 7: break;
        default: owner_.displayGameMessage("ERR_CLIENT_LOCKED_OUT", "You can't do that right now."); break;
    }
}

// ============================================================
// LFG Outgoing Packets
// ============================================================

void SocialHandler::lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (dungeonIds.empty()) return;   // the server drops a join with no slots
    // The dungeon finder is Wrath's. Classic and TBC have no CMSG_LFG_JOIN at
    // all - TBC's 0x35C is CMSG_LFG_SET_AUTOJOIN, an unrelated packet - so the
    // logical opcode is unmapped there and wireOpcode answers 0xFFFF. Sending
    // that would put a packet the server has no handler for on the wire.
    if (wireOpcode(Opcode::CMSG_LFG_JOIN) == 0xFFFF) return;
    owner_.getSocket()->send(LfgJoinPacket::build(dungeonIds, roles));
}

void SocialHandler::lfgLeave() {
    if (!owner_.getSocket()) return;
    lfgState_ = LfgState::None;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_LEAVE);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    pkt.writeUInt32(0); pkt.writeUInt32(0); pkt.writeUInt32(0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgSetRoles(uint8_t roles) {
    lfgOfferedRoles_ = roles;
    // Three frames draw the same three role checkboxes - the dungeon queue, the
    // raid browser queue and the role-check popup - and every one of them ticks
    // from GetLFGRoles when this event arrives. Setting the roles without
    // firing it left whichever frame was clicked correct and the other two
    // showing what they were last drawn with, which is the shape that reads as
    // "it only takes after a relog".
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_ROLE_UPDATE", {});
    }
    // Kept, not sent. CMSG_LFG_SET_ROLES is the answer to a role check
    // (HandleLfgSetRolesOpcode -> LFGMgr::UpdateRoleCheck); the role-check
    // popup ticks its boxes through SetLFGRoles and then answers with
    // CompleteLFGRoleCheck, and a join carries its roles in CMSG_LFG_JOIN.
    // Sending on every tick answered a check with half-chosen roles.
}

void SocialHandler::lfgCompleteRoleCheck(bool accept) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t wire = wireOpcode(Opcode::CMSG_LFG_SET_ROLES);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(static_cast<uint16_t>(wire));
    // Declining is answering with no role, which UpdateRoleCheck turns into
    // LFG_ROLECHECK_NO_ROLE for the whole group.
    pkt.writeUInt8(accept ? lfgOfferedRoles_ : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (!owner_.getSocket()) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_PROPOSAL_RESULT);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    pkt.writeUInt32(proposalId); pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgTeleport(bool out) {
    if (!owner_.getSocket()) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_TELEPORT);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    // HandleLfgTeleportOpcode reads `bool out` and passes it to
    // LFGMgr::TeleportPlayer(player, out); minimap.lua's TeleportOut calls
    // LFGTeleport(true) and TeleportIn LFGTeleport(false). Written as given -
    // it was inverted, so "Teleport Out" sent the player in, and the reverse.
    pkt.writeUInt8(out ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgSetBootVote(bool vote) {
    if (!owner_.getSocket()) return;
    uint16_t wireOp = wireOpcode(Opcode::CMSG_LFG_SET_BOOT_VOTE);
    if (wireOp == 0xFFFF) return;
    network::Packet pkt(wireOp);
    pkt.writeUInt8(vote ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

// ============================================================
// Arena Handlers
// ============================================================

// command(4) + team name + player name + error(4).
//
// The player name was never read, so the error came out of the first four
// bytes of it - a name, so never zero, and every command result read as a
// failure however well it had gone. Where the server sends no player name the
// error slid one byte instead, which is the same fault with a different value.
// Both strings are needed anyway: the server names the team and the player
// separately because most of these results are about one acting on the other.
void SocialHandler::handleArenaTeamCommandResult(network::Packet& packet) {
    if (!packet.hasRemaining(10)) return;
    const uint32_t command = packet.readUInt32();
    const std::string teamName = packet.readString();
    const std::string playerName = packet.readString();
    if (!packet.hasRemaining(4)) return;
    const uint32_t error = packet.readUInt32();

    // Every answer as Wow.exe's handler (0x6CCB20) gives it, all chat lines;
    // codes it does not know say nothing.
    const std::string& team = teamName;
    const std::string& who = playerName;
    if (error == 0) {
        switch (command) {
            case 0x00: owner_.displayGameMessage("ERR_ARENA_TEAM_CREATE_S", team + " created.  To disband, use /teamdisband [2v2, 3v3, 5v5].", {team}); break;
            case 0x01: owner_.displayGameMessage("ERR_ARENA_TEAM_INVITE_SS", "You have invited " + who + " to join " + team, {who, team}); break;
            case 0x03: owner_.displayGameMessage("ERR_ARENA_TEAM_QUIT_S", "You are no longer a member of " + team, {team}); break;
            case 0x0E: owner_.displayGameMessage("ERR_ARENA_TEAM_FOUNDER_S", "Congratulations, you are a founding member of " + team + "!  To leave, use /teamquit [2v2, 3v3, 5v5].", {team}); break;
            default: break;
        }
        return;
    }
    struct Answer { uint32_t code; const char* key; const char* text; bool team; };
    static constexpr Answer kAnswers[] = {
        {0x01, "ERR_ARENA_TEAM_INTERNAL", "Internal arena team error", false},
        {0x02, "ERR_ALREADY_IN_ARENA_TEAM", "You are already in an arena team of that size", false},
        {0x03, "ERR_ALREADY_IN_ARENA_TEAM_S", "%s is already in an arena team of that size", false},
        {0x04, "ERR_INVITED_TO_ARENA_TEAM", "You have already been invited into an arena team", false},
        {0x05, "ERR_ALREADY_INVITED_TO_ARENA_TEAM_S", "%s has already been invited to an arena team", false},
        {0x06, "ERR_ARENA_TEAM_NAME_INVALID", "That name contains invalid characters, please enter a new name", false},
        {0x07, "ERR_ARENA_TEAM_NAME_EXISTS_S", "There is already an arena team named \"%s\"", true},
        {0x09, "ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM", "You are not in an arena team of that size", false},
        {0x0B, "ERR_ARENA_TEAM_PLAYER_NOT_FOUND_S", "\"%s\" not found", false},
        {0x0C, "ERR_ARENA_TEAM_NOT_ALLIED", "You cannot invite players from the opposing alliance", false},
        {0x13, "ERR_IGNORING_YOU_S", "%s is ignoring you.", false},
        {0x15, "ERR_ARENA_TEAM_TARGET_TOO_LOW_S", "%s is not high enough level to join your team", false},
        {0x16, "ERR_ARENA_TEAM_TARGET_TOO_HIGH_S", "%s is too high level to join your team.", false},
        {0x17, "ERR_ARENA_TEAM_TOO_MANY_MEMBERS_S", "%s has too many members.", true},
        {0x1B, "ERR_ARENA_TEAM_NOT_FOUND", "Arena team not found.", false},
        {0x1E, "ERR_ARENA_TEAMS_LOCKED", "Arena teams are locked for the season.", false},
    };
    if (error == 0x08) {
        if (command == 0x03)
            owner_.displayGameMessage("ERR_ARENA_TEAM_LEADER_LEAVE_S", "You must promote a new team captain using /teamcaptain before leaving the team", {team});
        else
            owner_.displayGameMessage("ERR_ARENA_TEAM_PERMISSIONS", "You don't have permission to do that");
        return;
    }
    if (error == 0x0A) {
        owner_.displayGameMessage("ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM_SS", who + " is not in " + team, {who, team});
        return;
    }
    for (const auto& a : kAnswers) {
        if (a.code != error) continue;
        const std::string& arg = a.team ? team : who;
        owner_.displayGameMessage(a.key, formatGlobalString(a.text, {arg}), {arg});
        return;
    }
}

void SocialHandler::handleArenaTeamQueryResponse(network::Packet& packet) {
    // The team cache's answer (0x635480): the name, size and tabard, kept by
    // team id. The cache's callback (0x5A28C0) fires ARENA_TEAM_UPDATE, which
    // is what the PvP frame redraws its team tabs on.
    arena::TeamQuery q;
    if (!arena::parseTeamQuery(packet, q)) return;
    packet.skipAll();
    arenaTeamQueriesPending_.erase(q.teamId);
    for (auto& st : arenaTeamStats_) {
        if (st.teamId == q.teamId) { st.teamName = q.name; st.teamType = q.type; }
    }
    arenaTeamCache_[q.teamId] = std::move(q);
    owner_.fireAddonEvent("ARENA_TEAM_UPDATE", {});
}

void SocialHandler::handleArenaTeamRoster(network::Packet& packet) {
    // teamId(4) + unk308(1) + memberCount(4) + teamType(4), then the members.
    //
    // The team type was never read, so the first member's guid was taken from
    // four bytes earlier - the type plus the guid's low half - and every
    // member after it followed from there. The roster was wrong from its first
    // row, and nothing about it looked like a length fault: the packet is long
    // enough either way.
    if (!packet.hasRemaining(13)) return;
    const uint32_t teamId = packet.readUInt32();
    // 3.0.8 added this, and it does more than mark a version: the server writes
    // the two trailing floats per member only when it is set.
    const bool hasTrailingFloats = packet.readUInt8() != 0;
    const uint32_t memberCount = std::min(packet.readUInt32(), 100u);
    /*uint32_t teamType =*/ packet.readUInt32();
    ArenaTeamRoster roster; roster.teamId = teamId; roster.members.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        ArenaTeamMember m;
        m.guid = packet.readUInt64(); m.online = (packet.readUInt8() != 0); m.name = packet.readString();
        // Captain flag, level and class sit between the name and the record -
        // six bytes this went straight past, so the games played were the
        // captain flag, the wins were the level and class with two bytes of the
        // real figure behind them, and so on down the row.
        if (!packet.hasRemaining(6)) break;
        m.isCaptain = (packet.readUInt32() == 0);   // 0 captain, 1 member
        m.level     = packet.readUInt8();
        m.classId   = packet.readUInt8();
        if (!packet.hasRemaining(20)) break;
        m.weekGames = packet.readUInt32(); m.weekWins = packet.readUInt32();
        m.seasonGames = packet.readUInt32(); m.seasonWins = packet.readUInt32(); m.personalRating = packet.readUInt32();
        // Only when the header said so. Taking them whenever eight bytes were
        // left ate the next member's guid on every roster of more than one.
        if (hasTrailingFloats) {
            if (!packet.hasRemaining(8)) break;
            packet.readFloat(); packet.readFloat();
        }
        roster.members.push_back(std::move(m));
    }
    bool replaced = false;
    for (auto& r : arenaTeamRosters_) { if (r.teamId == teamId) { r = std::move(roster); replaced = true; break; } }
    if (!replaced) arenaTeamRosters_.push_back(std::move(roster));

    // The roster is stored and the arena panel is told, which it was not: the
    // team detail window redraws on this event and on nothing else, so a
    // roster that arrived after the window opened was held here and never
    // shown.
    //
    // Fired with no arguments, and that is the whole contract. pvpframe reads
    // arg1 as "the roster you hold is stale, ask for it again" and only
    // redraws from what arrived when arg1 is absent - so any argument at all
    // would answer this roster by requesting another one, forever. A zero
    // would do it too, zero being true in Lua.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("ARENA_TEAM_ROSTER_UPDATE", {});
    }
}

void SocialHandler::handleArenaTeamInvite(network::Packet& packet) {
    std::string playerName = packet.readString();
    std::string teamName = packet.readString();
    // ARENA_TEAM_INVITE_REQUEST and a chat line - which Wow.exe (0x6CC910)
    // words with the guild invitation's GameError, ERR_INVITED_TO_GUILD_SSS.
    owner_.fireAddonEvent("ARENA_TEAM_INVITE_REQUEST", {playerName, teamName});
    owner_.displayGameMessage("ERR_INVITED_TO_GUILD_SSS",
        playerName + " invites you to join " + teamName + ".", {playerName, playerName, teamName});
}

void SocialHandler::handleArenaTeamEvent(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t event = packet.readUInt8();
    uint8_t strCount = 0;
    if (packet.hasRemaining(1)) strCount = packet.readUInt8();
    std::vector<std::string> p;
    for (uint8_t i = 0; i < strCount && i < 3 && packet.getSize() > packet.getReadPos(); ++i)
        p.push_back(packet.readString());
    packet.skipAll();
    // ArenaTeamEvents as Wow.exe (0x6CC980) words them, chat lines, with all
    // the strings sent: 3 a join (the player's own is ERR_ARENA_TEAM_YOU_JOIN_S
    // with the team alone), 4 a leave, 5 a removal, 6 the captain, 7 a new
    // captain, 8 disbanded; anything else ERR_GUILD_INTERNAL.
    auto at = [&](size_t i) { return i < p.size() ? p[i] : std::string(); };
    switch (event) {
        case 3:
            if (at(0) == owner_.lookupName(owner_.getPlayerGuid()))
                owner_.displayGameMessage("ERR_ARENA_TEAM_YOU_JOIN_S", "You have joined " + at(1) + ".", {at(1)});
            else
                owner_.displayGameMessage("ERR_ARENA_TEAM_JOIN_SS", at(0) + " has joined " + at(1) + ".", p);
            break;
        case 4: owner_.displayGameMessage("ERR_ARENA_TEAM_LEAVE_SS", at(0) + " has left " + at(1) + ".", p); break;
        case 5: owner_.displayGameMessage("ERR_ARENA_TEAM_REMOVE_SSS", at(0) + " has been kicked out of " + at(1) + " by " + at(2) + ".", p); break;
        case 6: owner_.displayGameMessage("ERR_ARENA_TEAM_LEADER_IS_SS", at(0) + " is the captain of " + at(1) + ".", p); break;
        case 7: owner_.displayGameMessage("ERR_ARENA_TEAM_LEADER_CHANGED_SSS", at(0) + " has made " + at(1) + " the new captain of " + at(2) + ".", p); break;
        case 8: owner_.displayGameMessage("ERR_ARENA_TEAM_DISBANDED_S", at(0) + " has disbanded " + at(1) + ".", p); break;
        default: owner_.displayGameMessage("ERR_GUILD_INTERNAL", "Internal guild error."); break;
    }
    // 0x6CC980 -> 0x5A28E0: the rosters are stale; ARENA_TEAM_ROSTER_UPDATE
    // with "1" tells the PvP frame to ask again.
    owner_.fireAddonEvent("ARENA_TEAM_ROSTER_UPDATE", {"1"});
}

void SocialHandler::handleArenaTeamStats(network::Packet& packet) {
    if (!packet.hasRemaining(28)) return;
    ArenaTeamStats stats;
    stats.teamId = packet.readUInt32(); stats.rating = packet.readUInt32();
    stats.weekGames = packet.readUInt32(); stats.weekWins = packet.readUInt32();
    stats.seasonGames = packet.readUInt32(); stats.seasonWins = packet.readUInt32();
    stats.rank = packet.readUInt32();
    bool replaced = false;
    for (auto& s : arenaTeamStats_) {
        if (s.teamId == stats.teamId) { stats.teamName = std::move(s.teamName); stats.teamType = s.teamType; s = std::move(stats); replaced = true; break; }
    }
    if (!replaced) arenaTeamStats_.push_back(std::move(stats));
    // Wow.exe 0x5A2D50: ARENA_TEAM_UPDATE, no arguments.
    owner_.fireAddonEvent("ARENA_TEAM_UPDATE", {});
}

void SocialHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (!owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_ARENA_TEAM_ROSTER));
    pkt.writeUInt32(teamId);
    owner_.getSocket()->send(pkt);
}

// ---- Arena teams as the player's fields name them ----

arena::TeamInfo SocialHandler::arenaTeamInfo(size_t slot) const {
    if (slot >= arena::kTeamSlots) return {};
    const uint16_t base = fieldIndex(UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1);
    if (base == 0xFFFF) return {};
    auto me = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!me) return {};
    std::array<uint32_t, arena::kTeamInfoFields> f{};
    for (size_t i = 0; i < f.size(); ++i)
        f[i] = me->getField(static_cast<uint16_t>(base + slot * arena::kTeamInfoFields + i));
    return arena::teamInfoFromFields(f);
}

const arena::TeamQuery* SocialHandler::arenaTeamQuery(uint32_t teamId) {
    if (teamId == 0) return nullptr;
    if (auto it = arenaTeamCache_.find(teamId); it != arenaTeamCache_.end()) return &it->second;
    // A miss asks once (the cache's own request, CMSG_ARENA_TEAM_QUERY);
    // AzerothCore answers with the record and then the team's stats.
    if (arenaTeamQueriesPending_.insert(teamId).second &&
        owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        network::Packet pkt(wireOpcode(Opcode::CMSG_ARENA_TEAM_QUERY));
        pkt.writeUInt32(teamId);
        owner_.getSocket()->send(pkt);
    }
    return nullptr;
}

const ArenaTeamStats* SocialHandler::arenaTeamStatsFor(uint32_t teamId) const {
    if (teamId == 0) return nullptr;
    for (const auto& st : arenaTeamStats_)
        if (st.teamId == teamId) return &st;
    return nullptr;
}

// 0x5A3700: a slot that lost its team drops the team's record, and every
// slot with a team asks for its roster. The field callback (0x6CDDA0) then
// fires ARENA_TEAM_UPDATE; entering the world (0x5A3DC0) does not.
void SocialHandler::onArenaTeamInfoChanged(bool fireEvent) {
    for (size_t slot = 0; slot < arena::kTeamSlots; ++slot) {
        const uint32_t id = arenaTeamInfo(slot).teamId;
        if (arenaTeamSlotIds_[slot] != 0 && id == 0) {
            arenaTeamCache_.erase(arenaTeamSlotIds_[slot]);
            arenaTeamQueriesPending_.erase(arenaTeamSlotIds_[slot]);
        }
        arenaTeamSlotIds_[slot] = id;
        if (id != 0) requestArenaTeamRosterSlot(slot);
    }
    if (fireEvent) owner_.fireAddonEvent("ARENA_TEAM_UPDATE", {});
}

void SocialHandler::requestArenaTeamRosterSlot(size_t slot) {
    if (slot >= arena::kTeamSlots) return;
    const uint32_t id = arenaTeamInfo(slot).teamId;
    if (id == 0) return;
    const int64_t now = bf::clockMs();
    if (arenaRosterNextMs_[slot] != 0 && now < arenaRosterNextMs_[slot]) return;
    arenaRosterNextMs_[slot] = now + 10000;
    requestArenaTeamRoster(id);
}

// ---- Arena opponents (BattlefieldInfo.cpp) ----

namespace {

uint64_t guidField(const Entity& e, uint16_t lo) {
    if (lo == 0xFFFF) return 0;
    return uint64_t(e.getField(lo)) | (uint64_t(e.getField(static_cast<uint16_t>(lo + 1))) << 32);
}

// The pet a visible opponent has out: CHARM, else SUMMON two fields on
// (0x60A2F0, 0x54ABF0).
uint64_t charmOrSummon(EntityManager& em, uint64_t guid) {
    auto e = em.getEntity(guid);
    if (!e) return 0;
    const uint16_t charm = fieldIndex(UF::UNIT_FIELD_CHARM);
    if (charm == 0xFFFF) return 0;
    const uint64_t c = guidField(*e, charm);
    return c != 0 ? c : guidField(*e, static_cast<uint16_t>(charm + 2));
}

}  // namespace

uint64_t SocialHandler::arenaOpponentPet(size_t index) const {
    auto present = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };
    auto petOf = [this](uint64_t g) { return charmOrSummon(owner_.getEntityManager(), g); };
    return arenaOpponents_.pet(index, present, petOf);
}

void SocialHandler::fireArenaOpponentEvents(const std::vector<arena::OpponentEvent>& events) {
    for (const auto& e : events) owner_.fireAddonEvent("ARENA_OPPONENT_UPDATE", {e.unit, e.what});
}

void SocialHandler::onObjectAppeared(uint64_t guid) {
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity || guid == owner_.getPlayerGuid()) return;
    auto present = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };

    // A party member coming into view (0x52C760 with 1): PARTY_MEMBER_ENABLE.
    if (entity->getType() == ObjectType::PLAYER && !partyData.isEmpty()) {
        for (const auto& m : partyData.members) {
            if (m.guid != guid) continue;
            if (!groupIsRaid(partyData.groupType) || m.subGroup == partyData.subGroup)
                owner_.fireAddonEvent("PARTY_MEMBER_ENABLE", {});
            break;
        }
    }

    if (bgQueues_.active().instanceType != 4) return;
    if (entity->getType() == ObjectType::PLAYER) {
        // 0x6E8280: a player whose arena faction (PLAYER_BYTES_3 byte 3) is
        // not the player's own is an opponent.
        const uint16_t b2 = fieldIndex(UF::PLAYER_BYTES_2);
        if (b2 == 0xFFFF) return;
        const uint8_t faction = static_cast<uint8_t>(entity->getField(static_cast<uint16_t>(b2 + 1)) >> 24);
        if (faction != (bgQueues_.active().arenaFaction ? 1 : 0))
            fireArenaOpponentEvents(arenaOpponents_.playerSeen(guid, present));
    } else if (entity->getType() == ObjectType::UNIT) {
        // 0x7402B0: a unit with a pet number that is hostile to the player is
        // an opponent's pet; its owner is CHARMEDBY, else CREATEDBY.
        const uint16_t petNumber = fieldIndex(UF::UNIT_FIELD_PETNUMBER);
        if (petNumber == 0xFFFF || entity->getField(petNumber) == 0) return;
        auto* unit = static_cast<Unit*>(entity.get());
        if (!(unit->isHostile() || owner_.unitReactionToPlayer(*unit) <= 2)) return;
        uint64_t ownerGuid = guidField(*entity, fieldIndex(UF::UNIT_FIELD_CHARMEDBY));
        if (ownerGuid == 0) ownerGuid = guidField(*entity, fieldIndex(UF::UNIT_FIELD_CREATEDBY_LO));
        fireArenaOpponentEvents(arenaOpponents_.petSeen(guid, ownerGuid, present));
    }
}

void SocialHandler::onObjectLeaving(uint64_t guid, bool outOfRange) {
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity || guid == owner_.getPlayerGuid()) return;

    // A party member leaving view (0x52C760 with 0, 0x573070): what the
    // object knew becomes the member's stats until the server's answer to
    // CMSG_REQUEST_PARTY_MEMBER_STATS replaces it.
    if (entity->getType() == ObjectType::PLAYER && !partyData.isEmpty()) {
        for (auto& m : partyData.members) {
            if (m.guid != guid) continue;
            auto* unit = static_cast<Unit*>(entity.get());
            m.curHealth = unit->getHealth();
            m.maxHealth = unit->getMaxHealth();
            m.powerType = unit->getPowerType();
            m.curPower = static_cast<uint16_t>(std::min<uint32_t>(unit->getPower(), 0xFFFF));
            m.maxPower = static_cast<uint16_t>(std::min<uint32_t>(unit->getMaxPower(), 0xFFFF));
            if (unit->getLevel() != 0) m.level = static_cast<uint16_t>(unit->getLevel());
            requestPartyMemberStats(guid);
            if (!groupIsRaid(partyData.groupType) || m.subGroup == partyData.subGroup)
                owner_.fireAddonEvent("PARTY_MEMBER_DISABLE", {});
            break;
        }
    }

    // Out of range in an arena: "unseen" (0x4D7230 -> 0x54ABF0). A destroy
    // says nothing here; SMSG_ARENA_UNIT_DESTROYED, sent before it, does.
    if (outOfRange && bgQueues_.active().instanceType == 4) {
        auto petOf = [this](uint64_t g) { return charmOrSummon(owner_.getEntityManager(), g); };
        // Asked while the leaving object is still here, as stock asks.
        auto here = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };
        fireArenaOpponentEvents(arenaOpponents_.unseen(guid, here, petOf));
    }
}

// SMSG_ARENA_UNIT_DESTROYED (0x54B5E0): the guid, and in an arena "destroyed".
void SocialHandler::handleArenaUnitDestroyed(network::Packet& packet) {
    if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
    const uint64_t guid = packet.readUInt64();
    if (bgQueues_.active().instanceType != 4) return;
    auto here = [this](uint64_t g) { return owner_.getEntityManager().hasEntity(g); };
    auto petOf = [this](uint64_t g) { return charmOrSummon(owner_.getEntityManager(), g); };
    fireArenaOpponentEvents(arenaOpponents_.destroyed(guid, here, petOf));
}

void SocialHandler::requestPartyMemberStats(uint64_t guid) {
    if (guid == 0 || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS));
    pkt.writeUInt64(guid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::handleArenaError(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t error = packet.readUInt32();
    // Wow.exe (0x6CCE90): 0 no team of the size (the size byte twice),
    // 1 an expired account, 2 the dungeon system in the way.
    if (error == 0) {
        const std::string size = packet.hasRemaining(1) ? std::to_string(packet.readUInt8()) : std::string();
        owner_.displayGameMessage("ERR_ARENA_NO_TEAM_II", "You are not in a " + size + "v" + size + " arena team", {size, size});
    } else if (error == 1) {
        owner_.displayGameMessage("ERR_ARENA_EXPIRED_CAIS", "You may not queue while your account has expired.");
    } else if (error == 2) {
        owner_.displayGameMessage("ERR_LFG_CANT_USE_BATTLEGROUND", "You cannot queue for a battleground or arena while using the dungeon system.");
    }
}

void SocialHandler::handlePvpLogData(network::Packet& packet) {
    // 0x54D280: the rows replace the old ones, and the scoreboard waits for
    // every row's name before it is sorted and UPDATE_BATTLEFIELD_SCORE fires.
    bf::PvpLogPacket log;
    if (!bf::parsePvpLog(packet, log)) { packet.skipAll(); return; }
    const int filter = bgScoreboard_.factionFilter;
    bgScoreboard_.log = std::move(log);
    bgScoreboard_.hasData = true;
    bgScoreboard_.factionFilter = filter;
    bgScoreNamesPending_ = false;
    if (fillScoreNames()) resortScoreboard();
    else bgScoreNamesPending_ = true;
}

void SocialHandler::sayDuelCountdown(const std::vector<int>& counts) {
    for (const int n : counts)
        owner_.addGlobalStringChatLine("DUEL_COUNTDOWN", "Duel starting: %d", {std::to_string(n)});
}

void SocialHandler::updateDuelCountdown(float deltaTime) {
    if (!duelCountdown_.running()) return;
    duelCountdownCarryMs_ += deltaTime * 1000.0f;
    const auto whole = static_cast<uint32_t>(duelCountdownCarryMs_);
    duelCountdownCarryMs_ -= static_cast<float>(whole);
    sayDuelCountdown(duelCountdown_.tick(whole));
}

void SocialHandler::updateLogoutCountdown(float deltaTime) {
    // A scoreboard waiting for its names (0x54CA30 counts the answers down).
    // The name cache can know a name without the race the faction comes
    // from, so after three seconds the rows are shown as they stand.
    if (bgScoreNamesPending_) {
        bgScoreNameWaitSec_ += deltaTime;
        bgScoreNameCheckSec_ -= deltaTime;
        if (bgScoreNameCheckSec_ <= 0.0f) {
            bgScoreNameCheckSec_ = 0.25f;
            if (fillScoreNames() || bgScoreNameWaitSec_ >= 3.0f) {
                bgScoreNamesPending_ = false;
                resortScoreboard();
            }
        }
    } else {
        bgScoreNameWaitSec_ = 0.0f;
    }
    // The ready check's thirty seconds. On the initiator's client their end
    // closes the check (Wow.exe 0x573390): "The following players are Away:
    // ..." (RAID_MEMBERS_AFK) naming whoever did not answer, or "Everyone is
    // Ready" (READY_CHECK_ALL_READY), then MSG_RAID_READY_CHECK_FINISHED to
    // the server, which broadcasts it to the group.
    if (readyCheckSecondsLeft_ > 0.0f) {
        readyCheckSecondsLeft_ -= deltaTime;
        if (readyCheckSecondsLeft_ <= 0.0f && readyCheckActive_ &&
            readyCheckInitiatorGuid_ != 0 && readyCheckInitiatorGuid_ == owner_.getPlayerGuid()) {
            std::string away;
            for (const auto& m : partyData.members) {
                if (m.guid == owner_.getPlayerGuid()) continue;
                bool answered = false;
                for (const auto& r : readyCheckResults_)
                    if (r.guid == m.guid) { answered = true; break; }
                if (answered) continue;
                if (!away.empty()) away += ", ";
                away += m.name;
            }
            // RAID_MEMBERS_AFK / READY_CHECK_ALL_READY are plain GlobalStrings,
            // not GameErrors: 0x573390 prints them to the chat frame.
            if (away.empty())
                owner_.addGlobalStringChatLine("READY_CHECK_ALL_READY", "Everyone is Ready");
            else
                owner_.addGlobalStringChatLine("RAID_MEMBERS_AFK", "The following players are Away: %s", {away});
            if (owner_.getSocket()) {
                network::Packet fin(wireOpcode(Opcode::MSG_RAID_READY_CHECK_FINISHED));
                owner_.getSocket()->send(fin);
            }
        }
    }
    // A queue request the server let pass without a word. A realm that keeps
    // a battleground back - ChromieCraft opens the random one at level sixty,
    // and only through a command of its own - drops the join unanswered, and
    // the panel then did nothing at all when its button was pressed.
    if (bgJoinPendingType_ != 0) {
        bgJoinWaitSec_ -= deltaTime;
        if (bgJoinWaitSec_ <= 0.0f) {
            std::string name = "that battleground";
            if (const auto* row = owner_.getBattlemasterInfo(bgJoinPendingType_)) name = row->name;
            // Logged, not said: the stock client has no such message and
            // waits in silence.
            LOG_WARNING("Battleground join unanswered: type ", bgJoinPendingType_, " (", name, ")");
            bgJoinPendingType_ = 0;
        }
    }
    if (loggingOut_ && logoutCountdown_ > 0.0f) {
        logoutCountdown_ -= deltaTime;
        if (logoutCountdown_ < 0.0f) logoutCountdown_ = 0.0f;
    }
    // The bind prompt expires on its own; the server stops asking and the
    // pending bind lapses, so the dialog has to stop offering an answer.
    // The countdown alone; running out raises nothing (the dialog reads
    // zero and the server acts).
    if (instanceLock_.active) {
        instanceLock_.secondsLeft -= deltaTime;
        if (instanceLock_.secondsLeft <= 0.0f) {
            instanceLock_.secondsLeft = 0.0f;
            instanceLock_.active = false;
        }
    }
}

// RespondInstanceLock(accept) (0x613ED0): CMSG_INSTANCE_LOCK_RESPONSE with
// the answer, always; the prompt ends when the server saves the player
// (SMSG_INSTANCE_SAVE_CREATED) or moves them out.
void SocialHandler::respondInstanceLock(bool accept) {
    if (!owner_.getSocket()) return;
    network::Packet resp(wireOpcode(Opcode::CMSG_INSTANCE_LOCK_RESPONSE));
    resp.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(resp);
    LOG_INFO("CMSG_INSTANCE_LOCK_RESPONSE: ", accept ? "accepted" : "declined");
}

// 0x513B20: a positive time starts the prompt (INSTANCE_LOCK_START), zero
// ends it (INSTANCE_LOCK_STOP).
void SocialHandler::setInstanceLock(uint32_t timerMs, uint32_t completedMask, bool previouslySaved) {
    if (timerMs > 0) {
        instanceLock_.active = true;
        instanceLock_.secondsLeft = static_cast<float>(timerMs) / 1000.0f;
        instanceLock_.previouslySaved = previouslySaved;
        instanceLock_.completedEncounterMask = completedMask;
        owner_.fireAddonEvent("INSTANCE_LOCK_START", {});
    } else {
        instanceLock_ = InstanceLockPrompt{};
        owner_.fireAddonEvent("INSTANCE_LOCK_STOP", {});
    }
}

void SocialHandler::resetTransferState() {
    instanceLock_ = InstanceLockPrompt{};
    encounterUnits_.clear();
    raidTargetGuids_.fill(0);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void SocialHandler::handleInitializeFactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    size_t needed = static_cast<size_t>(count) * 5;
    if (!packet.hasRemaining(needed)) { packet.skipAll(); return; }
    owner_.initialFactionsRef().clear();
    owner_.initialFactionsRef().reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        GameHandler::FactionStandingInit fs{};
        fs.flags = packet.readUInt8();
        fs.standing = static_cast<int32_t>(packet.readUInt32());
        owner_.initialFactionsRef().push_back(fs);
    }
    // Also keyed by faction, which is how everything that draws a standing
    // asks for one. This packet is indexed by ReputationListID and the map is
    // indexed by faction id, and only SMSG_SET_FACTION_STANDING wrote to the
    // map - so a standing was known from login and unreadable until it next
    // moved. The reputation tab drew every bar empty at Neutral however much
    // the character had earned, and so did this client's own panel and the
    // watched-faction bar, all three reading the same empty map.
    //
    // And each made whole: the packet carries what has been earned, and the
    // standing is that on top of where the faction starts for this race and
    // class. See factionBaseReputation.
    owner_.loadFactionNameCache();
    for (size_t repListId = 0; repListId < owner_.initialFactionsRef().size(); ++repListId) {
        const uint32_t factionId =
            owner_.getFactionIdByRepListId(static_cast<uint32_t>(repListId));
        if (factionId == 0) continue;
        auto& standing = owner_.initialFactionsRef()[repListId].standing;
        standing += owner_.factionBaseReputation(factionId);
        owner_.factionStandingsRef()[factionId] = standing;
    }
    LOG_INFO("Reputation: ", owner_.initialFactionsRef().size(),
             " factions initialised, ", owner_.factionStandingsRef().size(),
             " resolved to a faction id");
    // Whether each faction with a standing is hostile is the at-war flag this
    // packet carries, so units judged before it arrived are judged again.
    owner_.refreshUnitHostility();
}

void SocialHandler::handleSetFactionStanding(network::Packet& packet) {
    // Expansion wire layouts differ before the shared count + entries payload:
    //   Classic: count
    //   TBC:     RAF bonus (float), count
    //   WotLK:   RAF bonus (float), showVisual (uint8), count
    // Treating WotLK's first float byte as showVisual shifted the entire packet,
    // producing bogus counts, faction names, standings, and chat messages.
    // AzerothCore sets showVisual when a rank went up (ReputationMgr's
    // _sendFactionIncreased); the stock client answers it with
    // ReputationLevelUp.m2 on the player, whose own $SND event is the sound.
    bool showVisual = false;
    if (isActiveExpansion("wotlk")) {
        if (!packet.hasRemaining(9)) return;
        /*float rafBonus =*/ packet.readFloat();
        showVisual = packet.readUInt8() != 0;
    } else if (isActiveExpansion("tbc")) {
        if (!packet.hasRemaining(8)) return;
        /*float rafBonus =*/ packet.readFloat();
    } else if (!packet.hasRemaining(4)) {
        return;
    }

    uint32_t count = packet.readUInt32();
    count = std::min(count, 128u);
    owner_.loadFactionNameCache();
    for (uint32_t i = 0; i < count && packet.hasRemaining(8); ++i) {
        // The packet carries Faction.dbc ReputationListID, not the faction ID.
        uint32_t repListId = packet.readUInt32();
        int32_t  standing  = static_cast<int32_t>(packet.readUInt32());

        uint32_t factionId = owner_.getFactionIdByRepListId(repListId);
        if (factionId == 0) {
            LOG_WARNING("SMSG_SET_FACTION_STANDING: unknown repListId=", repListId,
                        " standing=", standing);
            continue;
        }
        // Earned only, as at login: the whole is this on top of the start.
        standing += owner_.factionBaseReputation(factionId);

        int32_t  oldStanding = 0;
        // SMSG_INITIALIZE_FACTIONS is indexed by ReputationListID and supplies the
        // standing before incremental updates. Without using it, the first message
        // reports the character's entire accumulated reputation as the gained amount.
        if (repListId < owner_.initialFactionsRef().size()) {
            oldStanding = owner_.initialFactionsRef()[repListId].standing;
            owner_.initialFactionsRef()[repListId].standing = standing;
        } else {
            auto it = owner_.factionStandingsRef().find(factionId);
            if (it != owner_.factionStandingsRef().end()) oldStanding = it->second;
        }
        owner_.factionStandingsRef()[factionId] = standing;
        int32_t delta = standing - oldStanding;
        if (delta != 0) {
            std::string name = owner_.getFactionName(factionId);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "Reputation with %s %s by %d.",
                          name.c_str(), delta > 0 ? "increased" : "decreased", std::abs(delta));
            // Only CHAT_MSG_COMBAT_FACTION_CHANGE below (FACTION_STANDING_
            // INCREASED/DECREASED): a system line beside it printed every
            // reputation change twice.
            owner_.watchedFactionIdRef() = factionId;
            // These events fire unconditionally on any rep change (not gated by callback).
            owner_.fireAddonEvent("UPDATE_FACTION", {});
            owner_.addLocalChatLine(ChatType::COMBAT_FACTION_CHANGE, std::string(buf));
            // Blizzard_CombatText's "(Name +N)": the faction's name, then the
            // signed change, which the addon prefixes with + itself.
            owner_.fireCombatTextUpdate(owner_.getPlayerGuid(),
                                        {"FACTION", name, std::to_string(delta)});
        }
    }
    if (showVisual) {
        if (auto* spells = owner_.getSpellHandler())
            spells->playUnitEffectModel(owner_.getPlayerGuid(), "Spells\\ReputationLevelUp.m2");
    }
}

void SocialHandler::handleSetFactionAtWar(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  setAtWar  = packet.readUInt8();
    if (repListId < owner_.initialFactionsRef().size()) {
        if (setAtWar)
            owner_.initialFactionsRef()[repListId].flags |=  GameHandler::FACTION_FLAG_AT_WAR;
        else
            owner_.initialFactionsRef()[repListId].flags &= ~GameHandler::FACTION_FLAG_AT_WAR;
        owner_.refreshUnitHostility();
    }
}

void SocialHandler::handleSetFactionVisible(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  visible   = packet.readUInt8();
    if (repListId < owner_.initialFactionsRef().size()) {
        if (visible)
            owner_.initialFactionsRef()[repListId].flags |=  GameHandler::FACTION_FLAG_VISIBLE;
        else
            owner_.initialFactionsRef()[repListId].flags &= ~GameHandler::FACTION_FLAG_VISIBLE;
    }
}

void SocialHandler::handleGroupSetLeader(network::Packet& packet) {
    if (!packet.hasData()) return;
    std::string leaderName = packet.readString();
    auto& pd = mutablePartyData();
    for (const auto& m : pd.members) {
        if (m.name == leaderName) { pd.leaderGuid = m.guid; break; }
    }
    // ERR_NEW_LEADER_YOU / ERR_NEW_LEADER_S (Wow.exe 0x6CBD70); a Dungeon
    // Finder group says Dungeon Guide instead.
    if (!leaderName.empty()) {
        const bool lfg = isActiveExpansion("wotlk") && (partyData.groupType & 0x0C) != 0;
        if (leaderName == owner_.lookupName(owner_.getPlayerGuid()))
            owner_.displayGameMessage(lfg ? "ERR_NEW_GUIDE_YOU" : "ERR_NEW_LEADER_YOU",
                                      lfg ? "You are now the Dungeon Guide." : "You are now the group leader.");
        else
            owner_.displayGameMessage(lfg ? "ERR_NEW_GUIDE_S" : "ERR_NEW_LEADER_S",
                                      leaderName + (lfg ? " is now the Dungeon Guide." : " is now the group leader."),
                                      {leaderName});
    }
    owner_.fireAddonEvent("PARTY_LEADER_CHANGED", {});
    owner_.fireAddonEvent("GROUP_ROSTER_UPDATE", {});
}

// ============================================================
// Minimap Ping
// ============================================================

void SocialHandler::sendMinimapPing(float wowX, float wowY) {
    if (owner_.getState() != WorldState::IN_WORLD) return;

    // Minimap:PingLocation (0x0057ED70): the spot goes to the server only in a
    // party or a raid - nobody else would hear it - and is shown here either
    // way, as the player's own ping. The wire carries it in world order.
    const glm::vec3 server = core::coords::canonicalToServer(glm::vec3(wowX, wowY, 0.0f));
    if (owner_.isInGroup()) {
        network::Packet pkt(wireOpcode(Opcode::MSG_MINIMAP_PING));
        pkt.writeFloat(server.x);
        pkt.writeFloat(server.y);
        owner_.getSocket()->send(pkt);
    }

    // The server sends the ping to the rest of the group and not back, so the
    // sender's own copy is made here.
    GameHandler::MinimapPing localPing;
    localPing.senderGuid = owner_.activeCharacterGuidRef();
    localPing.wowX       = wowX;
    localPing.wowY       = wowY;
    localPing.age        = 0.0f;
    owner_.minimapPingsRef().push_back(localPing);
    owner_.notifyMinimapPing(owner_.getPlayerGuid(), server.x, server.y);
}

// ============================================================
// Summon Request
// ============================================================

void SocialHandler::handleSummonRequest(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;

    owner_.summonerGuidRef()        = packet.readUInt64();
    uint32_t zoneId             = packet.readUInt32();
    uint32_t timeoutMs          = packet.readUInt32();
    owner_.summonTimeoutSecRef()    = timeoutMs / 1000.0f;
    owner_.pendingSummonRequestRef()= true;

    owner_.summonerNameRef().clear();
    if (auto* unit = owner_.getUnitByGuid(owner_.summonerGuidRef())) {
        owner_.summonerNameRef() = unit->getName();
    }
    if (owner_.summonerNameRef().empty()) {
        owner_.summonerNameRef() = owner_.lookupName(owner_.summonerGuidRef());
    }
    if (owner_.summonerNameRef().empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(owner_.summonerGuidRef()));
        owner_.summonerNameRef() = tmp;
    }

    // CONFIRM_SUMMON and its popup are the whole announcement (Wow.exe
    // 0x6D8680 -> 0x513BF0); no chat line.
    LOG_INFO("SMSG_SUMMON_REQUEST: summoner=", owner_.summonerNameRef(),
             " zoneId=", zoneId, " timeout=", owner_.summonTimeoutSecRef(), "s");
    owner_.fireAddonEvent("CONFIRM_SUMMON", {});
}

/// Both answers to a summon: who summoned, then yes or no.
///
/// HandleSummonResponseOpcode reads `ObjectGuid summoner_guid` and then a bool,
/// nine bytes. Both answers were a single byte, so the server had one byte
/// where it wanted eight, ran off the end of the buffer and dropped the packet
/// - accepting a summon has never once reached it, and neither has declining
/// one. The same mistake as the battlefield answers below, made next door.
///
/// The guid is the one SMSG_SUMMON_REQUEST arrived with, which is already kept
/// so the message can name whoever cast it.
void SocialHandler::sendSummonResponse(bool accept) {
    if (!owner_.getSocket()) return;
    owner_.pendingSummonRequestRef() = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SUMMON_RESPONSE));
    pkt.writeUInt64(owner_.summonerGuidRef());
    pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::acceptSummon() {
    if (!owner_.pendingSummonRequestRef()) return;
    sendSummonResponse(true);
    LOG_INFO("Accepted summon from ", owner_.summonerNameRef());
}

void SocialHandler::declineSummon() {
    sendSummonResponse(false);
}

// ============================================================
// Battlefield Manager
// ============================================================

/// All three battlefield answers carry the battle id first.
///
/// HandleBfEntryInviteResponse reads `uint32 BattleId >> uint8 Accepted` and
/// the exit request reads a battle id alone. Both answers used to be a single
/// byte, so the server read the accept flag as the low byte of a battle id,
/// ran out of buffer and dropped the packet - accepting a Wintergrasp
/// invitation had never once reached it.
void SocialHandler::sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept,
                                      bool withFlag) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(op));
    pkt.writeUInt32(battleId);
    if (withFlag) pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::acceptBfMgrInvite() {
    if (!owner_.bfMgrInvitePendingRef()) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      owner_.getBfMgrBattleId(), true, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("acceptBfMgrInvite: battle ", owner_.getBfMgrBattleId());
}

void SocialHandler::declineBfMgrInvite() {
    if (!owner_.bfMgrInvitePendingRef()) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      owner_.getBfMgrBattleId(), false, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("declineBfMgrInvite: battle ", owner_.getBfMgrBattleId());
}

void SocialHandler::respondBfMgrQueueInvite(uint32_t battleId, bool accept) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_QUEUE_INVITE_RESPONSE,
                      battleId, accept, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("respondBfMgrQueueInvite: battle ", battleId,
             accept ? " accepted" : " declined");
}

void SocialHandler::requestBfMgrExit(uint32_t battleId) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_EXIT_REQUEST,
                      battleId, false, false);
    LOG_INFO("requestBfMgrExit: battle ", battleId);
}

// ============================================================
// Calendar
// ============================================================

void SocialHandler::inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId,
                                          const std::string& name,
                                          bool isPreInvite, bool isGuildEvent) {
    if (!owner_.isInWorld()) return;
    // eventId, inviteId, name, isPreInvite, isGuildEvent - CalendarHandler.cpp:515.
    // The two flags are ByteBuffer bools, which are one byte each on the wire.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_INVITE));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(name);
    pkt.writeUInt8(isPreInvite ? 1 : 0);
    pkt.writeUInt8(isGuildEvent ? 1 : 0);
    owner_.getSocket()->send(pkt);
    LOG_INFO("inviteToCalendarEvent: '", name, "' to event ", eventId,
             isPreInvite ? " (before it is created)" : "");
}

void SocialHandler::massInviteGuildToCalendarEvent(uint32_t minLevel,
                                                   uint32_t maxLevel,
                                                   uint32_t minRank) {
    if (!owner_.isInWorld()) return;
    // Its own opcode, not an invite with the name left out: the server picks
    // the members itself from these three bounds and never sees a name
    // (CalendarHandler.cpp:198, and Guild::MassInviteToEvent behind it).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GUILD_FILTER));
    pkt.writeUInt32(minLevel);
    pkt.writeUInt32(maxLevel);
    pkt.writeUInt32(minRank);
    owner_.getSocket()->send(pkt);
    LOG_INFO("massInviteGuildToCalendarEvent: levels ", minLevel, "-", maxLevel,
             ", rank ", minRank, " and above");
}

void SocialHandler::updateCalendarEvent(uint64_t eventId, uint64_t inviteId,
                                        const CalendarEventDraft& draft) {
    if (!owner_.isInWorld()) return;
    // The same body as adding, with the event and invite ids in front of it
    // (CalendarHandler.cpp:360). Written out rather than shared with
    // createCalendarEvent: the two are the same shape today and are two
    // different opcodes, and folding them together would make the next change
    // to one silently change the other.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_UPDATE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    owner_.getSocket()->send(pkt);
    LOG_INFO("updateCalendarEvent: ", eventId, " '", draft.title, "'");
}

void SocialHandler::removeCalendarEvent(uint64_t eventId, uint64_t inviteId) {
    if (!owner_.isInWorld()) return;
    // The server reads the id and discards the rest, but the rest is still
    // written: it calls rfinish() rather than stopping, so a short packet is
    // a short read on its side rather than a tidy one.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_REMOVE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(0);   // flags
    owner_.getSocket()->send(pkt);
    LOG_INFO("removeCalendarEvent: ", eventId);
}

void SocialHandler::setCalendarInviteStatus(uint64_t inviteeGuid,
                                            uint64_t eventId, uint64_t inviteId,
                                            uint8_t status) {
    if (!owner_.isInWorld()) return;
    // packedGuid invitee, eventId, inviteId, ownerInviteId, status
    // (CalendarHandler.cpp:695). Setting someone *else's* status, which is what
    // an event's owner does to confirm or bench them - the player's own answer
    // is CMSG_CALENDAR_EVENT_RSVP and a different packet entirely.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(status);
    owner_.getSocket()->send(pkt);
    LOG_INFO("setCalendarInviteStatus: invitee ", inviteeGuid, " status ", status);
}

void SocialHandler::setCalendarInviteModerator(uint64_t inviteeGuid,
                                               uint64_t eventId,
                                               uint64_t inviteId, uint8_t rank) {
    if (!owner_.isInWorld()) return;
    // The same shape with a rank in place of the status
    // (CalendarHandler.cpp:727).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_MODERATOR_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(rank);
    owner_.getSocket()->send(pkt);
    LOG_INFO("setCalendarInviteModerator: invitee ", inviteeGuid, " rank ", rank);
}

void SocialHandler::requestCalendarEvent(uint64_t eventId) {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_EVENT));
    pkt.writeUInt64(eventId);
    owner_.getSocket()->send(pkt);
    LOG_INFO("requestCalendarEvent: ", eventId);
}

void SocialHandler::createCalendarEvent(const CalendarEventDraft& draft,
                                        const std::vector<CalendarAddEventInvite>& invites) {
    if (!owner_.isInWorld()) return;
    // The header, then the invite list - see writeCalendarAddEvent. Without
    // the list the server's read of its count throws and nothing is created.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_ADD_EVENT));
    writeCalendarAddEvent(pkt, draft, invites);
    owner_.getSocket()->send(pkt);
    LOG_INFO("createCalendarEvent: '", draft.title, "' type ", draft.type,
             " on ", draft.eventTime.day, "/", draft.eventTime.month, "/",
             draft.eventTime.fullYear(), " with ", invites.size(), " invite(s)");
}

void SocialHandler::signUpForCalendarEvent(uint64_t eventId, bool tentative) {
    if (!owner_.isInWorld()) return;
    // uint64 eventId, bool tentative (CalendarHandler.cpp:613): the server
    // makes the invite itself, SIGNED_UP or TENTATIVE.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_SIGNUP));
    pkt.writeUInt64(eventId);
    pkt.writeUInt8(tentative ? 1 : 0);
    owner_.getSocket()->send(pkt);
    LOG_INFO("signUpForCalendarEvent: ", eventId, tentative ? " (tentative)" : "");
}

void SocialHandler::removeCalendarInvite(uint64_t inviteeGuid, uint64_t inviteId,
                                         uint64_t ownerInviteId, uint64_t eventId) {
    if (!owner_.isInWorld()) return;
    // packedGuid invitee, inviteId, ownerInviteId, eventId - in that order,
    // which is not the order the status packet uses (CalendarHandler.cpp:673).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_REMOVE_INVITE));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(ownerInviteId);
    pkt.writeUInt64(eventId);
    owner_.getSocket()->send(pkt);
    LOG_INFO("removeCalendarInvite: invite ", inviteId, " from event ", eventId);
}

void SocialHandler::copyCalendarEvent(uint64_t eventId, uint64_t inviteId,
                                      uint32_t packedTime) {
    if (!owner_.isInWorld()) return;
    // uint64 eventId, uint64 inviteId, packed time (CalendarHandler.cpp:448).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_COPY_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(packedTime);
    owner_.getSocket()->send(pkt);
    LOG_INFO("copyCalendarEvent: ", eventId);
}

void SocialHandler::complainCalendarEvent(uint64_t eventId, uint64_t creatorGuid) {
    if (!owner_.isInWorld()) return;
    // uint64 eventId, then the guid in full (CalendarPackets.cpp,
    // CalendarComplain::Read).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_COMPLAIN));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(creatorGuid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::massInviteArenaTeamToCalendarEvent(uint32_t teamId) {
    if (!owner_.isInWorld()) return;
    // uint32 arena team id (CalendarPackets.cpp, ArenaTeam::Read); the answer
    // is SMSG_CALENDAR_ARENA_TEAM, the members to add.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_ARENA_TEAM));
    pkt.writeUInt32(teamId);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::respondToCalendarInvite(uint64_t eventId, uint64_t inviteId,
                                            uint32_t status) {
    if (!owner_.isInWorld()) return;
    // uint64 eventId, uint64 inviteId, uint32 status - CalendarHandler.cpp:630.
    // The status is a CalendarInviteStatus: 1 accepted, 2 declined, 8
    // tentative, 9 removed. The server answers with an event-status alert and
    // clears the pending action, so nothing is assumed here.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_RSVP));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(status);
    owner_.getSocket()->send(pkt);
    LOG_INFO("respondToCalendarInvite: event ", eventId, " invite ", inviteId,
             " status ", status);
}

void SocialHandler::requestCalendar() {
    if (!owner_.isInWorld()) return;
    // CMSG_CALENDAR_GET_CALENDAR has no payload
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_CALENDAR));
    owner_.getSocket()->send(pkt);
    LOG_INFO("requestCalendar: sent CMSG_CALENDAR_GET_CALENDAR");
    // Also request pending invite count
    network::Packet numPkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));
    owner_.getSocket()->send(numPkt);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void SocialHandler::sendSetDifficulty(uint32_t difficulty, bool raid) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change difficulty: not in world");
        return;
    }

    // MSG_SET_DUNGEON_DIFFICULTY, not CMSG_CHANGEPLAYER_DIFFICULTY. The latter
    // is a real opcode number and the server registers it with Handle_NULL and
    // STATUS_NEVER - it is read off the wire and discarded - so every
    // difficulty change ever sent from here went nowhere. Both of the real ones
    // carry a single uint32 mode.
    const Opcode op = raid ? Opcode::MSG_SET_RAID_DIFFICULTY
                           : Opcode::MSG_SET_DUNGEON_DIFFICULTY;
    network::Packet packet(wireOpcode(op));
    packet.writeUInt32(difficulty);
    owner_.getSocket()->send(packet);
    LOG_INFO(raid ? "MSG_SET_RAID_DIFFICULTY sent: difficulty="
                  : "MSG_SET_DUNGEON_DIFFICULTY sent: difficulty=", difficulty);
}

void SocialHandler::toggleHelm() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle helm: not in world or not connected");
        return;
    }

    owner_.helmVisibleRef() = !owner_.helmVisibleRef();
    auto packet = ShowingHelmPacket::build(owner_.helmVisibleRef());
    owner_.getSocket()->send(packet);
    // No line: ShowHelm (Wow.exe 0x51C0B0) sets the flag and says nothing.
    // The flag alone changed nothing on screen: it was set, sent to the server
    // and never read. Mark equipment dirty so the helm model and the hair under
    // it are both rebuilt from it.
    owner_.markOnlineEquipmentDirty();
    LOG_INFO("Helm visibility toggled: ", owner_.helmVisibleRef());
}

void SocialHandler::toggleCloak() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle cloak: not in world or not connected");
        return;
    }

    owner_.cloakVisibleRef() = !owner_.cloakVisibleRef();
    auto packet = ShowingCloakPacket::build(owner_.cloakVisibleRef());
    owner_.getSocket()->send(packet);
    LOG_INFO("Cloak visibility toggled: ", owner_.cloakVisibleRef());
}

void SocialHandler::setStandState(uint8_t standState) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change stand state: not in world or not connected");
        return;
    }

    auto packet = StandStateChangePacket::build(standState);
    owner_.getSocket()->send(packet);
    LOG_INFO("Changed stand state to: ", static_cast<int>(standState));
}

void SocialHandler::sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                                        uint32_t facialHairEntry, uint32_t skinColorEntry) {
    if (!owner_.isInWorld()) return;
    auto pkt = AlterAppearancePacket::build(hairStyleEntry, hairColor,
                                            facialHairEntry, skinColorEntry);
    owner_.getSocket()->send(pkt);
    LOG_INFO("sendAlterAppearance: hairEntry=", hairStyleEntry,
             " color=", hairColor, " facialEntry=", facialHairEntry,
             " skinEntry=", skinColorEntry);
}

void SocialHandler::deleteGmTicket() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_DELETETICKET));
    owner_.getSocket()->send(pkt);
    owner_.gmTicketActiveRef() = false;
    owner_.gmTicketTextRef().clear();
    LOG_INFO("Deleting GM ticket");
}

void SocialHandler::setGroupAssistant(uint64_t guid, bool apply) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_ASSISTANT_LEADER));
    packet.writeUInt64(guid);
    packet.writeUInt8(apply ? 1 : 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestContactList() {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // Once a second. FriendsFrame_Update calls ShowFriends every time it
    // redraws, and it redraws on every friend status line - so an unthrottled
    // send answers each one with a request for the whole list again.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastContactListRequest_ < std::chrono::seconds(1)) return;
    lastContactListRequest_ = now;
    // Two names for one request. Vanilla calls it CMSG_FRIEND_LIST and sends
    // it empty; TBC renamed it CMSG_CONTACT_LIST and gave it a flags word.
    // Checked against the opcode maps: classic names only the first, TBC and
    // WotLK only the second.
    if (isClassicLikeExpansion()) {
        const uint32_t wire = wireOpcode(Opcode::CMSG_FRIEND_LIST);
        if (wire == 0xFFFF) return;
        network::Packet pkt(static_cast<uint16_t>(wire));
        owner_.getSocket()->send(pkt);
        return;
    }
    const uint32_t wire = wireOpcode(Opcode::CMSG_CONTACT_LIST);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    // Which lists to send back. AzerothCore reads one uint32 and hands it
    // straight to SendSocialList; SOCIAL_FLAG_ALL is friend, ignored and muted
    // together, which is what the panel shows across its three tabs.
    constexpr uint32_t kSocialFlagAll = 0x07;
    pkt.writeUInt32(kSocialFlagAll);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::requestBattlefieldPositions() {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // Only in one. AzerothCore drops the request outright when the player has
    // no battleground, so asking anywhere else is bytes for nothing.
    if (!owner_.isBattlegroundMap(owner_.getCurrentMapId())) return;
    // Once a second. The interface asks from WorldMapFrame_OnUpdate, so an
    // unthrottled send is one packet per frame for as long as the map is open.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastBgPositionRequest_ < std::chrono::seconds(1)) return;
    lastBgPositionRequest_ = now;
    network::Packet pkt(wireOpcode(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS));
    owner_.getSocket()->send(pkt);
}

// MSG_GUILD_EVENT_LOG_QUERY: ask, with no payload. The reply is the same
// opcode and is parsed below.
void SocialHandler::requestGuildBankLog(uint8_t tab) {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_BANK_LOG_QUERY));
    pkt.writeUInt8(tab);
    owner_.getSocket()->send(pkt);
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY: tab ", static_cast<int>(tab));
}

// The reply, which shares its opcode with the request.
//
//   uint8 tab
//   uint8 count
//   per entry:
//     int8   type
//     uint64 playerGuid
//     the payload the type calls for - an item and a count, those plus a
//     destination tab for a move, or a sum of money for everything else
//     uint32 secondsAgo
//
// Nothing sent the query, so nothing ever arrived to parse: the guild bank's
// log tab counted zero transactions and drew an empty page over a log the
// server keeps.
void SocialHandler::handleGuildBankLog(network::Packet& packet) {
    if (!packet.hasRemaining(2)) { packet.skipAll(); return; }
    const uint8_t tab = packet.readUInt8();
    const uint8_t count = packet.readUInt8();
    if (tab >= guildBankLogs_.size()) { packet.skipAll(); return; }

    auto& log = guildBankLogs_[tab];
    log.clear();
    log.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(9)) break;
        GuildBankLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        switch (e.type) {
            case 1: case 2:                 // deposit / withdraw an item
                if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
                e.itemId = packet.readUInt32();
                e.count  = packet.readUInt32();
                break;
            case 3: case 7:                 // moved between tabs
                if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
                e.itemId   = packet.readUInt32();
                e.count    = packet.readUInt32();
                e.otherTab = packet.readUInt8();
                break;
            default:                        // money, in copper
                if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
                e.count = packet.readUInt32();
                break;
        }
        if (!packet.hasRemaining(4)) break;
        e.secondsAgo = packet.readUInt32();
        if (e.itemId) owner_.ensureItemInfo(e.itemId);
        log.push_back(e);
    }
    packet.skipAll();
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(tab),
             ", ", log.size(), " entries");
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GUILDBANKLOG_UPDATE", {});
    }
}

void SocialHandler::requestGuildEventLog() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_EVENT_LOG_QUERY));
    owner_.getSocket()->send(pkt);
}

// The reply. Two of the five fields are conditional, which is the whole
// difficulty: a reader that always takes the second guid loses eight bytes on
// every join and leave, and those are most of a guild log.
//
// Types, from AzerothCore's GuildEventLogTypes: 1 invite, 2 join, 3 promote,
// 4 demote, 5 remove, 6 leave.
void SocialHandler::handleGuildEventLog(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return;
    const uint8_t count = packet.readUInt8();
    guildEventLog_.clear();
    guildEventLog_.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (rem() < 9) break;                 // type + one guid at minimum
        GuildEventLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        // Everything but join (2) and leave (6) names a second player.
        if (e.type != 2 && e.type != 6) {
            if (rem() < 8) break;
            e.otherGuid = packet.readUInt64();
        }
        // Promote (3) and demote (4) carry the new rank.
        if (e.type == 3 || e.type == 4) {
            if (rem() < 1) break;
            e.newRank = packet.readUInt8();
        }
        if (rem() < 4) break;
        e.secondsAgo = packet.readUInt32();
        guildEventLog_.push_back(e);
    }
    LOG_INFO("Guild event log: ", guildEventLog_.size(), " entries");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GUILD_EVENT_LOG_UPDATE", {});
}

// CMSG_GMTICKET_SYSTEMSTATUS: is the ticket queue open at all. The reply,
// SMSG_GMTICKET_SYSTEMSTATUS, was already parsed - asking was the missing half,
// and the help frame asks every time it is shown.
void SocialHandler::requestGmSystemStatus() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_SYSTEMSTATUS));
    owner_.getSocket()->send(pkt);
}

void SocialHandler::requestGmTicket() {
    if (!owner_.isInWorld()) return;
    // CMSG_GMTICKET_GETTICKET has no payload - server responds with SMSG_GMTICKET_GETTICKET
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_GETTICKET));
    owner_.getSocket()->send(pkt);
    LOG_DEBUG("Sent CMSG_GMTICKET_GETTICKET - querying open ticket status");
}

} // namespace game
} // namespace wowee
