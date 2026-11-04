#include "ui/world_ui_rules.hpp"
#include "game/chat_handler.hpp"
#include "game/global_strings.hpp"

#include <set>
#include "addons/lua_api_registrations.hpp"
#include "game/chat_filters.hpp"
#include "game/chat_rules.hpp"
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
#include <regex>
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
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet, false);
    };
    // The same handler with the GM flag (0x50BE70's second argument): the
    // ordinary types carry the GM's name before the rest.
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet, true);
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
    sendChatMessage(type, message, target, -1);
}

namespace {
bool sameTextIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

uint32_t chatNowMs() {
    // Never zero: zero is the empty slot of a throttle ring.
    return static_cast<uint32_t>(core::appTimeSeconds() * 1000.0) | 1u;
}
}  // namespace

uint32_t ChatHandler::defaultLanguage() const {
    // ChrRaces.dbc BaseLanguage (0x6D5E70): Common for the Alliance races,
    // Orcish for the Horde's.
    const uint8_t race = owner_.getPlayerRace();
    const bool horde = race == 2 || race == 5 || race == 6 || race == 8 || race == 10;
    return horde ? 1u : 7u;
}

// SendChatMessage(text [,type] [,language] [,target]) as 0x50D170 checks it.
// The return is a Lua error's text; empty when the call simply ends.
std::string ChatHandler::sendChatFromInterface(const std::string& text, const char* typeWord,
                                               const char* languageWord, const char* target) {
    if (owner_.getState() != WorldState::IN_WORLD) return {};
    uint8_t type = 0x01;
    if (typeWord) {
        type = chat_rules::chatTypeFromWord(typeWord);
        if (type == 0) return "SendChatMessage(): Unknown chat type";
    }
    if (text.empty() && type != 0x17 && type != 0x18) return {};
    if (!chat_rules::isUtf8Text(text)) return "SendChatMessage(): Chat message must be UTF-8 text";
    int32_t language = -1;
    if (languageWord) {
        uint32_t id = 0;
        bool found = false;
        for (uint32_t i = 1; i < 64 && !found; ++i) {
            const std::string& name = owner_.getLanguageName(i);
            if (!name.empty() && sameTextIgnoringCase(name, languageWord)) { id = i; found = true; }
        }
        if (!found) return "SendChatMessage(): Unknown language";
        if (languageComprehension(id) == 0) {
            owner_.displayGameMessage("ERR_CANT_SPEAK_LANGAGE", "You cannot speak that language.");
            return {};
        }
        language = static_cast<int32_t>(id);
    }
    std::string tgt = target ? target : "";
    const auto& party = owner_.getPartyData();
    const uint64_t me = owner_.getPlayerGuid();
    const bool inRaid = owner_.isInGroup() && party.groupType == 1;
    const bool leader = owner_.isInGroup() && party.leaderGuid == me;
    switch (type) {
        case 0x07:
            if (tgt.empty()) return "SendChatMessage(): Whisper message missing target player!";
            break;
        case 0x02:
            if (!owner_.isInGroup()) {
                owner_.displayGameMessage("ERR_NOT_IN_GROUP", "You aren't in a party.");
                return {};
            }
            if (leader) type = 0x33;   // PARTY_LEADER
            break;
        case 0x28:
            if (!inRaid) {
                if (!owner_.isInGroup()) {
                    owner_.displayGameMessage("ERR_NOT_IN_GROUP", "You aren't in a party.");
                    return {};
                }
            } else {
                bool assistant = leader;
                for (const auto& m : party.members)
                    if (m.guid == me && (m.flags & 0x01)) assistant = true;
                if (!assistant) {
                    owner_.displayGameMessage("ERR_NOT_LEADER", "You are not the party leader.");
                    return {};
                }
            }
            break;
        case 0x03: case 0x2C:
            if (!inRaid) {
                if (type == 0x03) owner_.displayGameMessage("ERR_NOT_IN_RAID", "You are not in a raid group");
                else owner_.displayGameMessage("ERR_NOT_IN_BATTLEGROUND", "You aren't in a Battleground.");
                return {};
            }
            if (leader) type = (type == 0x03) ? 0x27 : 0x2D;   // RAID_LEADER, BATTLEGROUND_LEADER
            break;
        case 0x11: {
            if (tgt.empty()) return "SendChatMessage(): Channel send missing channel number";
            const int number = std::atoi(tgt.c_str());
            const std::string name = getChannelByIndex(number);
            // A number with no channel, or one the server has suspended, ends
            // the call without a word.
            if (name.empty() || suspendedChannels_.count(name)) return {};
            tgt = name;
            break;
        }
        default: break;
    }
    // Dead or a ghost: only the group, guild, whisper and channel kinds go out.
    const bool talksWhileDead = type == 0x07 || type == 0x02 || type == 0x33 || type == 0x03 ||
                                type == 0x27 || type == 0x28 || type == 0x04 || type == 0x05 ||
                                type == 0x11 || type == 0x18 || type == 0x17 || type == 0x2C ||
                                type == 0x2D;
    if (!talksWhileDead && (owner_.isPlayerDead() || owner_.isPlayerGhost())) {
        owner_.displayGameMessage("ERR_CHAT_WHILE_DEAD", "You can't chat when you're dead!");
        return {};
    }
    std::string message = text;
    // The escape check is the last word before the packet; done here so its
    // error reaches Lua.
    {
        std::string probe = chat_rules::expandChatTokens(message, "", "");
        chat_rules::cutAtLineBreak(probe);
        if (!chat_rules::checkOutgoingEscapes(probe))
            return "SendChatMessage(): Invalid escape code in chat message";
    }
    sendChatMessage(static_cast<ChatType>(type), message, tgt, language);
    return {};
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target,
                                  int32_t language) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }
    // Away and busy are chat types of their own, with the client saying the
    // change (Wow.exe 0x50D170, 0x6DC640, 0x6D52D0). Any other line clears
    // away when autoClearAFK is on.
    if (type == ChatType::AFK) { toggleAfk(message); return; }
    if (owner_.afkStatusRef() && addons::storedCVarValue("autoClearAFK", "1") != "0") clearAfk();
    if (type == ChatType::DND) { toggleDnd(message); return; }
    if (message.empty()) return;

    // %t / %n / %f, 255 bytes (0x577D00); the first line break ends the line
    // (0x7E1640); the escape codes are checked (a colour with no |r is cut).
    std::string targetName, focusName;
    if (const uint64_t t = owner_.getTargetGuid()) targetName = owner_.lookupName(t);
    if (const uint64_t f = owner_.getFocusGuid()) focusName = owner_.lookupName(f);
    std::string text = chat_rules::expandChatTokens(
        message, targetName, focusName,
        globalString(owner_.services().assetManager, "TARGET_TOKEN_NOT_FOUND"),
        globalString(owner_.services().assetManager, "FOCUS_TOKEN_NOT_FOUND"));
    chat_rules::cutAtLineBreak(text);
    if (!chat_rules::checkOutgoingEscapes(text)) {
        LOG_WARNING("Chat: not sent - an invalid escape code");
        return;
    }
    if (text.empty()) return;

    const uint32_t lang = language >= 0 ? static_cast<uint32_t>(language) : defaultLanguage();

    // The client's throttles: whispers to people who have not whispered
    // lately, and each channel - ten in ten seconds, the rest held.
    if (type == ChatType::WHISPER || type == ChatType::CHANNEL) {
        flushHeldSends();
        const uint32_t now = chatNowMs();
        if (type == ChatType::WHISPER) {
            if (!isRecentWhisperer(target)) {
                if (!heldSends_.empty() || sendThrottle_.whisperHeld(now)) {
                    heldSends_.push_back({type, lang, target, text});
                    return;
                }
                sendThrottle_.recordWhisper(now);
            }
        } else {
            if (!heldSends_.empty() || sendThrottle_.channelHeld(target, now)) {
                heldSends_.push_back({type, lang, target, text});
                return;
            }
            sendThrottle_.recordChannel(target, now);
        }
    }
    sendChatPacket(type, lang, text, target);
}

bool ChatHandler::isRecentWhisperer(const std::string& name) const {
    for (const uint64_t guid : recentWhisperers_.guids()) {
        if (guid == 0) continue;
        const std::string known = knownPlayerName(guid);
        if (!known.empty() && sameTextIgnoringCase(known, name)) return true;
    }
    return false;
}

void ChatHandler::flushHeldSends() {
    // 0x5098B0: each held line in order, the ones the throttle now allows.
    if (heldSends_.empty() || owner_.getState() != WorldState::IN_WORLD) return;
    const uint32_t now = chatNowMs();
    for (auto it = heldSends_.begin(); it != heldSends_.end();) {
        bool go = false;
        if (it->type == ChatType::WHISPER) {
            if (isRecentWhisperer(it->target)) go = true;
            else if (!sendThrottle_.whisperHeld(now)) { sendThrottle_.recordWhisper(now); go = true; }
        } else if (!sendThrottle_.channelHeld(it->target, now)) {
            sendThrottle_.recordChannel(it->target, now);
            go = true;
        }
        if (!go) { ++it; continue; }
        sendChatPacket(it->type, it->language, it->message, it->target);
        it = heldSends_.erase(it);
    }
}

void ChatHandler::sendChatPacket(ChatType type, uint32_t language, const std::string& message,
                                 const std::string& target) {
    if (!owner_.getSocket()) return;
    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");
    auto packet = MessageChatPacket::build(type, static_cast<ChatLanguage>(language), message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);
    // No local echo: the stock client shows its own line only when the server
    // sends it back.
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

void ChatHandler::handleMessageChat(network::Packet& packet, bool gmMessage) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG RX SMSG_MESSAGECHAT bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }

    MessageChatData data;
    const bool parsed = gmMessage ? MessageChatParser::parse(packet, data, true)
                                  : owner_.getPacketParsers()->parseMessageChat(packet, data);
    if (!parsed) {
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

    if (!acceptIncomingChat(data)) return;
    queueChatLine(std::move(data));
}

// What Wow.exe's SMSG_MESSAGECHAT handler (0x50BE70) decides before a line
// reaches the chat frame: who whispered last, and the ignore list.
bool ChatHandler::acceptIncomingChat(const MessageChatData& data) {
    const auto type = static_cast<uint8_t>(data.type);
    auto ignored = [this](uint64_t guid) {
        return guid != 0 && owner_.ignoreListGuidsRef().count(guid) != 0;
    };
    // Monster and boss lines: dropped when the player they name is ignored.
    if (isMonsterChatType(data.type) || type == 0x2F) return !ignored(data.receiverGuid);
    // Battleground system and achievement lines are not checked.
    if ((type >= 0x24 && type <= 0x26) || type == 0x30 || type == 0x31) return true;

    if (data.type == ChatType::WHISPER) {
        recentWhisperers_.note(data.senderGuid);
        // A whisper to yourself still names you as the one to reply to.
        if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0 &&
            !data.senderName.empty()) {
            owner_.lastWhisperSenderRef() = data.senderName;
        }
    }
    // Someone ignored: nothing they say, unless they are a GM. A whisper
    // tells the server so (CMSG_CHAT_IGNORED, guid and a zero byte), which
    // answers them with "X is ignoring you". Only the lines about the player
    // themselves - the whisper they sent, the IGNORED and RESTRICTED notices -
    // still show.
    if (ignored(data.senderGuid) && !(data.chatTag & 0x04)) {
        if (data.type == ChatType::WHISPER && data.language != ChatLanguage::ADDON &&
            owner_.getSocket()) {
            network::Packet reply(wireOpcode(Opcode::CMSG_CHAT_IGNORED));
            reply.writeUInt64(data.senderGuid);
            reply.writeUInt8(0);
            owner_.getSocket()->send(reply);
        }
        if (data.type != ChatType::WHISPER_INFORM && data.type != ChatType::IGNORED &&
            data.type != ChatType::RESTRICTED) {
            return false;
        }
    }
    return true;
}

namespace {
bool isPlayerOrPetGuid(uint64_t guid) {
    return chat_rules::isPlayerGuid(guid) || chat_rules::isPetGuid(guid);
}
}  // namespace

void ChatHandler::resolveChatNames(MessageChatData& data) const {
    if (data.senderName.empty() && isPlayerOrPetGuid(data.senderGuid))
        data.senderName = knownPlayerName(data.senderGuid);
    if (data.receiverName.empty() && isPlayerOrPetGuid(data.receiverGuid))
        data.receiverName = knownPlayerName(data.receiverGuid);
}

uint64_t ChatHandler::missingChatName(const MessageChatData& data) const {
    if (data.senderName.empty() && isPlayerOrPetGuid(data.senderGuid)) return data.senderGuid;
    if (data.receiverName.empty() && isPlayerOrPetGuid(data.receiverGuid)) return data.receiverGuid;
    return 0;
}

// The chat frame's order is the server's (0x509DD0 -> 0x502D00): a line
// whose sender's or receiver's name is not yet known waits for the name
// query, and while anything waits every later line queues behind it, so a
// reply never prints above the line it answers.
void ChatHandler::queueChatLine(MessageChatData data) {
    resolveChatNames(data);
    const uint64_t missing = missingChatName(data);
    if (missing != 0) owner_.queryPlayerName(missing);
    if (chatAwaitingName_.empty() && missing == 0) {
        finishChatLine(std::move(data));
        return;
    }
    chatAwaitingName_.push_back({std::move(data), missing,
                                 core::appTimeSeconds() + kNameWaitSeconds});
    pumpChatQueue();
}

void ChatHandler::pumpChatQueue() {
    const double now = core::appTimeSeconds();
    while (!chatAwaitingName_.empty()) {
        ChatAwaitingName& head = chatAwaitingName_.front();
        resolveChatNames(head.data);
        const uint64_t missing = missingChatName(head.data);
        if (missing != 0 && now < head.deadline) {
            if (missing != head.guid) {
                head.guid = missing;
                owner_.queryPlayerName(missing);
            }
            return;
        }
        if (missing != 0) {
            LOG_WARNING("Chat: no name for ", formatChatLogGuid(missing),
                        " within ", kNameWaitSeconds, "s - showing the line without one");
        }
        MessageChatData ready = std::move(head.data);
        chatAwaitingName_.pop_front();
        finishChatLine(std::move(ready));
    }
}

void ChatHandler::ensureLanguageWords() {
    if (languageWordsLoaded_) return;
    auto* am = owner_.services().assetManager;
    if (!am || !am->isInitialized()) return;
    languageWordsLoaded_ = true;
    // ID, language, word (0x50EDD0 keys the words by language and length).
    if (auto dbc = am->loadDBC("LanguageWords.dbc"); dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            languageWords_.add(dbc->getUInt32(i, 1), dbc->getString(i, 2));
    }
    if (languageWords_.empty()) LOG_WARNING("LanguageWords.dbc: no words - other languages print as they arrive");
}

uint32_t ChatHandler::languageComprehension(uint32_t language) const {
    // 0x6E0640: the skill of the language's skill line, base plus bonus. No
    // skill, no understanding.
    const uint32_t line = chat_rules::languageSkillLine(language);
    if (line == 0) return 0;
    const auto& skills = owner_.getPlayerSkills();
    auto it = skills.find(line);
    return it == skills.end() ? 0u : it->second.effectiveValue();
}

bool ChatHandler::isSpamLine(const std::string& text) {
    // SpamMessages.dbc's patterns (0x509BD0), remembered per text as the
    // client does, since the same line comes round again.
    if (!spamPatternsLoaded_) {
        auto* am = owner_.services().assetManager;
        if (!am || !am->isInitialized()) return false;
        spamPatternsLoaded_ = true;
        if (auto dbc = am->loadDBC("SpamMessages.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const std::string pattern = dbc->getString(i, 1);
                if (pattern.empty()) continue;
                try {
                    spamPatterns_.emplace_back(pattern, std::regex::ECMAScript | std::regex::icase |
                                                            std::regex::optimize);
                } catch (const std::regex_error&) {
                    LOG_DEBUG("SpamMessages.dbc: pattern ", i, " is not one std::regex reads");
                }
            }
        }
    }
    if (spamPatterns_.empty() || text.empty()) return false;
    if (auto it = spamVerdicts_.find(text); it != spamVerdicts_.end()) return it->second;
    bool spam = false;
    for (const auto& re : spamPatterns_) {
        if (std::regex_search(text, re)) { spam = true; break; }
    }
    if (spamVerdicts_.size() > 2048) spamVerdicts_.clear();
    spamVerdicts_.emplace(text, spam);
    return spam;
}

void ChatHandler::playTalkAnimation(const MessageChatData& data) {
    // 0x509DD0 -> 0x73AD70: a say, yell or party line from a unit in the
    // world moves its speaker - Emotes.dbc's talk, question, exclamation,
    // shout or laugh row (the first row with flag 0x8, 0x10, 0x20, 0x40 or
    // 0x100: emotes 1, 6, 5, 22, 11). Not for a unit feigning death.
    if (!owner_.emoteAnimCallbackRef() || data.senderGuid == 0) return;
    auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
    auto unit = entity ? std::dynamic_pointer_cast<Unit>(entity) : nullptr;
    if (!unit) return;
    if (unit->getUnitFlags() & 0x20000000u) return;
    // Not reproduced for the player's own model, whose animation is driven by
    // its own controller, nor for a mounted unit, whose rider pose a one-shot
    // would replace (0x73AD70's own movement and descriptor checks).
    if (data.senderGuid == owner_.getPlayerGuid() || unit->getMountDisplayId() != 0) return;
    if (laughWords_.empty()) {
        for (int i = 1; i < 32; ++i) {
            std::string w = globalString(owner_.services().assetManager,
                                         "LAUGH_WORD" + std::to_string(i));
            if (w.empty()) break;
            laughWords_.push_back(std::move(w));
        }
        if (laughWords_.empty()) laughWords_.push_back("");  // asked once
    }
    const int kind = chat_rules::talkAnimationKind(static_cast<uint8_t>(data.type), data.message,
                                                   laughWords_);
    static constexpr uint32_t kEmoteForKind[5] = {1, 6, 5, 22, 11};
    if (kind < 0 || kind > 4) return;
    const uint32_t anim = rendering::AnimationController::getEmoteAnimByEmotesId(kEmoteForKind[kind]);
    if (anim != 0) owner_.emoteAnimCallbackRef()(data.senderGuid, anim, false);
}

// The rest of 0x509DD0, once the names are in: addon traffic, the UTF-8
// check, $-tokens, the talking animation, the language, the spam filter, the
// line id, and CHAT_MSG_<type> with the client's arguments (0x4FDBC0).
void ChatHandler::finishChatLine(MessageChatData data) {
    const auto type = static_cast<uint8_t>(data.type);

    // Addon traffic is language -1 and nothing else: CHAT_MSG_ADDON with the
    // text split at its first tab and the client's word for the distribution.
    if (data.language == ChatLanguage::ADDON) {
        if (owner_.addonEventCallbackRef()) {
            auto [prefix, body] = chat_rules::splitAddonMessage(data.message);
            owner_.addonEventCallbackRef()(
                "CHAT_MSG_ADDON", {prefix, body, chat_rules::addonDistribution(type), data.senderName});
        }
        return;
    }
    if (!chat_rules::isUtf8Text(data.message)) {
        LOG_DEBUG("Chat: dropped a line that is not UTF-8 text");
        return;
    }

    if (data.type == ChatType::ACHIEVEMENT || data.type == ChatType::GUILD_ACHIEVEMENT) {
        if (data.achievementId != 0) {
            owner_.ensureAchievementNamesLoaded();
            std::string achievementName = owner_.getAchievementName(data.achievementId);
            if (achievementName.empty())
                achievementName = "Achievement #" + std::to_string(data.achievementId);
            // 0x61BFF0: the receiver (the one who earned it) as the guid,
            // completed, today's date, no criteria.
            const std::time_t now = std::time(nullptr);
            const std::tm tmNow = core::localTime(now);
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "|cffffff00|Hachievement:%u:%016llX:%d:%d:%d:%d:%u:%u:%u:%u|h[%s]|h|r",
                          data.achievementId,
                          static_cast<unsigned long long>(data.receiverGuid ? data.receiverGuid : data.senderGuid),
                          1, tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_year - 100, 0u, 0u, 0u, 0u,
                          achievementName.c_str());
            const std::string link = buf;
            for (size_t pos = data.message.find("$a"); pos != std::string::npos;
                 pos = data.message.find("$a", pos)) {
                data.message.replace(pos, 2, link);
                pos += link.size();
            }
        }
    }
    // The $-tokens of the lines the quest-text parser runs over (0x579590).
    // A player typing "$N" in say means the two characters.
    if (chat_rules::resolvesTextTokens(type)) {
        data.message = resolveTextTokens(data.message, monsterLineSubject(data));
    }

    playTalkAnimation(data);

    if (chat_rules::languageIgnored(type)) data.language = ChatLanguage::UNIVERSAL;

    // An empty say or yell is the stock emote "says something unintelligible."
    // (CHAT_YELL_UNKNOWN has a _FEMALE form); an empty emote "makes some
    // strange gestures."
    if (data.message.empty()) {
        const char* key = chat_rules::unknownSpeechKey(type);
        if (key) {
            std::string text;
            if (data.type == ChatType::YELL && owner_.lookupPlayerGender(data.senderGuid) == 1)
                text = globalString(owner_.services().assetManager, std::string(key) + "_FEMALE");
            if (text.empty()) text = globalString(owner_.services().assetManager, key);
            data.message = text;
            data.type = ChatType::EMOTE;
        }
    }

    // Quest links coloured for the reader's level.
    if (data.message.find("|Hquest:") != std::string::npos)
        data.message = chat_rules::recolorQuestLinks(data.message,
                                                     static_cast<int>(owner_.getPlayerLevel()));

    // A language the reader does not know is scrambled with its own words. A
    // GM reads everything and sees no language.
    const bool readerIsGm = owner_.isPlayerGmFlagged();
    if (readerIsGm) {
        data.language = ChatLanguage::UNIVERSAL;
    } else if (data.language != ChatLanguage::UNIVERSAL) {
        const auto lang = static_cast<uint32_t>(data.language);
        const uint32_t comprehension = languageComprehension(lang);
        if (comprehension < chat_rules::kFullComprehension) {
            ensureLanguageWords();
            data.message = chat_rules::garbleText(data.message, lang, comprehension, languageWords_);
        }
    }

    // The spam filter (cvar spamFilter, on as it ships): not for a GM's or a
    // developer's line, not for a GM reader, not for system lines.
    const char* chatFlag = chat_rules::chatFlagName(data.chatTag);
    if (data.type != ChatType::FILTERED && data.type != ChatType::SYSTEM &&
        std::strcmp(chatFlag, "GM") != 0 && std::strcmp(chatFlag, "DEV") != 0 && !readerIsGm &&
        addons::storedCVarValue("spamFilter", "1") != "0" && isSpamLine(data.message)) {
        if (data.type == ChatType::WHISPER && data.senderGuid != 0 && owner_.getSocket()) {
            network::Packet report(wireOpcode(Opcode::CMSG_CHAT_FILTERED));
            report.writeUInt64(data.senderGuid);
            owner_.getSocket()->send(report);
        }
        return;
    }

    // Wowee's own chat panel reads the history, and prints a monster's line
    // as it is: its "%s" is filled here. The interface gets the text as sent
    // and fills it itself (CHAT_MONSTER_*_GET .. arg1 formatted with arg2).
    MessageChatData shown = data;
    if (!shown.senderName.empty() &&
        (isMonsterChatType(shown.type) || shown.type == ChatType::RAID_BOSS_EMOTE)) {
        for (size_t pos = shown.message.find("%s"); pos != std::string::npos;
             pos = shown.message.find("%s", pos + shown.senderName.size())) {
            shown.message.replace(pos, 2, shown.senderName);
        }
    }
    shown.uid = ++chatUidCounter_;
    chatHistory_.push_back(shown);
    if (chatHistory_.size() > maxChatHistory_) chatHistory_.pop_front();
    logChatMessage(shown, "server");

    if (data.type == ChatType::WHISPER) {
        if (data.senderGuid != 0) owner_.lastWhisperSenderGuidRef() = data.senderGuid;
        if (!data.senderName.empty()) owner_.lastWhisperSenderRef() = data.senderName;
    }

    showChatBubble(data.senderGuid, data.type, data.message);
    fireChatLineEvent(data, chatFlag);
}

void ChatHandler::fireChatLineEvent(const MessageChatData& data, const char* chatFlag) {
    // Every line through the chat frame takes the next id (0x509DD0), and
    // FrameXML builds it into the player link on the name, so a right-click
    // on that name hands it back: CanComplainChat and the report menu ask by
    // it.
    const uint32_t lineId = nextChatLineId_++;
    if (data.senderGuid != 0) {
        chatLineSenders_.emplace_back(lineId, data.senderGuid);
        constexpr size_t kRememberedLines = 512;
        while (chatLineSenders_.size() > kRememberedLines) chatLineSenders_.pop_front();
    }
    if (!owner_.addonEventCallbackRef()) return;
    std::string eventName = "CHAT_MSG_";
    eventName += getChatTypeString(data.type);
    // 0x4FDBC0's thirteen: text, sender, language name (Languages.dbc, empty
    // for 0), the channel as "N. Name", the receiver's name, the flag word,
    // the zone channel's ChatChannels id, the channel's number, its name
    // without the zone, the channel's instance (0), the line id, the
    // sender's guid - for a player only, empty for anything else - and the
    // conversation id (0).
    const int channelNumber = getChannelIndex(data.channelName);
    const size_t dash = data.channelName.find(" - ");
    const std::string shortChannel = channelNumber > 0
        ? (dash == std::string::npos ? data.channelName : data.channelName.substr(0, dash))
        : std::string();
    const std::string numberedChannel = channelNumber > 0
        ? std::to_string(channelNumber) + ". " + data.channelName : data.channelName;
    const uint32_t zoneChannelId =
        (channelNumber > 0 && static_cast<size_t>(channelNumber) <= joinedChannelIds_.size())
            ? joinedChannelIds_[static_cast<size_t>(channelNumber) - 1] : 0;
    std::string guidText;
    if (chat_rules::isPlayerGuid(data.senderGuid)) guidText = formatChatLogGuid(data.senderGuid);
    {
        static std::set<std::string> saidFor;
        if (saidFor.size() < 32 && saidFor.insert(eventName).second) {
            LOG_INFO("Chat: fired ", eventName, " to the interface from '",
                     data.senderName, "' - said once per event name");
        }
    }
    owner_.addonEventCallbackRef()(eventName, {
        data.message,
        data.senderName,
        owner_.getLanguageName(static_cast<uint32_t>(data.language)),
        numberedChannel,
        data.receiverName,
        chatFlag ? chatFlag : "",
        std::to_string(zoneChannelId),
        std::to_string(channelNumber),
        shortChannel,
        "0",
        std::to_string(lineId),
        guidText,
        "0",
    });
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
    pumpChatQueue();
}

void ChatHandler::expireChatAwaitingName() {
    // Riding on this per-tick call rather than adding another: the zone
    // channels wait for the zone to be known, and this is chat's tick.
    servicePendingAutoJoin();
    pumpChatQueue();
    flushHeldSends();
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
        std::to_string(index), shortName, "0", std::to_string(nextChatLineId_++), "", "0"
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
        std::to_string(index), shortName, "0", std::to_string(nextChatLineId_++), "", "0"
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
    // A line the client makes itself takes the same path to the interface as
    // the server's, with the same thirteen arguments (0x4FDBC0). A player-kind
    // line with no name is the player's own.
    MessageChatData line = msg;
    if (line.senderName.empty() && !line.noSender && line.type != ChatType::SYSTEM) {
        if (const Character* ac = owner_.getActiveCharacter()) line.senderName = ac->name;
        if (line.senderGuid == 0) line.senderGuid = owner_.getPlayerGuid();
    }
    fireChatLineEvent(line, chat_rules::chatFlagName(line.chatTag));
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
// defense channel - WorldDefense always, LocalDefense only when the area (or
// its parent) is the zone the player is in.
void ChatHandler::postDefenseLine(uint32_t areaId, const std::string& text) {
    for (size_t i = 0; i < joinedChannels_.size(); ++i) {
        const uint32_t id = i < joinedChannelIds_.size() ? joinedChannelIds_[i] : 0;
        const bool world = id == 23 || joinedChannels_[i].rfind("WorldDefense", 0) == 0;
        const bool local = id == 22 || joinedChannels_[i].rfind("LocalDefense", 0) == 0;
        if (!world && !local) continue;
        // The area's AreaTable parent, or the area, against the zone the
        // player is in - one step up, not a walk (0x50C3C0, 0x50C850).
        if (local && !world && owner_.areaZoneForDefense(areaId) != owner_.getWorldStateZoneId())
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
