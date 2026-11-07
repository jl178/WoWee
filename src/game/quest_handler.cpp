#include "ui/framexml_takeover.hpp"
#include "game/item_text.hpp"
#include "game/quest_handler.hpp"
#include "game/global_strings.hpp"
#include "game/quest_query_layout.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/quest_progress.hpp"
#include "game/quest_text.hpp"
#include "game/packet_parsers.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <set>
#include <sstream>

namespace wowee {
namespace game {

QuestGiverStatus QuestHandler::getQuestGiverStatus(uint64_t guid) const {
    auto it = npcQuestStatus_.find(guid);
    return (it != npcQuestStatus_.end()) ? it->second : QuestGiverStatus::NONE;
}

void QuestHandler::reconcileItemObjectivesFromInventory(
    const std::unordered_map<uint32_t, uint32_t>& carriedCounts) {
    bool changedAny = false;
    bool maybeCompletedObjective = false;
    // Which quests moved, so QUEST_WATCH_UPDATE can name each of them. The
    // event carries one log index and the interface auto-watches that quest;
    // firing it once with nothing named watched nothing at all.
    std::vector<uint32_t> movedQuests;
    for (auto& quest : questLog_) {
        // Complete quests are counted too: the log and tracker draw their
        // objective lines from these counts ("Tough Wolf Meat: 8/8"), and
        // skipping them left a finished quest reading 0/8. Only the progress
        // message is for quests still short.
        // Seen only once its item objectives are known: a quest restored at
        // login is reconciled before its query answers, and marking it then
        // made the first real count look like a gain and print at login.
        const bool firstLook = !quest.itemCountsSeen;
        for (const auto& obj : quest.itemObjectives)
            if (obj.itemId != 0 && obj.required != 0) quest.itemCountsSeen = true;
        for (const auto& obj : quest.itemObjectives) {
            if (obj.itemId == 0 || obj.required == 0) continue;
            // Keep the derived required-count map in sync so the
            // SMSG_QUESTUPDATE_ADD_ITEM path and this one agree.
            quest.requiredItemCounts[obj.itemId] = obj.required;

            auto cit = carriedCounts.find(obj.itemId);
            const uint32_t held = (cit != carriedCounts.end()) ? cit->second : 0;
            const uint32_t newCount = std::min(held, obj.required);
            uint32_t& tracked = quest.itemCounts[obj.itemId];
            if (tracked == newCount) continue;

            const bool wasComplete = tracked >= obj.required;
            const bool gained = newCount > tracked;
            tracked = newCount;
            changedAny = true;
            if (std::find(movedQuests.begin(), movedQuests.end(), quest.questId)
                    == movedQuests.end()) {
                movedQuests.push_back(quest.questId);
            }
            if (!wasComplete && newCount >= obj.required)
                maybeCompletedObjective = true;

            // Push the per-objective progress to the on-screen tracker.
            std::string itemLabel = "item #" + std::to_string(obj.itemId);
            if (const ItemQueryResponseData* info = owner_.getItemInfo(obj.itemId)) {
                if (!info->name.empty()) itemLabel = info->name;
            } else {
                // Name not cached yet - request it so the next refresh labels
                // the objective properly.
                owner_.queryItemInfo(obj.itemId, 0);
            }
            // "Tough Wolf Meat: 3/8" (ERR_QUEST_ADD_ITEM_SII) in the info
            // frame. The server sends SMSG_QUESTUPDATE_ADD_ITEM empty in 3.3.5;
            // the client counts its bags and prints this itself (Wow.exe
            // 0x6d7c30), only while the count was still short of the
            // requirement, capped at it - so an item gained past the goal says
            // nothing. Not on the first count of a quest's items (login, a new
            // quest accepted with the items already carried).
            if (!firstLook && !wasComplete && gained)
                owner_.addUIInfoMessage(itemLabel + ": " + std::to_string(newCount) + "/" +
                                        std::to_string(obj.required));
        }
    }

    if (changedAny && owner_.addonEventCallbackRef()) {
        for (uint32_t moved : movedQuests) {
            const int index = questLogIndexOf(moved);
            if (index > 0) {
                owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE",
                                               {std::to_string(index)});
            }
        }
        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
    }
    // Collecting the last item may have made a quest turn-in-able (! → ?),
    // so refresh nearby giver markers immediately.
    if (maybeCompletedObjective) requeryNearbyQuestGiverStatus();
}

/// Say that a quest moved, to everything that draws one.
///
/// The quest log this client keeps and the quest log the interface draws are
/// two different things, and only an event joins them. Marking a quest
/// complete and telling nobody is why an objective the server credited -
/// searching the Altar of Zul, and every other explore objective - appeared
/// only after a relog: the state was right the whole time, and nothing had
/// asked the tracker to look at it again.
///
/// The same three every other quest-progress path fires, by the same rules:
/// the watch update carries the quest's position in the log, because that is
/// what the interface auto-watches from.
void QuestHandler::announceQuestLogChanged(uint32_t questId) {
    if (!owner_.addonEventCallbackRef()) return;
    const int index = questLogIndexOf(questId);
    if (index > 0) {
        owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE", {std::to_string(index)});
    }
    owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
    owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
}

void QuestHandler::requeryNearbyQuestGiverStatus() {
    if (questGiverRequeryCooldown_ > 0.0f) {
        questGiverRequeryPending_ = true;
        return;
    }
    sendQuestGiverStatusQueries();
    questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
}

void QuestHandler::tickQuestGiverStatusRequery(float deltaTime) {
    // A greeting whose text never came: show the window without it.
    if (gossipShowWaitSec_ > 0.0f) {
        gossipShowWaitSec_ -= deltaTime;
        if (gossipShowWaitSec_ <= 0.0f) {
            gossipShowWaitSec_ = 0.0f;
            if (gossipWindowOpen_ && owner_.addonEventCallbackRef())
                owner_.addonEventCallbackRef()("GOSSIP_SHOW", {});
        }
    }
    if (questGiverRequeryCooldown_ <= 0.0f) return;
    questGiverRequeryCooldown_ -= deltaTime;
    if (questGiverRequeryCooldown_ > 0.0f) return;

    questGiverRequeryCooldown_ = 0.0f;
    if (questGiverRequeryPending_) {
        questGiverRequeryPending_ = false;
        sendQuestGiverStatusQueries();
        questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
    }
}

void QuestHandler::sendQuestGiverStatusQueries() {
    if (!owner_.getSocket()) return;
    // CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY is what the stock client sends when
    // its quest log changes: an empty packet, answered by one
    // SMSG_QUESTGIVER_STATUS_MULTIPLE covering every questgiver the server
    // considers visible, game objects included. Classic has no such opcode, so
    // it falls back to one CMSG_QUESTGIVER_STATUS_QUERY per quest-giving unit.
    const auto* opcodes = getActiveOpcodeTable();
    if (opcodes && opcodes->hasOpcode(Opcode::CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY)) {
        network::Packet multiPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY));
        owner_.getSocket()->send(multiPkt);
        return;
    }
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (entity->getType() != ObjectType::UNIT) continue;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (unit->getNpcFlags() & NPC_FLAG_QUESTGIVER) {
            network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
            qsPkt.writeUInt64(guid);
            owner_.getSocket()->send(qsPkt);
        }
    }
}

// igQuestFailed, which the stock client plays with every quest refusal and
// failure it reports: Wow.exe's error table (0xac8af0) gives it to
// ERR_QUEST_FAILED_S and its variants, ERR_QUEST_ALREADY_ON/DONE,
// ERR_QUEST_NEED_PREREQS, ERR_QUEST_LOG_FULL and the rest of the quest errors.
static void playQuestFailedSound(GameHandler& owner) {
    if (auto* ac = owner.services().audioCoordinator)
        if (auto* sfx = ac->getUiSoundManager()) sfx->playQuestFailed();
}


// The readability test and the completed-line choice live in game/quest_text.hpp
// so they can be tested without a packet; this is the name the file has always
// called the first of them by.
static bool isReadableQuestText(const std::string& s, size_t minLen, size_t maxLen) {
    return questTextIsReadable(s, minLen, maxLen);
}

static bool isPlaceholderQuestTitle(const std::string& s) {
    return s.rfind("Quest #", 0) == 0;
}

static bool looksLikeQuestDescriptionText(const std::string& s) {
    int spaces = 0;
    int commas = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if (c == ',') commas++;
    }
    const int words = spaces + 1;
    if (words > 8) return true;
    if (commas > 0 && words > 5) return true;
    if (s.find(". ") != std::string::npos) return true;
    if (s.find(':') != std::string::npos && words > 5) return true;
    return false;
}

static bool isStrongQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 6, 72)) return false;
    if (looksLikeQuestDescriptionText(s)) return false;
    unsigned char first = static_cast<unsigned char>(s.front());
    return std::isupper(first) != 0;
}

static int scoreQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 4, 72)) return -1000;
    if (looksLikeQuestDescriptionText(s)) return -1000;
    int score = 0;
    score += static_cast<int>(std::min<size_t>(s.size(), 32));
    unsigned char first = static_cast<unsigned char>(s.front());
    if (std::isupper(first)) score += 20;
    if (std::islower(first)) score -= 20;
    if (s.find(' ') != std::string::npos) score += 8;
    if (s.find('.') != std::string::npos) score -= 18;
    if (s.find('!') != std::string::npos || s.find('?') != std::string::npos) score -= 6;
    return score;
}

static bool readCStringAt(const std::vector<uint8_t>& data, size_t start, std::string& out, size_t& nextPos) {
    out.clear();
    if (start >= data.size()) return false;
    size_t i = start;
    while (i < data.size()) {
        uint8_t b = data[i++];
        if (b == 0) {
            nextPos = i;
            return true;
        }
        out.push_back(static_cast<char>(b));
    }
    return false;
}

struct QuestQueryTextCandidate {
    std::string title;
    std::string objectives;
    /// The third string: what the quest giver says, shown above the objectives
    /// in the quest log. It was being read and thrown away - the walk to the
    /// fifth string passes straight over it.
    std::string description;
    std::string completionText;
    /// The raw third string as read from the wire, before the readability
    /// check - a temporary probe into why the description comes back empty on
    /// some realms even where the title and objectives read cleanly.
    std::string debugThirdRaw;
    int score = -1000;
};

// Expand WoW text tokens ($b line breaks etc.) in quest strings; titles are
// single-line so their line breaks collapse to spaces instead
static std::string normalizeQuestText(std::string s, bool singleLine) {
    s = normalizeWowTextTokens(std::move(s));
    if (singleLine) {
        std::string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (char ch : s) {
            if (ch == '\n' || ch == '\r' || ch == ' ') {
                if (!lastSpace && !out.empty()) out.push_back(' ');
                lastSpace = true;
            } else {
                out.push_back(ch);
                lastSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        s = std::move(out);
    }
    return s;
}

/// Whether the objective records really do begin where this string run ends.
///
/// The five strings are followed by the kill and item objectives, which are
/// numbers with a shape - an entry id and a count, both inside plausible
/// bounds. So a candidate offset can be checked rather than guessed at: read
/// past its strings and ask whether what follows parses as the block that must
/// follow them. Defined below, beside the parse it borrows.
static bool questStringRunFitsObjectives(const std::vector<uint8_t>& data,
                                         size_t stringsStart, int nStrings,
                                         bool wotlkLayout);

/// The completed line, chosen in game/quest_text.hpp and then expanded: the
/// $b and $n tokens in it are the server's, and every other quest string on
/// its way to the interface goes through the same normalisation.
static std::string pickQuestCompletedLine(const std::string& endText,
                                          const std::string& completedText) {
    std::string chosen = questCompletedText(endText, completedText);
    if (chosen.empty()) return {};
    return normalizeQuestText(std::move(chosen), false);
}

static QuestQueryTextCandidate pickBestQuestQueryTexts(const std::vector<uint8_t>& data, bool classicHint) {
    QuestQueryTextCandidate best;
    if (data.size() <= 9) return best;

    // Where the first string sits: straight after the numeric block. The
    // WotLK count is itemized in quest_query_layout.hpp and pinned by a test
    // that builds the block the way the server writes it.
    //
    // This said fifty-seven fields, which lands thirty-two bytes inside the
    // reward block. The read there is not a string, the readability check drops
    // it, and the scan below rescues most quests by finding the first printable
    // run - but the scan knows a title only by looking like one, so a quest
    // whose objectives read more like a title than its title does came out with
    // the objectives in the title and everything after it shifted by one.
    // Quests 1712 and 5250 in the log are both that.
    //
    // Classic's count is not verified the same way and is left alone; a seed
    // that reads as nothing still falls through to the scan, which is where
    // both of them were already being answered from.
    std::vector<size_t> seedOffsets;
    const size_t base = 8;
    const size_t classicOffset = base + 40u * 4u;
    const size_t wotlkOffset = kWotlkQuestQueryStringsOffset;
    if (classicHint) {
        seedOffsets.push_back(classicOffset);
        seedOffsets.push_back(wotlkOffset);
    } else {
        seedOffsets.push_back(wotlkOffset);
        seedOffsets.push_back(classicOffset);
    }
    for (size_t off : seedOffsets) {
        if (off < data.size()) {
            std::string title;
            size_t next = off;
            if (readCStringAt(data, off, title, next)) {
                QuestQueryTextCandidate c;
                c.title = normalizeQuestText(title, true);
                c.score = scoreQuestTitle(c.title) + 20; // Prefer expected struct offsets
                // And prefer, far above anything a scan can claim, an offset
                // whose strings are followed by a block that reads as the
                // objectives. That is not a preference between two guesses:
                // the numbers after the strings have a shape, and an offset
                // whose run ends exactly at one is the offset the server
                // wrote. Without this a suffix of some other string could
                // outscore a short title - "Sven's Revenge" is fourteen
                // characters - and win the whole candidate, completed line
                // included, which is one way that line came back empty on a
                // quest that had one.
                const bool wotlkSeed = (off == wotlkOffset);
                if (questStringRunFitsObjectives(data, off, wotlkSeed ? 5 : 4,
                                                 wotlkSeed)) {
                    c.score += 100;
                }

                std::string s2;
                size_t n2 = next;
                if (readCStringAt(data, next, s2, n2) && isReadableQuestText(s2, 8, 600)) {
                    c.objectives = normalizeQuestText(s2, false);
                }

                // The strings run Title, Objectives, Details, AreaDescription,
                // CompletedText - read off AzerothCore's Quest::BuildQuestData,
                // where they are written in that order after the numeric block.
                // The fifth is what the quest log and the world map's quest
                // list show once every objective is done, and answering it with
                // "" left a completed quest with a blank line where "Return to
                // Marshal Dughan" belongs.
                //
                // The strings after the title read Title, Objectives, Details,
                // (AreaDescription,) CompletedText in every expansion - the
                // order is the same for vmangos classic and AzerothCore WotLK,
                // which the quest-reward tests pin byte for byte. Only the
                // numeric block before them changes size, and the seed offsets
                // above already try both, with the score picking whichever read
                // cleanly. So the third string is the description whatever the
                // client is, and gating it to WotLK left classic and TBC quests
                // with a blank description panel - the one thing a universal
                // client cannot do.
                //
                // Still only from the seeded offsets, never the scored scan
                // below: that finds a title by printability alone and says
                // nothing about what follows it. And still guarded by
                // readability, so a wrong offset - a title matched at the other
                // expansion's seed - yields an unreadable third string and is
                // dropped rather than pasted onto the quest.
                {
                    std::string s3, s4, s5;
                    size_t n3 = n2, n4 = n2, n5 = n2;
                    if (readCStringAt(data, n2, s3, n3)) {
                        c.debugThirdRaw = s3;   // probe: what the third string held
                        // The details, above the objectives in the log.
                        if (isReadableQuestText(s3, 8, 4096)) {
                            c.description = normalizeQuestText(s3, false);
                        }
                        // The completed line sits one string later on WotLK
                        // (past EndText) than on the earlier clients, where
                        // EndText is the last string there is. Read both and
                        // let questCompletedText say which one this quest
                        // actually carries.
                        if (readCStringAt(data, n3, s4, n4)) {
                            readCStringAt(data, n4, s5, n5);
                            c.completionText = pickQuestCompletedLine(s4, s5);
                        }
                    }
                }
                if (c.score > best.score) best = c;
            }
        }
    }

    // Fallback: scan packet for best printable C-string title candidate.
    for (size_t start = 8; start < data.size(); ++start) {
        std::string title;
        size_t next = start;
        if (!readCStringAt(data, start, title, next)) continue;

        QuestQueryTextCandidate c;
        c.title = normalizeQuestText(title, true);
        c.score = scoreQuestTitle(c.title);
        if (c.score < 0) continue;

        std::string s2, s3;
        size_t n2 = next, n3 = next;
        size_t afterObj = next;
        if (readCStringAt(data, next, s2, n2)) {
            if (isReadableQuestText(s2, 8, 600)) { c.objectives = normalizeQuestText(s2, false); afterObj = n2; }
            else if (readCStringAt(data, n2, s3, n3) && isReadableQuestText(s3, 8, 600)) { c.objectives = normalizeQuestText(s3, false); afterObj = n3; }
        }
        // The fallback found the title by its printable content, so whatever
        // follows is the same run the seeded path reads - Objectives, Details,
        // AreaDescription, CompletedText - only reached without knowing the size
        // of the numeric block ahead of it. AzerothCore's block is larger than
        // either seed above (its reward/reputation arrays push the strings past
        // the WotLK seed), so on a live 3.3.5 realm this scan is what wins, and
        // it used to stop at the objectives and leave every quest's description
        // panel blank while title and objectives came through fine - the exact
        // shape the logs showed. Read the description and completed line here
        // too, anchored to the end of the objectives string and guarded the
        // same way, so the winner carries a description whichever path wins.
        if (!c.objectives.empty()) {
            std::string d3, d4, d5;
            size_t m3 = afterObj, m4 = afterObj, m5 = afterObj;
            if (readCStringAt(data, afterObj, d3, m3)) {
                if (c.debugThirdRaw.empty()) c.debugThirdRaw = d3;
                if (isReadableQuestText(d3, 8, 4096)) c.description = normalizeQuestText(d3, false);
                if (readCStringAt(data, m3, d4, m4)) {
                    readCStringAt(data, m4, d5, m5);
                    c.completionText = pickQuestCompletedLine(d4, d5);
                }
            }
        }
        if (c.score > best.score) best = c;
    }

    return best;
}

struct QuestQueryObjectives {
    struct Kill { int32_t npcOrGoId; uint32_t required; };
    struct Item { uint32_t itemId; uint32_t required; };
    std::array<Kill, 4> kills{};
    std::array<Item, 6> items{};
    /// WotLK only: the fourth string (AreaDescription) and the four
    /// ObjectiveText strings after the objective block (QuestDef.cpp
    /// Quest::BuildQueryData).
    std::string areaDescription;
    std::array<std::string, 4> objectiveTexts{};
    bool valid = false;
};

static uint32_t readU32At(const std::vector<uint8_t>& d, size_t pos) {
    return static_cast<uint32_t>(d[pos])
         | (static_cast<uint32_t>(d[pos + 1]) << 8)
         | (static_cast<uint32_t>(d[pos + 2]) << 16)
         | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

static int32_t decodeQuestNpcOrGo(uint32_t raw) {
    if ((raw & 0x80000000u) != 0)
        return -static_cast<int32_t>(raw & 0x7FFFFFFFu);
    return static_cast<int32_t>(raw);
}

static bool plausibleQuestObjective(int32_t id, uint32_t required) {
    const uint32_t magnitude = id < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(id))
                                      : static_cast<uint32_t>(id);
    return magnitude <= 0x00FFFFFFu && required <= 0x0000FFFFu;
}

static QuestQueryObjectives tryParseQuestObjectivesAt(const std::vector<uint8_t>& data,
                                                       size_t startPos, int nStrings,
                                                       bool wotlkLayout) {
    QuestQueryObjectives out;
    size_t pos = startPos;

    for (int si = 0; si < nStrings; ++si) {
        const size_t begin = pos;
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return out;
        if (wotlkLayout && si == 3)
            out.areaDescription.assign(reinterpret_cast<const char*>(&data[begin]), pos - begin);
        ++pos;
    }

    if (wotlkLayout) {
        // Wrath: four {npc/go, count, sourceItem, sourceCount} records,
        // followed by six {requiredItem, count} records.
        for (int i = 0; i < 4; ++i) {
            if (pos + 16 > data.size()) return out;
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(readU32At(data, pos)); pos += 4;
            out.kills[i].required  = readU32At(data, pos);                     pos += 4;
            pos += 8; // ItemDrop + source count (not inventory objectives)
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required))
                return {};
        }
        for (int i = 0; i < 6; ++i) {
            if (pos + 8 > data.size()) return out;
            out.items[i].itemId   = readU32At(data, pos); pos += 4;
            out.items[i].required = readU32At(data, pos); pos += 4;
            if (!plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId),
                                         out.items[i].required))
                return {};
        }
        for (int i = 0; i < 4 && pos < data.size(); ++i) {
            const size_t begin = pos;
            while (pos < data.size() && data[pos] != 0) ++pos;
            if (pos >= data.size()) break;
            out.objectiveTexts[i].assign(reinterpret_cast<const char*>(&data[begin]), pos - begin);
            ++pos;
        }
    } else {
        // Classic/TBC: four interleaved {npc/go, count, requiredItem, count} records.
        for (int i = 0; i < 4; ++i) {
            if (pos + 16 > data.size()) return out;
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(readU32At(data, pos)); pos += 4;
            out.kills[i].required  = readU32At(data, pos);                     pos += 4;
            out.items[i].itemId    = readU32At(data, pos);                     pos += 4;
            out.items[i].required  = readU32At(data, pos);                     pos += 4;
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required) ||
                !plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId),
                                         out.items[i].required))
                return {};
        }
    }

    out.valid = true;
    return out;
}

static bool questStringRunFitsObjectives(const std::vector<uint8_t>& data,
                                         size_t stringsStart, int nStrings,
                                         bool wotlkLayout) {
    return tryParseQuestObjectivesAt(data, stringsStart, nStrings, wotlkLayout).valid;
}

static QuestQueryObjectives extractQuestQueryObjectives(const std::vector<uint8_t>& data,
                                                         uint8_t questLogStride) {
    if (data.size() < 16) return {};

    const size_t base = 8;
    // Fixed-width fields between questMethod and the first localized string.
    // These counts come directly from each expansion's query response serializer.
    if (questLogStride >= 5)
        return tryParseQuestObjectivesAt(data, base + 63u * 4u, 5, true);  // WotLK
    if (questLogStride == 4)
        return tryParseQuestObjectivesAt(data, base + 41u * 4u, 4, false); // TBC
    return tryParseQuestObjectivesAt(data, base + 37u * 4u, 4, false);     // Classic/Turtle
}

// Quest log reward extraction lives in QuestQueryRewardsParser
// (world_packets_world.cpp) so the per-expansion offsets are unit-testable.

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

QuestHandler::QuestHandler(GameHandler& owner)
    : owner_(owner) {}

// ---------------------------------------------------------------------------
// Opcode registrations
// ---------------------------------------------------------------------------

void QuestHandler::registerOpcodes(DispatchTable& table) {

    // ---- SMSG_GOSSIP_MESSAGE ----
    table[Opcode::SMSG_GOSSIP_MESSAGE] = [this](network::Packet& packet) { handleGossipMessage(packet); };

    // ---- SMSG_QUESTGIVER_QUEST_LIST ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_LIST] = [this](network::Packet& packet) { handleQuestgiverQuestList(packet); };

    // ---- SMSG_GOSSIP_COMPLETE ----
    table[Opcode::SMSG_GOSSIP_COMPLETE] = [this](network::Packet& packet) { handleGossipComplete(packet); };

    // ---- SMSG_NPC_TEXT_UPDATE ----
    table[Opcode::SMSG_NPC_TEXT_UPDATE] = [this](network::Packet& packet) { handleNpcTextUpdate(packet); };

    // ---- SMSG_GOSSIP_POI ----
    table[Opcode::SMSG_GOSSIP_POI] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(20)) return;
        const uint32_t flags = packet.readUInt32();
        float poiX = packet.readFloat();
        float poiY = packet.readFloat();
        uint32_t icon = packet.readUInt32();
        uint32_t data = packet.readUInt32();
        std::string name = packet.readString();
        // Wire order is server X,Y; store canonical (swapped), like quest POIs.
        GossipPoi poi; poi.x = poiY; poi.y = poiX; poi.icon = icon; poi.data = data; poi.name = std::move(name);
        poi.flags = flags;
        poi.receivedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
        gossipPois_.push_back(std::move(poi));
        LOG_DEBUG("SMSG_GOSSIP_POI: x=", poiX, " y=", poiY, " icon=", icon);
    };

    // ---- SMSG_QUESTGIVER_QUEST_DETAILS ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_DETAILS] = [this](network::Packet& packet) { handleQuestDetails(packet); };

    // ---- SMSG_QUESTLOG_FULL ----
    table[Opcode::SMSG_QUESTLOG_FULL] = [this](network::Packet& /*packet*/) {
        // ERR_QUEST_LOG_FULL, the red error with igQuestFailed (Wow.exe
        // 0x6D7F10, opcode 0x195).
        owner_.displayGameMessage("ERR_QUEST_LOG_FULL", "Your quest log is full.");
        // Roll back the optimistic local entries the server refused; without
        // this the local log drifts past the server cap with phantom quests.
        bool removed = false;
        for (const auto& [pendingQuestId, npcGuid] : pendingQuestAcceptNpcGuids_) {
            (void)npcGuid;
            if (findQuestLogSlotIndexFromServer(pendingQuestId) < 0) {
                removed |= std::erase_if(questLog_, [&](const QuestLogEntry& q) {
                    return q.questId == pendingQuestId;
                }) > 0;
            }
        }
        pendingQuestAcceptTimeouts_.clear();
        pendingQuestAcceptNpcGuids_.clear();
        if (removed && owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
    };

    // ---- SMSG_QUESTGIVER_REQUEST_ITEMS ----
    table[Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS] = [this](network::Packet& packet) { handleQuestRequestItems(packet); };

    // ---- SMSG_QUESTGIVER_OFFER_REWARD ----
    table[Opcode::SMSG_QUESTGIVER_OFFER_REWARD] = [this](network::Packet& packet) { handleQuestOfferReward(packet); };

    // ---- SMSG_QUEST_CONFIRM_ACCEPT ----
    table[Opcode::SMSG_QUEST_CONFIRM_ACCEPT] = [this](network::Packet& packet) { handleQuestConfirmAccept(packet); };

    // ---- MSG_QUEST_PUSH_RESULT ----
    // What happened to a quest this player shared, per party member: guid,
    // then AzerothCore's QuestShareMessages (QuestDef.h). Each is one of the
    // ERR_QUEST_PUSH_* GlobalStrings, shown in the info frame. Unhandled
    // before, so the sharer heard nothing back.
    table[Opcode::MSG_QUEST_PUSH_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(9)) return;
        const uint64_t guid = packet.readUInt64();
        const uint8_t msg = packet.readUInt8();
        const std::string who = owner_.lookupName(guid);
        // QuestShareMessages order; each a system line (GameError kind 0).
        static const char* const kKeys[] = {
            "ERR_QUEST_PUSH_SUCCESS_S", "ERR_QUEST_PUSH_INVALID_S", "ERR_QUEST_PUSH_ACCEPTED_S",
            "ERR_QUEST_PUSH_DECLINED_S", "ERR_QUEST_PUSH_BUSY_S", "ERR_QUEST_PUSH_LOG_FULL_S",
            "ERR_QUEST_PUSH_ONQUEST_S", "ERR_QUEST_PUSH_ALREADY_DONE_S",
            "ERR_QUEST_PUSH_NOT_DAILY_S", "ERR_QUEST_PUSH_TIMER_EXPIRED_S",
            "ERR_QUEST_PUSH_NOT_IN_PARTY_S",
        };
        static const char* const kFormats[] = {
            "Sharing quest with %s...", "%s is not eligible for that quest",
            "%s has accepted your quest", "%s has declined your quest", "%s is busy",
            "%s's quest log is full", "%s is already on that quest",
            "%s has completed that quest", "That quest cannot be shared today",
            "Quest sharing timer has expired", "You are not in a party",
        };
        if (msg >= sizeof(kKeys) / sizeof(kKeys[0])) return;
        std::string text = kFormats[msg];
        if (const size_t at = text.find("%s"); at != std::string::npos) text.replace(at, 2, who);
        owner_.displayGameMessage(kKeys[msg], text, {who});
    };

    // ---- SMSG_QUEST_POI_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_POI_QUERY_RESPONSE] = [this](network::Packet& packet) { handleQuestPoiQueryResponse(packet); };

    // ---- SMSG_QUESTGIVER_STATUS ----
    table[Opcode::SMSG_QUESTGIVER_STATUS] = [this](network::Packet& packet) {
        if (packet.hasRemaining(9)) {
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.getPacketParsers()->readQuestGiverStatus(packet);
            LOG_INFO("SMSG_QUESTGIVER_STATUS: npcGuid=0x", std::hex, npcGuid, std::dec,
                     " status=", static_cast<int>(status));
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTGIVER_STATUS_MULTIPLE ----
    table[Opcode::SMSG_QUESTGIVER_STATUS_MULTIPLE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count; ++i) {
            if (!packet.hasRemaining(9)) break;
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.getPacketParsers()->readQuestGiverStatus(packet);
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILED ----
    table[Opcode::SMSG_QUESTUPDATE_FAILED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            // ERR_QUEST_FAILED_S, "%s failed." (Wow.exe 0x6d8030 case 0x196):
            // a system line - its GameError kind is 0 - with its own sound.
            if (!questTitle.empty())
                owner_.displayGameMessage("ERR_QUEST_FAILED_S", questTitle + " failed.", {questTitle});
            else
                playQuestFailedSound(owner_);
            // Same omission as the complete handler had: a line of chat is not
            // a redraw, and a failed quest goes on looking fine in the log.
            announceQuestLogChanged(questId);
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILEDTIMER ----
    table[Opcode::SMSG_QUESTUPDATE_FAILEDTIMER] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            // Case 0x197: the timer quest is failed locally, then the same
            // ERR_QUEST_FAILED_S line.
            if (!questTitle.empty())
                owner_.displayGameMessage("ERR_QUEST_FAILED_S", questTitle + " failed.", {questTitle});
            else
                playQuestFailedSound(owner_);
            announceQuestLogChanged(questId);
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_FAILED ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_FAILED] = [this](network::Packet& packet) {
        // uint32 questId + uint32 reason
        if (packet.hasRemaining(8)) {
            uint32_t questId = packet.readUInt32();
            uint32_t reason = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            if (questTitle.empty() && currentQuestDetails_.questId == questId)
                questTitle = currentQuestDetails_.title;
            // The InventoryResult the turn-in or accept failed on, as Wow.exe
            // (0x6CDA70) words it: a full bag (4, 16, 50) is
            // ERR_QUEST_FAILED_BAG_FULL_S and ERR_INV_FULL, a unique item
            // (17) ERR_QUEST_FAILED_MAX_COUNT_S, too many dailies (74)
            // ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I, anything else
            // ERR_QUEST_FAILED_S - but not for a quest still in the log.
            if (reason == 4 || reason == 16 || reason == 50) {
                if (!questTitle.empty())
                    owner_.displayGameMessage("ERR_QUEST_FAILED_BAG_FULL_S", questTitle + " failed: Inventory is full.", {questTitle});
                owner_.displayGameMessage("ERR_INV_FULL", "Inventory is full.");
            } else if (reason == 17) {
                if (!questTitle.empty())
                    owner_.displayGameMessage("ERR_QUEST_FAILED_MAX_COUNT_S", questTitle + " failed: Duplicate item found.", {questTitle});
            } else if (reason == 74) {
                owner_.displayGameMessage("ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I",
                    "You have already completed 25 daily quests today", {"25"});
            } else {
                bool inLog = false;
                for (const auto& q : questLog_) if (q.questId == questId) { inLog = true; break; }
                if (!inLog && !questTitle.empty())
                    owner_.displayGameMessage("ERR_QUEST_FAILED_S", questTitle + " failed.", {questTitle});
            }
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_INVALID ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_INVALID] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t failReason = packet.readUInt32();
            pendingTurnInRewardRequest_ = false;
            const char* reasonStr = "Unknown";
            switch (failReason) {
                case 0: reasonStr = "Don't have quest"; break;
                case 1: reasonStr = "Quest level too low"; break;
                case 4: reasonStr = "Insufficient money"; break;
                case 5: reasonStr = "Inventory full"; break;
                case 13: reasonStr = "Already on that quest"; break;
                case 18: reasonStr = "Already completed quest"; break;
                case 19: reasonStr = "Can't take any more quests"; break;
            }
            LOG_WARNING("Quest invalid: reason=", failReason, " (", reasonStr, ")");
            if (!pendingQuestAcceptTimeouts_.empty()) {
                std::vector<uint32_t> pendingQuestIds;
                pendingQuestIds.reserve(pendingQuestAcceptTimeouts_.size());
                for (const auto& pending : pendingQuestAcceptTimeouts_) {
                    pendingQuestIds.push_back(pending.first);
                }
                for (uint32_t questId : pendingQuestIds) {
                    const uint64_t npcGuid = pendingQuestAcceptNpcGuids_.count(questId) != 0
                        ? pendingQuestAcceptNpcGuids_[questId] : 0;
                    if (failReason == 13) {
                        std::string fallbackTitle = "Quest #" + std::to_string(questId);
                        std::string fallbackObjectives;
                        if (currentQuestDetails_.questId == questId) {
                            if (!currentQuestDetails_.title.empty()) fallbackTitle = currentQuestDetails_.title;
                            fallbackObjectives = currentQuestDetails_.objectives;
                        }
                        addQuestToLocalLogIfMissing(questId, fallbackTitle, fallbackObjectives);
                        triggerQuestAcceptResync(questId, npcGuid, "already-on-quest");
                    } else if (failReason == 18) {
                        triggerQuestAcceptResync(questId, npcGuid, "already-completed");
                    }
                    clearPendingQuestAccept(questId);
                }
            }
            // QuestFailedReason as Wow.exe (0x6CD7C0) words it, chat lines
            // with igQuestFailed; every reason says something, the ones it
            // does not name ERR_QUEST_NEED_PREREQS.
            struct Answer { uint32_t code; const char* key; const char* text; };
            static constexpr Answer kAnswers[] = {
                {1,  "ERR_QUEST_FAILED_LOW_LEVEL", "You are not high enough level for that quest."},
                {6,  "ERR_QUEST_FAILED_WRONG_RACE", "That quest is not available to your race."},
                {7,  "ERR_QUEST_ALREADY_DONE", "You have completed that quest."},
                {12, "ERR_QUEST_ONLY_ONE_TIMED", "You can only be on one timed quest at a time."},
                {13, "ERR_QUEST_ALREADY_ON", "You are already on that quest."},
                {16, "ERR_QUEST_FAILED_EXPANSION", "This quest requires an expansion enabled account."},
                {18, "ERR_QUEST_ALREADY_ON", "You are already on that quest."},
                {21, "ERR_QUEST_FAILED_MISSING_ITEMS", "You don't have the required items with you.  Check storage."},
                {23, "ERR_QUEST_FAILED_NOT_ENOUGH_MONEY", "You don't have enough money for that quest."},
                {27, "ERR_QUEST_FAILED_CAIS", "You cannot complete quests once you have reached tired time."},
                {29, "ERR_QUEST_ALREADY_DONE_DAILY", "You have completed that daily quest today."},
            };
            (void)reasonStr;
            if (failReason == 26) {
                owner_.displayGameMessage("ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I",
                    "You have already completed 25 daily quests today", {"25"});
            } else {
                const Answer* hit = nullptr;
                for (const auto& a : kAnswers) if (a.code == failReason) hit = &a;
                if (hit) owner_.displayGameMessage(hit->key, hit->text);
                else owner_.displayGameMessage("ERR_QUEST_NEED_PREREQS", "You don't meet the requirements for that quest.");
            }
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_COMPLETE ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE] = [this](network::Packet& packet) {
        uint32_t nextInChain = 0;
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            // The rewards after the id - XP, money, honor, talent points,
            // arena points (AzerothCore SendQuestReward) - each said by
            // Wow.exe after ERR_QUEST_COMPLETE_S (0x6D1110 -> 0x6D0E10).
            uint32_t rewards[5] = {0, 0, 0, 0, 0};
            nextInChain = 0;
            for (auto& r : rewards) if (packet.hasRemaining(4)) r = packet.readUInt32();
            LOG_INFO("Quest completed: questId=", questId);
            if (pendingTurnInQuestId_ == questId) {
                pendingTurnInQuestId_ = 0;
                pendingTurnInNpcGuid_ = 0;
                pendingTurnInRewardRequest_ = false;
            }
            for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
                if (it->questId == questId) {
                    nextInChain = it->nextQuestInChain;
                    // ERR_QUEST_COMPLETE_S, then the rewards: ERR_QUEST_REWARD_EXP_I,
                    // ERR_QUEST_REWARD_MONEY_S, COMBATLOG_HONORAWARD (honor
                    // line), LEVEL_UP_CHAR_POINTS, COMBATLOG_ARENAPOINTSAWARD.
                    if (!it->title.empty())
                        owner_.displayGameMessage("ERR_QUEST_COMPLETE_S", it->title + " completed.", {it->title});
                    if (rewards[0] > 0)
                        owner_.displayGameMessage("ERR_QUEST_REWARD_EXP_I",
                            "Experience gained: " + std::to_string(rewards[0]) + ".", {std::to_string(rewards[0])});
                    if (static_cast<int32_t>(rewards[1]) > 0) {
                        const std::string m = owner_.formatMoneyText(rewards[1]);
                        owner_.displayGameMessage("ERR_QUEST_REWARD_MONEY_S", "Received " + m + ".", {m});
                    }
                    if (static_cast<int32_t>(rewards[2]) > 0) {
                        std::string fmt = globalString(owner_.services().assetManager, "COMBATLOG_HONORAWARD");
                        if (fmt.empty()) fmt = "You have been awarded %d honor points.";
                        owner_.addLocalChatLine(ChatType::COMBAT_HONOR_GAIN,
                                                formatGlobalString(fmt, {std::to_string(rewards[2])}));
                    }
                    if (static_cast<int32_t>(rewards[3]) > 0)
                        owner_.addGlobalStringChatLine("LEVEL_UP_CHAR_POINTS",
                            "You have gained %d talent |4point:points;.", {std::to_string(rewards[3])});
                    if (static_cast<int32_t>(rewards[4]) > 0)
                        owner_.addGlobalStringChatLine("COMBATLOG_ARENAPOINTSAWARD",
                            "You have been awarded %d arena points.", {std::to_string(rewards[4])});
                    // igQuestListComplete, which Wow.exe plays from its own
                    // SMSG_QUESTGIVER_QUEST_COMPLETE handler (0x6d0e10)
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager())
                            sfx->playQuestComplete();
                    }
                    questLog_.erase(it);
                    owner_.setQuestTracked(questId, false);
                    LOG_INFO("  Removed quest ", questId, " from quest log");
                    if (owner_.addonEventCallbackRef())
                        owner_.addonEventCallbackRef()("QUEST_TURNED_IN", {std::to_string(questId)});
                    break;
                }
            }
        }
        // The reward frame waited for this (0x58ca70): it closes, letting
        // the NPC go, unless the quest leads straight on to another, whose
        // details the server sends next and which take the frame over.
        if (questOfferRewardOpen_ && questRewardChosen_) {
            if (nextInChain == 0) closeQuestOfferReward(true);
        }
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
        }
        // Re-query all nearby quest giver NPCs so markers refresh
        requeryNearbyQuestGiverStatus();
    };

    // ---- SMSG_QUESTUPDATE_ADD_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_KILL] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            const uint32_t wireEntry = packet.readUInt32();
            const bool isObject = (wireEntry & 0x80000000u) != 0;
            uint32_t entry = normalizeQuestObjectiveEntry(wireEntry);
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) {
                reqCount = packet.readUInt32();
            }

            LOG_INFO("Quest kill update: questId=", questId, " entry=", entry,
                     " count=", count, "/", reqCount);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    if (reqCount == 0) {
                        auto it = quest.killCounts.find(entry);
                        if (it != quest.killCounts.end()) reqCount = it->second.second;
                    }
                    // Fall back to killObjectives (parsed from SMSG_QUEST_QUERY_RESPONSE).
                    if (reqCount == 0) {
                        for (const auto& obj : quest.killObjectives) {
                            if (obj.npcOrGoId == 0 || obj.required == 0) continue;
                            uint32_t objEntry = static_cast<uint32_t>(
                                obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
                            if (objEntry == entry) {
                                reqCount = obj.required;
                                break;
                            }
                        }
                    }
                    // Some quests (e.g. escort/event quests) report kill credit updates without
                    // a corresponding objective count in SMSG_QUEST_QUERY_RESPONSE. Fall back to
                    // current count so the progress display shows "N/N" instead of "N/0".
                    if (reqCount == 0) reqCount = count;
                    count = std::min(count, reqCount);
                    quest.killCounts[entry] = {count, reqCount};

                    std::string creatureName;
                    if (!isObject) {
                        creatureName = owner_.getCachedCreatureName(entry);
                    } else if (const auto* go = owner_.getCachedGameObjectInfo(entry)) {
                        creatureName = go->name;
                    }
                    // Wow.exe 0x6d8030: the objective's own ObjectiveText when
                    // it has one ("%s: %d/%d", ERR_QUEST_ADD_FOUND_SII), else
                    // the creature ("%s slain: %d/%d") or object name.
                    std::string objectiveText;
                    for (size_t oi = 0; oi < quest.killObjectives.size(); ++oi) {
                        const auto& ko = quest.killObjectives[oi];
                        const uint32_t koEntry = static_cast<uint32_t>(
                            ko.npcOrGoId > 0 ? ko.npcOrGoId : -ko.npcOrGoId);
                        if (koEntry == entry) { objectiveText = quest.objectiveTexts[oi]; break; }
                    }
                    owner_.addUIInfoMessage(objectiveText.empty()
                        ? questObjectiveLine(creatureName, isObject, count, reqCount)
                        : questObjectiveLine(objectiveText, true, count, reqCount));

                    if (owner_.addonEventCallbackRef()) {
                        // The log INDEX, not the quest id. QuestLog_OnEvent
                        // hands arg1 straight to GetNumQuestLeaderBoards and
                        // AddQuestWatch, both of which count positions in the
                        // log - so a quest id addressed whichever quest
                        // happened to sit at that position, or none.
                        owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE",
                            {std::to_string(questLogIndexOf(questId))});
                        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
                        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
                    }

                    LOG_INFO("Updated kill count for quest ", questId, ": ",
                             count, "/", reqCount);
                    // When this objective just finished, a quest may have become
                    // turn-in-able - refresh nearby giver markers (! → ?) now
                    // instead of only after leaving and re-entering the area.
                    if (reqCount > 0 && count >= reqCount) requeryNearbyQuestGiverStatus();
                    break;
                }
            }
        } else if (rem >= 4) {
            // Swapped mapping fallback: treat as QUESTUPDATE_COMPLETE packet.
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            LOG_INFO("Quest objectives completed (compat via ADD_KILL): questId=", questId);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    // As the real SMSG_QUESTUPDATE_COMPLETE path says it.
                    owner_.displayGameMessage("ERR_QUEST_UNKNOWN_COMPLETE", "Objective Complete.");
                    break;
                }
            }
            requeryNearbyQuestGiverStatus();
        }
    };

    // ---- SMSG_QUESTUPDATE_ADD_ITEM ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_ITEM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t itemId = packet.readUInt32();
            uint32_t count = packet.readUInt32();
            owner_.queryItemInfo(itemId, 0);

            std::string itemLabel = "item #" + std::to_string(itemId);
            uint32_t questItemQuality = 1;
            if (const ItemQueryResponseData* info = owner_.getItemInfo(itemId)) {
                if (!info->name.empty()) itemLabel = info->name;
                questItemQuality = info->quality;
            }

            bool updatedAny = false;
            bool maybeCompletedObjective = false;
            for (auto& quest : questLog_) {
                if (quest.complete) continue;
                bool tracksItem =
                    quest.requiredItemCounts.count(itemId) > 0 ||
                    quest.itemCounts.count(itemId) > 0;
                // Also check itemObjectives parsed from SMSG_QUEST_QUERY_RESPONSE in case
                // requiredItemCounts hasn't been populated yet (race during quest accept).
                if (!tracksItem) {
                    for (const auto& obj : quest.itemObjectives) {
                        if (obj.itemId == itemId && obj.required > 0) {
                            quest.requiredItemCounts.emplace(itemId, obj.required);
                            tracksItem = true;
                            break;
                        }
                    }
                }
                if (!tracksItem) continue;
                // SMSG_QUESTUPDATE_ADD_ITEM carries the amount added by this
                // loot operation, not the new absolute objective count.
                const uint32_t required = quest.requiredItemCounts.count(itemId) != 0
                    ? quest.requiredItemCounts[itemId] : std::numeric_limits<uint32_t>::max();
                const uint32_t current = quest.itemCounts[itemId];
                quest.itemCounts[itemId] = current >= required - std::min(required, count)
                    ? required
                    : current + count;
                updatedAny = true;
                // An unknown requirement (no SMSG_QUEST_QUERY_RESPONSE yet) can't
                // be compared against, so assume it may have just filled rather
                // than leaving those quests without a marker refresh. The requery
                // is rate-limited, so guessing wide costs nothing.
                const bool requiredKnown = required != std::numeric_limits<uint32_t>::max();
                if (!requiredKnown || quest.itemCounts[itemId] >= required) {
                    maybeCompletedObjective = true;
                }
            }
            // No line here: the "%s: %d/%d" text (ERR_QUEST_ADD_ITEM_SII) is
            // said from the bag count, as Wow.exe does (0x6D7C30).
            (void)questItemQuality; (void)itemLabel;

            if (owner_.addonEventCallbackRef() && updatedAny) {
                // Once per quest this item counts towards, each named by its
                // position in the log - which is what the interface auto-watches
                // from, and what it could not do with no argument at all.
                for (const auto& quest : questLog_) {
                    if (quest.complete) continue;
                    if (quest.itemCounts.count(itemId) == 0) continue;
                    const int index = questLogIndexOf(quest.questId);
                    if (index > 0) {
                        owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE",
                                                       {std::to_string(index)});
                    }
                }
                owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
                owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
            }
            // Refresh nearby giver markers now if collecting this item may have
            // made a quest turn-in-able (! → ?), rather than on leave/return.
            if (maybeCompletedObjective) requeryNearbyQuestGiverStatus();
            LOG_INFO("Quest item update: itemId=", itemId, " count=", count,
                     " trackedQuestsUpdated=", updatedAny);
        }
    };

    // ---- SMSG_QUESTUPDATE_COMPLETE ----
    table[Opcode::SMSG_QUESTUPDATE_COMPLETE] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            uint32_t entry = normalizeQuestObjectiveEntry(packet.readUInt32());
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) reqCount = packet.readUInt32();
            if (reqCount == 0) reqCount = count;
            count = std::min(count, reqCount);
            LOG_INFO("Quest kill update (compat via COMPLETE): questId=", questId,
                     " entry=", entry, " count=", count, "/", reqCount);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.killCounts[entry] = {count, reqCount};
                    // The kill text the stock ADD_KILL path says.
                    const std::string victim = owner_.getCachedCreatureName(entry);
                    if (!victim.empty())
                        owner_.displayGameMessage("ERR_QUEST_ADD_KILL_SII",
                            victim + " slain: " + std::to_string(count) + "/" + std::to_string(reqCount),
                            {victim, std::to_string(count), std::to_string(reqCount)});
                    announceQuestLogChanged(questId);
                    break;
                }
            }
        } else if (rem >= 4) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            // Visible at the default log level. This is the answer to "I did
            // the thing and nothing happened": it separates a server that
            // never credited from a server that did and an interface that did
            // not show it, and there is no other way to tell those apart from
            // outside. One line per objective completed, which is rare.
            LOG_WARNING("Quest objectives completed: questId=", questId);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    // Wow.exe 0x6d8030 case 0x198: "<AreaDescription> (Complete)"
                    // (ERR_QUEST_OBJECTIVE_COMPLETE_S) in the info frame, or
                    // "Objective Complete." (ERR_QUEST_UNKNOWN_COMPLETE) for a
                    // quest without one. Not a chat line.
                    if (quest.areaDescription.empty())
                        owner_.displayGameMessage("ERR_QUEST_UNKNOWN_COMPLETE", "Objective Complete.");
                    else
                        owner_.displayGameMessage("ERR_QUEST_OBJECTIVE_COMPLETE_S",
                            quest.areaDescription + " (Complete)", {quest.areaDescription});
                    LOG_INFO("Marked quest ", questId, " as complete");
                    announceQuestLogChanged(questId);
                    break;
                }
            }
            // The mark over the giver's head is now wrong, and this is the one
            // completion path that never said so. Every other one re-asks -
            // the kill counter when its objective fills, both item paths, and
            // the compatibility branch above - so a quest finished by this
            // opcode kept the grey "in progress" question mark on an NPC that
            // was ready to take the turn-in.
            //
            // Only stale for an NPC already spawned when the quest finished: a
            // giver is asked about as it spawns, so walking up to a fresh one
            // showed the right mark and walking back to a familiar one did
            // not. That is what made it look intermittent.
            requeryNearbyQuestGiverStatus();
        }
    };

    // ---- SMSG_QUEST_FORCE_REMOVE ----
    table[Opcode::SMSG_QUEST_FORCE_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("SMSG_QUEST_FORCE_REMOVE too short");
            return;
        }
        // A quest the server dropped from the log (uint32 questId). Resting is
        // not read from here: it comes from PLAYER_FLAGS_RESTING.
        uint32_t questId = packet.readUInt32();
        clearPendingQuestAccept(questId);
        pendingQuestQueryIds_.erase(questId);
        if (questId == 0) {
            return;
        }

        bool removed = false;
        std::string removedTitle;
        for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
            if (it->questId == questId) {
                removedTitle = it->title;
                questLog_.erase(it);
                removed = true;
                break;
            }
        }
        if (currentQuestDetails_.questId == questId) {
            questDetailsOpen_ = false;
            questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
            currentQuestDetails_ = QuestDetailsData{};
            removed = true;
        }
        if (currentQuestRequestItems_.questId == questId) {
            questRequestItemsOpen_ = false;
            currentQuestRequestItems_ = QuestRequestItemsData{};
            removed = true;
        }
        if (currentQuestOfferReward_.questId == questId) {
            questOfferRewardOpen_ = false;
            currentQuestOfferReward_ = QuestOfferRewardData{};
            removed = true;
        }
        if (removed) {
            owner_.setQuestTracked(questId, false);
            // ERR_QUEST_FORCE_REMOVED_S naming the quest (Wow.exe 0x6D8370).
            if (!removedTitle.empty())
                owner_.displayGameMessage("ERR_QUEST_FORCE_REMOVED_S",
                    "The quest " + removedTitle + " has been removed from your quest log", {removedTitle});
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
                owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
                owner_.addonEventCallbackRef()("QUEST_REMOVED", {std::to_string(questId)});
            }
        }
    };

    // ---- SMSG_QUEST_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (packet.getSize() < 8) {
            LOG_WARNING("SMSG_QUEST_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
            return;
        }

        uint32_t questId = packet.readUInt32();
        packet.readUInt32(); // questMethod

        const uint8_t questLogStride = owner_.getPacketParsers()
                                           ? owner_.getPacketParsers()->questLogStride() : 5;
        const bool isClassicLayout = questLogStride <= 4;

        // questLevel is the third field (after questId + questMethod) in every
        // expansion's SMSG_QUEST_QUERY_RESPONSE; -1 = player-scaling (WotLK)
        int32_t questLevel = 0;
        if (packet.getData().size() >= 12) {
            questLevel = static_cast<int32_t>(readU32At(packet.getData(), 8));
            if (questLevel < -1 || questLevel > 255) questLevel = 0; // sanity: wrong layout
        }

        // ZoneOrSort follows questLevel: offset 12 in Classic/TBC, 16 in WotLK
        // (which inserted MinLevel before it). >0 = AreaTable zone id, <0 =
        // QuestSort.dbc category. Used to group the quest log by zone.
        int32_t zoneOrSort = 0;
        {
            const size_t zosOffset = (questLogStride >= 5) ? 16 : 12;
            if (packet.getData().size() >= zosOffset + 4) {
                zoneOrSort = static_cast<int32_t>(readU32At(packet.getData(), zosOffset));
                if (zoneOrSort < -100000 || zoneOrSort > 100000) zoneOrSort = 0; // sanity
            }
        }

        const QuestQueryTextCandidate parsed = pickBestQuestQueryTexts(packet.getData(), isClassicLayout);
        const QuestQueryObjectives objs = extractQuestQueryObjectives(packet.getData(), questLogStride);
        // What the text and objective parses actually found, at warning so it
        // survives the default log. Two reports meet here: a blank description
        // panel, and no objectives shown on screen or in the log. The text
        // parse feeds the description; the objective parse feeds the leader
        // boards the watch frame and log draw. Naming both - the description's
        // raw third string, and how many kill and item objectives came out
        // valid - says which parse missed on this realm. One line per query.
        int objKills = 0, objItems = 0;
        if (objs.valid) {
            for (const auto& k : objs.kills)
                if (k.npcOrGoId != 0 || k.required > 0) ++objKills;
            for (const auto& it : objs.items)
                if (it.itemId != 0 || it.required > 0) ++objItems;
        }
        // At debug now. This was raised to warning for two reports - a blank
        // description panel and no objectives on screen - and both are answered:
        // the string block starts sixty-five fields in, not fifty-seven, and a
        // session's worth of quests now parse with a real title, real
        // objectives and a real description every time. Kept, because it is the
        // thing that showed the fault, and one WOWEE_LOG_LEVEL=debug away.
        LOG_DEBUG("Quest text parse: id=", questId,
                    " classicLayout=", isClassicLayout ? "yes" : "no",
                    " title=\"", parsed.title.substr(0, 40),
                    "\" (", parsed.title.size(), " ch) objectives=",
                    parsed.objectives.size(), "ch description=",
                    parsed.description.size(), "ch completion=",
                    parsed.completionText.size(), "ch score=", parsed.score,
                    " thirdRaw(", parsed.debugThirdRaw.size(), ")=\"",
                    parsed.debugThirdRaw.substr(0, 50), "\"",
                    " objParse: valid=", objs.valid ? 1 : 0,
                    " kills=", objKills, " items=", objItems);
        const QuestQueryRewardsData rwds = QuestQueryRewardsParser::parse(packet.getData(), questLogStride);

        bool reconcileAfterQuery = false;
        for (auto& q : questLog_) {
            if (q.questId != questId) continue;

            if (questLevel != 0) q.level = questLevel;
            if (zoneOrSort != 0) q.zoneOrSort = zoneOrSort;
            // WotLK header: id, method, level, minLevel, zoneOrSort, type,
            // suggestedPlayers ... sourceItem (19), flags (20)
            // (Quest::InitializeQueryData).
            if (questLogStride >= 5 && packet.getData().size() >= 84) {
                q.questType        = readU32At(packet.getData(), 20);
                q.suggestedPlayers = readU32At(packet.getData(), 24);
                q.questFlags       = readU32At(packet.getData(), 80);
                q.nextQuestInChain = readU32At(packet.getData(), 44);
                if (q.suggestedPlayers > 40) q.suggestedPlayers = 0;   // sanity: wrong layout
            }

            const int existingScore = scoreQuestTitle(q.title);
            const bool parsedStrong = isStrongQuestTitle(parsed.title);
            const bool parsedLongEnough = parsed.title.size() >= 6;
            const bool notShorterThanExisting =
                isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.title.size() + 2 >= q.title.size();
            const bool shouldReplaceTitle =
                parsed.score > -1000 &&
                parsedStrong &&
                parsedLongEnough &&
                notShorterThanExisting &&
                (isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.score >= existingScore + 12);

            if (shouldReplaceTitle && !parsed.title.empty()) {
                q.title = parsed.title;
            }
            if (q.announceAcceptOnQuery && !isPlaceholderQuestTitle(q.title) && !q.title.empty()) {
                q.announceAcceptOnQuery = false;
                owner_.displayGameMessage("ERR_QUEST_ACCEPTED_S", "Quest accepted: " + q.title, {q.title});
            }
            if (!parsed.objectives.empty() &&
                (q.objectives.empty() || q.objectives.size() < 16)) {
                q.objectives = parsed.objectives;
            }
            if (!parsed.completionText.empty()) q.completionText = parsed.completionText;
            if (!parsed.description.empty()) q.description = parsed.description;

            // Store structured kill/item objectives for later kill-count restoration.
            if (objs.valid) {
                q.areaDescription = normalizeQuestText(objs.areaDescription, true);
                reconcileAfterQuery = true;
                for (int i = 0; i < 4; ++i)
                    q.objectiveTexts[i] = normalizeQuestText(objs.objectiveTexts[i], true);
                std::unordered_set<uint32_t> validKillKeys;
                std::unordered_set<uint32_t> validItemKeys;
                for (int i = 0; i < 4; ++i) {
                    q.killObjectives[i].npcOrGoId = objs.kills[i].npcOrGoId;
                    q.killObjectives[i].required  = objs.kills[i].required;
                    if (objs.kills[i].npcOrGoId != 0 && objs.kills[i].required != 0) {
                        validKillKeys.insert(static_cast<uint32_t>(
                            objs.kills[i].npcOrGoId > 0 ? objs.kills[i].npcOrGoId
                                                       : -objs.kills[i].npcOrGoId));
                    }
                }
                for (int i = 0; i < 6; ++i) {
                    q.itemObjectives[i].itemId   = objs.items[i].itemId;
                    q.itemObjectives[i].required = objs.items[i].required;
                    if (objs.items[i].itemId != 0 && objs.items[i].required != 0)
                        validItemKeys.insert(objs.items[i].itemId);
                }
                // Remove entries produced by an earlier malformed/heuristic decode while
                // retaining live progress for objectives that are still part of the quest.
                std::erase_if(q.killCounts, [&](const auto& value) {
                    return validKillKeys.count(value.first) == 0;
                });
                std::erase_if(q.itemCounts, [&](const auto& value) {
                    return validItemKeys.count(value.first) == 0;
                });
                std::erase_if(q.requiredItemCounts, [&](const auto& value) {
                    return validItemKeys.count(value.first) == 0;
                });
                applyPackedKillCountsFromFields(q);
                for (int i = 0; i < 4; ++i) {
                    int32_t id = objs.kills[i].npcOrGoId;
                    if (id == 0 || objs.kills[i].required == 0) continue;
                    if (id > 0) owner_.queryCreatureInfo(static_cast<uint32_t>(id), 0);
                    else        owner_.queryGameObjectInfo(static_cast<uint32_t>(-id), 0);
                }
                for (int i = 0; i < 6; ++i) {
                    if (objs.items[i].itemId != 0 && objs.items[i].required != 0)
                        owner_.queryItemInfo(objs.items[i].itemId, 0);
                }
                LOG_DEBUG("Quest ", questId, " objectives parsed: kills=[",
                          objs.kills[0].npcOrGoId, "/", objs.kills[0].required, ", ",
                          objs.kills[1].npcOrGoId, "/", objs.kills[1].required, ", ",
                          objs.kills[2].npcOrGoId, "/", objs.kills[2].required, ", ",
                          objs.kills[3].npcOrGoId, "/", objs.kills[3].required, "]");
            }

            // Store reward data and pre-fetch item info for icons.
            if (rwds.valid) {
                q.rewardMoney = rwds.rewardMoney;
                if (rwds.rewardSpellId != 0) q.rewardSpellId = rwds.rewardSpellId;
                if (rwds.xpId != 0) q.rewardXPId = rwds.xpId;
                q.rewardHonor = rwds.rewardHonor;
                q.rewardTalents = rwds.bonusTalents;
                q.rewardArenaPoints = rwds.arenaPoints;
                if (rwds.rewardTitleId != 0) q.rewardTitleId = rwds.rewardTitleId;
                for (size_t i = 0; i < 5; ++i) {
                    q.factionRewards[i].factionId = rwds.factionId[i];
                    q.factionRewards[i].valueId   = rwds.factionValueId[i];
                    q.factionRewards[i].override  = rwds.factionValueOverride[i];
                }
                q.sourceItemId = rwds.sourceItemId;
                // The watch frame needs its name and icon to draw a button.
                if (q.sourceItemId != 0) owner_.queryItemInfo(q.sourceItemId, 0);
                for (int i = 0; i < 4; ++i) {
                    q.rewardItems[i].itemId = rwds.itemId[i];
                    q.rewardItems[i].count  = (rwds.itemId[i] != 0) ? rwds.itemCount[i] : 0;
                    if (rwds.itemId[i] != 0) owner_.queryItemInfo(rwds.itemId[i], 0);
                }
                for (int i = 0; i < 6; ++i) {
                    q.rewardChoiceItems[i].itemId = rwds.choiceItemId[i];
                    q.rewardChoiceItems[i].count  = (rwds.choiceItemId[i] != 0) ? rwds.choiceItemCount[i] : 0;
                    if (rwds.choiceItemId[i] != 0) owner_.queryItemInfo(rwds.choiceItemId[i], 0);
                }
            }

            // The response arrives a beat after the log panel already drew,
            // so the title, objectives and description it just filled in reach
            // a quest the detail pane rendered while they were still blank.
            // QuestLog_UpdateQuestDetails runs on QUEST_LOG_UPDATE for the
            // selected quest, so firing it here repaints the pane with the text
            // that just landed - without it a queried quest keeps its empty
            // description until the log is closed and reopened, which is the
            // "only after a relog" shape reported for the description panel.
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            break;
        }
        // The item objectives are known now: count what the bags already hold,
        // so the first item looted afterwards is a gain against that.
        if (reconcileAfterQuery) owner_.reconcileQuestItemObjectivesFromBags();

        pendingQuestQueryIds_.erase(questId);
    };

    // ---- SMSG_QUESTUPDATE_ADD_PVP_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_PVP_KILL] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            uint32_t questId = packet.readUInt32();
            uint32_t count   = packet.readUInt32();
            uint32_t reqCount = packet.readUInt32();

            constexpr uint32_t PVP_KILL_ENTRY = 0u;
            for (auto& quest : questLog_) {
                if (quest.questId != questId) continue;

                if (reqCount == 0) {
                    auto it = quest.killCounts.find(PVP_KILL_ENTRY);
                    if (it != quest.killCounts.end()) reqCount = it->second.second;
                }
                if (reqCount == 0) {
                    for (const auto& obj : quest.killObjectives) {
                        if (obj.npcOrGoId == 0 && obj.required > 0) {
                            reqCount = obj.required;
                            break;
                        }
                    }
                }
                if (reqCount == 0) reqCount = count;
                count = std::min(count, reqCount);
                quest.killCounts[PVP_KILL_ENTRY] = {count, reqCount};

                owner_.addUIInfoMessage("Players slain: " + std::to_string(count) + "/" +
                                        std::to_string(reqCount));
                break;
            }
        }
    };

    // ---- Completed quests response (moved from GameHandler) ----
    table[Opcode::SMSG_QUERY_QUESTS_COMPLETED_RESPONSE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t count = packet.readUInt32();
            if (count <= 4096) {
                for (uint32_t i = 0; i < count; ++i) {
                    if (!packet.hasRemaining(4)) break;
                    uint32_t questId = packet.readUInt32();
                    owner_.completedQuestsRef().insert(questId);
                }
                LOG_DEBUG("SMSG_QUERY_QUESTS_COMPLETED_RESPONSE: ", count, " completed quests");
            }
        }
        packet.skipAll();
    };
}

// ---------------------------------------------------------------------------
// Public API methods
// ---------------------------------------------------------------------------

void QuestHandler::selectGossipOption(uint32_t optionId, const std::string& code) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !gossipWindowOpen_) return;
    LOG_INFO("selectGossipOption: optionId=", optionId,
             " npcGuid=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " menuId=", currentGossip_.menuId,
             " numOptions=", currentGossip_.options.size());
    // The code goes on the wire only for an option the server flagged coded -
    // it reads the string conditionally, on GossipOptionCoded, so sending one
    // for an ordinary option leaves a trailing string it never consumes.
    auto packet = GossipSelectOptionPacket::build(currentGossip_.npcGuid,
                                                  currentGossip_.menuId, optionId, code);
    owner_.getSocket()->send(packet);

    // The stock client sends the select and nothing else: the server opens
    // the vendor, bank, auction house or stable from its own gossip handler
    // (AzerothCore Player::OnGossipSelect -> SendListInventory, SendShowBank,
    // SendAuctionHello, SendStablePet). The activations below were sent as
    // well, so each of those windows was opened twice - two MERCHANT_SHOWs,
    // two list requests. Kept only for this client's own gossip window.
    if (ui::frameXmlOwns(ui::UiElement::Gossip)) return;

    for (const auto& opt : currentGossip_.options) {
        if (opt.id != optionId) continue;
        LOG_INFO("  matched option: id=", opt.id, " icon=", (int)opt.icon, " text='", opt.text, "'");

        std::string text = opt.text;
        std::string textLower = text;
        std::transform(textLower.begin(), textLower.end(), textLower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        // Icon- and text-based NPC interaction fallbacks.
        // Use flags to avoid sending the same activation packet twice when
        // both the icon and text match (e.g., banker icon 6 + "deposit box").
        bool sentBanker = false;
        bool sentAuction = false;

        if (opt.icon == 6) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (icon) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentAuction && (text == "GOSSIP_OPTION_AUCTIONEER" || textLower.find("auction") != std::string::npos)) {
            auto pkt = AuctionHelloPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentAuction = true;
            LOG_INFO("Sent MSG_AUCTION_HELLO for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentBanker && (text == "GOSSIP_OPTION_BANKER" || textLower.find("deposit box") != std::string::npos)) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (text) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        const bool isVendor = (text == "GOSSIP_OPTION_VENDOR" ||
                               (textLower.find("browse") != std::string::npos &&
                                (textLower.find("goods") != std::string::npos || textLower.find("wares") != std::string::npos)));
        const bool isArmorer = (text == "GOSSIP_OPTION_ARMORER" || textLower.find("repair") != std::string::npos);
        if (isVendor || isArmorer) {
            if (isArmorer) {
                owner_.setVendorCanRepair(true);
            }
            auto pkt = ListInventoryPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            LOG_DEBUG("Sent CMSG_LIST_INVENTORY (gossip) to npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        // Only while this client draws the gossip window. It has no confirm
        // step, so matching the option's text and sending the activate is the
        // whole of how a hearthstone gets set there.
        //
        // FrameXML follows the server's own flow instead - the select goes out,
        // the server asks with SMSG_BINDER_CONFIRM, the popup answers - so
        // sending here as well would bind before the question was asked and
        // then ask it anyway.
        if (!ui::frameXmlOwns(ui::UiElement::Gossip) &&
            (textLower.find("make this inn your home") != std::string::npos ||
             textLower.find("set your home") != std::string::npos)) {
            auto bindPkt = BinderActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(bindPkt);
            LOG_INFO("Sent CMSG_BINDER_ACTIVATE for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        // Stable master detection
        if (text == "GOSSIP_OPTION_STABLE" ||
            textLower.find("stable") != std::string::npos ||
            textLower.find("my pet") != std::string::npos) {
            owner_.stableMasterGuidRef() = currentGossip_.npcGuid;
            owner_.stableWindowOpenRef() = false;
            auto listPkt = ListStabledPetsPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(listPkt);
            LOG_INFO("Sent MSG_LIST_STABLED_PETS (gossip) to npc=0x",
                     std::hex, currentGossip_.npcGuid, std::dec);
        }
        break;
    }
}

void QuestHandler::selectGossipQuest(uint32_t questId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !gossipWindowOpen_) return;

    const QuestLogEntry* activeQuest = nullptr;
    for (const auto& q : questLog_) {
        if (q.questId == questId) {
            activeQuest = &q;
            break;
        }
    }

    // Validate against server-auth quest slot fields
    auto questInServerLogSlots = [&](uint32_t qid) -> bool {
        if (qid == 0 || owner_.lastPlayerFieldsRef().empty()) return false;
        const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
        const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
        const uint16_t ufQuestEnd = ufQuestStart + 25 * qStride;
        for (const auto& [key, val] : owner_.lastPlayerFieldsRef()) {
            if (key < ufQuestStart || key >= ufQuestEnd) continue;
            if ((key - ufQuestStart) % qStride != 0) continue;
            if (val == qid) return true;
        }
        return false;
    };
    const bool questInServerLog = questInServerLogSlots(questId);
    if (questInServerLog && !activeQuest) {
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        requestQuestQuery(questId, false);
        for (const auto& q : questLog_) {
            if (q.questId == questId) {
                activeQuest = &q;
                break;
            }
        }
    }
    const bool activeQuestConfirmedByServer = questInServerLog;
    const bool shouldStartProgressFlow = activeQuestConfirmedByServer;
    if (shouldStartProgressFlow) {
        pendingTurnInQuestId_ = questId;
        pendingTurnInNpcGuid_ = currentGossip_.npcGuid;
        // No auto-advance past the progress panel: stock shows QUEST_PROGRESS
        // and waits for its Continue (CompleteQuest), even for a finished
        // quest. Skipping it hid the progress text and required items.
        pendingTurnInRewardRequest_ = false;
        auto packet = QuestgiverCompleteQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.getSocket()->send(packet);
    } else {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
        auto packet = owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildQueryQuestPacket(currentGossip_.npcGuid, questId)
            : QuestgiverQueryQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.getSocket()->send(packet);
    }
    // The gossip stays open until the answer replaces it: the stock
    // SelectGossipAvailableQuest / ActiveQuest (0x58b070) only sends the
    // query. Closing it here left the NPC with no window for the round trip,
    // and it said its farewell in the middle of the conversation.
}

bool QuestHandler::requestQuestQuery(uint32_t questId, bool force) {
    if (questId == 0 || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    if (!force && pendingQuestQueryIds_.count(questId)) return false;

    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_QUERY));
    pkt.writeUInt32(questId);
    owner_.getSocket()->send(pkt);
    pendingQuestQueryIds_.insert(questId);

    // WotLK supports CMSG_QUEST_POI_QUERY to get objective map locations.
    if (owner_.getPacketParsers() && owner_.getPacketParsers()->questLogStride() == 5) {
        const uint32_t wirePoiQuery = wireOpcode(Opcode::CMSG_QUEST_POI_QUERY);
        if (wirePoiQuery != 0xFFFF) {
            network::Packet poiPkt(static_cast<uint16_t>(wirePoiQuery));
            poiPkt.writeUInt32(1);          // count = 1
            poiPkt.writeUInt32(questId);
            owner_.getSocket()->send(poiPkt);
        }
    }
    return true;
}

void QuestHandler::acceptQuest() {
    if (!questDetailsOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t questId = currentQuestDetails_.questId;
    if (questId == 0) return;
    uint64_t npcGuid = currentQuestDetails_.npcGuid;
    if (pendingQuestAcceptTimeouts_.count(questId) != 0) {
        LOG_DEBUG("Ignoring duplicate quest accept while pending: questId=", questId);
        triggerQuestAcceptResync(questId, npcGuid, "duplicate-accept");
        dismissQuestDetails();
        return;
    }
    const bool inLocalLog = hasQuestInLog(questId);
    const int serverSlot = findQuestLogSlotIndexFromServer(questId);
    if (serverSlot >= 0) {
        LOG_INFO("Quest already in server quest log: questId=", questId,
                 " slot=", serverSlot, " inLocalLog=", inLocalLog);
        // Ensure it's in our local log even if server already has it
        addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);
        requestQuestQuery(questId, false);
        // Re-query NPC status from server
        if (npcGuid && owner_.getSocket()) {
            network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
            qsPkt.writeUInt64(npcGuid);
            owner_.getSocket()->send(qsPkt);
        }
        dismissQuestDetails();
        return;
    }
    if (inLocalLog) {
        LOG_WARNING("Quest accept local/server mismatch, allowing re-accept: questId=", questId);
        std::erase_if(questLog_, [&](const QuestLogEntry& q) { return q.questId == questId; });
    }

    // The server caps the quest log (20 slots in Vanilla, 25 in TBC/WotLK).
    // Accepting past the cap only earns SMSG_QUESTLOG_FULL, so stop it here
    // with a clearer message and no optimistic local entry to roll back.
    const int maxSlots = maxQuestLogSlots();
    if (static_cast<int>(questLog_.size()) >= maxSlots) {
        owner_.displayGameMessage("ERR_QUEST_LOG_FULL", "Your quest log is full.");
        return;
    }

    network::Packet packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildAcceptQuestPacket(npcGuid, questId)
        : QuestgiverAcceptQuestPacket::build(npcGuid, questId);
    owner_.getSocket()->send(packet);
    pendingQuestAcceptTimeouts_[questId] = 5.0f;
    pendingQuestAcceptNpcGuids_[questId] = npcGuid;

    // Immediately add to local quest log using available details
    addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);

    // No tracking here. Stock tracks a new quest from the interface:
    // QuestLog_OnEvent answers QUEST_ACCEPTED with AddQuestWatch only when
    // AUTO_QUEST_WATCH is "1" and fewer than MAX_WATCHABLE_QUESTS are watched
    // (questlogframe.lua). Tracking from here ignored that option and the cap.

    // No sound here: QUESTADDED plays when the quest reaches the server's
    // quest log (applyQuestStateFromFields), not on the click, which the
    // server may still refuse.

    dismissQuestDetails();

    // Re-query quest giver status so marker updates (! → ?)
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);
    }
}

void QuestHandler::dismissQuestDetails(bool announce) {
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    currentQuestDetails_ = QuestDetailsData{};
    // The interface's quest frame opens on QUEST_DETAIL and closes on this and
    // nothing else, so an accept that only reset the state left FrameXML's
    // window open over the world while this client's own window - which reads
    // isQuestDetailsOpen directly - closed.
    //
    // Not when the interface is the one closing it. QuestFrame_OnHide calls
    // CloseQuest, and answering that with the event that hides QuestFrame is
    // an echo: the panel is told to close by its own closing. It is absorbed
    // today only because OnHide is deferred a frame, and it is the reason
    // OnHide cannot simply be made synchronous.
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::declineQuest(bool announce) {
    // Declining and dismissing leave the client in the same state and always
    // did, character for character.
    dismissQuestDetails(announce);
}

void QuestHandler::closeGossip() {
    // GOSSIP_CLOSED only when a gossip was open (CloseGossip 0x58a550 returns
    // when the gossip guid is already 0): gossipframe.lua's OnHide calls
    // CloseGossip from the event's own HideUIPanel.
    const bool wasOpen = gossipWindowOpen_;
    gossipWindowOpen_ = false;
    if (wasOpen && owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
    questGreeting_.clear();
}

void QuestHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (itemGuid == 0 || questId == 0) {
        owner_.raiseUiError("Cannot start quest right now.");
        return;
    }
    // Send CMSG_QUESTGIVER_QUERY_QUEST with the item GUID as the "questgiver."
    // The server responds with SMSG_QUESTGIVER_QUEST_DETAILS which handleQuestDetails()
    // picks up and opens the Accept/Decline dialog.
    auto queryPkt = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildQueryQuestPacket(itemGuid, questId)
        : QuestgiverQueryQuestPacket::build(itemGuid, questId);
    owner_.getSocket()->send(queryPkt);
    LOG_INFO("offerQuestFromItem: itemGuid=0x", std::hex, itemGuid, std::dec,
             " questId=", questId);
}

void QuestHandler::completeQuest() {
    if (!questRequestItemsOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingTurnInQuestId_ = currentQuestRequestItems_.questId;
    pendingTurnInNpcGuid_ = currentQuestRequestItems_.npcGuid;
    pendingTurnInRewardRequest_ = false;

    LOG_DEBUG("completeQuest: questId=", currentQuestRequestItems_.questId,
              " completable=", currentQuestRequestItems_.isCompletable());

    // Stock CompleteQuest (0x58d610 -> 0x58cf40 -> 0x6d4dd0) sends
    // CMSG_QUESTGIVER_REQUEST_REWARD (0x18C, npc guid + quest id) - once, and
    // only while the progress panel is up. This sent COMPLETE_QUEST again,
    // which made the server answer with the progress panel a second time and
    // relied on an automatic REQUEST_REWARD after it.
    auto packet = QuestgiverRequestRewardPacket::build(
        currentQuestRequestItems_.npcGuid, currentQuestRequestItems_.questId);
    owner_.getSocket()->send(packet);
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
}

void QuestHandler::closeQuestRequestItems(bool announce) {
    pendingTurnInRewardRequest_ = false;
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
    // What closes the quest frame. It opens on QUEST_DETAIL, QUEST_PROGRESS or
    // QUEST_COMPLETE and hides on this, so without it the window would open and
    // stay open over whatever came next - unless the interface is the one
    // closing it, in which case see dismissQuestDetails.
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (!questOfferRewardOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Once per offer: the stock client ignores GetQuestReward while its last
    // choice is still unanswered (0x58cfa0, the flag at 0xc0d6ac).
    if (questRewardChosen_) return;
    uint64_t npcGuid = currentQuestOfferReward_.npcGuid;
    LOG_INFO("Completing quest: questId=", currentQuestOfferReward_.questId,
             " npcGuid=", npcGuid, " rewardIndex=", rewardIndex);
    auto packet = QuestgiverChooseRewardPacket::build(
        npcGuid, currentQuestOfferReward_.questId, rewardIndex);
    owner_.getSocket()->send(packet);
    pendingTurnInQuestId_ = 0;
    pendingTurnInNpcGuid_ = 0;
    // The frame stays until the server answers: GetQuestReward (Wow.exe
    // 0x58d620 -> 0x58cfa0) sends the choice and nothing else, and
    // SMSG_QUESTGIVER_QUEST_COMPLETE (0x6d0e10 -> 0x58ca70) closes it with
    // QUEST_FINISHED - or keeps it, when the quest leads straight on, for the
    // next quest's details. Closing here let the NPC go for a moment: its
    // farewell played on every hand-in, and again when the next quest's
    // window was closed.
    questRewardChosen_ = true;

    // Re-query quest giver status so markers update
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);
    }
}

void QuestHandler::closeQuestOfferReward(bool announce) {
    pendingTurnInRewardRequest_ = false;
    questRewardChosen_ = false;
    questOfferRewardOpen_ = false;
    currentQuestOfferReward_ = QuestOfferRewardData{};
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::abandonQuest(uint32_t questId) {
    clearPendingQuestAccept(questId);
    int localIndex = -1;
    for (size_t i = 0; i < questLog_.size(); ++i) {
        if (questLog_[i].questId == questId) {
            localIndex = static_cast<int>(i);
            break;
        }
    }

    int slotIndex = findQuestLogSlotIndexFromServer(questId);
    if (slotIndex < 0 && localIndex >= 0) {
        slotIndex = localIndex;
        LOG_WARNING("Abandon quest using local slot fallback: questId=", questId, " slot=", slotIndex);
    }

    if (slotIndex >= 0 && slotIndex < 25) {
        if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
            network::Packet pkt(wireOpcode(Opcode::CMSG_QUESTLOG_REMOVE_QUEST));
            pkt.writeUInt8(static_cast<uint8_t>(slotIndex));
            owner_.getSocket()->send(pkt);
        }
    } else {
        LOG_WARNING("Abandon quest failed: no quest-log slot found for questId=", questId);
    }

    if (localIndex >= 0) {
        questLog_.erase(questLog_.begin() + static_cast<ptrdiff_t>(localIndex));
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
            owner_.addonEventCallbackRef()("QUEST_REMOVED", {std::to_string(questId)});
        }
    }

    // Re-query nearby quest giver NPCs so markers refresh (e.g. "?" → "!")
    if (owner_.getSocket()) {
        for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
            if (entity->getType() != ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<Unit>(entity);
            if (unit->getNpcFlags() & NPC_FLAG_QUESTGIVER) {
                network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                qsPkt.writeUInt64(guid);
                owner_.getSocket()->send(qsPkt);
            }
        }
    }

    // Remove any quest POI minimap markers for this quest.
    gossipPois_.erase(
        std::remove_if(gossipPois_.begin(), gossipPois_.end(),
            [questId](const GossipPoi& p) { return p.data == questId; }),
        gossipPois_.end());
}

void QuestHandler::shareQuestWithParty(uint32_t questId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        owner_.raiseUiError("Cannot share quest: not in world.");
        return;
    }
    if (!owner_.isInGroup()) {
        owner_.raiseUiError("You are not in a party");   // ERR_QUEST_PUSH_NOT_IN_PARTY_S
        return;
    }
    network::Packet pkt(wireOpcode(Opcode::CMSG_PUSHQUESTTOPARTY));
    pkt.writeUInt32(questId);
    owner_.getSocket()->send(pkt);
    // No local line: the server answers per member with MSG_QUEST_PUSH_RESULT
    // ("Sharing quest with %s...", accepted, declined...).
}

void QuestHandler::acceptSharedQuest() {
    if (!pendingSharedQuest_ || !owner_.getSocket()) return;
    pendingSharedQuest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_CONFIRM_ACCEPT));
    pkt.writeUInt32(sharedQuestId_);
    owner_.getSocket()->send(pkt);
    // Tracked, if at all, by the interface on QUEST_ACCEPTED, as any accept.
}

void QuestHandler::declineSharedQuest() {
    pendingSharedQuest_ = false;
    // No response packet needed - just dismiss the UI
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int QuestHandler::maxQuestLogSlots() const {
    return isClassicLikeExpansion() ? 20 : 25;
}

const std::string& QuestHandler::getQuestSortName(uint32_t sortId) {
    static const std::string kEmpty;
    if (!questSortDbcLoaded_) {
        questSortDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestSort.dbc");
            // ID(0) + Name locstring whose English text sits at field 1
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    uint32_t id = dbc->getUInt32(i, 0);
                    std::string name = dbc->getString(i, 1);
                    if (id != 0 && !name.empty())
                        questSortNames_[id] = std::move(name);
                }
                LOG_INFO("Loaded ", questSortNames_.size(), " quest sort names");
            }
        }
    }
    auto it = questSortNames_.find(sortId);
    return it != questSortNames_.end() ? it->second : kEmpty;
}

uint32_t QuestHandler::getQuestRewardXP(int32_t level, uint32_t xpDifficulty) {
    // xpDifficulty is AzerothCore's RewardXPDifficulty, indexing the ten Exp
    // columns directly (Exp[d] = column d = row[d] here). Index 0's column is
    // zero at every level, and an unparsed layout leaves the index at 0 too, so
    // both correctly answer no reward XP. Valid difficulty indices are 0..9.
    if (level <= 0 || xpDifficulty > 9) return 0;
    if (!questXpDbcLoaded_) {
        questXpDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestXP.dbc");
            // Field 0 is the level; fields 1..10 are the ten difficulty columns.
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 11) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    const int32_t lvl = static_cast<int32_t>(dbc->getUInt32(i, 0));
                    std::array<uint32_t, 10> row{};
                    for (uint32_t c = 0; c < 10; ++c) row[c] = dbc->getUInt32(i, c + 1);
                    questXpByLevel_[lvl] = row;
                }
                LOG_INFO("Loaded ", questXpByLevel_.size(), " quest XP levels");
            }
        }
    }
    auto it = questXpByLevel_.find(level);
    if (it == questXpByLevel_.end()) return 0;
    // The same column AzerothCore's Quest::XPValue reads: Exp[xpDifficulty].
    return it->second[xpDifficulty];
}

int32_t QuestHandler::getQuestRewardReputation(int32_t valueId, int32_t override) {
    // Returned in hundredths, the unit GetQuestLogRewardFactionInfo answers in
    // and questinfo.lua divides back down by 100. The override is already in
    // hundredths and wins when set - most WotLK quests use it. Otherwise the
    // dbc: row 1 for a gain, row 2 for a loss, column at the absolute value
    // index, a whole reputation scaled up to hundredths. Matches the display
    // value in Player::RewardReputation.
    if (override != 0) return override;
    if (valueId == 0) return 0;
    if (!questFactionRewDbcLoaded_) {
        questFactionRewDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestFactionReward.dbc");
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 11) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    const int32_t id = static_cast<int32_t>(dbc->getUInt32(i, 0));
                    std::array<int32_t, 10> row{};
                    for (uint32_t c = 0; c < 10; ++c)
                        row[c] = static_cast<int32_t>(dbc->getUInt32(i, c + 1));
                    questFactionRew_[id] = row;
                }
            }
        }
    }
    const int32_t rowId = (valueId < 0) ? 2 : 1;
    const uint32_t field = static_cast<uint32_t>(std::abs(valueId));
    if (field > 9) return 0;
    auto it = questFactionRew_.find(rowId);
    if (it == questFactionRew_.end()) return 0;
    return it->second[field] * 100;
}

bool QuestHandler::hasQuestInLog(uint32_t questId) const {
    for (const auto& q : questLog_) {
        if (q.questId == questId) return true;
    }
    return false;
}

std::vector<std::pair<uint32_t, uint32_t>> QuestHandler::getQuestTimers() const {
    std::vector<std::pair<uint32_t, uint32_t>> timers;
    if (owner_.lastPlayerFieldsRef().empty()) return timers;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride == 0) return timers;
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    const uint16_t maxSlots = static_cast<uint16_t>(maxQuestLogSlots());

    // Walked in quest log order rather than slot order: the timer frame numbers
    // its rows the way the log lists the quests, and the two disagree once a
    // quest has been turned in and its slot reused.
    for (const auto& q : questLog_) {
        if (q.questId == 0) continue;
        for (uint16_t slot = 0; slot < maxSlots; ++slot) {
            const uint16_t idField = ufQuestStart + slot * qStride;
            auto idIt = owner_.lastPlayerFieldsRef().find(idField);
            if (idIt == owner_.lastPlayerFieldsRef().end() || idIt->second != q.questId) continue;

            // The expiry is the last field of the slot in every expansion:
            // Classic packs state and counts together and so uses three, TBC
            // four, WotLK five.
            const uint16_t timeField = static_cast<uint16_t>(idField + qStride - 1);
            auto tIt = owner_.lastPlayerFieldsRef().find(timeField);
            if (tIt == owner_.lastPlayerFieldsRef().end() || tIt->second == 0) break;
            const auto expiry = static_cast<uint64_t>(tIt->second);
            // An expired timer is reported as zero rather than dropped, so the
            // row stays put until the server removes the quest.
            timers.emplace_back(q.questId,
                                expiry > now ? static_cast<uint32_t>(expiry - now) : 0u);
            break;
        }
    }
    return timers;
}

int QuestHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questId == 0 || owner_.lastPlayerFieldsRef().empty()) return -1;
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    const uint16_t maxSlots = static_cast<uint16_t>(maxQuestLogSlots());
    for (uint16_t slot = 0; slot < maxSlots; ++slot) {
        const uint16_t idField = ufQuestStart + slot * qStride;
        auto it = owner_.lastPlayerFieldsRef().find(idField);
        if (it != owner_.lastPlayerFieldsRef().end() && it->second == questId) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void QuestHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questId == 0 || hasQuestInLog(questId)) return;
    QuestLogEntry entry;
    entry.questId = questId;
    entry.title = title.empty() ? ("Quest #" + std::to_string(questId))
                                : normalizeQuestText(title, true);
    entry.objectives = normalizeQuestText(objectives, false);
    questLog_.push_back(std::move(entry));
    // QUEST_ACCEPTED is not raised here. This runs on the accept click (before
    // the server agrees) and for every quest restored at login, and the event
    // means "a quest log slot took a new quest" - applyQuestStateFromFields
    // raises it then, with the quest's log row.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
    }
}

bool QuestHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    if (owner_.lastPlayerFieldsRef().empty()) return false;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    // Unmapped for this build's field table. applyQuestStateFromFields checks
    // this and this did not, so the slot arithmetic below ran from 0xFFFF and
    // every lookup missed - which reaches the same erase with an empty set.
    if (ufQuestStart == 0xFFFF) return false;
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;

    // Whether the quest area of the player's fields has arrived at all, which
    // is a different question from whether any player field has.
    //
    // This used to answer "done" on the first frame the field map was
    // non-empty, and the erase below then measured the local log against an
    // empty set and emptied it - a partial update block carrying a health or a
    // position change was enough. The resync runs once, so there was no second
    // chance: the quest log stayed empty for the session and the tracker with
    // it.
    //
    // Any key inside the quest slot range counts, including a zero one: a slot
    // written as zero is the server saying that slot is empty, which is real
    // information. Only the absence of every one of them means the block has
    // not come yet.
    const uint32_t questAreaEnd = static_cast<uint32_t>(ufQuestStart) + 25u * qStride;
    bool sawQuestArea = false;
    for (const auto& [key, value] : owner_.lastPlayerFieldsRef()) {
        (void)value;
        if (key >= ufQuestStart && key < questAreaEnd) { sawQuestArea = true; break; }
    }
    if (!sawQuestArea) return false;

    std::unordered_map<uint32_t, bool> serverQuestComplete;
    std::unordered_map<uint32_t, bool> serverQuestFailed;
    serverQuestComplete.reserve(25);
    serverQuestFailed.reserve(25);
    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = ufQuestStart + slot * qStride + 1;
        auto it = owner_.lastPlayerFieldsRef().find(idField);
        if (it == owner_.lastPlayerFieldsRef().end()) continue;
        uint32_t questId = it->second;
        if (questId == 0) continue;

        bool complete = false;
        bool failed = false;
        if (qStride >= 2) {
            auto stateIt = owner_.lastPlayerFieldsRef().find(stateField);
            if (stateIt != owner_.lastPlayerFieldsRef().end()) {
                complete = isQuestSlotComplete(qStride, stateIt->second);
                // The bit beside it, in the field already in hand.
                failed = isQuestSlotFailed(qStride, stateIt->second);
            }
        }
        serverQuestComplete[questId] = complete;
        serverQuestFailed[questId] = failed;
    }

    std::unordered_set<uint32_t> serverQuestIds;
    serverQuestIds.reserve(serverQuestComplete.size());
    for (const auto& [qid, _] : serverQuestComplete) serverQuestIds.insert(qid);

    const size_t localBefore = questLog_.size();
    std::erase_if(questLog_, [&](const QuestLogEntry& q) {
        return q.questId == 0 || serverQuestIds.count(q.questId) == 0;
    });
    const size_t removed = localBefore - questLog_.size();

    size_t added = 0;
    for (uint32_t questId : serverQuestIds) {
        if (hasQuestInLog(questId)) continue;
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        ++added;
    }

    size_t marked = 0;
    for (auto& quest : questLog_) {
        auto it = serverQuestComplete.find(quest.questId);
        if (it == serverQuestComplete.end()) continue;
        if (it->second && !quest.complete) {
            quest.complete = true;
            ++marked;
            LOG_DEBUG("Quest ", quest.questId, " marked complete from update fields");
        }
        // Assigned rather than latched: a quest that stops being failed -
        // the server clears the bit when a timer is restarted - has to stop
        // reading as failed, and only the server knows when that happens.
        auto fit = serverQuestFailed.find(quest.questId);
        const bool nowFailed = (fit != serverQuestFailed.end()) && fit->second;
        if (nowFailed != quest.failed) {
            quest.failed = nowFailed;
            LOG_DEBUG("Quest ", quest.questId, " failed=", nowFailed,
                      " from update fields");
        }
    }

    if (forceQueryMetadata) {
        for (uint32_t questId : serverQuestIds) {
            requestQuestQuery(questId, false);
        }
    }

    LOG_INFO("Quest log resync from server slots: server=", serverQuestIds.size(),
             " localBefore=", localBefore, " removed=", removed, " added=", added,
             " markedComplete=", marked);
    return true;
}

void QuestHandler::applyQuestStateFromFields(const FlatFieldMap& fields) {
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF) return;

    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride < 2) return;

    // Quest id to its complete/failed bits, as these fields describe the log
    // now. Compared with the previous call's snapshot below.
    std::unordered_map<uint32_t, uint32_t> slotStates;

    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = idField + 1;
        auto idIt = fields.find(idField);
        if (idIt == fields.end()) continue;
        uint32_t questId = idIt->second;
        if (questId == 0) continue;

        {
            auto sIt = fields.find(stateField);
            slotStates[questId] = (sIt != fields.end())
                ? (questSlotState(qStride, sIt->second) & 0x3u) : 0u;
        }

        // A quest the slots hold is in the log, however it got there: an
        // accept, a shared quest, an escort's party accept, a script or an
        // area trigger. This used to add one only behind a pending accept,
        // so every other way in left the quest invisible until relog.
        if (!hasQuestInLog(questId) &&
            (questSlotStatesSeeded_ || pendingQuestAcceptTimeouts_.count(questId) != 0)) {
            const bool offered = currentQuestDetails_.questId == questId;
            addQuestToLocalLogIfMissing(questId,
                offered ? currentQuestDetails_.title : "Quest #" + std::to_string(questId),
                offered ? currentQuestDetails_.objectives : "");
            requestQuestQuery(questId, false);
            // Re-query quest giver status for the NPC that gave us this quest
            auto pendingIt = pendingQuestAcceptNpcGuids_.find(questId);
            if (pendingIt != pendingQuestAcceptNpcGuids_.end() && pendingIt->second != 0 && owner_.getSocket()) {
                network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                qsPkt.writeUInt64(pendingIt->second);
                owner_.getSocket()->send(qsPkt);
            }
            clearPendingQuestAccept(questId);
        }
        // The accept is answered once the slots hold the quest. acceptQuest
        // adds the quest to the local log at once, so the branch above was
        // skipped for every ordinary accept and the pending timer always ran
        // out into a resync - whose quest query the server answers with
        // "You are already on that quest".
        clearPendingQuestAccept(questId);

        // VALUES updates are authoritative quest progress too. Some servers do
        // not emit (or clients may miss) a separate ADD_KILL notification for
        // every counter change, so refresh the tracker from the quest slot.
        for (auto& quest : questLog_) {
            if (quest.questId == questId) {
                applyPackedKillCountsFromFields(quest);
                break;
            }
        }

        auto stateIt = fields.find(stateField);
        if (stateIt == fields.end()) continue;
        bool serverComplete = isQuestSlotComplete(qStride, stateIt->second);
        if (!serverComplete) {
            // The server clears the bit again when a finished quest stops
            // being finished - a collected item sold or destroyed, required
            // money spent. Only a cleared bit that was seen set counts, so a
            // quest completed by a packet that the fields have not caught up
            // with yet is left alone.
            auto seenIt = questSlotStatesSeen_.find(questId);
            const bool wasServerComplete =
                seenIt != questSlotStatesSeen_.end() && (seenIt->second & 0x1u) != 0;
            if (!wasServerComplete) continue;
            for (auto& quest : questLog_) {
                if (quest.questId == questId && quest.complete) {
                    quest.complete = false;
                    LOG_INFO("Quest ", questId, " no longer complete from VALUES update field state");
                    announceQuestLogChanged(questId);
                    break;
                }
            }
            continue;
        }

        for (auto& quest : questLog_) {
            if (quest.questId == questId && !quest.complete) {
                quest.complete = true;
                LOG_INFO("Quest ", questId, " marked complete from VALUES update field state");
                announceQuestLogChanged(questId);
                break;
            }
        }
    }

    // Re-ask every visible questgiver whenever the log changed shape: a quest
    // added or removed, or one whose complete or failed bit moved.
    //
    // Most quests finish here and nowhere else. AzerothCore's
    // Player::CompleteQuest only sets QUEST_STATE_COMPLETE in the slot's
    // update field; SMSG_QUESTUPDATE_COMPLETE is sent for explore and event
    // quests alone, and SMSG_QUESTUPDATE_ADD_ITEM is never sent at all. The
    // server pushes SMSG_QUESTGIVER_STATUS_MULTIPLE by itself only on login,
    // level-up, turn-in and item destruction; for everything else the stock
    // client asks with CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY when its quest
    // log changes. Without this, a collect, talk-to, spell or reputation
    // quest finished in sight of its giver kept the grey "in progress" ?
    // until the giver respawned for the client.
    if (questSlotStatesSeeded_ && slotStates != questSlotStatesSeen_) {
        requeryNearbyQuestGiverStatus();
    }
    // QUESTADDED, the sound of ERR_QUEST_ACCEPTED_S, which Wow.exe raises when
    // a quest log slot's update field takes a new quest id (0x6df370) - the
    // server's word that the quest is in the log, whether it came from an
    // accept, a shared quest or an escort. Once however many arrived. Not on
    // the login snapshot, which is the log as it already was.
    if (questSlotStatesSeeded_) {
        for (const auto& [questId, state] : slotStates) {
            (void)state;
            if (questSlotStatesSeen_.count(questId) != 0) continue;
            // ERR_QUEST_ACCEPTED_S ("Quest accepted: %s", a system line with
            // the QUESTADDED sound) before QUEST_ACCEPTED, as 0x6df2b0 does.
            // A quest whose title has not arrived says it when the query does.
            for (auto& q : questLog_) {
                if (q.questId != questId) continue;
                if (isPlaceholderQuestTitle(q.title) || q.title.empty()) q.announceAcceptOnQuery = true;
                else owner_.displayGameMessage("ERR_QUEST_ACCEPTED_S", "Quest accepted: " + q.title, {q.title});
                break;
            }
            // QUEST_ACCEPTED(questLogIndex): the stock QuestLog_OnEvent hands
            // arg1 to AddQuestWatch, a log row, not a quest id.
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("QUEST_ACCEPTED",
                    {std::to_string(questLogIndexOf(questId))});
            }
        }

        // And the other way: a quest the slots held and no longer hold has
        // left the log - abandoned, turned in, failed out, or removed by the
        // server (a GM's .quest remove, a script). The client's log is the
        // slots; keeping an entry the server dropped showed a quest, with its
        // old progress, that the server no longer knew.
        bool removed = false;
        for (const auto& [questId, state] : questSlotStatesSeen_) {
            (void)state;
            if (slotStates.count(questId) != 0) continue;
            if (pendingQuestAcceptTimeouts_.count(questId) != 0) continue;
            auto it = std::find_if(questLog_.begin(), questLog_.end(),
                [&](const QuestLogEntry& q) { return q.questId == questId; });
            if (it == questLog_.end()) continue;
            questLog_.erase(it);
            owner_.setQuestTracked(questId, false);
            removed = true;
            LOG_INFO("Quest ", questId, " left the server's quest log slots - removed");
        }
        if (removed && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
        }
    }
    questSlotStatesSeen_ = std::move(slotStates);
    questSlotStatesSeeded_ = true;
}

void QuestHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (owner_.lastPlayerFieldsRef().empty()) return;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF) return;

    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride < 3) return;

    int slot = findQuestLogSlotIndexFromServer(quest.questId);
    if (slot < 0) return;

    const uint16_t countField1 = ufQuestStart + static_cast<uint16_t>(slot) * qStride
                               + questObjectiveCountFieldOffset(qStride);
    const uint16_t countField2 = (qStride >= 5)
                                     ? static_cast<uint16_t>(countField1 + 1)
                                     : static_cast<uint16_t>(0xFFFF);

    auto f1It = owner_.lastPlayerFieldsRef().find(countField1);
    if (f1It == owner_.lastPlayerFieldsRef().end()) return;
    const uint32_t packed1 = f1It->second;

    uint32_t packed2 = 0;
    if (countField2 != 0xFFFF) {
        auto f2It = owner_.lastPlayerFieldsRef().find(countField2);
        if (f2It != owner_.lastPlayerFieldsRef().end()) packed2 = f2It->second;
    }

    const auto counts = decodeQuestObjectiveCounts(qStride, packed1, packed2);

    // Apply kill objective counts (indices 0-3).
    for (int i = 0; i < 4; ++i) {
        const auto& obj = quest.killObjectives[i];
        if (obj.npcOrGoId == 0 || obj.required == 0) continue;
        const uint32_t entryKey = static_cast<uint32_t>(
            obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
        if (counts[i] == 0 && quest.killCounts.count(entryKey)) continue;
        const uint32_t current = std::min(counts[i], obj.required);
        quest.killCounts[entryKey] = {current, obj.required};
        LOG_DEBUG("Quest ", quest.questId, " objective[", i, "]: npcOrGo=",
                  obj.npcOrGoId, " count=", current, "/", obj.required);
    }

    // Item collection progress is not stored in PLAYER_QUEST_LOG counters;
    // those fields contain the four creature/GO counters in every expansion.
    for (int i = 0; i < 6; ++i) {
        const auto& obj = quest.itemObjectives[i];
        if (obj.itemId == 0 || obj.required == 0) continue;
        quest.requiredItemCounts.emplace(obj.itemId, obj.required);
    }
}

void QuestHandler::clearPendingQuestAccept(uint32_t questId) {
    pendingQuestAcceptTimeouts_.erase(questId);
    pendingQuestAcceptNpcGuids_.erase(questId);
}

void QuestHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questId == 0 || !owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;

    LOG_INFO("Quest accept resync: questId=", questId, " reason=", reason ? reason : "unknown");
    requestQuestQuery(questId, true);

    // Only the quest's data and the giver's marker. No CMSG_QUESTGIVER_QUERY_QUEST:
    // stock sends that only when the player picks the quest from a giver, and
    // for a quest already in the log the server answers it with
    // QUEST_INVALID 13, which the player reads as "You are already on that
    // quest" seconds after accepting.
    if (npcGuid != 0) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);
    }
}

// ---------------------------------------------------------------------------
// Packet handlers
// ---------------------------------------------------------------------------

void QuestHandler::handleGossipMessage(network::Packet& packet) {
    bool ok = owner_.getPacketParsers() ? owner_.getPacketParsers()->parseGossipMessage(packet, currentGossip_)
                                    : GossipMessageParser::parse(packet, currentGossip_);
    if (!ok) return;
    if (questDetailsOpen_) return; // Don't reopen gossip while viewing quest
    gossipWindowOpen_ = true;
    // Gossip carries its text as an npc-text id, not inline, so any greeting
    // left over from a quest list belongs to a different window.
    questGreeting_.clear();

    // Asked for before the window is announced rather than after it.
    //
    // This client's own gossip window read the greeting again on every frame
    // it drew, so it collected the reply whenever it landed and the order here
    // could not matter. An interface driven by events reads it once, when it
    // is told the window opened - so announcing first meant it always read an
    // empty cache and drew the NPC dialog blank, and the text arriving a
    // moment later had nobody left to tell.
    //
    // Moving it earlier is enough on any NPC talked to twice, where the text
    // is already cached and the query is not sent at all. The first visit
    // still needs the arrival to say so, which handleNpcTextUpdate now does.
    gossipShowWaitSec_ = 0.0f;
    if (currentGossip_.titleTextId > 0 && !npcTextCache_.count(currentGossip_.titleTextId)) {
        uint32_t wireOp = wireOpcode(Opcode::CMSG_NPC_TEXT_QUERY);
        if (wireOp != 0xFFFF && owner_.getSocket()) {
            network::Packet qPkt(static_cast<uint16_t>(wireOp));
            qPkt.writeUInt32(currentGossip_.titleTextId);
            qPkt.writeUInt64(currentGossip_.npcGuid);
            owner_.getSocket()->send(qPkt);
            // GOSSIP_SHOW waits for the text (handleNpcTextUpdate). It used
            // to fire now and again when the text landed, and GossipFrame
            // answers every GOSSIP_SHOW of a one-option NPC with
            // SelectGossipOption(1) (gossipframe.lua GossipFrame_OnEvent) -
            // so each first visit to a vendor, banker or flight master sent
            // the option twice and opened its window twice.
            gossipShowWaitSec_ = 1.0f;
        }
    }

    if (gossipShowWaitSec_ <= 0.0f && owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GOSSIP_SHOW", {});
    owner_.closeVendor(); // Close vendor if gossip opens

    // Classify gossip quests and update quest log + overhead NPC markers.
    classifyGossipQuests(true);

    // The greeting voice plays on the interaction (GameHandler::interactWithNpc),
    // not here: submenus re-send this packet and replayed it each time.
}

void QuestHandler::handleQuestgiverQuestList(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;

    GossipMessageData data;
    data.npcGuid = packet.readUInt64();
    data.menuId = 0;
    data.titleTextId = 0;

    // What the quest giver says above its list. Read and discarded before, so
    // GetGreetingText had nothing to answer with and the greeting panel was
    // blank over a list that was otherwise correct.
    questGreeting_ = normalizeWowTextTokens(packet.readString());
    if (packet.hasRemaining(8)) {
        (void)packet.readUInt32(); // emoteDelay / unk
        (void)packet.readUInt32(); // emote / unk
    }

    // questCount is uint8 in all WoW versions for SMSG_QUESTGIVER_QUEST_LIST.
    uint32_t questCount = 0;
    if (packet.hasRemaining(1)) {
        questCount = packet.readUInt8();
    }

    const bool hasQuestFlagsField = !isClassicLikeExpansion() && !isActiveExpansion("tbc");

    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        GossipQuestItem q;
        q.questId = packet.readUInt32();
        q.questIcon = packet.readUInt32();
        q.questLevel = static_cast<int32_t>(packet.readUInt32());

        if (hasQuestFlagsField && packet.hasRemaining(5)) {
            q.questFlags = packet.readUInt32();
            q.isRepeatable = packet.readUInt8();
        } else {
            q.questFlags = 0;
            q.isRepeatable = 0;
        }
        q.title = normalizeWowTextTokens(packet.readString());
        if (q.questId != 0) {
            data.quests.push_back(std::move(q));
        }
    }

    currentGossip_ = std::move(data);
    gossipWindowOpen_ = true;
    // QUEST_GREETING, not GOSSIP_SHOW. This is SMSG_QUESTGIVER_QUEST_LIST - a
    // quest giver listing what it has, which the greeting frame answers.
    // GOSSIP_SHOW belongs to SMSG_GOSSIP_MESSAGE and opens the gossip frame
    // instead, so firing it here put the wrong panel over the list. Both would
    // be worse: each frame shows itself on its own event, so sending both shows
    // two.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_GREETING", {});
    owner_.closeVendor();

    classifyGossipQuests(false);

    LOG_INFO("Questgiver quest list: npc=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " quests=", currentGossip_.quests.size());
}

// Shared quest-icon classification for gossip windows. Derives NPC quest status
// from icon values so overhead markers stay aligned with what the NPC offers.
// updateQuestLog: if true, also patches quest log completion state (gossip handler
// does this because it has the freshest data; quest-list handler skips it because
// completion updates arrive via separate packets).
void QuestHandler::classifyGossipQuests(bool updateQuestLog) {
    // Icon values come from the server's QUEST_STATUS enum, not a client constant,
    // so these magic numbers are protocol-defined and stable across expansions.
    auto isCompletable = [](uint32_t icon) { return icon == 5 || icon == 6 || icon == 10; };
    auto isIncomplete  = [](uint32_t icon) { return icon == 3 || icon == 4; };
    auto isAvailable   = [](uint32_t icon) { return icon == 2 || icon == 7 || icon == 8; };

    bool hasAvailable = false, hasReward = false, hasIncomplete = false;
    for (const auto& q : currentGossip_.quests) {
        bool completable = isCompletable(q.questIcon);
        bool incomplete  = isIncomplete(q.questIcon);
        bool available   = isAvailable(q.questIcon);
        hasAvailable |= available;
        hasReward |= completable;
        hasIncomplete |= incomplete;

        if (updateQuestLog) {
            for (auto& entry : questLog_) {
                if (entry.questId == q.questId) {
                    // Not entry.complete: the icon cannot say it. AzerothCore
                    // sends 4 for a quest in progress and a finished one alike,
                    // so this flipped finished quests back to incomplete every
                    // time their NPC was spoken to. The slot fields own it.
                    entry.title = normalizeQuestText(q.title, true);
                    if (q.questLevel > 0 && entry.level <= 0) entry.level = q.questLevel;
                    break;
                }
            }
        }
    }
    if (currentGossip_.npcGuid != 0) {
        QuestGiverStatus status = QuestGiverStatus::NONE;
        if (hasReward) status = QuestGiverStatus::REWARD;
        else if (hasAvailable) status = QuestGiverStatus::AVAILABLE;
        else if (hasIncomplete) status = QuestGiverStatus::INCOMPLETE;

        // The overhead mark, but only where the server has not already said.
        //
        // SMSG_QUESTGIVER_STATUS carries a DIALOG_STATUS worked out for exactly
        // this question, and the client re-asks for it whenever a quest is
        // taken, advanced or handed in. What is derived here comes from the
        // per-quest icons in the list, which cannot say as much: the other
        // reader of that same field in this client - GetNumGossipAvailableQuests
        // - treats 4 as "active" and everything else as available, and a quest
        // that is finished and one that is not both arrive as active. Nothing
        // in the icons distinguishes them, so no list here can produce REWARD.
        //
        // Overwriting with that turned a turn-in's gold ? grey, or replaced it
        // with the ! of whatever else the NPC was offering - and it happened
        // only once the player had opened the window, which is why the mark
        // looked right until it was looked at.
        //
        // So this fills a gap rather than replacing an answer. Nothing and
        // NONE are both gaps; a real status is not.
        if (status != QuestGiverStatus::NONE) {
            const auto known = npcQuestStatus_.find(currentGossip_.npcGuid);
            if (known == npcQuestStatus_.end() ||
                known->second == QuestGiverStatus::NONE) {
                npcQuestStatus_[currentGossip_.npcGuid] = status;
            } else if (known->second != status) {
                // Once per questgiver, and only on a disagreement. The icon
                // values are the part still in doubt - two places in this
                // client read them differently - so this prints the ones that
                // actually arrived beside the status the server sent for the
                // same NPC. A run with no such line is a run where the two
                // agreed.
                static std::set<uint64_t> said;
                if (said.insert(currentGossip_.npcGuid).second) {
                    std::string icons;
                    for (const auto& q : currentGossip_.quests) {
                        if (!icons.empty()) icons += ",";
                        icons += std::to_string(q.questIcon);
                    }
                    LOG_WARNING("Questgiver 0x", std::hex, currentGossip_.npcGuid,
                                std::dec, ": server status ",
                                static_cast<int>(known->second),
                                ", quest icons (", icons, ") read as ",
                                static_cast<int>(status),
                                " - the server's is kept");
                }
            }
        }
    }
}

void QuestHandler::handleGossipComplete(network::Packet& packet) {
    (void)packet;

    // No farewell here: the NPC says it when it stops being the interact
    // target (GameHandler::updateInteractTarget, 0x720860), which also covers
    // walking away and Escape, and does not say it when another window from
    // the same NPC takes over.

    gossipWindowOpen_ = false;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
    questGreeting_.clear();
}

void QuestHandler::handleNpcTextUpdate(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t textId = packet.readUInt32();
    // 8 text entries: each has probability(4) + text0 + text1 + lang(4) + 6 emote fields(24)
    for (int i = 0; i < 8; ++i) {
        if (!packet.hasRemaining(4)) break;
        /*float prob =*/ packet.readFloat();
        if (!packet.hasData()) break;
        std::string text0 = packet.readString();
        if (!packet.hasData()) break;
        std::string text1 = packet.readString();
        if (!packet.hasRemaining(28)) break; // lang(4) + 6×emote(4) = 28
        packet.setReadPos(packet.getReadPos() + 28);
        if (!text0.empty() && !npcTextCache_.count(textId)) {
            npcTextCache_[textId] = std::move(text0);
        } else if (!text1.empty() && !npcTextCache_.count(textId)) {
            npcTextCache_[textId] = std::move(text1);
        }
    }
    LOG_DEBUG("NPC text update: id=", textId,
             " cached=", npcTextCache_.count(textId) ? "yes" : "no");

    // The window is already open and waiting on exactly this text, so say so.
    //
    // A cache filled without an event is this client's oldest shape of bug and
    // it reads as "it only works the second time": the greeting is there for
    // every later visit to the same NPC and blank on the first, because the
    // first is the only one where the text arrives after the window did.
    // GOSSIP_SHOW is the event the interface redraws the greeting on, and
    // firing it again is what a second look at an open window means.
    // Once: only while the window's GOSSIP_SHOW is still being held for it.
    if (gossipShowWaitSec_ > 0.0f && gossipWindowOpen_ &&
        currentGossip_.titleTextId == textId && owner_.addonEventCallbackRef()) {
        gossipShowWaitSec_ = 0.0f;
        owner_.addonEventCallbackRef()("GOSSIP_SHOW", {});
    }
}

const std::string& QuestHandler::getNpcText(uint32_t textId) const {
    static const std::string empty;
    auto it = npcTextCache_.find(textId);
    return (it != npcTextCache_.end()) ? it->second : empty;
}

void QuestHandler::handleQuestPoiQueryResponse(network::Packet& packet) {
    // WotLK 3.3.5a SMSG_QUEST_POI_QUERY_RESPONSE format:
    //   uint32 questCount
    //   per quest:
    //     uint32 questId
    //     uint32 poiCount
    //     per poi:
    //       uint32 poiId
    //       int32  objIndex      (-1 = no specific objective)
    //       uint32 mapId
    //       uint32 areaId
    //       uint32 floorId
    //       uint32 unk1
    //       uint32 unk2
    //       uint32 pointCount
    //       per point: int32 x, int32 y
    if (!packet.hasRemaining(4)) return;
    const uint32_t questCount = packet.readUInt32();
    for (uint32_t qi = 0; qi < questCount; ++qi) {
        if (!packet.hasRemaining(8)) return;
        const uint32_t questId  = packet.readUInt32();
        const uint32_t poiCount = packet.readUInt32();

        // Remove any previously added POI markers for this quest
        gossipPois_.erase(
            std::remove_if(gossipPois_.begin(), gossipPois_.end(),
                [questId](const GossipPoi& p) {
                    return p.data == questId;
                }),
            gossipPois_.end());

        // Find the quest title for the marker label.
        std::string questTitle;
        for (const auto& q : questLog_) {
            if (q.questId == questId) { questTitle = q.title; break; }
        }

        // The whole quest as the client keeps it (0x005E7370), every map
        // included - the world map asks about whichever map it is showing,
        // which is often not the one the player stands on.
        QuestPoiSet poiSet;
        poiSet.questId = questId;

        for (uint32_t pi = 0; pi < poiCount; ++pi) {
            if (!packet.hasRemaining(32)) return;
            QuestPoiArea area;
            area.poiId = static_cast<int32_t>(packet.readUInt32());
            const int32_t objIndex = static_cast<int32_t>(packet.readUInt32());
            const uint32_t mapId    = packet.readUInt32();
            area.objectiveIndex = objIndex;
            area.mapId = mapId;
            area.worldMapAreaId = packet.readUInt32();
            area.floor = packet.readUInt32();
            area.priority = packet.readUInt32();   // unk3
            area.flags = packet.readUInt32();      // unk4
            const uint32_t pointCount = packet.readUInt32();
            if (packet.getRemainingSize() < static_cast<size_t>(pointCount) * 8) return;
            float sumX = 0.0f, sumY = 0.0f;
            int64_t isumX = 0, isumY = 0;
            std::vector<std::pair<float, float>> outline;
            outline.reserve(pointCount);
            area.points.reserve(pointCount);
            for (uint32_t pt = 0; pt < pointCount; ++pt) {
                const int32_t px = static_cast<int32_t>(packet.readUInt32());
                const int32_t py = static_cast<int32_t>(packet.readUInt32());
                sumX += static_cast<float>(px);
                sumY += static_cast<float>(py);
                isumX += px;
                isumY += py;
                area.points.emplace_back(px, py);
                // Canonical, the same swap the centroid below makes.
                outline.emplace_back(static_cast<float>(py), static_cast<float>(px));
            }
            // Integer mean, and only divided when there is more than one point.
            area.centerX = static_cast<int32_t>(pointCount > 1 ? isumX / static_cast<int64_t>(pointCount) : isumX);
            area.centerY = static_cast<int32_t>(pointCount > 1 ? isumY / static_cast<int64_t>(pointCount) : isumY);
            poiSet.areas.push_back(area);
            if (pointCount == 0) continue;
            // Skip POIs for maps other than the player's current map.
            if (mapId != owner_.currentMapIdRef()) continue;
            GossipPoi poi;
            // QuestPOIPoint uses the opposite axis order from the canonical
            // world coordinates used by movement and the map renderer. Store
            // it canonically here so canonicalToRender() does not place every
            // objective far outside its zone bounds.
            poi.x    = sumY / static_cast<float>(pointCount);
            poi.y    = sumX / static_cast<float>(pointCount);
            poi.icon = 6;  // generic quest POI icon
            poi.data = questId;
            poi.questObjectiveIndex = objIndex;
            poi.name = questTitle.empty() ? "Quest objective" : questTitle;
            // Three points is the least that encloses anything. Fewer is a
            // line or a dot, which the centroid already says.
            if (outline.size() >= 3) poi.area = std::move(outline);
            LOG_DEBUG("Quest POI: questId=", questId, " mapId=", mapId,
                      " centroid=(", poi.x, ",", poi.y, ") title=", poi.name);
            if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
            gossipPois_.push_back(std::move(poi));
        }
        // One entry per quest, replaced when it is sent again. The client
        // holds 25; a quest log holds 25 too, so the oldest goes only when a
        // twenty-sixth quest arrives.
        auto existing = std::find_if(questPoiSets_.begin(), questPoiSets_.end(),
            [questId](const QuestPoiSet& s) { return s.questId == questId; });
        if (existing != questPoiSets_.end()) {
            *existing = std::move(poiSet);
        } else {
            if (questPoiSets_.size() >= 25) questPoiSets_.erase(questPoiSets_.begin());
            questPoiSets_.push_back(std::move(poiSet));
        }
    }
    // The tracker draws its POI marks from this and had no way to know the
    // points had arrived - the query is answered well after the row is drawn.
    //
    // Fired for the tracker, not the map: this client owns the world map and
    // its POI layer, but the quest tracker is a separate frame and
    // QuestPOIGetIconInfo already answers it.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_POI_UPDATE", {});
    }
}

void QuestHandler::handleQuestDetails(network::Packet& packet) {
    QuestDetailsData data;
    bool ok = owner_.getPacketParsers() ? owner_.getPacketParsers()->parseQuestDetails(packet, data)
                                    : QuestDetailsParser::parse(packet, data);
    if (!ok) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_QUEST_DETAILS");
        return;
    }
    currentQuestDetails_ = data;
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        if (!data.title.empty() && (isPlaceholderQuestTitle(q.title) || data.title.size() >= q.title.size())) {
            q.title = normalizeQuestText(data.title, true);
        }
        if (!data.objectives.empty() && (q.objectives.empty() || data.objectives.size() > q.objectives.size())) {
            q.objectives = normalizeQuestText(data.objectives, false);
        }
        break;
    }
    // Pre-fetch item info for all reward items
    for (const auto& item : data.rewardChoiceItems) owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.rewardItems)       owner_.queryItemInfo(item.itemId, 0);
    // Open now, and let this client's own window wait for the item names.
    //
    // These two used to be one thing: the window was held shut for a hundred
    // milliseconds so the queries above could answer, and "open" was what the
    // interface's bindings read as well. See isQuestDetailsOpen.
    questDetailsOpen_ = true;
    questDetailsOpenTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    gossipWindowOpen_ = false;
    // The next quest of a chain takes over a reward frame kept for it.
    questOfferRewardOpen_ = false;
    questRewardChosen_ = false;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_DETAIL", {});
}

void QuestHandler::handleQuestRequestItems(network::Packet& packet) {
    QuestRequestItemsData data;
    if (!QuestRequestItemsParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_REQUEST_ITEMS");
        return;
    }
    clearPendingQuestAccept(data.questId);

    if (pendingTurnInRewardRequest_ &&
        data.questId == pendingTurnInQuestId_ &&
        data.npcGuid == pendingTurnInNpcGuid_ &&
        data.isCompletable() &&
        owner_.getSocket()) {
        auto rewardReq = QuestgiverRequestRewardPacket::build(data.npcGuid, data.questId);
        owner_.getSocket()->send(rewardReq);
        pendingTurnInRewardRequest_ = false;
    }

    currentQuestRequestItems_ = data;
    questRequestItemsOpen_ = true;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};

    // The panel shown when a quest is handed back before it is finished, or
    // when it asks for something in return. Its siblings QUEST_DETAIL and
    // QUEST_COMPLETE were both fired and this one was not, so the original
    // interface had the text for this step and no event to draw it on.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_PROGRESS", {});

    // Query item names for required items
    for (const auto& item : data.requiredItems) {
        owner_.queryItemInfo(item.itemId, 0);
    }

    // The turn-in's required items, for the item counters. Nothing else in
    // the log entry changes: the progress panel draws this packet's own text
    // (GetProgressText, GetQuestItemInfo("required")), and the log keeps the
    // quest's objectives - this used to overwrite them with a made-up
    // "Required items: - X x10" block and set completion from the panel,
    // which the quest slot fields own.
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        q.requiredItemCounts.clear();
        for (const auto& item : data.requiredItems) q.requiredItemCounts[item.itemId] = item.count;
        break;
    }
}

void QuestHandler::handleQuestOfferReward(network::Packet& packet) {
    QuestOfferRewardData data;
    if (!QuestOfferRewardParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_OFFER_REWARD");
        return;
    }
    clearPendingQuestAccept(data.questId);
    LOG_INFO("Quest offer reward: questId=", data.questId, " title=\"", data.title, "\"");
    if (pendingTurnInQuestId_ == data.questId) {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
    }
    currentQuestOfferReward_ = data;
    questOfferRewardOpen_ = true;
    questRequestItemsOpen_ = false;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_COMPLETE", {});

    // Query item names for reward items
    for (const auto& item : data.choiceRewards)
        owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.fixedRewards)
        owner_.queryItemInfo(item.itemId, 0);
}

void QuestHandler::handleQuestConfirmAccept(network::Packet& packet) {
    size_t rem = packet.getRemainingSize();
    if (rem < 4) return;

    sharedQuestId_    = packet.readUInt32();
    sharedQuestTitle_ = packet.readString();
    if (packet.hasRemaining(8)) {
        sharedQuestSharerGuid_ = packet.readUInt64();
    }

    sharedQuestSharerName_.clear();
    auto entity = owner_.getEntityManager().getEntity(sharedQuestSharerGuid_);
    if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
        sharedQuestSharerName_ = unit->getName();
    }
    if (sharedQuestSharerName_.empty()) {
        auto nit = owner_.getPlayerNameCache().find(sharedQuestSharerGuid_);
        if (nit != owner_.getPlayerNameCache().end())
            sharedQuestSharerName_ = nit->second;
    }
    if (sharedQuestSharerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(sharedQuestSharerGuid_));
        sharedQuestSharerName_ = tmp;
    }

    pendingSharedQuest_ = true;
    // No chat line: this is the escort/party-accept prompt ("%s is starting
    // %s"), and the popup below is all stock shows for it.
    // Who and what, in that order: uiparent.lua hands both straight to
    // StaticPopup_Show, whose text is "%s is starting %s". It picks between
    // two popups on whether the log is full, and answers the yes with
    // ConfirmAcceptQuest.
    owner_.fireAddonEvent("QUEST_ACCEPT_CONFIRM",
                          {sharedQuestSharerName_, sharedQuestTitle_});
    LOG_INFO("SMSG_QUEST_CONFIRM_ACCEPT: questId=", sharedQuestId_,
             " title=", sharedQuestTitle_, " sharer=", sharedQuestSharerName_);
}

} // namespace game
} // namespace wowee
