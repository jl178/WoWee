#include "ui/world_ui_rules.hpp"
#include "game/chat_handler.hpp"
#include "game/global_strings.hpp"

#include <set>
#include "addons/lua_api_registrations.hpp"
#include "game/chat_filters.hpp"
#include "game/text_tokens.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "core/app_clock.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include "core/local_time.hpp"

namespace wowee {
namespace game {

ChatHandler::ChatHandler(GameHandler& owner)
    : owner_(owner) {
    initializeChatLog();
}

namespace {

bool isTruthyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool isFalsyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.empty() || lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

std::string escapeChatLogField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string formatChatLogTimestamp(std::chrono::system_clock::time_point timestamp) {
    auto time = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    std::tm tm = core::localTime(time);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string formatChatLogGuid(uint64_t guid) {
    if (guid == 0) return "";
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(16) << guid;
    return oss.str();
}

std::string chatParticipant(const std::string& name, uint64_t guid, const char* fallback) {
    if (!name.empty()) return name;
    if (guid != 0) return formatChatLogGuid(guid);
    return fallback ? fallback : "";
}

/// A line an NPC or a boss said - the kinds whose text carries $-tokens.
bool isMonsterChatType(ChatType type) {
    switch (type) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_PARTY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_EMOTE:
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER:
            return true;
        default:
            return false;
    }
}

/// A player's guid: the type in its top sixteen bits is zero.
bool isPlayerGuid(uint64_t guid) {
    return guid != 0 && (guid >> 48) == 0;
}

bool chatPacketDiagEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("WOWEE_CHAT_PACKET_DIAG");
        if (!raw) return false;
        return !isFalsyEnvValue(raw);
    }();
    return enabled;
}

} // namespace

void ChatHandler::initializeChatLog() {
    const char* enabledRaw = std::getenv("WOWEE_CHAT_LOG");
    if (!enabledRaw) return;

    std::string enabledValue(enabledRaw);
    if (isFalsyEnvValue(enabledValue)) return;

    const char* pathRaw = std::getenv("WOWEE_CHAT_LOG_PATH");
    const bool hasPathOverride = pathRaw && *pathRaw;
    chatLogPath_ = hasPathOverride ? pathRaw : "logs/chat.log";
    if (!hasPathOverride && !isTruthyEnvValue(enabledValue) && !enabledValue.empty()) {
        chatLogPath_ = enabledValue;
    }

    std::filesystem::path path(chatLogPath_);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    chatLogStream_.open(path, std::ios::out | std::ios::app);
    if (!chatLogStream_.is_open()) {
        LOG_WARNING("Chat external log requested but could not open: ", path.string());
        return;
    }

    chatLogEnabled_ = true;
    chatLogInitialized_ = true;
    chatLogStream_ << "# WoWee chat log started " << formatChatLogTimestamp(std::chrono::system_clock::now()) << '\n';
    chatLogStream_ << "# timestamp\tsource\ttype\tsender\tsender_guid\treceiver\treceiver_guid\tchannel\tmessage\n";
    chatLogStream_.flush();
    LOG_INFO("External chat logging enabled: ", path.string());
}

void ChatHandler::logChatMessage(const MessageChatData& msg, const char* source) {
    if (!chatLogEnabled_ || !chatLogInitialized_ || !chatLogStream_.is_open()) return;
    if (msg.message.empty()) return;

    const std::string sender = chatParticipant(msg.senderName, msg.senderGuid, "System");
    const std::string receiver = chatParticipant(msg.receiverName, msg.receiverGuid, "");
    chatLogStream_
        << formatChatLogTimestamp(msg.timestamp) << '\t'
        << (source ? source : "") << '\t'
        << getChatTypeString(msg.type) << '\t'
        << escapeChatLogField(sender) << '\t'
        << formatChatLogGuid(msg.senderGuid) << '\t'
        << escapeChatLogField(receiver) << '\t'
        << formatChatLogGuid(msg.receiverGuid) << '\t'
        << escapeChatLogField(msg.channelName) << '\t'
        << escapeChatLogField(msg.message) << '\n';
    chatLogStream_.flush();
}

void ChatHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet);
    };
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        // SMSG_GM_MESSAGECHAT has the same header as SMSG_MESSAGECHAT
        // (type[1]+lang[4]+senderGuid[8]+unk[4] = 17 bytes) followed by an
        // extra gmNameLen[4]+gmName[N] before the type-specific body.
        // Strip the GM name field to produce standard SMSG_MESSAGECHAT format.
        if (!packet.hasRemaining(21)) return; // 17 header + 4 gmNameLen min
        uint8_t  type       = packet.readUInt8();
        uint32_t lang       = packet.readUInt32();
        uint64_t senderGuid = packet.readUInt64();
        uint32_t unk        = packet.readUInt32();
        uint32_t gmNameLen  = packet.readUInt32();
        if (!packet.hasRemaining(gmNameLen)) return;
        packet.setReadPos(packet.getReadPos() + gmNameLen); // skip gmName

        // Rebuild as regular SMSG_MESSAGECHAT (header + remaining body)
        network::Packet regular(0);
        regular.writeUInt8(type);
        regular.writeUInt32(lang);
        regular.writeUInt64(senderGuid);
        regular.writeUInt32(unk);
        const auto& raw = packet.getData();
        size_t pos = packet.getReadPos();
        if (pos < raw.size())
            regular.writeBytes(raw.data() + pos, raw.size() - pos);
        handleMessageChat(regular);
    };
    table[Opcode::SMSG_TEXT_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleTextEmote(packet);
    };
    table[Opcode::SMSG_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!packet.hasRemaining(12)) return;
        uint32_t emoteId    = packet.readUInt32();
        uint64_t sourceGuid = packet.readUInt64();
        uint32_t animId = rendering::AnimationController::getEmoteAnimByEmotesId(emoteId);
        // Emotes.dbc EmoteSpecProc distinguishes persistent STATE_ emotes from
        // one-shots. Emote 0 (ONESHOT_NONE) cancels a one-shot but must not
        // clear a UNIT_NPC_EMOTESTATE work loop, so it is forwarded as non-state.
        const bool isState = emoteId != 0 &&
            rendering::AnimationController::isStateEmoteById(emoteId);
        if (owner_.emoteAnimCallbackRef() && sourceGuid != 0 && (animId != 0 || emoteId == 0)) {
            owner_.emoteAnimCallbackRef()(sourceGuid, animId, isState);
        } else if (emoteId != 0 && animId == 0) {
            LOG_DEBUG("SMSG_EMOTE emoteId=", emoteId, " had no Emotes.dbc animation mapping");
        }
    };
    table[Opcode::SMSG_CHANNEL_NOTIFY] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD ||
            owner_.getState() == WorldState::ENTERING_WORLD)
            handleChannelNotify(packet);
    };
    table[Opcode::SMSG_CHAT_PLAYER_NOT_FOUND] = [this](network::Packet& packet) {
        // ERR_CHAT_PLAYER_NOT_FOUND_S, a chat line (Wow.exe 0x6E2E90).
        std::string name = packet.readString();
        if (!name.empty())
            owner_.displayGameMessage("ERR_CHAT_PLAYER_NOT_FOUND_S", "No player named '" + name + "' is currently playing.", {name});
    };
    table[Opcode::SMSG_CHAT_PLAYER_AMBIGUOUS] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty())
            owner_.displayGameMessage("ERR_CHAT_PLAYER_AMBIGUOUS_S", name + " is ambiguous.", {name});
    };
    // These three as Wow.exe (0x6E2E90) words them, chat lines. Each raised
    // its own sentence twice on the red error line.
    table[Opcode::SMSG_CHAT_WRONG_FACTION] = [this](network::Packet& /*packet*/) {
        owner_.displayGameMessage("ERR_CHAT_WRONG_FACTION", "You cannot speak to members of the other faction.");
    };
    // The chat type it was refused for: party (2, 51) or raid (3, 39, 40).
    table[Opcode::SMSG_CHAT_NOT_IN_PARTY] = [this](network::Packet& packet) {
        const uint32_t type = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        if (type == 2 || type == 0x33)
            owner_.displayGameMessage("ERR_NOT_IN_GROUP", "You aren't in a party.");
        else if (type == 3 || type == 0x27 || type == 0x28)
            owner_.displayGameMessage("ERR_NOT_IN_RAID", "You are not in a raid group");
    };
    table[Opcode::SMSG_CHAT_RESTRICTED] = [this](network::Packet& packet) {
        const uint8_t why = packet.hasRemaining(1) ? packet.readUInt8() : 0;
        static const char* const kKeys[] = {"ERR_CHAT_RESTRICTED", "ERR_CHAT_THROTTLED",
                                            "ERR_USER_SQUELCHED", "ERR_YELL_RESTRICTED"};
        if (why < 4) owner_.displayGameMessage(kKeys[why], "");
    };

    // ---- Channel list ----

    // ---- Server / defense / area-trigger messages (moved from GameHandler) ----
    // Wow.exe 0x50C850: zone id, a uint32 length and that many bytes (not a
    // C string), posted like SMSG_ZONE_UNDER_ATTACK's line (postDefenseLine).
    table[Opcode::SMSG_DEFENSE_MESSAGE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
        const uint32_t zoneId = packet.readUInt32();
        const uint32_t len = packet.readUInt32();
        std::string text;
        for (uint32_t i = 0; i < len && packet.hasRemaining(1); ++i) {
            const char c = static_cast<char>(packet.readUInt8());
            if (c) text.push_back(c);
        }
        packet.skipAll();
        if (!text.empty()) postDefenseLine(zoneId, text);
    };
    // Server messages
    table[Opcode::SMSG_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t msgType = packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                std::string prefix;
                switch (msgType) {
                    case 1: prefix = "[Shutdown] ";   owner_.addUIError("Server shutdown: " + msg);  break;
                    case 2: prefix = "[Restart] ";    owner_.addUIError("Server restart: " + msg);   break;
                    case 4: prefix = "[Shutdown cancelled] "; break;
                    case 5: prefix = "[Restart cancelled] ";  break;
                    default: prefix = "[Server] "; break;
                }
                addSystemChatMessage(prefix + msg);
            }
        }
    };
    table[Opcode::SMSG_CHAT_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // The type's ServerMessages.dbc format with the text, a system line
            // ("[%d]: %s" for a type the table lacks), Wow.exe 0x50C980.
            const uint32_t msgType = packet.readUInt32();
            const std::string text = packet.hasData() ? packet.readString() : std::string();
            static const char* const kServerMessages[] = {
                nullptr, "[SERVER] Shutdown in %s", "[SERVER] Restart in %s", "%s",
                "[SERVER] Shutdown cancelled", "[SERVER] Restart cancelled",
                "[SERVER] Battleground shutdown in %s", "[SERVER] Battleground restart in %s",
                "[SERVER] Instance shutdown in %s", "[SERVER] Instance restart in %s",
            };
            std::string line;
            if (msgType >= 1 && msgType <= 9) {
                line = text.empty() ? std::string(kServerMessages[msgType])
                                    : formatGlobalString(kServerMessages[msgType], {text});
            } else {
                line = "[" + std::to_string(msgType) + "]: " + text;
            }
            if (!line.empty()) addSystemChatMessage(line);
        }
    };
    table[Opcode::SMSG_AREA_TRIGGER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            // The yellow info text alone (Wow.exe 0x526530, UI_INFO_MESSAGE).
            /*uint32_t len =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) owner_.addUIInfoMessage(msg);
        }
    };

    table[Opcode::SMSG_CHANNEL_LIST] = [this](network::Packet& p) { handleChannelList(p); };
    // Add and update carry the same fields and mean the same thing to a
    // roster that keys on the guid.
    table[Opcode::SMSG_USERLIST_ADD] = [this](network::Packet& p) { handleUserlistAdd(p); };
    table[Opcode::SMSG_USERLIST_UPDATE] = [this](network::Packet& p) { handleUserlistAdd(p); };
    table[Opcode::SMSG_USERLIST_REMOVE] = [this](network::Packet& p) { handleUserlistRemove(p); };
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }

    // Auto Clear AFK, which the panel offers and nothing read: the flag was
    // only ever cleared by typing /afk a second time, so a player who came
    // back and started talking stayed away as far as everyone else could see,
    // still auto-replying to whispers.
    //
    // Sending a message is the signal used here. It is not the only one WoW
    // takes - moving clears it too - but it is the one that matters, because
    // it is the one where the player is visibly present and being answered
    // for by a machine.
    //
    // Away and busy are chat types of their own, with the client saying the
    // change (Wow.exe 0x50D170): handled before the empty-message refusal
    // below, since an empty one is how either is cleared.
    if (type == ChatType::AFK) { toggleAfk(message); return; }
    if (owner_.afkStatusRef() &&
        addons::storedCVarValue("autoClearAFK", "1") != "0") {
        clearAfk();
    }
    if (type == ChatType::DND) { toggleDnd(message); return; }

    if (message.empty()) {
        LOG_WARNING("Cannot send empty chat message");
        return;
    }

    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");

    // Use the player's faction language. AzerothCore rejects wrong language.
    // Alliance races: Human(1), Dwarf(3), NightElf(4), Gnome(7), Draenei(11) → COMMON (7)
    // Horde races: Orc(2), Undead(5), Tauren(6), Troll(8), BloodElf(10) → ORCISH (1)
    uint8_t race = owner_.getPlayerRace();
    bool isHorde = (race == 2 || race == 5 || race == 6 || race == 8 || race == 10);
    ChatLanguage language = isHorde ? ChatLanguage::ORCISH : ChatLanguage::COMMON;

    auto packet = MessageChatPacket::build(type, language, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);

    // No local echo. The stock client shows its own line - and raises its
    // own bubble - only when the server sends the message back, so a line the
    // server refuses (muted, a GM command, a language not known, a channel not
    // joined) never appears, and every line carries what the server made of
    // it. The local copy here had shown ".tele ..." as speech over the head.
}

void ChatHandler::showChatBubble(uint64_t senderGuid, ChatType type, const std::string& message) {
    if (!owner_.chatBubbleCallbackRef() || senderGuid == 0 || message.empty()) return;
    // Stock (0x720010): say, yell and the monster say and yell answer to
    // chatBubbles, which the bubble manager reads; party and party leader to
    // chatBubblesParty, on as it ships. Nothing else makes a bubble - no raid,
    // no monster party line, no whisper or emote.
    const int gate = ui::stockui::chatBubbleGate(static_cast<uint8_t>(type));
    if (gate == 0) return;
    if (gate == 2 && addons::storedCVarValue("chatBubblesParty", "1") == "0") return;
    std::string text = message;
    // A monster's line has its "%%" collapsed to "%" first (0x509dd0).
    if (type == ChatType::MONSTER_SAY || type == ChatType::MONSTER_YELL) {
        for (size_t at = text.find("%%"); at != std::string::npos; at = text.find("%%", at + 1))
            text.erase(at, 1);
    }
    owner_.chatBubbleCallbackRef()(senderGuid, text, static_cast<uint8_t>(type));
}

void ChatHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD || message.empty()) return;

    auto packet = MessageChatPacket::build(type, ChatLanguage::ADDON, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT addon type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);
}

void ChatHandler::handleMessageChat(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG RX SMSG_MESSAGECHAT bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }

    MessageChatData data;
    if (!owner_.getPacketParsers()->parseMessageChat(packet, data)) {
        const auto& raw = packet.getData();
        LOG_WARNING("Failed to parse SMSG_MESSAGECHAT, size=", packet.getSize(),
                    " data=[", raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
        return;
    }
    if (chatPacketDiagEnabled()) {
        LOG_WARNING("CHAT PACKET DIAG PARSED SMSG_MESSAGECHAT type=", getChatTypeString(data.type),
                    " sender=", formatChatLogGuid(data.senderGuid), " senderName='", data.senderName,
                    "' receiver=", formatChatLogGuid(data.receiverGuid), " receiverName='", data.receiverName,
                    "' channel='", data.channelName, "' msg='", data.message, "'");
    }
    LOG_DEBUG("INCOMING CHAT: type=", static_cast<int>(data.type),
             " (", getChatTypeString(data.type), ") sender=0x", std::hex, data.senderGuid, std::dec,
             " '", data.senderName, "' msg='", data.message.substr(0, 60), "'");

    // A whisper to yourself still names you as the one to reply to.
    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0 &&
        data.type == ChatType::WHISPER && !data.senderName.empty()) {
        owner_.lastWhisperSenderRef() = data.senderName;
    }

    deliverChatMessage(std::move(data), /*alreadyWaited=*/false);
}

void ChatHandler::deliverChatMessage(MessageChatData data, bool alreadyWaited) {
    // Resolve sender name from entity/cache if not already set by parser
    if (data.senderName.empty() && data.senderGuid != 0) {
        auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
        if (nameIt != owner_.getPlayerNameCache().end()) {
            data.senderName = nameIt->second;
        } else {
            auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
            if (entity) {
                if (entity->getType() == ObjectType::PLAYER) {
                    auto player = std::dynamic_pointer_cast<Player>(entity);
                    if (player && !player->getName().empty()) {
                        data.senderName = player->getName();
                    }
                } else if (entity->getType() == ObjectType::UNIT) {
                    auto unit = std::dynamic_pointer_cast<Unit>(entity);
                    if (unit && !unit->getName().empty()) {
                        data.senderName = unit->getName();
                    }
                }
            }
        }

        if (data.senderName.empty()) {
            const auto& partyData = owner_.getPartyData();
            for (const auto& member : partyData.members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        // The guild roster, which was not among the places looked and is the
        // one that holds the names this most often needs.
        //
        // Every other source here is someone nearby or in the party. A
        // guildmate is characteristically neither: they are in another zone,
        // which is the whole point of guild chat. So a guild line from anyone
        // not recently met arrived with no name at all - "[Guild] : message" -
        // and a guild achievement, which is announced by whoever earned it
        // wherever they are, read "[] has earned the achievement".
        //
        // The roster is already held and already carries the name beside the
        // guid; nothing had asked it. It is only populated once the roster has
        // been requested, so this is a source rather than a guarantee, and the
        // query below still runs when it misses.
        if (data.senderName.empty()) {
            for (const auto& member : owner_.getGuildRoster().members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        if (data.senderName.empty()) {
            owner_.queryPlayerName(data.senderGuid);
            // Hold the line rather than showing it with nobody's name on it.
            //
            // SMSG_MESSAGECHAT carries a guid and no name for a player line,
            // and every source above is somebody nearby, in the party or in
            // the guild. A whisper is characteristically from none of those -
            // that is what makes it a whisper - so it missed all of them, went
            // out with an empty name, and the interface printed it with no
            // sender. The backfill that follows a name query reaches this
            // client's own history and not what the interface has already
            // drawn, so the name has to be there the first time.
            if (!alreadyWaited) {
                const uint64_t waitingOn = data.senderGuid;
                chatAwaitingName_.push_back({std::move(data), waitingOn,
                                             core::appTimeSeconds() + kNameWaitSeconds});
                return;
            }
        }
    }

    // And the player an NPC's line is written for, when that is someone else.
    //
    // "$N has defeated the hero of the Warmaul" is a yell the whole area hears,
    // and $N is whoever earned it: the packet names them as the receiver. This
    // wrote the logged-in character's name into every such line, so each
    // bystander read that they had done it themselves. A player receiver comes
    // as a guid alone, so a name nothing holds yet is asked for and the line
    // waits for it, as a player's own line does.
    if (isMonsterChatType(data.type) && data.message.find('$') != std::string::npos &&
        isPlayerGuid(data.receiverGuid) && data.receiverGuid != owner_.getPlayerGuid() &&
        knownPlayerName(data.receiverGuid).empty()) {
        owner_.queryPlayerName(data.receiverGuid);
        if (!alreadyWaited) {
            const uint64_t waitingOn = data.receiverGuid;
            chatAwaitingName_.push_back({std::move(data), waitingOn,
                                         core::appTimeSeconds() + kNameWaitSeconds});
            return;
        }
    }

    if (data.message.empty()) {
        return;
    }

    if (data.type == ChatType::ACHIEVEMENT ||
        data.type == ChatType::GUILD_ACHIEVEMENT) {
        auto replaceAll = [](std::string& text, const std::string& marker,
                             const std::string& replacement) {
            size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                text.replace(pos, marker.size(), replacement);
                pos += replacement.size();
            }
        };

        if (data.achievementId != 0) {
            owner_.ensureAchievementNamesLoaded();
            std::string achievementName = owner_.getAchievementName(data.achievementId);
            if (achievementName.empty()) {
                achievementName = "Achievement #" + std::to_string(data.achievementId);
            }
            std::ostringstream link;
            link << "|cffffff00|Hachievement:" << data.achievementId
                 << ":0:0:0:0:0:0:0:0:0|h[" << achievementName << "]|h|r";
            replaceAll(data.message, "$a", link.str());
        }
    }

    // Addon messages use the same chat types as player messages, but belong to
    // CHAT_MSG_ADDON and must never be inserted into visible chat history.
    std::string addonPrefix;
    std::string addonPayload;
    if (decodeAddonChatPayload(data, addonPrefix, addonPayload)) {
        if (owner_.addonEventCallbackRef() && !addonPrefix.empty()) {
            owner_.addonEventCallbackRef()(
                "CHAT_MSG_ADDON",
                {addonPrefix, addonPayload, getChatTypeString(data.type), data.senderName});
        }
        return;
    }

    // Server monster messages use %s as a placeholder for the creature's name.
    if (!data.senderName.empty() && (
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_EMOTE || data.type == ChatType::MONSTER_WHISPER ||
            data.type == ChatType::MONSTER_PARTY ||
            data.type == ChatType::RAID_BOSS_EMOTE || data.type == ChatType::RAID_BOSS_WHISPER)) {
        size_t pos = data.message.find("%s");
        while (pos != std::string::npos) {
            data.message.replace(pos, 2, data.senderName);
            pos = data.message.find("%s", pos + data.senderName.size());
        }
    }

    // Filter BG/Arena queue announcer spam (server-side modules on
    // ChromieCraft/AzerothCore). Common formats:
    //   |cffff0000[BG Queue Announcer]:|r ...
    //   |cffff0000[Arena Queue Announcer]:|r ...
    //   |cFFFFA500<player> joined : |cFF00FFFF2x2|r
    //   |cFFFFA500<player> exited |cFF00FFFF3x3|r
    // The third/fourth forms drop the "Queue Announcer" prefix entirely, so
    // we also detect the announcer-shaped pattern: a colored message that
    // names an arena/BG bracket suffix like "2x2|r" / "3v3|r".
    {
        const auto& msg = data.message;
        auto containsCI = [&](const char* needle) {
            const size_t nlen = std::strlen(needle);
            if (msg.size() < nlen) return false;
            const size_t last = msg.size() - nlen;
            for (size_t i = 0; i <= last; ++i) {
                bool match = true;
                for (size_t j = 0; j < nlen; ++j) {
                    unsigned char a = static_cast<unsigned char>(msg[i + j]);
                    unsigned char b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b)) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };
        if (containsCI("queue announcer") || containsCI("queue status")) {
            return;
        }
        // Pattern-based catch for prefix-less variants. Require the message to
        // contain a color code (server-formatted) AND an arena/BG bracket token
        // immediately followed by |r AND a verb word ("joined", "exited",
        // "left", "entered"). Plain player chat won't hit all three.
        const bool hasColor = msg.find("|c") != std::string::npos;
        if (hasColor) {
            static const char* kBracketTokens[] = {
                "2x2|r", "3x3|r", "5x5|r",
                "2v2|r", "3v3|r", "5v5|r",
                "2X2|r", "3X3|r", "5X5|r",
                "2V2|r", "3V3|r", "5V5|r",
            };
            bool hasBracket = false;
            for (const char* t : kBracketTokens) {
                if (msg.find(t) != std::string::npos) { hasBracket = true; break; }
            }
            if (hasBracket && (containsCI("joined") || containsCI("exited") ||
                               containsCI(" left ") || containsCI("entered") ||
                               containsCI("queue"))) {
                return;
            }
        }
    }

    // Filter officer chat if player doesn't have officer chat permission.
    // Some servers send officer chat to all guild members regardless of rank.
    // WoW guild right bit 0x40 = GR_RIGHT_OFFCHATSPEAK, 0x80 = GR_RIGHT_OFFCHATLISTEN
    if (data.type == ChatType::OFFICER) {
        // Through the shared lookup, which answers zero rights when the roster
        // has not arrived - the same "say nothing about it" this had before,
        // since the test below only hides chat when a rank is known and lacks
        // the bit.
        const uint32_t idx = owner_.getPlayerGuildRankIndex();
        if (idx != 0xFFFFFFFFu && idx < owner_.getGuildRoster().ranks.size()) {
            if (!(owner_.getPlayerGuildRankRights() & 0x80)) { // GR_RIGHT_OFFCHATLISTEN
                return; // Don't show officer chat to non-officers
            }
        }
    }

    // Filter addon-to-addon whispers (GearScore, DBM, oRA, etc.) from player chat.
    // These are invisible in the real WoW client.
    if (data.type == ChatType::WHISPER || data.type == ChatType::WHISPER_INFORM) {
        const auto& msg = data.message;
        if (msg.size() >= 3 && (
            msg.rfind("GS_", 0) == 0 ||          // GearScore
            msg.rfind("DVNE", 0) == 0 ||          // DBM (DeadlyBossMods)
            msg.rfind("oRA", 0) == 0 ||            // oRA raid addon
            msg.rfind("BWVQ", 0) == 0 ||           // BigWigs
            msg.rfind("AVR", 0) == 0 ||            // AVR (Augmented Virtual Reality)
            msg.rfind('\t', 0) == 0 ||             // Tab-prefixed addon messages
            (msg.size() > 4 && static_cast<unsigned char>(msg[0]) > 127))) {  // Binary data
            return; // Silently discard addon whisper
        }
    }

    // Fill in the $-tokens before anything sees the line.
    //
    // A monster's say, yell, emote or whisper arrives with the player left as a
    // blank - "$N, you have done well" - and the client is what writes the name
    // in. Nothing did, so every scripted NPC in the game addressed the player as
    // "$N", and a $gsir:madam; came out with the whole switch printed.
    //
    // Only what an NPC or a boss said. A player typing "$N" into say has typed
    // those two characters and means them: the real client does not resolve
    // tokens in player chat, and resolving them here would let one player make
    // another's client print that player's own name.
    //
    // For whoever the line is written for, which is not always the reader:
    // see monsterLineSubject.
    if (isMonsterChatType(data.type)) {
        data.message = resolveTextTokens(data.message, monsterLineSubject(data));
    }

    // Add to chat history
    data.uid = ++chatUidCounter_;
    chatHistory_.push_back(data);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.erase(chatHistory_.begin());
    }
    logChatMessage(data, "server");
    // No fireChatEvent here. This function announces the message itself,
    // further down and with the fuller argument list - the channel number and
    // short name, the line id, the sender's guid. Calling fireChatEvent as
    // well fired CHAT_MSG_* twice for every line the server sent, and the
    // interface drew both: every whisper, say, guild and channel message
    // appeared in the window doubled.
    //
    // The note that stood here said the event was fired only on the local
    // path, which was true when it was written and stopped being true when
    // this function grew its own. fireChatEvent stays for
    // addLocalChatMessage's callers, which have no richer arguments to give.

    // Track whisper sender for /r command
    if (data.type == ChatType::WHISPER) {
        // Always store GUID so getLastWhisperSender() can resolve the name
        // from the player name cache even if name wasn't available yet
        if (data.senderGuid != 0)
            owner_.lastWhisperSenderGuidRef() = data.senderGuid;
        if (!data.senderName.empty())
            owner_.lastWhisperSenderRef() = data.senderName;

        if (!data.senderName.empty()) {
            // Only auto-reply once per sender per AFK/DND session to prevent loops
            if (owner_.afkStatusRef() && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.afkMessageRef().empty() ? "Away from Keyboard" : owner_.afkMessageRef();
                sendChatMessage(ChatType::WHISPER, "<AFK> " + reply, data.senderName);
            } else if (owner_.dndStatusRef() && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.dndMessageRef().empty() ? "Do Not Disturb" : owner_.dndMessageRef();
                sendChatMessage(ChatType::WHISPER, "<DND> " + reply, data.senderName);
            }
        }
    }

    // The Social panel's two filters, before anything shows this line: the
    // bubble below, the CHAT_MSG_ event, and the log all take the text from
    // here, so filtering at one point covers all three.
    //
    // Only what a player typed. A quest giver, a boss emote or a system notice
    // is not somebody spamming, and masking words in them would edit the
    // game's own writing.
    const bool fromAPlayer =
        data.type == ChatType::SAY || data.type == ChatType::YELL ||
        data.type == ChatType::PARTY || data.type == ChatType::RAID ||
        data.type == ChatType::GUILD || data.type == ChatType::OFFICER ||
        data.type == ChatType::WHISPER || data.type == ChatType::EMOTE ||
        data.type == ChatType::CHANNEL;

    if (fromAPlayer) {
        // Disable Spam Filter, so the CVar being on means filtering happens.
        if (addons::storedCVarValue("spamFilter", "1") != "0") {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (repeatsRecentLine(recentChatLines_, data.senderGuid, data.message, now)) {
                return;
            }
            recentChatLines_.push_back({.senderGuid = data.senderGuid, .text = data.message, .at = now});
            // A short memory is the point: this is looking for a line pasted
            // again a moment later, not keeping a record of the conversation.
            while (recentChatLines_.size() > 64) recentChatLines_.pop_front();
        }
    }

    // Chat bubble for say/yell (and party, when asked for) over the speaker.
    showChatBubble(data.senderGuid, data.type, data.message);

    // Log the message
    std::string senderInfo;
    if (!data.senderName.empty()) {
        senderInfo = data.senderName;
    } else if (data.senderGuid != 0) {
        senderInfo = "Unknown-" + std::to_string(data.senderGuid);
    } else {
        senderInfo = "System";
    }

    std::string channelInfo;
    if (!data.channelName.empty()) {
        channelInfo = "[" + data.channelName + "] ";
    }

    LOG_DEBUG("[", getChatTypeString(data.type), "] ", channelInfo, senderInfo, ": ", data.message);

    // Fire CHAT_MSG_* addon events
    if (owner_.addonEventCallbackRef()) {
        std::string eventName = "CHAT_MSG_";
        eventName += getChatTypeString(data.type);
        // The language's *name*, not its id. ChatFrame_MessageEventHandler
        // does
        //
        //     if strlen(arg3) > 0 and arg3 ~= "Universal"
        //                          and arg3 ~= self.defaultLanguage then
        //         languageHeader = "["..arg3.."] "
        //
        // and GetDefaultLanguage answers "Common" or "Orcish". A number never
        // matches either, so every line in the player's own language was
        // printed with its id in front of it - guild chat in Common came out
        // as "[7] Name: hello". Universal is the id the file does not carry
        // and the word the test above looks for.
        const std::string lang = owner_.getLanguageName(static_cast<uint32_t>(data.language));
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)data.senderGuid);
        // arg8 is the channel's number and arg9 its name without the zone
        // after it, and both were sent empty. That is the whole of whether a
        // channel line is shown: the handler builds "CHANNEL"..arg8 and looks
        // it up in ChatTypeInfo, so a zero made every channel message
        // CHANNEL0, which no table has, and the message was dropped before it
        // reached the window. arg9 is what the frame matches against the
        // channels it carries when the zone id does not settle it.
        // A line id of its own, remembered against the sender. FrameXML builds
        // this into the player link on the name - "|Hplayer:Name:lineId:..." -
        // so a right-click on that name hands it back and the client is
        // expected to know which message it was. Zero for every line is one id
        // shared by all of them, which is what made Report Spam unofferable:
        // CanComplainChat is asked about the id before the entry is shown.
        const uint32_t lineId = nextChatLineId_++;
        if (data.senderGuid != 0) {
            chatLineSenders_.emplace_back(lineId, data.senderGuid);
            // Recent lines only. A session's chat has no end, and a message old
            // enough to have fallen out of this is old enough not to be
            // reportable.
            constexpr size_t kRememberedLines = 512;
            while (chatLineSenders_.size() > kRememberedLines)
                chatLineSenders_.pop_front();
        }
        const int channelNumber = getChannelIndex(data.channelName);
        const size_t dash = data.channelName.find(" - ");
        const std::string shortChannel = (dash == std::string::npos)
            ? data.channelName : data.channelName.substr(0, dash);
        // arg4 is the channel *with its number*, "1. General - Elwynn Forest",
        // as the real client sends it. FrameXML cuts the zone off and prints
        // "[1. General]", and before that only accepts the line if arg4 is
        // longer than the name the window lists - which a bare "mychannel"
        // against "mychannel" never is, so every custom channel was dropped.
        const std::string numberedChannel = (channelNumber > 0)
            ? std::to_string(channelNumber) + ". " + data.channelName : data.channelName;
        // arg7, the zone channel's ChatChannels.dbc id, 0 for a custom one.
        const uint32_t zoneChannelId =
            (channelNumber > 0 && static_cast<size_t>(channelNumber) <= joinedChannelIds_.size())
                ? joinedChannelIds_[static_cast<size_t>(channelNumber) - 1] : 0;
        // arg5 is the *target*, not the sender. It was the sender's own name,
        // which is only read by CHANNEL_NOTICE_USER - and read as "there are
        // two names in this notice", so every kick and ban was formatted as
        // though someone had done it to someone else. receiverName is what the
        // packet carries for it, empty on an ordinary message.
        //
        // arg6 is the flag beside the name: FrameXML looks up CHAT_FLAG_<arg6>
        // and prints <Away>, <Busy> or <GM>. The tag is on the packet and was
        // being dropped, so nothing ever showed as away or as a game master.
        // AzerothCore's values are a bitmask - AFK 0x01, DND 0x02, GM 0x04,
        // COM 0x08, DEV 0x10 - and the interface takes one word, so the most
        // significant is the one to name.
        const char* chatFlag = (data.chatTag & 0x04) ? "GM"
                             : (data.chatTag & 0x10) ? "DEV"
                             : (data.chatTag & 0x02) ? "DND"
                             : (data.chatTag & 0x01) ? "AFK"
                                                     : "";
        // Says a line reached the interface at all. A blank chat window is
        // either nothing arriving or something arriving and not being drawn,
        // and those have opposite causes with the same appearance.
        //
        // Once per event name, not once every two seconds: what is being
        // asked is whether a SAY, a YELL, a WHISPER reaches the interface,
        // and the second SAY answers nothing the first did. Rate limiting it
        // wrote one of these every two seconds for as long as anyone in the
        // zone was talking.
        {
            static std::set<std::string> saidFor;
            if (saidFor.size() < 32 && saidFor.insert(eventName).second) {
                LOG_WARNING("Chat: fired ", eventName, " to the interface from '",
                            data.senderName, "' - said once per event name");
            }
        }
        // arg2 is the name the interface prints, and for an outgoing whisper
        // that is the person written to rather than the one writing:
        // CHAT_WHISPER_INFORM_GET is "To %s: " and takes the same argument
        // CHAT_WHISPER_GET does. Passing the sender there addressed every
        // whisper the player sent to the player themselves.
        const std::string& shownName =
            (data.type == ChatType::WHISPER_INFORM && !data.receiverName.empty())
                ? data.receiverName
                : data.senderName;
        owner_.addonEventCallbackRef()(eventName, {
            data.message,
            shownName,
            lang,
            numberedChannel,
            data.receiverName,
            chatFlag,
            std::to_string(zoneChannelId),
            std::to_string(channelNumber),
            shortChannel,
            "0",
            std::to_string(lineId),
            guidBuf
        });
    }
}

void ChatHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = TextEmotePacket::build(textEmoteId, targetGuid);
    owner_.getSocket()->send(packet);
}

std::string ChatHandler::knownPlayerName(uint64_t guid) const {
    if (guid == 0) return {};
    if (const std::string& name = owner_.lookupName(guid); !name.empty()) return name;
    for (const auto& member : owner_.getPartyData().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    for (const auto& member : owner_.getGuildRoster().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    return {};
}

TextSubject ChatHandler::monsterLineSubject(const MessageChatData& data) {
    // Addressed to nobody, or to us: the reader, which is who a line with no
    // one else in it is for.
    if (data.receiverGuid == 0 || data.receiverGuid == owner_.getPlayerGuid()) {
        return textSubjectFor(owner_);
    }
    TextSubject subject;
    if (isPlayerGuid(data.receiverGuid)) {
        if (std::string name = knownPlayerName(data.receiverGuid); !name.empty()) {
            subject.name = std::move(name);
        }
        // Class, race and gender come with the name query's answer. Unknown,
        // they stay neutral rather than borrowing the reader's.
        if (const uint8_t cls = owner_.lookupPlayerClass(data.receiverGuid)) {
            subject.className = getClassName(static_cast<Class>(cls));
        }
        if (const uint8_t race = owner_.lookupPlayerRace(data.receiverGuid)) {
            subject.raceName = getRaceName(static_cast<Race>(race));
        }
        const uint8_t gender = owner_.lookupPlayerGender(data.receiverGuid);
        if (gender == 0) subject.gender = Gender::MALE;
        else if (gender == 1) subject.gender = Gender::FEMALE;
    } else if (!data.receiverName.empty()) {
        subject.name = data.receiverName;
    } else if (const std::string& name = owner_.lookupName(data.receiverGuid); !name.empty()) {
        subject.name = name;
    }
    return subject;
}

void ChatHandler::flushChatAwaitingName(uint64_t guid) {
    if (guid != 0 && !heldUserNotices_.empty()) {
        std::vector<HeldUserNotice> retry;
        for (auto it = heldUserNotices_.begin(); it != heldUserNotices_.end();) {
            if (it->guid1 == guid || it->guid2 == guid) { retry.push_back(std::move(*it)); it = heldUserNotices_.erase(it); }
            else ++it;
        }
        for (const auto& n : retry)
            fireChannelUserNotice(n.notice.c_str(), n.channelName, n.guid1, n.name1, n.guid2);
    }
    if (chatAwaitingName_.empty() || guid == 0) return;
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (it->guid == guid) {
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    // Delivered outside the walk: each one runs the whole chat path, which can
    // hold another line of its own.
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
}

void ChatHandler::expireChatAwaitingName() {
    // Riding on this per-tick call rather than adding another: the zone
    // channels wait for the zone to be known, and this is chat's tick.
    servicePendingAutoJoin();
    if (chatAwaitingName_.empty()) return;
    const double now = core::appTimeSeconds();
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (now >= it->deadline) {
            LOG_WARNING("Chat: no name for ", formatChatLogGuid(it->guid),
                        " within ", kNameWaitSeconds, "s - showing the line without one");
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
}

void ChatHandler::handleTextEmote(network::Packet& packet) {
    const bool legacyFormat = isClassicLikeExpansion();
    TextEmoteData data;
    if (!TextEmoteParser::parse(packet, data, legacyFormat)) {
        LOG_WARNING("Failed to parse SMSG_TEXT_EMOTE");
        return;
    }
    // The voice line plays for every sender, the player included.
    if (owner_.textEmoteSoundCallbackRef()) owner_.textEmoteSoundCallbackRef()(data.senderGuid, data.textEmoteId);

    // The player's own name, for the echo and for "Name laughs at you."
    std::string playerName;
    if (const Character* active = owner_.getActiveCharacter()) playerName = active->name;
    if (playerName.empty()) playerName = owner_.lookupName(owner_.getPlayerGuid());

    const bool fromPlayer = data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0;
    const std::string* targetPtr = data.targetName.empty() ? nullptr : &data.targetName;
    auto& emotes = rendering::EmoteRegistry::instance();
    emotes.loadFromDbc();

    // SMSG_TEXT_EMOTE is the narrated chat line. The server sends the actual
    // visual separately in SMSG_EMOTE; replaying an animation here restarts
    // one-shots and replaces correctly resolved STATE_* loops such as /dance.
    std::string senderName;
    std::string emoteText;
    if (fromPlayer) {
        // The player's own echo is the only place "You laugh." comes from.
        // This used to be skipped as a duplicate of a local line, but DoEmote
        // - the route FrameXML's chat takes for every /emote - adds none, so
        // the player's own emotes never appeared in chat at all. The real
        // client prints it from this echo too, which is also why a dead
        // player's /laugh (which the server refuses) prints nothing.
        senderName = playerName;
        emoteText = emotes.selfTextByDbcId(data.textEmoteId, targetPtr);
    } else {
        senderName = knownPlayerName(data.senderGuid);
        if (senderName.empty()) {
            owner_.queryPlayerName(data.senderGuid);
            LOG_DEBUG("Deferred chat text for unresolved SMSG_TEXT_EMOTE sender=0x",
                      std::hex, data.senderGuid, std::dec,
                      " emoteId=", data.textEmoteId);
            return;
        }
        // Aimed at the player: "Name laughs at you.", as the target reads it.
        if (targetPtr && !playerName.empty() && data.targetName == playerName) {
            emoteText = emotes.atYouTextByDbcId(data.textEmoteId, senderName, playerName);
        } else {
            emoteText = emotes.textByDbcId(data.textEmoteId, senderName, targetPtr);
        }
    }
    if (emoteText.empty()) {
        emoteText = data.targetName.empty()
            ? senderName + " performs an emote."
            : senderName + " performs an emote at " + data.targetName + ".";
    }

    MessageChatData chatMsg;
    chatMsg.type = ChatType::TEXT_EMOTE;
    // Universal, so the chat frame puts no "[Common] " header in front of it
    // for a Horde reader: a text emote is narration, not speech.
    chatMsg.language = ChatLanguage::UNIVERSAL;
    chatMsg.senderGuid = data.senderGuid;
    chatMsg.senderName = senderName;
    chatMsg.message = emoteText;

    addLocalChatMessage(chatMsg);

    LOG_INFO("TEXT_EMOTE from ", senderName, " (emoteId=", data.textEmoteId, ")");
}

void ChatHandler::joinChannel(const std::string& channelName, const std::string& password,
                              uint32_t channelId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // A zone channel's id goes in the prefix WotLK and TBC share; the
    // expansion builders only ever send zero there, which the server reads
    // as a custom channel of that name.
    auto packet = (channelId != 0 && !isClassicLikeExpansion())
        ? JoinChannelPacket::build(channelName, password, channelId)
        : owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildJoinChannel(channelName, password)
            : JoinChannelPacket::build(channelName, password);
    owner_.getSocket()->send(packet);
    // Asking to join is the opposite of asking to leave.
    leavingChannels_.erase(channelName);
    LOG_INFO("Requesting to join channel: ", channelName, " id=", channelId);
}

void ChatHandler::requestChannelList(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The channel name and nothing else. SMSG_CHANNEL_LIST was parsed all
    // along and nothing ever asked for one, so the roster only existed in
    // theory.
    network::Packet packet(wireOpcode(Opcode::CMSG_CHANNEL_LIST));
    packet.writeString(channelName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting member list for channel: ", channelName);
}

void ChatHandler::leaveChannel(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildLeaveChannel(channelName)
        : LeaveChannelPacket::build(channelName);
    owner_.getSocket()->send(packet);
    leavingChannels_.insert(channelName);
    LOG_INFO("Requesting to leave channel: ", channelName);
}

const char* ChatHandler::rememberJoinedChannel(const std::string& name, uint32_t channelId) {
    if (joinedChannelIds_.size() != joinedChannels_.size())
        joinedChannelIds_.resize(joinedChannels_.size(), 0);
    for (size_t i = 0; i < joinedChannels_.size(); ++i) {
        if (joinedChannels_[i] != name) continue;
        if (channelId != 0) joinedChannelIds_[i] = channelId;
        // Rejoined after the server suspended it: a join worth announcing.
        return suspendedChannels_.erase(name) ? "YOU_JOINED" : nullptr;
    }
    // A zone channel under its new zone's name takes the old one's number,
    // as the real client does: "Changed Channel: [1. General - Westfall]".
    if (channelId != 0) {
        for (size_t i = 0; i < joinedChannels_.size(); ++i) {
            if (joinedChannelIds_[i] != channelId) continue;
            const bool wasSuspended = suspendedChannels_.erase(joinedChannels_[i]) != 0;
            ownedChannels_.erase(joinedChannels_[i]);
            joinedChannels_[i] = name;
            return wasSuspended ? "YOU_JOINED" : "YOU_CHANGED";
        }
    }
    joinedChannels_.push_back(name);
    joinedChannelIds_.push_back(channelId);
    return "YOU_JOINED";
}

void ChatHandler::fireChannelNotice(const char* notice, const std::string& channelName) {
    if (!notice || !owner_.addonEventCallbackRef()) return;
    const int index = getChannelIndex(channelName);
    uint32_t zoneChannelId = 0;
    if (index > 0 && static_cast<size_t>(index) <= joinedChannelIds_.size())
        zoneChannelId = joinedChannelIds_[static_cast<size_t>(index) - 1];
    const size_t dash = channelName.find(" - ");
    const std::string shortName = dash == std::string::npos ? channelName : channelName.substr(0, dash);
    // arg4 is "N. Name" and arg8 the number: CHAT_YOU_JOINED_NOTICE is
    // "Joined Channel: |Hchannel:%d|h[%s]|h" formatted with arg8 and arg4.
    // arg9 is matched against the names the chat window carries ("General").
    const std::string numbered = index > 0
        ? std::to_string(index) + ". " + channelName : channelName;
    owner_.addonEventCallbackRef()("CHAT_MSG_CHANNEL_NOTICE", {
        notice, "", "", numbered, "", "", std::to_string(zoneChannelId),
        std::to_string(index), shortName, "0", "0", ""
    });
}

void ChatHandler::fireChannelUserNotice(const char* notice, const std::string& channelName,
                                        uint64_t guid1, const std::string& name1In, uint64_t guid2) {
    if (!notice || !owner_.addonEventCallbackRef()) return;
    const std::string name1 = !name1In.empty() ? name1In : (guid1 ? knownPlayerName(guid1) : std::string());
    const std::string name2 = guid2 ? knownPlayerName(guid2) : std::string();
    if ((guid1 && name1.empty()) || (guid2 && name2.empty())) {
        if (heldUserNotices_.size() < 32)
            heldUserNotices_.push_back({notice, channelName, name1In, guid1, guid2});
        if (guid1 && name1.empty()) owner_.queryPlayerName(guid1);
        if (guid2 && name2.empty()) owner_.queryPlayerName(guid2);
        return;
    }
    const int index = getChannelIndex(channelName);
    uint32_t zoneChannelId = 0;
    if (index > 0 && static_cast<size_t>(index) <= joinedChannelIds_.size())
        zoneChannelId = joinedChannelIds_[static_cast<size_t>(index) - 1];
    const size_t dash = channelName.find(" - ");
    const std::string shortName = dash == std::string::npos ? channelName : channelName.substr(0, dash);
    const std::string numbered = index > 0
        ? std::to_string(index) + ". " + channelName : channelName;
    // chatframe.lua formats CHAT_<notice>_NOTICE with arg8, arg4, arg2 and,
    // for two players, arg5; INVITE with arg4 and arg2.
    owner_.addonEventCallbackRef()("CHAT_MSG_CHANNEL_NOTICE_USER", {
        notice, name1, "", numbered, name2, "", std::to_string(zoneChannelId),
        std::to_string(index), shortName, "0", "0", ""
    });
}

std::string ChatHandler::getChannelByIndex(int index) const {
    if (index < 1 || index > static_cast<int>(joinedChannels_.size())) return "";
    return joinedChannels_[index - 1];
}

int ChatHandler::getChannelIndex(const std::string& channelName) const {
    for (int i = 0; i < static_cast<int>(joinedChannels_.size()); ++i) {
        if (joinedChannels_[i] == channelName) return i + 1;
    }
    return 0;
}

void ChatHandler::handleChannelNotify(network::Packet& packet) {
    ChannelNotifyData data;
    if (!ChannelNotifyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CHANNEL_NOTIFY");
        return;
    }

    switch (data.notifyType) {
        case ChannelNotifyType::YOU_JOINED: {
            // Through CHAT_MSG_CHANNEL_NOTICE, which FrameXML prints as
            // "Joined Channel: [1. General - Elwynn Forest]" (or "Changed
            // Channel:" when a zone change renamed it in place). This was a
            // SYSTEM line with no number, and the list it kept had no idea a
            // zone change had replaced General, so the numbers drifted.
            const char* notice = rememberJoinedChannel(data.channelName, data.channelId);
            fireChannelNotice(notice, data.channelName);
            LOG_INFO("Joined channel: ", data.channelName, " id=", data.channelId,
                     " as #", getChannelIndex(data.channelName));
            break;
        }
        case ChannelNotifyType::YOU_LEFT: {
            const bool asked = leavingChannels_.erase(data.channelName) != 0;
            if (data.constant && !asked && getChannelIndex(data.channelName) > 0) {
                // The server took a built-in channel away (left the city that
                // Trade belongs to). The real client keeps its number for when
                // it comes back and says SUSPENDED: "Left Channel: [2. Trade -
                // City]". YOU_LEFT would make FrameXML drop the channel from
                // the chat window, and Trade would stay filtered out of it
                // after the next visit to a city.
                suspendedChannels_.insert(data.channelName);
                fireChannelNotice("SUSPENDED", data.channelName);
                LOG_INFO("Channel suspended: ", data.channelName);
                break;
            }
            // Announced before it is taken off the list, so it still has its
            // number.
            fireChannelNotice("YOU_LEFT", data.channelName);
            ownedChannels_.erase(data.channelName);
            suspendedChannels_.erase(data.channelName);
            if (joinedChannelIds_.size() != joinedChannels_.size())
                joinedChannelIds_.resize(joinedChannels_.size(), 0);
            for (size_t i = joinedChannels_.size(); i-- > 0;) {
                if (joinedChannels_[i] == data.channelName) {
                    joinedChannels_.erase(joinedChannels_.begin() + static_cast<std::ptrdiff_t>(i));
                    joinedChannelIds_.erase(joinedChannelIds_.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
            LOG_INFO("Left channel: ", data.channelName);
            break;
        }
        case ChannelNotifyType::PLAYER_ALREADY_MEMBER: {
            // Server confirms we're in this channel but our local list doesn't have it yet -
            // can happen after reconnect or if the join notification was missed.
            if (rememberJoinedChannel(data.channelName, 0)) {
                LOG_INFO("Already in channel: ", data.channelName);
            }
            // Wow.exe also prints CHAT_PLAYER_ALREADY_MEMBER_NOTICE here. Not
            // done: this client re-joins its channels itself at world entry,
            // where the stock client's join is answered once, and every
            // re-join would print it.
            break;
        }
        case ChannelNotifyType::NOT_IN_AREA:
            fireChannelNotice("NOT_IN_AREA", data.channelName);   // CHAT_NOT_IN_AREA_NOTICE
            break;
        case ChannelNotifyType::WRONG_PASSWORD:
            fireChannelNotice("WRONG_PASSWORD", data.channelName);   // CHAT_WRONG_PASSWORD_NOTICE
            break;
        case ChannelNotifyType::NOT_MEMBER:
            fireChannelNotice("NOT_MEMBER", data.channelName);   // CHAT_NOT_MEMBER_NOTICE
            break;
        case ChannelNotifyType::NOT_MODERATOR:
            fireChannelNotice("NOT_MODERATOR", data.channelName);   // CHAT_NOT_MODERATOR_NOTICE
            break;
        case ChannelNotifyType::MUTED:
            fireChannelNotice("MUTED", data.channelName);   // CHAT_MUTED_NOTICE
            break;
        case ChannelNotifyType::BANNED:
            fireChannelNotice("BANNED", data.channelName);   // CHAT_BANNED_NOTICE
            break;
        case ChannelNotifyType::THROTTLED:
            fireChannelNotice("THROTTLED", data.channelName);   // CHAT_THROTTLED_NOTICE
            break;
        case ChannelNotifyType::NOT_IN_LFG:
            // 3.3.5 has no CHAT_NOT_IN_LFG_NOTICE string, so the stock client
            // shows nothing for it. AzerothCore sends it on every zone change
            // (Player::UpdateLocalChannels joins the constant LFG channel and
            // Channel::JoinChannel refuses a player not queued), which made
            // this client print a line at every teleport.
            LOG_DEBUG("Channel ", data.channelName, ": not in the LFG queue");
            break;
        // The notices naming a player go out as CHANNEL_NOTICE_USER with the
        // names, as Wow.exe sends them (0x50E120, chat type 0x16); the words
        // are FrameXML's CHAT_<token>_NOTICE.
        case ChannelNotifyType::PLAYER_KICKED:
            fireChannelUserNotice("PLAYER_KICKED", data.channelName, data.senderGuid, "", data.targetGuid);
            break;
        case ChannelNotifyType::PASSWORD_CHANGED:
            fireChannelUserNotice("PASSWORD_CHANGED", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::OWNER_CHANGED:
            // The guid beside the name is the new owner's -
            // Channel::MakeOwnerChanged writes _ownerGUID into it. Kept so the
            // unit menu can offer the moderator entries, which FrameXML hides
            // behind IsDisplayChannelOwner: the verbs behind them were built
            // and could not be reached.
            if (data.senderGuid != 0 && data.senderGuid == owner_.getPlayerGuid()) {
                ownedChannels_.insert(data.channelName);
            } else {
                ownedChannels_.erase(data.channelName);
            }
            fireChannelUserNotice("OWNER_CHANGED", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::NOT_OWNER:
            fireChannelNotice("NOT_OWNER", data.channelName);   // CHAT_NOT_OWNER_NOTICE
            break;
        case ChannelNotifyType::INVALID_NAME:
            fireChannelUserNotice("INVALID_NAME", data.channelName, 0, "", 0);
            break;
        case ChannelNotifyType::PLAYER_NOT_FOUND:
            fireChannelUserNotice("PLAYER_NOT_FOUND", data.channelName, 0, data.playerName, 0);
            break;
        case ChannelNotifyType::CHANNEL_OWNER:
            fireChannelUserNotice("CHANNEL_OWNER", data.channelName, 0, data.playerName, 0);
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_ON:
            fireChannelUserNotice("ANNOUNCEMENTS_ON", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_OFF:
            fireChannelUserNotice("ANNOUNCEMENTS_OFF", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::MODERATION_ON:
            fireChannelUserNotice("MODERATION_ON", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::MODERATION_OFF:
            fireChannelUserNotice("MODERATION_OFF", data.channelName, data.senderGuid, "", 0);
            break;
        case ChannelNotifyType::PLAYER_BANNED:
            fireChannelUserNotice("PLAYER_BANNED", data.channelName, data.senderGuid, "", data.targetGuid);
            break;
        case ChannelNotifyType::PLAYER_UNBANNED:
            fireChannelUserNotice("PLAYER_UNBANNED", data.channelName, data.senderGuid, "", data.targetGuid);
            break;
        case ChannelNotifyType::PLAYER_NOT_BANNED:
            fireChannelUserNotice("PLAYER_NOT_BANNED", data.channelName, 0, data.playerName, 0);
            break;
        case ChannelNotifyType::INVITE: {
            // Nothing from someone ignored; otherwise the notice and the
            // CHANNEL_INVITE_REQUEST popup, both naming the inviter.
            if (owner_.ignoreListGuidsRef().count(data.senderGuid)) break;
            fireChannelUserNotice("INVITE", data.channelName, data.senderGuid, "", 0);
            const std::string inviter = knownPlayerName(data.senderGuid);
            if (!inviter.empty() && owner_.addonEventCallbackRef())
                owner_.addonEventCallbackRef()("CHANNEL_INVITE_REQUEST", {data.channelName, inviter});
            break;
        }
        case ChannelNotifyType::INVITE_WRONG_FACTION:
            fireChannelUserNotice("INVITE_WRONG_FACTION", data.channelName, 0, "", 0);
            break;
        case ChannelNotifyType::WRONG_FACTION:
            fireChannelUserNotice("WRONG_FACTION", data.channelName, 0, "", 0);
            break;
        case ChannelNotifyType::NOT_MODERATED:
            fireChannelUserNotice("NOT_MODERATED", data.channelName, 0, "", 0);
            break;
        case ChannelNotifyType::PLAYER_INVITED:
            fireChannelUserNotice("PLAYER_INVITED", data.channelName, 0, data.playerName, 0);
            break;
        case ChannelNotifyType::PLAYER_INVITE_BANNED:
            fireChannelUserNotice("PLAYER_INVITE_BANNED", data.channelName, 0, data.playerName, 0);
            break;
        default:
            LOG_DEBUG("Channel notify type ", static_cast<int>(data.notifyType),
                     " for channel ", data.channelName);
            break;
    }
}

void ChatHandler::autoJoinDefaultChannels() {
    LOG_INFO("autoJoinDefaultChannels: general=", chatAutoJoin.general,
             " trade=", chatAutoJoin.trade, " localDefense=", chatAutoJoin.localDefense,
             " lfg=", chatAutoJoin.lfg, " local=", chatAutoJoin.local);

    // A new session: the server has us in no channel yet, whatever the list
    // remembered from the last character.
    joinedChannels_.clear();
    joinedChannelIds_.clear();
    ownedChannels_.clear();
    leavingChannels_.clear();
    suspendedChannels_.clear();

    if (isClassicLikeExpansion()) {
        // Plain names, as before; 1.12's join packet has no channel id.
        if (chatAutoJoin.general) joinChannel("General");
        if (chatAutoJoin.trade) joinChannel("Trade");
        if (chatAutoJoin.localDefense) joinChannel("LocalDefense");
        if (chatAutoJoin.lfg) joinChannel("LookingForGroup");
        if (chatAutoJoin.local) joinChannel("Local");
        if (addons::storedCVarValue("guildRecruitmentChannel", "0") != "0") {
            joinChannel("GuildRecruitment");
        }
        return;
    }

    // The zone channels are named after the zone, and at world entry the
    // zone is not known yet: SMSG_INIT_WORLD_STATES brings it a moment later.
    // Held until then (see servicePendingAutoJoin). The server will not do
    // this for us - UpdateLocalChannels skips a player who is still loading,
    // "the client handles it automatically after loading".
    autoJoinPending_ = true;
    autoJoinRequestedAt_ = core::appTimeSeconds();
    // What the zone said before this login, which may be the last
    // character's. Not trusted until it changes or a moment has passed for
    // this login's SMSG_INIT_WORLD_STATES to arrive: the server takes the
    // channel name as given, so a stale zone would put the player in
    // another zone's General.
    autoJoinZoneAtRequest_ = owner_.getWorldStateZoneId();
    servicePendingAutoJoin();
}

void ChatHandler::servicePendingAutoJoin() {
    if (!autoJoinPending_) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t zoneId = owner_.getWorldStateZoneId();
    if (zoneId != 0 && zoneId == autoJoinZoneAtRequest_ &&
        core::appTimeSeconds() - autoJoinRequestedAt_ < 3.0) {
        return;
    }
    if (zoneId == 0) {
        static bool warned = false;
        if (!warned && core::appTimeSeconds() - autoJoinRequestedAt_ > 15.0) {
            warned = true;
            LOG_WARNING("Chat: no zone after 15s in world - zone channels not joined yet");
        }
        return;
    }
    autoJoinPending_ = false;
    joinZoneChannels(zoneId);
}

void ChatHandler::joinZoneChannels(uint32_t zoneId) {
    // ChatChannels.dbc as 3.3.5 ships it, for when the file cannot be read.
    struct ZoneChannel { uint32_t id; uint32_t flags; std::string pattern; };
    std::vector<ZoneChannel> table;
    if (auto* am = owner_.services().assetManager; am && am->isInitialized()) {
        auto dbc = am->loadDBC("ChatChannels.dbc");
        // ID, Flags, FactionGroup, Name_lang[...] - enUS name at field 3.
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() > 3) {
            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                std::string pattern = dbc->getString(r, 3);
                if (!pattern.empty())
                    table.push_back({dbc->getUInt32(r, 0), dbc->getUInt32(r, 1), std::move(pattern)});
            }
        }
    }
    if (table.empty()) {
        table = {{1, 0x80003, "General - %s"}, {2, 0x3B, "Trade - %s"},
                 {22, 0x10003, "LocalDefense - %s"}, {23, 0x10004, "WorldDefense"},
                 {25, 0x20032, "GuildRecruitment - %s"}, {26, 0x40039, "LookingForGroup"}};
    }

    // AzerothCore's flag names (CHANNEL_DBC_FLAG_*).
    constexpr uint32_t kGlobal = 0x4, kTrade = 0x8, kCityOnly = 0x10,
                       kDefense = 0x10000, kGuildReq = 0x20000, kLfg = 0x40000;
    const std::string zoneName = owner_.getWhoAreaName(zoneId);
    const bool guildRecruitment =
        addons::storedCVarValue("guildRecruitmentChannel", "0") != "0";

    for (const auto& ch : table) {
        // Which of the client's switches covers this row. Trade and the
        // recruitment channel both carry the city flag, so the more specific
        // flags decide first.
        bool wanted;
        // The LookingForGroup channel takes only players in the dungeon
        // finder's queue (AzerothCore Channel.RestrictedLfg, the stock
        // "You must be in a LFG queue to join 'LookingForGroup'." refusal), so
        // asking on every zone change when not queued printed that refusal on
        // every teleport and zone line.
        if (ch.flags & kLfg)            wanted = chatAutoJoin.lfg &&
                                                 owner_.getLfgState() != LfgState::None;
        else if (ch.flags & kGuildReq)  wanted = guildRecruitment;
        else if (ch.flags & kTrade)     wanted = chatAutoJoin.trade;
        else if (ch.flags & kDefense)   wanted = chatAutoJoin.localDefense;  // Local and World
        else if (ch.id == 1)            wanted = chatAutoJoin.general;
        else                            wanted = false;
        if (!wanted) continue;

        // The name the server keys the channel on, built the way
        // Player::UpdateLocalChannels builds it, so this client lands in the
        // same "General - Elwynn Forest" as everyone else rather than a
        // custom channel called "General".
        std::string name = ch.pattern;
        if (!(ch.flags & kGlobal)) {
            const std::string ext = (ch.flags & kCityOnly) ? std::string("City") : zoneName;
            if (const size_t pos = name.find("%s"); pos != std::string::npos) {
                if (ext.empty()) continue;  // zone name unknown: wrong name is worse than none
                name.replace(pos, 2, ext);
            }
        }
        // Not ours to filter by zone: the server checks the city and guild
        // rules (CanJoinConstantChannelInZone) and ignores a join that fails
        // them, silently, which is what Trade outside a city should do.
        joinChannel(name, "", ch.id);
    }

    // A custom channel of this client's own, not one of the real client's.
    if (chatAutoJoin.local) joinChannel("Local");
}

void ChatHandler::addLocalChatMessage(const MessageChatData& msg) {
    chatHistory_.push_back(msg);
    chatHistory_.back().uid = ++chatUidCounter_;
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.pop_front();
    }
    logChatMessage(msg, "local");

    fireChatEvent(msg);
}

void ChatHandler::fireChatEvent(const MessageChatData& msg) {
    if (!owner_.addonEventCallbackRef()) return;
    std::string eventName = "CHAT_MSG_";
    eventName += getChatTypeString(msg.type);
    const Character* ac = owner_.getActiveCharacter();
    std::string senderName = msg.senderName.empty() && !msg.noSender
        ? (ac ? ac->name : std::string{}) : msg.senderName;
    char guidBuf[32];
    snprintf(guidBuf, sizeof(guidBuf), "0x%016llX",
             (unsigned long long)(msg.senderGuid != 0 ? msg.senderGuid : owner_.getPlayerGuid()));
    // The name, for the reason handleMessageChat gives: a number here is
    // printed as a language header in front of every line.
    {
        // Once per event name, for the reason the remote path above gives.
        static std::set<std::string> saidForLocal;
        if (saidForLocal.size() < 32 && saidForLocal.insert(eventName).second) {
            LOG_WARNING("Chat: fired ", eventName, " to the interface (local)"
                        " - said once per event name");
        }
    }
    // The channel's index, which the chat frame builds "CHANNEL"..arg8 out of
    // and looks up in ChatTypeInfo - a zero there is CHANNEL0, which no table
    // has, and the line is dropped before it reaches the window. This is the
    // one thing the callback that used to announce these as well did better,
    // and it is here now so nothing was lost when that went.
    const int channelIndex = getChannelIndex(msg.channelName);
    // arg2 is the name the interface prints, and for a whisper the player
    // sent that is who it went to, not who wrote it: CHAT_WHISPER_INFORM_GET
    // is "To %s: " and reads the same argument CHAT_WHISPER_GET does. The
    // other announce path already does this; this one did not, and this is
    // the path an outgoing whisper actually takes - the server's echo is
    // dropped in favour of the local row made on send - so every whisper the
    // player sent came back addressed to the player.
    const std::string& shownName =
        (msg.type == ChatType::WHISPER_INFORM && !msg.receiverName.empty())
            ? msg.receiverName
            : senderName;
    // The same channel arguments the server path sends: arg4 numbered, arg9
    // without the zone. arg9 was the full "General - Elwynn Forest", which
    // matches nothing in the chat window's list ("General"), and arg4 had no
    // number, so the player's own /1 lines were filtered out of the window.
    const size_t dash = msg.channelName.find(" - ");
    const std::string shortChannel = (dash == std::string::npos)
        ? msg.channelName : msg.channelName.substr(0, dash);
    const std::string numberedChannel = (channelIndex > 0)
        ? std::to_string(channelIndex) + ". " + msg.channelName : msg.channelName;
    const uint32_t zoneChannelId =
        (channelIndex > 0 && static_cast<size_t>(channelIndex) <= joinedChannelIds_.size())
            ? joinedChannelIds_[static_cast<size_t>(channelIndex) - 1] : 0;
    owner_.addonEventCallbackRef()(eventName, {
        msg.message, shownName,
        owner_.getLanguageName(static_cast<uint32_t>(msg.language)),
        numberedChannel, msg.receiverName, "", std::to_string(zoneChannelId),
        std::to_string(channelIndex), shortChannel, "0", "0", guidBuf
    });
}

void ChatHandler::addLocalChatLine(ChatType type, const std::string& message) {
    if (message.empty()) return;
    MessageChatData msg;
    msg.type = type;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.message = message;
    addLocalChatMessage(msg);
}

void ChatHandler::addSystemChatMessage(const std::string& message) {
    addLocalChatLine(ChatType::SYSTEM, message);
}

// /afk and /dnd as Wow.exe has them (0x50D170, 0x6DC640, 0x6D52D0): the
// client says the change - MARKED_AFK_MESSAGE ("You are now Away: %s", with
// DEFAULT_AFK_MESSAGE for no reason), CLEARED_AFK, MARKED_DND, CLEARED_DND,
// plain chat lines - and sends the chat type with the reason, empty to
// clear. Not a toggle: a reason while already away only changes it.
void ChatHandler::sendAwayPacket(ChatType type, const std::string& message) {
    if (!owner_.getSocket()) return;
    auto packet = MessageChatPacket::build(type, ChatLanguage::UNIVERSAL, message, "");
    owner_.getSocket()->send(packet);
}

void ChatHandler::clearAfk() {
    if (!owner_.afkStatusRef()) return;
    owner_.afkStatusRef() = false;
    owner_.afkMessageRef().clear();
    afkAutoRepliedSenders_.clear();
    owner_.addGlobalStringChatLine("CLEARED_AFK", "You are no longer Away.");
    sendAwayPacket(ChatType::AFK, "");
}

void ChatHandler::toggleAfk(const std::string& message) {
    if (owner_.afkStatusRef() && message.empty()) {
        clearAfk();
        return;
    }
    std::string reason = message;
    if (reason.empty()) {
        reason = globalString(owner_.services().assetManager, "DEFAULT_AFK_MESSAGE");
        if (reason.empty()) reason = "Away";
    }
    if (!owner_.afkStatusRef()) {
        if (owner_.dndStatusRef()) {
            owner_.dndStatusRef() = false;
            owner_.dndMessageRef().clear();
            owner_.addGlobalStringChatLine("CLEARED_DND", "You are no longer marked Busy.");
        }
        owner_.addGlobalStringChatLine("MARKED_AFK_MESSAGE", "You are now Away: %s", {reason});
    }
    owner_.afkStatusRef() = true;
    owner_.afkMessageRef() = reason;
    sendAwayPacket(ChatType::AFK, reason);
    LOG_INFO("AFK status: ", owner_.afkStatusRef(), ", message: ", reason);
}

void ChatHandler::toggleDnd(const std::string& message) {
    std::string reason = message;
    if (owner_.dndStatusRef() && reason.empty()) {
        owner_.dndStatusRef() = false;
        owner_.dndMessageRef().clear();
        afkAutoRepliedSenders_.clear();
        owner_.addGlobalStringChatLine("CLEARED_DND", "You are no longer marked Busy.");
    } else {
        if (reason.empty()) {
            reason = globalString(owner_.services().assetManager, "DEFAULT_DND_MESSAGE");
            if (reason.empty()) reason = "Busy";
        }
        owner_.dndStatusRef() = true;
        owner_.dndMessageRef() = reason;
        owner_.addGlobalStringChatLine("MARKED_DND", "You are now Busy: %s.", {reason});
    }
    sendAwayPacket(ChatType::DND, owner_.dndStatusRef() ? reason : std::string());
    LOG_INFO("DND status: ", owner_.dndStatusRef(), ", message: ", reason);
}

void ChatHandler::replyToLastWhisper(const std::string& message) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot send whisper: not in world or not connected");
        return;
    }

    if (owner_.lastWhisperSenderRef().empty()) {
        owner_.displayGameMessage("ERR_NO_REPLY_TARGET", "You have nobody to reply to yet.");
        return;
    }

    if (message.empty()) {
        owner_.raiseUiError("You must specify a message to send.");
        return;
    }

    // Send whisper using the standard message chat function
    sendChatMessage(ChatType::WHISPER, message, owner_.lastWhisperSenderRef());
    LOG_INFO("Replied to ", owner_.lastWhisperSenderRef(), ": ", message);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void ChatHandler::handleChannelList(network::Packet& packet) {
    // A channel type byte comes first - Channel::List writes `uint8(1)` before
    // the name - and this read the name straight from it. The stray byte does
    // not shift anything, because the name's terminator ends the string either
    // way, but it rides along on the front of every use of it: the roster was
    // filed under "\x01General" while getChannelRoster is asked for "General",
    // so the member count answered zero for every channel.
    //
    // Recognised rather than assumed, since not every realm's build sends it: a
    // channel name always begins with a printable character, so a control byte
    // here is the type and nothing else can be.
    if (!packet.hasRemaining(1)) return;
    // The byte decides where the list goes (Wow.exe 0x505DC0): zero prints
    // it in chat (CHAT_MSG_CHANNEL_LIST, 0x5029C0), anything else only
    // updates the channel panel's roster. AzerothCore always writes 1.
    uint8_t listType = 1;
    if (!packet.getData().empty() &&
        packet.getReadPos() < packet.getData().size() &&
        packet.getData()[packet.getReadPos()] < 0x20) {
        listType = packet.readUInt8();  // channel type
    }
    std::string chanName = packet.readString();
    if (!packet.hasRemaining(5)) return;
    /*uint8_t chanFlags =*/ packet.readUInt8();
    uint32_t memberCount = packet.readUInt32();
    memberCount = std::min(memberCount, 200u);

    // The server's own member flags. What was here read 0x01 as moderator and
    // 0x02 as muted, so the channel's owner was announced as its moderator and
    // its moderators as muted.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;

    std::vector<ChannelMember> roster;
    roster.reserve(memberCount);
    std::vector<std::string> listLines;
    std::string listLine;
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(9)) break;
        ChannelMember m;
        m.guid = packet.readUInt64();
        const uint8_t memberFlags = packet.readUInt8();
        m.owner     = (memberFlags & kOwner) != 0;
        m.moderator = (memberFlags & kModerator) != 0;
        m.muted     = (memberFlags & kMuted) != 0;

        auto entity = owner_.getEntityManager().getEntity(m.guid);
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) m.name = player->getName();
        }
        if (m.name.empty()) m.name = owner_.lookupName(m.guid);
        if (m.name.empty()) m.name = "(unknown)";

        // Each name with its mark - "*" owner, "@" moderator, "#" neither
        // and not voiced - joined by ", ", a new line past 255 characters.
        const char* mark = m.owner ? "*" : m.moderator ? "@" : (memberFlags & 0x04) ? "" : "#";
        std::string entry = std::string(listLine.empty() ? "" : ", ") + mark + m.name;
        if (listLine.size() + entry.size() >= 0x100) {
            listLines.push_back(listLine);
            listLine = std::string(mark) + m.name;
        } else {
            listLine += entry;
        }
        roster.push_back(std::move(m));
    }

    if (!listLine.empty()) listLines.push_back(listLine);
    if (listType == 0) {
        for (const auto& line : listLines) {
            MessageChatData msg;
            msg.type = ChatType::CHANNEL_LIST;
            msg.language = ChatLanguage::UNIVERSAL;
            msg.channelName = chanName;
            msg.message = line;
            msg.noSender = true;
            addLocalChatMessage(msg);
        }
    }
    channelRosters_[chanName] = std::move(roster);
    // Both, because the panel redraws its member list on the first and its
    // member count on the second, and it registers for each separately.
    //
    // By display index, not by name. ChannelRoster_Update and
    // ChannelList_CountUpdate both take a position in the channel list -
    // _G["ChannelButton"..id] is how the second finds its row, and the first is
    // called elsewhere with GetSelectedDisplayChannel(), which is an index. A
    // name built "ChannelButtonGeneral", which is nothing, so neither the
    // roster nor the count ever redrew.
    const int displayIndex = getChannelIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx});
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE",
                              {idx, std::to_string(channelRosters_[chanName].size())});
    }
}


/// SMSG_USERLIST_ADD / _REMOVE / _UPDATE - one member of a channel changing.
///
/// The full roster comes on SMSG_CHANNEL_LIST and is asked for; these arrive
/// unasked as people join, leave and are promoted, and without them the roster
/// is only ever as fresh as the last time somebody opened the list.
///
/// Two readers rather than one with a branch, because the three layouts are
/// not quite the same - remove carries no player flags - and a handler that
/// reads its fields under an `if` cannot be checked against the server by
/// tools/packet_layout_check.py, which reads the run of widths a body asks
/// for. It flagged the first version of this, correctly by its own lights.
///
///   ADD, UPDATE  guid(8) playerFlags(1) channelFlags(1) count(4) name
///   REMOVE       guid(8)                channelFlags(1) count(4) name
void ChatHandler::applyUserlistChange(const std::string& chanName, uint64_t guid,
                                      uint8_t memberFlags, bool removing) {
    if (chanName.empty()) return;
    // The same bits the full list reads, and the same trap: 0x01 is the owner
    // and 0x02 the moderator, not the other way round.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;
    auto& roster = channelRosters_[chanName];
    auto it = std::find_if(roster.begin(), roster.end(),
                           [&](const ChannelMember& m) { return m.guid == guid; });

    if (removing) {
        if (it != roster.end()) roster.erase(it);
    } else {
        if (it == roster.end()) {
            ChannelMember m;
            m.guid = guid;
            m.name = owner_.lookupName(guid);
            if (m.name.empty()) m.name = "(unknown)";
            roster.push_back(std::move(m));
            it = roster.end() - 1;
        }
        it->owner     = (memberFlags & kOwner) != 0;
        it->moderator = (memberFlags & kModerator) != 0;
        it->muted     = (memberFlags & kMuted) != 0;
    }

    // By display index, for the reason spelled out in handleChannelList: both
    // of these take a position in the channel list, not a name.
    const int displayIndex = getChannelIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx});
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE",
                              {idx, std::to_string(roster.size())});
    }
}

void ChatHandler::handleUserlistAdd(network::Packet& packet) {
    if (!packet.hasRemaining(14)) return;
    const uint64_t guid = packet.readUInt64();
    const uint8_t memberFlags = packet.readUInt8();
    packet.readUInt8();    // the channel's own flags
    packet.readUInt32();   // how many are in it now
    applyUserlistChange(packet.readString(), guid, memberFlags, /*removing=*/false);
}

void ChatHandler::handleUserlistRemove(network::Packet& packet) {
    if (!packet.hasRemaining(13)) return;
    const uint64_t guid = packet.readUInt64();
    packet.readUInt8();    // the channel's own flags
    packet.readUInt32();   // how many are in it now
    applyUserlistChange(packet.readString(), guid, 0, /*removing=*/true);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void ChatHandler::updateGmTicket(const std::string& text) {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // CMSG_GMTICKET_UPDATETEXT (WotLK 3.3.5a): the new text and nothing else.
    // Creating carries the player's position because a new ticket records where
    // it was raised; editing one does not move it.
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_UPDATETEXT));
    pkt.writeString(text);
    owner_.getSocket()->send(pkt);
    LOG_INFO("Updated GM ticket text");
}

void ChatHandler::submitGmTicket(const std::string& text) {
    if (!owner_.isInWorld()) return;

    // CMSG_GMTICKET_CREATE:
    //   uint32  mapId
    //   float   x, y, z          (server coords, no facing)
    //   string  message
    //   uint32  needResponse
    //   uint8   needMoreHelp
    //   uint32  count            - how many chat-log timestamps follow
    //   uint32  decompressedSize - the zlib'd chat log after it, if any
    //
    // The comment above these writes described a layout in the other order,
    // opening with the text and carrying a facing the server does not read,
    // and the writes matched the comment rather than the wire. So the map id
    // was taken out of the middle of the ticket text, the position out of
    // whatever followed it, and the message read from four bytes of a float -
    // and the trailing fields were missing entirely, which runs the read off
    // the end of the buffer. AzerothCore catches that, logs, and drops the
    // packet: the ticket was never created and nothing said so.
    //
    // One shape on every expansion, and it is the long one.
    //
    // The prefix through the message is what all three read: a 1.12 and a
    // 2.4.3 server take mapId, x, y, z and the message and stop there. The
    // four fields after it are 3.x, and the asymmetry is what decides this -
    // a WotLK server that does not get them reads off the end of the buffer
    // and drops the whole request, while a vanilla or TBC one simply leaves
    // them unread. Trailing bytes nobody reads cost nothing; missing ones
    // cost the ticket. So there is nothing to gate on the expansion here, and
    // a gate would be a second thing to keep right.
    //
    // The chat log is a client-side transcript this client does not keep, so
    // both of its lengths go across as zero, which is how the real client
    // sends a ticket raised outside a conversation.
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_CREATE));
    pkt.writeUInt32(owner_.currentMapIdRef());
    pkt.writeFloat(owner_.movementInfoRef().x);
    pkt.writeFloat(owner_.movementInfoRef().y);
    pkt.writeFloat(owner_.movementInfoRef().z);
    pkt.writeString(text);
    pkt.writeUInt32(1);  // needResponse = yes
    pkt.writeUInt8(0);   // needMoreHelp = no
    pkt.writeUInt32(0);  // no chat-log timestamps
    pkt.writeUInt32(0);  // and so no compressed chat log
    owner_.getSocket()->send(pkt);
    LOG_INFO("Submitted GM ticket: '", text, "'");
}

void ChatHandler::handleMotd(network::Packet& packet) {
    LOG_INFO("Handling SMSG_MOTD");

    MotdData data;
    if (!MotdParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MOTD");
        return;
    }

    if (!data.isEmpty()) {
        LOG_INFO("========================================");
        LOG_INFO("   MESSAGE OF THE DAY");
        LOG_INFO("========================================");
        // Each line as a system line of its own, with no prefix and nothing
        // after (Wow.exe 0x551660).
        for (const auto& line : data.lines) {
            LOG_INFO(line);
            addSystemChatMessage(line);
        }
        LOG_INFO("========================================");
    }
}

// A defense line (Wow.exe 0x50C3C0, 0x50C850): a channel line on each joined
// defense channel - WorldDefense always, LocalDefense only for the zone the
// player is in.
void ChatHandler::postDefenseLine(uint32_t areaId, const std::string& text) {
    for (size_t i = 0; i < joinedChannels_.size(); ++i) {
        const uint32_t id = i < joinedChannelIds_.size() ? joinedChannelIds_[i] : 0;
        const bool world = id == 23 || joinedChannels_[i].rfind("WorldDefense", 0) == 0;
        const bool local = id == 22 || joinedChannels_[i].rfind("LocalDefense", 0) == 0;
        if (!world && !local) continue;
        if (local && !world && areaId != owner_.getWorldStateZoneId() &&
            areaId != owner_.getWorldStateAreaId())
            continue;
        MessageChatData line;
        line.type = ChatType::CHANNEL;
        line.language = ChatLanguage::UNIVERSAL;
        line.channelName = joinedChannels_[i];
        line.message = text;
        line.noSender = true;
        addLocalChatMessage(line);
    }
}

} // namespace game
} // namespace wowee
