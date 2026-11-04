// lua_quest_api.cpp - Quest log, skills, talents, glyphs, and achievements Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "core/local_time.hpp"
#include "game/item_text.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "game/auction_filters.hpp"
#include "game/game_utils.hpp"
#include "game/packed_time.hpp"
#include "game/quest_progress.hpp"
#include "game/quest_objectives.hpp"
#include "game/spell_handler.hpp"
#include "game/stock_trade_skill.hpp"
#include "addons/world_map_api.hpp"
#include "ui/chat/chat_utils.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

// ── The quest log is a list of headers with quests under them ───────────────
//
// WoW's quest log is grouped: a zone header, the quests in that zone, the next
// header, and so on - and every index-taking quest function counts the headers
// as rows. This log was flat, so it listed 23 quests in server order with no
// grouping at all, ExpandQuestHeader/CollapseQuestHeader were no-ops, and
// GetQuestLogTitle answered false for isHeader on every row.
//
// The grouping key is the quest's zoneOrSort: an AreaTable id when positive
// (the zone the quest belongs to) and a QuestSort id negated when negative
// (Epic, Seasonal, a class or a profession). Zero means the query response has
// not arrived yet, and those wait under their own header rather than being
// dropped.
//
// Everything that takes a quest log index goes through questRows() rather than
// indexing getQuestLog() directly, because with headers in the list the two are
// no longer the same number: quest 3 of the log can be row 5 of the display.
namespace {

struct QuestRow {
    bool isHeader = false;
    int32_t group = 0;                              ///< zoneOrSort of the group
    std::string headerTitle;                        ///< headers only
    const game::GameHandler::QuestLogEntry* quest = nullptr;     ///< quests only
};

/// Headers the player has collapsed, by group key. Interface state, so it lives
/// here rather than in the model - and it is deliberately not persisted, which
/// is what WoW does with a fresh session too.
std::set<int32_t>& collapsedQuestGroups() {
    static std::set<int32_t> collapsed;
    return collapsed;
}

/// What a group's header row is called.
std::string questGroupTitle(game::GameHandler* gh, int32_t group) {
    if (!gh) return "Miscellaneous";
    if (group > 0) {
        std::string name = gh->getAreaName(static_cast<uint32_t>(group));
        if (!name.empty()) return name;
    } else if (group < 0) {
        std::string name = gh->getQuestSortName(static_cast<uint32_t>(-group));
        if (!name.empty()) return name;
    }
    // A quest whose query response has not landed has no zone yet, and one
    // whose zone is not in the file still has to sit somewhere nameable.
    return "Miscellaneous";
}

/// The display list: one row per header, then the rows of its quests unless the
/// header is collapsed. Rebuilt per call - the log holds tens of entries, and a
/// cache here would have to be invalidated by every quest update, every query
/// response and every collapse.
std::vector<QuestRow> questRows(game::GameHandler* gh) {
    std::vector<QuestRow> rows;
    if (!gh) return rows;
    const auto& ql = gh->getQuestLog();

    // Group, keeping each group's quests in the order the log holds them.
    std::map<int32_t, std::vector<const game::GameHandler::QuestLogEntry*>> groups;
    for (const auto& q : ql) {
        if (q.questId == 0) continue;
        groups[q.zoneOrSort].push_back(&q);
    }

    // Zones first and in name order, then the QuestSort groups (Epic, class,
    // profession), then the not-yet-known ones - which is the order the real
    // log reads in, and stable so the list does not reshuffle under the cursor
    // as query responses land.
    struct Group { int32_t key; std::string title; int rank; };
    std::vector<Group> ordered;
    ordered.reserve(groups.size());
    for (const auto& [key, _] : groups) {
        const int rank = (key > 0) ? 0 : (key < 0 ? 1 : 2);
        ordered.push_back({key, questGroupTitle(gh, key), rank});
    }
    std::sort(ordered.begin(), ordered.end(), [](const Group& a, const Group& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.title != b.title) return a.title < b.title;
        return a.key < b.key;
    });

    for (const Group& g : ordered) {
        QuestRow header;
        header.isHeader = true;
        header.group = g.key;
        header.headerTitle = g.title;
        rows.push_back(std::move(header));
        if (collapsedQuestGroups().count(g.key)) continue;
        for (const auto* q : groups[g.key]) {
            QuestRow row;
            row.group = g.key;
            row.quest = q;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

/// Server text as the player should read it.
///
/// Quest text arrives with the reader left blank - $n for their name, $c for
/// class, $r for race, $g male:female; for a phrase that splits on gender - and
/// the client is what fills them in. The quest giver's own panels have resolved
/// them since pushQuestText was written, and the quest log did not: the same
/// quest greeted the player by name while it was being offered and by the two
/// characters "$N" once it was accepted, which is where it is read most.
std::string forPlayer(lua_State* L, const std::string& text) {
    if (text.empty()) return text;
    auto* gh = getGameHandler(L);
    return gh ? ui::chat_utils::replaceGenderPlaceholders(text, *gh) : text;
}

}  // namespace

// Declared in lua_api_helpers.hpp; defined here because questRows above is the
// one list every quest log index is counted against.
int questLogRowForQuest(game::GameHandler* gh, uint32_t questId) {
    if (!gh || questId == 0) return 0;
    const auto rows = questRows(gh);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].isHeader && rows[i].quest && rows[i].quest->questId == questId) {
            return static_cast<int>(i) + 1;  // the interface counts from one
        }
    }
    return 0;
}

/// QuestInfo.dbc's name for a quest type id (column 1), or "" for none.
static std::string questTypeName(game::GameHandler* gh, uint32_t type) {
    if (!gh || type == 0) return {};
    static std::unordered_map<uint32_t, std::string> names;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh->services().assetManager;
        if (!am || !am->isInitialized()) return {};
        loaded = true;
        if (auto dbc = am->loadDBC("QuestInfo.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                names[dbc->getUInt32(i, 0)] = dbc->getString(i, 1);
        }
    }
    auto it = names.find(type);
    return it == names.end() ? std::string() : it->second;
}

// The game layer's quest events count rows the way this file does.
static const bool kQuestRowResolverInstalled =
    (game::questLogRowResolver() = &questLogRowForQuest, true);

namespace {

/// The quest at a display index, or null when that row is a header or the index
/// is off either end. This is what every "…(questLogIndex)" binding needs: a
/// header row genuinely has no quest, and answering with the wrong one is how a
/// log shows the details of a quest the player did not click.
const game::GameHandler::QuestLogEntry* questAtRow(game::GameHandler* gh, int index) {
    if (index < 1) return nullptr;
    const auto rows = questRows(gh);
    if (index > static_cast<int>(rows.size())) return nullptr;
    return rows[index - 1].quest;
}

} // namespace

// The same mapping, reachable from the other binding files. Declared in
// lua_api_helpers.hpp because anything holding a quest log index needs it.
const game::GameHandler::QuestLogEntry* questAtLogRow(game::GameHandler* gh, int index) {
    return questAtRow(gh, index);
}

static int lua_GetNumQuestLogEntries(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    // numEntries counts the rows the log displays - headers included, and the
    // quests under a collapsed header excluded, because that is what the frame
    // walks with GetQuestLogTitle.
    lua_pushnumber(L, questRows(gh).size());
    // numQuests is every quest held, whatever is collapsed: it is what
    // QuestLog_UpdateQuestCount prints against MAX_QUESTLOG_QUESTS, and
    // QuestLogFrame only picks a first selection when it is above zero.
    size_t quests = 0;
    for (const auto& q : gh->getQuestLog()) {
        if (q.questId != 0) ++quests;
    }
    lua_pushnumber(L, quests);
    return 2;
}

// GetQuestLogTitle(index) → title, level, suggestedGroup, isHeader, isCollapsed, isComplete, frequency, questID
// ---- The quest info panel's reward block ----
//
// Shared by the quest giver and the quest log, so a raise here takes both down.
// None of these appeared in a scan of either frame's own file, because the
// panel that draws the rewards is a third file they both pull in.

// GetQuestLogTimeLeft() → seconds left on the selected quest, or nil.
// Backed by the same PLAYER_QUEST_LOG expiry the tracker's timers read.
static int lua_GetQuestLogTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    // The selection is a display row (zone headers count), so it resolves
    // through the row list like every other quest log index.
    const auto* sel = questAtLogRow(gh, gh->getSelectedQuestLogIndex());
    if (!sel) return luaReturnNil(L);
    const uint32_t questId = sel->questId;
    for (const auto& t : gh->getQuestTimers()) {
        if (t.first == questId) { lua_pushnumber(L, t.second); return 1; }
    }
    return luaReturnNil(L);   // not a timed quest
}

// Defined further down, with the other quest-log readers.
static const game::GameHandler::QuestLogEntry* selectedLogEntry(game::GameHandler* gh);

// Whether the quest being looked at has been failed.
//
// The quest slot's state field carries it beside completion - the server names
// the two bits QUEST_STATE_COMPLETE and QUEST_STATE_FAIL - and the field was
// already being read for the first of them.
static int lua_IsCurrentQuestFailed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* q = selectedLogEntry(gh);
    lua_pushboolean(L, (q && q->failed) ? 1 : 0);
    return 1;
}

// The spell, title and faction rewards a quest can carry. The query response
// this client parses parses none of them, and each is asked behind `if ( ... )`
// before its block is drawn - so nil leaves the block out rather than drawing
// an empty one.
// GetRewardSpell() - the quest *giver's* reward spell.
//
// The offer's own, not the log's: the panel asking this is showing a quest
// being offered, which is usually not the one selected in the log, so the log
// form cannot stand in for it.
//
// It answered nil because the details packet was not read this far. It is now
// - four fields past the XP - so a quest that teaches a recipe shows what it
// teaches. Same four values and same three flags as the log form beside it.
static int lua_GetQuestRewardSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    const uint32_t spellId = gh->getQuestDetails().rewardSpellId;
    if (spellId == 0) return luaReturnNil(L);
    const std::string name = gh->getSpellName(spellId);
    if (name.empty()) return luaReturnNil(L);
    const std::string icon = gh->getSpellIconPath(spellId);
    lua_pushstring(L, icon.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                   : icon.c_str());
    lua_pushstring(L, name.c_str());
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0);
    lua_pushboolean(L, gh->getKnownSpells().count(spellId) > 0 ? 1 : 0);
    return 4;
}
// GetQuestLogRewardTitle() → the title a quest awards, formatted with the
// player's name, or nil for the many quests that award none. The CharTitleId is
// in the query response (field 19) and the name in CharTitles.dbc, both of
// which this now reads; the old answer was a flat nil.
static int lua_GetQuestRewardTitle(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    // The offer panel (GetRewardTitle) and the log panel (GetQuestLogRewardTitle)
    // share this; read whichever is open, the offer's own title id when it is,
    // else the selected log quest's - the log selection is not the offer's.
    uint32_t titleId = 0;
    if (gh->isQuestOfferRewardOpen()) {
        titleId = gh->getQuestOfferReward().rewardTitleId;
    } else {
        if (const auto* q = questAtLogRow(gh, gh->getSelectedQuestLogIndex()))
            titleId = q->rewardTitleId;
    }
    if (titleId == 0) { return luaReturnNil(L); }
    const std::string name = gh->getFormattedTitleById(titleId);
    if (name.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, name.c_str());
    return 1;
}
static int lua_ProcessQuestLogRewardFactions(lua_State* L) { (void)L; return 0; }

// The selected quest's five reputation reward slots, packed to the front (the
// panel walks 1..GetNumQuestLogRewardFactions expecting no gaps). Inlined
// selection because selectedQuest is defined further down.
/// The quest log entry the player has selected, or null when nothing is.
///
/// The log index is 1-based and counts display rows, zone headers included -
/// it is what SelectQuestLogEntry was handed. This used to subscript the raw
/// log with it, which has no header rows, so every reader here (share, XP,
/// honor, reputation, title) answered for the quest one or more rows further
/// down than the one selected; row 1 is always a header.
static const game::QuestHandler::QuestLogEntry* selectedQuest(game::GameHandler* gh) {
    if (!gh) return nullptr;
    return questAtLogRow(gh, gh->getSelectedQuestLogIndex());
}
// The reputation reward slots for whichever panel is open - the offer's own
// when it is up, the selected log quest's otherwise (same reason as the title:
// the log selection is not the offered quest). Copied into a common shape so
// the two source structs, identical in fields, are read the same way.
struct RewardFaction { uint32_t factionId; int32_t valueId; int32_t override; };
static std::vector<RewardFaction> currentFactionRewards(game::GameHandler* gh) {
    std::vector<RewardFaction> out;
    if (!gh) return out;
    if (gh->isQuestOfferRewardOpen()) {
        for (const auto& fr : gh->getQuestOfferReward().factionRewards)
            if (fr.factionId != 0) out.push_back({fr.factionId, fr.valueId, fr.override});
    } else if (const auto* q = selectedQuest(gh)) {
        for (const auto& fr : q->factionRewards)
            if (fr.factionId != 0) out.push_back({fr.factionId, fr.valueId, fr.override});
    }
    return out;
}
// GetNumQuestLogRewardFactions() → how many faction rewards the quest carries.
static int lua_GetNumQuestLogRewardFactions(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(currentFactionRewards(getGameHandler(L)).size()));
    return 1;
}
// GetQuestLogRewardFactionInfo(i) → factionId, amount(in hundredths). The panel
// pairs the id with GetFactionInfoByID and divides the amount by 100 itself.
static int lua_GetQuestLogRewardFactionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int want = static_cast<int>(luaL_optnumber(L, 1, 0));
    const auto list = currentFactionRewards(gh);
    if (want < 1 || want > static_cast<int>(list.size())) { return luaReturnNil(L); }
    const auto& fr = list[static_cast<size_t>(want - 1)];
    auto* qh = gh->getQuestHandler();
    const int32_t amount = qh ? qh->getQuestRewardReputation(fr.valueId, fr.override) : 0;
    lua_pushnumber(L, fr.factionId);
    lua_pushnumber(L, amount);
    return 2;
}

// GetFactionInfoByID(id) - the same thirteen values GetFactionInfo gives by
// position, found by faction id instead. The reputation list carries the id
// already.
//
// This used to answer a name and five nils. The quest reward panel reads the
// ninth and eleventh - isHeader and hasRep - to decide whether to print a
// reputation line, and got nil for both; it survived only because `not nil` is
// true and the test happens to fall the right way.
static int lua_GetFactionInfoByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
    if (!gh || id == 0) return luaReturnNil(L);
    for (const auto& r : gh->getReputationList()) {
        if (r.factionId == id) return pushFactionInfo(L, gh, r);
    }
    return luaReturnNil(L);
}

// ---- Quest watch ordering ----
//
// The watch list here is a set of quest ids, so its order is the quest log's
// order rather than one of its own. That is what these three say.

// GetQuestWatchIndex(questLogIndex) → where that quest sits in the watch list.
static int lua_GetQuestWatchIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || idx < 1) return luaReturnNil(L);
    const auto* at = questAtLogRow(gh, idx);   // a display row, like every log index
    if (!at) return luaReturnNil(L);
    const uint32_t wanted = at->questId;
    int position = 0;
    for (const auto& q : gh->getQuestLog()) {
        if (!gh->isQuestTracked(q.questId)) continue;
        ++position;
        if (q.questId == wanted) { lua_pushnumber(L, position); return 1; }
    }
    return luaReturnNil(L);   // not watched
}

// SortQuestWatches() → whether the order changed.
//
// False, and that is the truthful answer rather than a shrug: the watch order
// follows the quest log, so there is never a separate order to sort. The
// caller reads it as "did anything move" and rebuilds the tracker when it did.
static int lua_SortQuestWatches(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// ShiftQuestWatches(from, to) - reorder the list by hand. Nothing is stored to
// reorder, so dragging a tracker entry leaves it where the log puts it.
static int lua_ShiftQuestWatches(lua_State* L) { (void)L; return 0; }

// GetQuestSortIndex(questLogIndex) → the row of the header the quest sits
// under. WatchFrame expands it (ExpandQuestHeader(GetQuestSortIndex(i))) before
// opening the log at a tracked quest; nil there became ExpandQuestHeader(0),
// which opens every header.
static int lua_GetQuestSortIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || idx < 1) return luaReturnNil(L);
    const auto rows = questRows(gh);
    if (idx > static_cast<int>(rows.size())) return luaReturnNil(L);
    for (int i = idx; i >= 1; --i) {
        if (rows[static_cast<size_t>(i - 1)].isHeader) { lua_pushnumber(L, i); return 1; }
    }
    return luaReturnNil(L);
}

// ---- Quest log special items ----
//
// Whether the quest item's target is close enough to use it on. Nothing here
// knows an item's range, and nil is the answer that hides the range indicator
// rather than colouring it wrongly - WatchFrameItem_OnUpdate takes the third
// branch and hides the count text.
static int lua_IsQuestLogSpecialItemInRange(lua_State* L) { (void)L; return luaReturnNil(L); }

// UseQuestLogSpecialItem(questLogIndex) - clicking that button.
//
// By slot rather than by item id, for the same reason UseContainerItem is:
// searching by id can find a different stack of the same thing.
static int lua_UseQuestLogSpecialItem(lua_State* L);

// GetQuestLogSpecialItemCooldown(index) → start, duration, enable.
//
// Enable is one, not zero: zero means the cooldown swipe is switched off, and
// the caller feeds all three straight to CooldownFrame_SetTimer.
//
// The item's cooldown is its on-use spell's, the same relationship the bag
// buttons read. This answered a flat zero, so a quest item just used was drawn
// ready again - and WatchFrameItem_UpdateCooldown runs on every
// BAG_UPDATE_COOLDOWN, so it had something to ask and nothing to hear.
static int lua_GetQuestLogSpecialItemCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const auto item = questSpecialItemAt(gh, index);
    double start = 0.0, duration = 0.0;
    if (item.itemId && itemUseCooldown(gh, item.itemId, start, duration)) {
        lua_pushnumber(L, start);
        lua_pushnumber(L, duration);
        lua_pushnumber(L, 1);
        return 3;
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 1);
    return 3;
}

// GetQuestTimers() - the seconds left on each timed quest, as separate values.
//
// QuestTimerFrame counts them with select("#", ...) and reads them with
// select(i, ...), so the count is the return count. Returning nothing is the
// honest answer for a log with no timed quest in it, and the frame hides
// itself - which it could not do while this was missing, because the OnEvent
// that calls it runs on every QUEST_LOG_UPDATE.
static int lua_GetQuestTimers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto timers = gh->getQuestTimers();
    // One per timed quest, against Lua's twenty guaranteed slots. A log holds
    // twenty-five.
    if (!timers.empty() && !lua_checkstack(L, static_cast<int>(timers.size()))) {
        return 0;
    }
    for (const auto& t : timers) lua_pushnumber(L, t.second);
    return static_cast<int>(timers.size());
}

// GetQuestIndexForTimer(i) - the quest log index the i-th timer belongs to,
// so clicking a timer row selects its quest.
static int lua_GetQuestIndexForTimer(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || idx < 1) return luaReturnNil(L);
    const auto timers = gh->getQuestTimers();
    if (idx > static_cast<int>(timers.size())) return luaReturnNil(L);
    const uint32_t questId = timers[static_cast<size_t>(idx) - 1].first;
    // A display index, since that is what every other quest binding takes.
    const auto rows = questRows(gh);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].quest && rows[i].quest->questId == questId) {
            lua_pushnumber(L, static_cast<double>(i + 1));
            return 1;
        }
    }
    return luaReturnNil(L);
}

/// Which page of an open book is being read, zero-based.
///
/// The reader's own state, not the client's: every page is already in hand by
/// the time the frame draws one, so turning a page moves this and nothing else.
/// Reset when a book is closed, which is the only moment it can be stale.
static int& bookPage() { static int page = 0; return page; }

/// Tell the frame to redraw from whatever the page bindings now answer.
static void fireItemTextReady(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("ITEM_TEXT_READY", {});
}

static int lua_GetQuestLogTitle(lua_State* L) {
    auto* gh = getGameHandler(L);
    // optnumber, not checknumber: FrameXML walks the quest log with an index
    // that can be nil before anything has been selected, and raising there
    // loses whatever asked rather than answering that there is no such quest.
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh) { return luaReturnNil(L); }
    const auto rows = questRows(gh);
    // A header row: the zone (or Epic/class/profession) the quests under it
    // belong to. isHeader is what tells QuestLog_Update to draw it as one and to
    // hang a collapse box off it, and isCollapsed is what that box reads.
    if (index >= 1 && index <= static_cast<int>(rows.size()) && rows[index - 1].isHeader) {
        const auto& row = rows[index - 1];
        lua_pushstring(L, row.headerTitle.c_str());                        // 1: title
        lua_pushnumber(L, 0);                                              // 2: level
        lua_pushnil(L);                                                    // 3: questTag
        lua_pushnumber(L, 0);                                              // 4: suggestedGroup
        lua_pushboolean(L, 1);                                             // 5: isHeader
        lua_pushboolean(L, collapsedQuestGroups().count(row.group) ? 1 : 0); // 6: isCollapsed
        lua_pushnil(L);                                                    // 7: isComplete
        lua_pushboolean(L, 0);                                             // 8: isDaily
        lua_pushnumber(L, 0);                                              // 9: questID
        lua_pushboolean(L, 0);                                             // 10: displayQuestID
        return 10;
    }
    // An index off either end gets an empty row, not a bare nil.
    //
    // QuestLog_Update reads the count once and then walks GetQuestLogTitle in a
    // loop; a server update landing between frames while the log is scrolled
    // can shrink the log under a count the frame still holds, so an index it
    // believes valid comes back past the end. In colourblind mode the very next
    // line is `title = "["..level.."] "..title`, and a nil title raised there
    // and tore down the whole redraw - which is what left the list a mess. An
    // empty title concatenates cleanly and the row corrects itself on the next
    // pass; a raise does not.
    //
    // The low end too, not just the high: the button loop guards only the upper
    // bound (`if questIndex <= numEntries`), so a scroll offset that drives the
    // index to zero or below still reaches here and hit `"  "..title` at a nil
    // - the questlogframe.lua:430 raise seen driving the log headlessly. The
    // `if title` callers all iterate 1..numEntries and never reach a low index,
    // so an empty row here changes nothing for them.
    const game::GameHandler::QuestLogEntry* qp =
        (index >= 1 && index <= static_cast<int>(rows.size())) ? rows[index - 1].quest : nullptr;
    if (!qp) {
        lua_pushstring(L, "");   // 1: title (empty, never nil)
        lua_pushnumber(L, 0);    // 2: level
        lua_pushnil(L);          // 3: questTag
        lua_pushnumber(L, 0);    // 4: suggestedGroup
        lua_pushboolean(L, 0);   // 5: isHeader
        lua_pushboolean(L, 0);   // 6: isCollapsed
        lua_pushnil(L);          // 7: isComplete
        lua_pushboolean(L, 0);   // 8: isDaily
        lua_pushnumber(L, 0);    // 9: questID
        lua_pushboolean(L, 0);   // 10: displayQuestID
        return 10;
    }
    const auto& q = *qp;
    // The client's ten, in its order:
    //
    //   title, level, questTag, suggestedGroup, isHeader, isCollapsed,
    //   isComplete, isDaily, questID, displayQuestID
    //
    // Eight were returned, with questTag and isDaily absent, so everything
    // from the third value on landed one or two places early. isComplete
    // received a zero and so no quest ever showed as complete; isDaily
    // received the quest id, which is a large number and therefore true, so
    // every quest in the log was marked daily; and questID arrived nil.
    lua_pushstring(L, forPlayer(L, q.title).c_str());  // 1: title
    // The level is tracked - the query response carries it - and was being
    // answered as a flat zero beside a comment saying it was not.
    lua_pushnumber(L, q.level);          // 2: level
    // 3: questTag - the QuestInfo.dbc name of the quest's type ("Group",
    // "PvP", "Dungeon", "Raid", "Heroic"...), nil for a plain quest.
    {
        const std::string tag = questTypeName(gh, q.questType);
        if (tag.empty()) lua_pushnil(L); else lua_pushstring(L, tag.c_str());
    }
    lua_pushnumber(L, q.suggestedPlayers); // 4: suggestedGroup
    lua_pushboolean(L, 0);               // 5: isHeader
    lua_pushboolean(L, 0);               // 6: isCollapsed
    // A number, not a boolean: 1 for complete, -1 for failed, nil otherwise.
    // watchframe.lua writes `if ( isComplete and isComplete < 0 )` to tell a
    // failed quest from a finished one, and comparing a boolean with a number
    // raises. Correcting the *position* of this value without correcting its
    // type turned a quiet wrong answer into an error on the quest tracker.
    //
    // Failed is tested first: the server can set both bits, and a quest whose
    // timer ran out is failed whatever else is true of it.
    if (q.failed)        lua_pushnumber(L, -1);
    else if (q.complete) lua_pushnumber(L, 1);
    else                 lua_pushnil(L);                        // 7: isComplete
    // 8: isDaily - QUEST_FLAGS_DAILY (0x1000), 1 or nil.
    if (q.questFlags & 0x1000u) lua_pushnumber(L, 1); else lua_pushnil(L);
    lua_pushnumber(L, q.questId);        // 9: questID
    // Not the id again: this is the developer switch that asks for the id to
    // be *shown*, and the quest log writes "26102 - Wolves Across the Border"
    // when it is set. An id is a large number and therefore true, so every
    // quest in the log wore its own number. Off is what the real client
    // ships.
    lua_pushboolean(L, 0);               // 10: displayQuestID
    return 10;
}

// GetQuestLogQuestText(index) → description, objectives
/// The quest log index an argument-less call means: the one the log has
/// selected. WoW's quest log functions take the index only when asking about
/// some other entry, and demanding it raised a Lua error on every bare call.
static int questLogIndexOrSelected(lua_State* L, int arg) {
    if (!lua_isnoneornil(L, arg)) return static_cast<int>(luaL_checknumber(L, arg));
    auto* gh = getGameHandler(L);
    return gh ? gh->getSelectedQuestLogIndex() : 0;
}

static int lua_GetQuestLogQuestText(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = questLogIndexOrSelected(L, 1);
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto* qp = questAtRow(gh, index);
    if (!qp) { return luaReturnNil(L); }
    const auto& q = *qp;
    // The quest giver's own text. It is the third string in the query
    // response and the parser already walked over it to reach the fifth, so
    // "not stored" was true only of the store - the bytes were in hand and
    // discarded, and the quest log drew a blank panel above every objective
    // list because of it.
    lua_pushstring(L, forPlayer(L, q.description).c_str());  // description
    lua_pushstring(L, forPlayer(L, q.objectives).c_str());   // objectives
    return 2;
}

// IsQuestComplete(questID) → boolean
static int lua_IsQuestComplete(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t questId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (!gh) { return luaReturnFalse(L); }
    for (const auto& q : gh->getQuestLog()) {
        if (q.questId == questId) {
            lua_pushboolean(L, q.complete);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// SelectQuestLogEntry(index) - select a quest in the quest log
static int lua_SelectQuestLogEntry(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) return 0;
    gh->setSelectedQuestLogIndex(index);
    // Fetch the full text if this quest arrived without it.
    //
    // A quest accepted this session carries its description from the offer
    // packet, but a quest already in the log at login comes with only its id,
    // title and objectives - the description is never sent with the log and
    // has to be asked for. Selecting the entry is when the panel wants it, and
    // nothing had ever requested it, so every quest carried from a previous
    // session showed an empty description above its objectives. The query
    // answers with QUEST_LOG_UPDATE, which the panel already redraws on.
    if (const auto* q = questAtRow(gh, index)) {
        if (q->description.empty() && q->questId != 0) {
            gh->requestQuestQuery(q->questId, false);
        }
    }
    return 0;
}

// GetQuestLogSelection() → index
static int lua_GetQuestLogSelection(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getSelectedQuestLogIndex() : 0);
    return 1;
}

/// The quest the log has selected, or null if none is.
// GetQuestLogPushable() → whether the selected quest may be offered to the party.
//
// Yes for any real selection. Which quests the server will actually share is a
// flag on the quest, and no packet this client parses carries it - so the
// choice is between offering the attempt and letting the server refuse, or
// never offering it at all. The button is disabled without this, and sharing
// works, so silence would be the more misleading answer of the two.
static int lua_GetQuestLogPushable(lua_State* L) {
    // WatchFrame's menu asks about a tracked quest by its row
    // (GetQuestLogPushable(GetQuestIndexForWatch(i))); the log asks bare.
    auto* gh = getGameHandler(L);
    const auto* q = gh ? questAtLogRow(gh, questLogIndexOrSelected(L, 1)) : nullptr;
    lua_pushboolean(L, q != nullptr ? 1 : 0);
    return 1;
}

// GetQuestLogRewardXP() → the experience the reward panel shows for the
// selected quest. It is in no packet - the client derives it from QuestXP.dbc
// at the quest's level and the XP-difficulty index the query carried, which is
// what this now does. Zero (the old hard-coded answer) hid the reward line on
// every quest; a real value restores "You will receive N experience."
static int lua_GetQuestLogRewardXP(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* q = gh ? selectedQuest(gh) : nullptr;
    auto* qh = gh ? gh->getQuestHandler() : nullptr;
    lua_pushnumber(L, (q && qh) ? qh->getQuestRewardXP(q->level, q->rewardXPId) : 0.0);
    return 1;
}

// Honor, talent points and arena points a quest awards - direct values the
// query response carries, and hard zeros before, on a stale "not in any quest
// packet" comment. WotLK-only, so classic/TBC keep zero until their layout is
// read off a serializer.
static int lua_GetQuestLogRewardHonor(lua_State* L) {
    const auto* q = selectedQuest(getGameHandler(L));
    lua_pushnumber(L, q ? q->rewardHonor : 0.0);
    return 1;
}
static int lua_GetQuestLogRewardTalents(lua_State* L) {
    const auto* q = selectedQuest(getGameHandler(L));
    lua_pushnumber(L, q ? q->rewardTalents : 0.0);
    return 1;
}
static int lua_GetQuestLogRewardArenaPoints(lua_State* L) {
    const auto* q = selectedQuest(getGameHandler(L));
    lua_pushnumber(L, q ? q->rewardArenaPoints : 0.0);
    return 1;
}

// QuestLogPushQuest() - offer the selected quest to the party.
static int lua_QuestLogPushQuest(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Optional row, as WatchFrame's "Share" passes one.
    if (const auto* quest = gh ? questAtLogRow(gh, questLogIndexOrSelected(L, 1)) : nullptr) {
        gh->shareQuestWithParty(quest->questId);
    }
    return 0;
}

// GetNumQuestWatches() → count
static int lua_GetNumQuestWatches(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getTrackedQuestIds().size() : 0);
    return 1;
}

// GetQuestIndexForWatch(watchIndex) → questLogIndex
// Maps the Nth watched quest to its quest log index (1-based)
static int lua_GetQuestIndexForWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int watchIdx = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || watchIdx < 1) { return luaReturnNil(L); }
    // The index this answers is a quest log index, and a quest log index now
    // counts header rows - so it has to be found in the display list rather
    // than by counting through getQuestLog(). Answering the raw log position
    // pointed the watch frame at whatever row happened to hold that number,
    // which after grouping is usually a different quest or a header.
    const auto rows = questRows(gh);
    const auto& tracked = gh->getTrackedQuestIds();
    int found = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].quest) continue;
        if (tracked.count(rows[i].quest->questId)) {
            found++;
            if (found == watchIdx) {
                lua_pushnumber(L, static_cast<int>(i) + 1); // 1-based
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// AddQuestWatch(questLogIndex) - add a quest to the watch list
static int lua_AddQuestWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) return 0;
    if (const auto* q = questAtRow(gh, index)) {
        gh->setQuestTracked(q->questId, true);
    }
    return 0;
}

// RemoveQuestWatch(questLogIndex) - remove a quest from the watch list
static int lua_RemoveQuestWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) return 0;
    if (const auto* q = questAtRow(gh, index)) {
        gh->setQuestTracked(q->questId, false);
    }
    return 0;
}

// IsQuestWatched(questLogIndex) → boolean
static int lua_IsQuestWatched(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnFalse(L); }
    const auto* q = questAtRow(gh, index);
    lua_pushboolean(L, (q && gh->isQuestTracked(q->questId)) ? 1 : 0);
    return 1;
}

// GetQuestLink(questLogIndex) → "|cff...|Hquest:id:level|h[title]|h|r"
static int lua_GetQuestLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto* qp = questAtRow(gh, index);
    if (!qp) { return luaReturnNil(L); }
    const auto& q = *qp;
    // Yellow quest link format matching WoW
    std::string link = "|cff808000|Hquest:" + std::to_string(q.questId) +
                       ":0|h[" + q.title + "]|h|r";
    lua_pushstring(L, link.c_str());
    return 1;
}

// GetNumQuestLeaderBoards(questLogIndex) → count of objectives
static int lua_GetNumQuestLeaderBoards(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = questLogIndexOrSelected(L, 1);
    if (!gh || index < 1) { return luaReturnZero(L); }
    const auto* qp = questAtRow(gh, index);
    if (!qp) { return luaReturnZero(L); }
    lua_pushnumber(L, game::questObjectiveCount(*qp));
    return 1;
}

// GetQuestLogLeaderBoard(objIndex, questLogIndex) → text, type, finished
// objIndex is 1-based within the quest's objectives

static int lua_GetQuestLogLeaderBoard(lua_State* L);

// ── Quest points of interest ───────────────────────────────────────────────
//
// The map's quest markers. The server sends these as SMSG_QUEST_POI and this
// client already keeps them - it draws its own markers from the same list - so
// FrameXML's world map can read the real thing rather than an empty one.
//
// WoW numbers them by "visible index", which is a position in the list of
// quests that have a POI on the map now, not a quest log index. The two differ
// as soon as one quest in the log has no marker.

/// The quest ids that have a marker, completed ones first, each appearing once.
/// Built on demand: the list is short and changes whenever the server sends a
/// new one.
///
/// The order is not decoration. WorldMapFrame numbers its POI buttons from
/// this list: a completed quest at visible index i takes button i of the
/// completed set, and an incomplete one takes button `i - completedSoFar` of
/// the numeric set. Both are only contiguous from 1 if every completed quest
/// comes first, which is how the real client orders them - and where it does
/// not, QuestPOI_HideButtons walks from 1 to the highest button made and
/// indexes a name nothing created:
///
///     QuestPOI.lua:200: attempt to index local 'poiButton' (a nil value)
///
/// which fired on every map update, took the rest of the update with it, and
/// left the map with no quest list and no objectives on it.
static std::vector<uint32_t> questsWithPois(game::GameHandler* gh) {
    std::vector<uint32_t> out;
    if (!gh) return out;
    for (const auto& poi : gh->getGossipPois()) {
        // -2 is an ordinary gossip marker rather than a quest one.
        if (poi.questObjectiveIndex == -2 || poi.data == 0) continue;
        if (std::find(out.begin(), out.end(), poi.data) == out.end()) out.push_back(poi.data);
    }
    const auto isComplete = [gh](uint32_t questId) {
        for (const auto& quest : gh->getQuestLog()) {
            if (quest.questId == questId) return quest.complete;
        }
        return false;
    };
    // Stable, so the server's order still decides within each group.
    std::stable_partition(out.begin(), out.end(), isComplete);
    return out;
}

/// QuestMapUpdateAllQuests() → how many quests have a marker on the map.
///
/// Both a verb and a question in the real client: it refreshes the POI set and
/// answers how many there are. There is nothing to refresh here - the list is
/// whatever the server last sent - so this is the answer alone.
///
/// It was not bound at all, and WatchFrame_GetCurrentMapQuests reads it
/// straight into `for i = 1, numQuests`. A nil limit there is not an empty
/// loop but an error, so the tracker's map-quest table was never built and the
/// handler around it died on the way. The count it needs was already sitting
/// in the same list QuestPOIGetQuestIDByVisibleIndex indexes.
static int lua_QuestMapUpdateAllQuests_Legacy(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(questsWithPois(getGameHandler(L)).size()));
    return 1;
}

/// QuestPOIGetQuestIDByVisibleIndex(i) → questId, questLogIndex.
///
/// Both, because the world map uses the second to reach everything else about
/// the quest: `if ( questLogIndex and questLogIndex > 0 )` gates the whole
/// block that builds the map's quest list, and with only one value returned
/// that gate never opened - so the list was always empty, quietly, with no
/// error to say why.
static int lua_QuestPOIGetQuestIDByVisibleIndex_Legacy(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_checknumber(L, 1));
    const auto ids = questsWithPois(gh);
    if (index < 1 || index > static_cast<int>(ids.size())) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    const uint32_t questId = ids[static_cast<size_t>(index - 1)];
    lua_pushnumber(L, questId);
    // Where that quest sits in the log, counted as Lua counts. A marker can
    // outlive the log entry - the server sends POIs separately - so a quest
    // that is no longer held answers zero rather than a stale position.
    int logIndex = 0;
    if (gh) {
        const auto rows = questRows(gh);
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].quest && rows[i].quest->questId == questId) {
                logIndex = static_cast<int>(i) + 1;
                break;
            }
        }
    }
    lua_pushnumber(L, logIndex);
    return 2;
}

/// QuestPOIGetIconInfo(questId) → completed, x, y, objectiveIndex.
///
/// Where the map marks a quest: on the work while there is work to do, and on
/// the hand-in once there is not. This reported the hand-in either way - it
/// took the marker with objective index -1 whenever there was one - so every
/// quest in the log was marked at whichever NPC it is returned to, and a zone
/// full of objectives had all its numbers piled on the town it is quested from.
///
/// The position is a fraction across the map now showing, not a place in the
/// world: worldmapframe.lua multiplies what it gets by WorldMapDetailFrame's
/// width. World coordinates there are tens of thousands of pixels off the
/// parchment, which is the same fault GetPlayerMapPosition had. A quest whose
/// marker is on another map answers no position at all, and the caller's
/// `if ( posX and posY )` leaves its button where it was.
static int lua_QuestPOIGetIconInfo_Legacy(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const uint32_t questId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (!gh || questId == 0) { return luaReturnNil(L); }

    bool complete = false;
    for (const auto& q : gh->getQuestLog()) {
        if (q.questId == questId) { complete = q.complete; break; }
    }

    // The kind that suits the quest's state, and whatever there is when the
    // server sent only the other kind.
    const game::GossipPoi* best = nullptr;
    for (const auto& poi : gh->getGossipPois()) {
        if (poi.data != questId || poi.questObjectiveIndex == -2) continue;
        const bool endpoint = (poi.questObjectiveIndex == -1);
        if (endpoint == complete) { best = &poi; break; }
        if (!best) best = &poi;
    }
    if (!best) { return luaReturnNil(L); }

    float u = 0.0f, v = 0.0f;
    const bool onThisMap = svc && svc->mapUVForWorldPos &&
                           svc->mapUVForWorldPos(best->x, best->y, 0.0f, u, v);

    lua_pushboolean(L, complete);
    if (!onThisMap) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 4;
    }
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    // The fourth value the API names: which objective this marker belongs to.
    // The client has carried it all along as questObjectiveIndex, where -1
    // means the quest itself rather than one of its lines - which is nil here,
    // because that is what "not an objective" is in Lua and a zero would be a
    // real objective number.
    if (best->questObjectiveIndex < 0) lua_pushnil(L);
    else lua_pushnumber(L, best->questObjectiveIndex + 1);
    return 4;
}

// ── The client's QuestPOI functions, rebuilt from Wow.exe 3.3.5a ───────────
//
// The client keeps up to 25 quests' POI blocks (0x00C23B00, 0x30 bytes each)
// straight from SMSG_QUEST_POI_QUERY_RESPONSE, and these functions read them
// against whatever the world map is showing:
//
//   QuestMapUpdateAllQuests (0x005E63D0 -> 0x005E5A50) works out each quest's
//     objective mask and whether any of its blocks for those objectives lands
//     on the map showing, and answers how many do. Nothing on a continent.
//   QuestPOIUpdateIcons (0x005E5740 -> 0x005E2EB0) picks, for each quest, the
//     block its icon goes on: the lowest priority, then the nearest to the
//     player, among the visible blocks for unfinished objectives - or for the
//     turn-in once every objective is done.
//   QuestPOIGetIconInfo (0x005E0590) answers that block's centre on the map.
//   QuestPOIGetQuestIDByVisibleIndex (0x005E5750 -> 0x005E3840) numbers the
//     visible quests: finished ones first, then watched ones in watch order,
//     then the rest - the order WorldMapFrame's button numbering assumes.
//
// Positions are the blocks' own world coordinates, wire order (north axis
// first), converted by the map's own function (worldmap::worldToMap).
namespace {

struct PoiSlot {
    uint32_t questId = 0;
    uint32_t mask = 0;         // +0x2C, 0xFFFFFFFF = every objective done
    bool visible = false;      // +0x28
    int mapId = 0;             // +0x14
    int objective = 0;         // +0x18
    int floor = 0;             // +0x1C
    int32_t x = 999999;        // +0x20
    int32_t y = 999999;        // +0x24
    bool movable = true;       // the chosen area has more than one point
    bool spread = false;       // an unfinished quest: its icon is spread from the others
};

std::vector<PoiSlot>& poiSlots() {
    static std::vector<PoiSlot> slots;
    return slots;
}

PoiSlot& poiSlotFor(uint32_t questId) {
    auto& slots = poiSlots();
    for (auto& s : slots) {
        if (s.questId == questId) return s;
    }
    slots.push_back({});
    slots.back().questId = questId;
    return slots.back();
}

const game::GameHandler::QuestLogEntry* questInLog(game::GameHandler* gh, uint32_t questId) {
    if (!gh) return nullptr;
    for (const auto& q : gh->getQuestLog()) {
        if (q.questId == questId) return &q;
    }
    return nullptr;
}

/// 0x005E10B0: a quest with nothing to do but hand it in.
bool questHasNoObjectives(const game::GameHandler::QuestLogEntry& q) {
    return !q.failed && game::questObjectiveCount(q) == 0;
}

/// The objective mask, 0x005E2950: a bit for each objective not yet done -
/// kills and objects 0-3, items 4-9 - and all bits when the quest is complete.
uint32_t questObjectiveMask(game::GameHandler* gh, uint32_t questId) {
    const auto* q = questInLog(gh, questId);
    if (!q || q->failed) return 0;
    if (q->complete) return 0xFFFFFFFFu;
    uint32_t mask = 0;
    for (size_t i = 0; i < q->killObjectives.size(); ++i) {
        const auto& kill = q->killObjectives[i];
        if (!game::isQuestObjective(kill.npcOrGoId, kill.required)) continue;
        uint32_t current = 0;
        const uint32_t key = static_cast<uint32_t>(std::abs(kill.npcOrGoId));
        if (auto it = q->killCounts.find(key); it != q->killCounts.end()) current = it->second.first;
        if (current < kill.required) mask |= 1u << i;
    }
    for (size_t i = 0; i < q->itemObjectives.size(); ++i) {
        const auto& item = q->itemObjectives[i];
        if (!game::isQuestObjective(static_cast<int32_t>(item.itemId), item.required)) continue;
        uint32_t current = 0;
        if (auto it = q->itemCounts.find(item.itemId); it != q->itemCounts.end()) current = it->second;
        if (current < item.required) mask |= 1u << (4 + i);
    }
    // An exploration or event quest still open: bit 16. The client reads the
    // quest's flags for this; with none here, a quest that is neither done nor
    // has a count to make is the same thing.
    if (mask == 0 && game::questObjectiveCount(*q) == 0) mask |= 0x10000u;
    return mask;
}

bool poiForObjectives(uint32_t mask, int32_t objectiveIndex) {
    if (objectiveIndex == -1) return mask == 0xFFFFFFFFu;
    return mask != 0xFFFFFFFFu && (mask & (1u << (static_cast<uint32_t>(objectiveIndex) & 31u))) != 0;
}

/// What the map shows, as the QuestPOI functions ask about it.
struct PoiView {
    bool valid = false;
    bool dungeon = false;
    int dungeonMapId = -1, dungeonMapMap = -1, dungeonFloor = -1;
    int wmaId = -1, wmaMapId = -1;
};

PoiView poiView(lua_State* L) {
    PoiView v;
    v.dungeonMapId = worldmap::currentDungeonMapId(L);
    if (worldmap::dungeonMapInfo(L, v.dungeonMapId, v.dungeonMapMap, v.dungeonFloor)) {
        v.dungeon = true;
        v.valid = true;
        return v;
    }
    v.wmaId = worldmap::currentWorldMapAreaId(L);
    v.wmaMapId = worldmap::currentWorldMapAreaMapId(L);
    v.valid = v.wmaId != -1 && v.wmaMapId != -1;
    return v;
}

/// 0x005E0110 / 0x005E0180: a block's centre lands on the map showing.
bool poiOnMap(lua_State* L, const PoiView& view, const game::QuestPoiArea& a) {
    float u = 0.0f, v = 0.0f;
    bool hidden = false;
    if (view.dungeon) {
        if (static_cast<int>(a.mapId) != view.dungeonMapMap ||
            static_cast<int>(a.floor) != view.dungeonFloor) {
            return false;
        }
        const bool placed = worldmap::worldToMap(L, static_cast<float>(a.centerX),
            static_cast<float>(a.centerY), 0.0f, a.mapId, view.dungeonMapId, u, v, &hidden);
        return placed && !hidden && std::fabs(u * v) >= 0.001f;
    }
    if ((a.flags & 4u) != 0 && static_cast<int>(a.worldMapAreaId) != view.wmaId) return false;
    if (static_cast<int>(a.mapId) != view.wmaMapId) return false;
    const bool placed = worldmap::worldToMap(L, static_cast<float>(a.centerX),
        static_cast<float>(a.centerY), 0.0f, a.mapId, 0, u, v, &hidden);
    return placed && !hidden && std::fabs(u * v) >= 0.001f;
}

/// 0x005E5A50.
int updateAllQuestPois(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || gh->getPlayerGuid() == 0) return 0;
    const PoiView view = poiView(L);
    if (!view.valid) return 0;
    int count = 0;
    for (const auto& set : gh->getQuestPoiSets()) {
        PoiSlot& slot = poiSlotFor(set.questId);
        slot.mask = questObjectiveMask(gh, set.questId);
        slot.visible = false;
        for (const auto& a : set.areas) {
            if (!poiForObjectives(slot.mask, a.objectiveIndex)) continue;
            if (!poiOnMap(L, view, a)) continue;
            slot.visible = true;
            ++count;
            break;
        }
    }
    return count;
}

const game::QuestPoiSet* poiSetFor(game::GameHandler* gh, uint32_t questId) {
    if (!gh) return nullptr;
    for (const auto& set : gh->getQuestPoiSets()) {
        if (set.questId == questId) return &set;
    }
    return nullptr;
}

/// The quest log index (header rows counted) of a quest, or 0.
int questLogRowOf(game::GameHandler* gh, uint32_t questId) {
    const auto rows = questRows(gh);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].quest && rows[i].quest->questId == questId) return static_cast<int>(i) + 1;
    }
    return 0;
}

}  // namespace

/// QuestMapUpdateAllQuests() - 0x005E63D0.
static int lua_QuestMapUpdateAllQuests(lua_State* L) {
    if (!worldmap::ready(L)) return lua_QuestMapUpdateAllQuests_Legacy(L);
    int continent = -1, zone = -1;
    worldmap::currentView(L, continent, zone);
    const bool dungeon = worldmap::currentDungeonMapId(L) != -1 &&
                         poiView(L).dungeon;
    const bool area = worldmap::currentWorldMapAreaMapId(L) != -1;
    int count = 0;
    if ((area && (zone != -1 || continent == -2)) || dungeon) count = updateAllQuestPois(L);
    lua_pushnumber(L, count);
    return 1;
}

/// QuestPOIUpdateIcons() - 0x005E2EB0: which block each quest's icon is on.
/// The client then nudges icons that overlap apart over twenty passes; that
/// spreading is cosmetic and not reproduced.
static int lua_QuestPOIUpdateIcons(lua_State* L) {
    if (!worldmap::ready(L)) return 0;
    auto* gh = getGameHandler(L);
    if (!gh || gh->getPlayerGuid() == 0) return 0;
    const PoiView view = poiView(L);
    if (!view.valid) return 0;
    // The player in wire order (north axis first), as the blocks are.
    const auto& mi = gh->getMovementInfo();
    const float playerX = mi.y;
    const float playerY = mi.x;
    for (const auto& set : gh->getQuestPoiSets()) {
        PoiSlot& slot = poiSlotFor(set.questId);
        float bestDist = 3.4028234663852886e+38f;
        int bestPriority = 10;
        const game::QuestPoiArea* best = nullptr;
        for (const auto& a : set.areas) {
            if (!poiForObjectives(slot.mask, a.objectiveIndex)) continue;
            if (!poiOnMap(L, view, a)) continue;
            const float dx = std::fabs(playerX - static_cast<float>(a.centerX));
            const float dy = std::fabs(playerY - static_cast<float>(a.centerY));
            const float dist = dy * dy + dx * dx;
            const int priority = static_cast<int>(a.priority);
            if (priority < bestPriority || (dist < bestDist && priority == bestPriority)) {
                best = &a;
                bestDist = dist;
                bestPriority = priority;
            }
        }
        if (!best) {
            slot.mapId = 0;
            slot.objective = 0;
            slot.floor = 0;
            slot.x = 999999;
            slot.y = 999999;
            continue;
        }
        slot.mapId = static_cast<int>(best->mapId);
        slot.objective = best->objectiveIndex;
        slot.floor = static_cast<int>(best->floor);
        slot.x = best->centerX;
        slot.y = best->centerY;
        slot.movable = best->points.size() != 1;
        const auto* q = questInLog(gh, set.questId);
        slot.spread = !(q && q->complete);
    }

    // Then the icons of unfinished quests are pushed apart until none is
    // closer than 24/1024 of the map's width to another (0x005E2EB0 from
    // 0x005E3224): twenty passes, each moving an icon by the sum of a push of
    // that length away from every icon too near it, capped at a twentieth of
    // it. Y is squeezed by three quarters while this runs, which is roughly
    // the map's aspect, and stretched back after. An area of one point is an
    // exact place and does not move. Only on a zone's own map - not a
    // dungeon floor.
    if (!view.dungeon) {
        std::vector<PoiSlot*> icons;
        for (const auto& set : gh->getQuestPoiSets()) {
            PoiSlot& slot = poiSlotFor(set.questId);
            if (slot.x == 999999 && slot.y == 999999) continue;
            if (slot.spread) icons.push_back(&slot);
        }
        const float width = std::fabs(worldmap::worldMapAreaWidth(L, view.wmaId));
        const float reach = (12.0f + 12.0f) * static_cast<float>(static_cast<int>(width)) * 0.0009765625f;
        const float step = 0.05f * reach;
        const float reach2 = reach * reach;
        for (PoiSlot* s : icons) s->y = static_cast<int32_t>(static_cast<float>(s->y) * 0.7500187754631042f);
        static constexpr int kNudge[3][2] = {{0, 2}, {1, -2}, {-2, 1}};
        int nudge = 0;
        std::vector<std::pair<int32_t, int32_t>> push(icons.size());
        for (int pass = 0; pass < 20 && !icons.empty(); ++pass) {
            std::fill(push.begin(), push.end(), std::make_pair(0, 0));
            for (size_t a = 0; a < icons.size(); ++a) {
                for (size_t b = a + 1; b < icons.size(); ++b) {
                    int32_t dx = icons[a]->x - icons[b]->x;
                    int32_t dy = icons[a]->y - icons[b]->y;
                    const int32_t d2 = dy * dy + dx * dx;
                    if (!(static_cast<float>(d2) < reach2)) continue;
                    if (d2 == 0) {
                        dx = kNudge[nudge][0];
                        dy = kNudge[nudge][1];
                        nudge = (nudge + 1) % 3;
                    }
                    const float len = std::sqrt(static_cast<float>(dx) * static_cast<float>(dx) +
                                                static_cast<float>(dy) * static_cast<float>(dy));
                    const float k = reach / len;
                    const int32_t ox = static_cast<int32_t>(k * static_cast<float>(dx));
                    const int32_t oy = static_cast<int32_t>(k * static_cast<float>(dy));
                    if (icons[a]->movable) {
                        push[a].first += ox;
                        push[a].second += oy;
                    }
                    if (d2 != 0 && icons[b]->movable) {
                        push[b].first -= ox;
                        push[b].second -= oy;
                    }
                }
            }
            float most = -9999999.0f;
            for (const auto& p : push) {
                most = std::max(most, std::fabs(static_cast<float>(p.first)));
                most = std::max(most, std::fabs(static_cast<float>(p.second)));
            }
            if (most == 0.0f) break;
            if (step < most) {
                const float f = step / most;
                for (auto& p : push) {
                    p.first = static_cast<int32_t>(static_cast<float>(p.first) * f);
                    p.second = static_cast<int32_t>(static_cast<float>(p.second) * f);
                }
            }
            for (size_t i = 0; i < icons.size(); ++i) {
                icons[i]->x += push[i].first;
                icons[i]->y += push[i].second;
            }
        }
        for (PoiSlot* s : icons) s->y = static_cast<int32_t>(static_cast<float>(s->y) * 1.3332999944686890f);
    }
    return 0;
}

/// QuestPOIGetIconInfo(questId) - 0x005E0590: isComplete, x, y, objective -
/// or nothing at all when the quest's icon is not on this map.
static int lua_QuestPOIGetIconInfo(lua_State* L) {
    if (!worldmap::ready(L)) return lua_QuestPOIGetIconInfo_Legacy(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: QuestPOIGetIconInfo(questID)");
    auto* gh = getGameHandler(L);
    const uint32_t questId = static_cast<uint32_t>(lua_tonumber(L, 1));
    if (!poiSetFor(gh, questId)) return 0;
    const PoiSlot& slot = poiSlotFor(questId);
    const int dungeon = worldmap::currentDungeonMapId(L);
    int dmMap = 0, dmFloor = 0;
    const int override = worldmap::dungeonMapInfo(L, dungeon, dmMap, dmFloor) ? dungeon : 0;
    float u = 0.0f, v = 0.0f;
    bool hidden = false;
    const bool placed = worldmap::worldToMap(L, static_cast<float>(slot.x),
        static_cast<float>(slot.y), 0.0f, static_cast<uint32_t>(slot.mapId), override, u, v, &hidden);
    if (!placed || hidden || std::fabs(u * v) < 0.001f) return 0;
    lua_pushboolean(L, slot.mask == 0xFFFFFFFFu);
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    lua_pushnumber(L, slot.objective);
    return 4;
}

/// QuestPOIGetQuestIDByVisibleIndex(index) - 0x005E5750 / 0x005E3840:
/// questId, questLogIndex.
static int lua_QuestPOIGetQuestIDByVisibleIndex(lua_State* L) {
    if (!worldmap::ready(L)) return lua_QuestPOIGetQuestIDByVisibleIndex_Legacy(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: QuestPOIGetQuestIDByVisibleIndex(index)");
    auto* gh = getGameHandler(L);
    const int want = static_cast<int>(lua_tonumber(L, 1)) - 1;
    if (!gh || want < 0 || want > 24) return 0;

    struct Entry { uint32_t questId; bool done; bool watched; };
    std::vector<Entry> visible;
    for (const auto& set : gh->getQuestPoiSets()) {
        const PoiSlot& slot = poiSlotFor(set.questId);
        if (!slot.visible) continue;
        const auto* q = questInLog(gh, set.questId);
        if (!q) continue;
        visible.push_back({set.questId, q->complete || questHasNoObjectives(*q),
                           gh->isQuestTracked(set.questId)});
    }
    std::vector<uint32_t> ordered;
    // Finished first, in slot order.
    for (const auto& e : visible) if (e.done) ordered.push_back(e.questId);
    // Then the watched ones, in watch order (the quest log's order here).
    for (const auto& row : questRows(gh)) {
        if (!row.quest || !gh->isQuestTracked(row.quest->questId)) continue;
        for (const auto& e : visible) {
            if (e.questId == row.quest->questId && !e.done) ordered.push_back(e.questId);
        }
    }
    // Then everything else.
    for (const auto& e : visible) if (!e.done && !e.watched) ordered.push_back(e.questId);

    if (want >= static_cast<int>(ordered.size())) return 0;
    const uint32_t questId = ordered[static_cast<size_t>(want)];
    lua_pushnumber(L, questId);
    lua_pushnumber(L, questLogRowOf(gh, questId));
    return 2;
}

/// GetQuestWorldMapAreaID(questId) - 0x005E56C0 / 0x005E2B60: the map the
/// quest's work is on, and its floor. The client moves the world map to the
/// player's zone first and then asks which block is best from there.
static int lua_GetQuestWorldMapAreaID(lua_State* L) {
    if (!worldmap::ready(L)) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetQuestWorldMapAreaID(questID)");
    auto* gh = getGameHandler(L);
    const uint32_t questId = static_cast<uint32_t>(lua_tonumber(L, 1));
    int areaId = 0, floor = 0;
    if (gh && gh->getPlayerGuid() != 0 && questId > 0) {
        worldmap::setMapToCurrentZone(L);
        const int here = worldmap::currentWorldMapAreaId(L);
        const int hereMap = worldmap::currentWorldMapAreaMapId(L);
        int dmMap = 0, hereFloor = 0;
        if (!worldmap::dungeonMapInfo(L, worldmap::currentDungeonMapId(L), dmMap, hereFloor)) hereFloor = 0;
        const auto* set = poiSetFor(gh, questId);
        if (here != -1 && set) {
            PoiSlot& slot = poiSlotFor(questId);
            slot.mask = questObjectiveMask(gh, questId);
            const auto& mi = gh->getMovementInfo();
            const float playerX = mi.y, playerY = mi.x;
            int bestFloorDelta = 0x7FFFFFFF;
            float bestDist = 3.4028234663852886e+38f;
            int candidate = 0, candidateFloor = 0;
            for (const auto& a : set->areas) {
                if (!poiForObjectives(slot.mask, a.objectiveIndex)) continue;
                if (static_cast<int>(a.worldMapAreaId) == here &&
                    static_cast<int>(a.floor) == hereFloor) {
                    lua_pushnumber(L, a.worldMapAreaId);
                    lua_pushnumber(L, a.floor);
                    return 2;
                }
                if (worldmap::worldMapAreaMapId(L, static_cast<int>(a.worldMapAreaId)) == hereMap) {
                    const float dx = playerX - static_cast<float>(a.centerX);
                    const float dy = playerY - static_cast<float>(a.centerY);
                    const int delta = std::abs(hereFloor - static_cast<int>(a.floor));
                    const float dist = dy * dy + dx * dx;
                    if (delta < bestFloorDelta || (delta == bestFloorDelta && dist < bestDist)) {
                        candidate = static_cast<int>(a.worldMapAreaId);
                        candidateFloor = static_cast<int>(a.floor);
                        bestFloorDelta = delta;
                        bestDist = dist;
                    }
                } else if (candidate == 0 &&
                           worldmap::worldMapAreaMapId(L, static_cast<int>(a.worldMapAreaId)) != -1) {
                    candidate = static_cast<int>(a.worldMapAreaId);
                    candidateFloor = static_cast<int>(a.floor);
                }
            }
            if (candidate != 0) {
                areaId = candidate;
                floor = candidateFloor;
            }
        }
        if (areaId == 0) {
            // No blocks: the quest's own zone (its zoneOrSort, taken up to
            // the top-level area) and that zone's WorldMapArea.
            if (const auto* q = questInLog(gh, questId); q && q->zoneOrSort > 0) {
                areaId = worldmap::worldMapAreaForZone(L, q->zoneOrSort);
            }
        }
    }
    lua_pushnumber(L, areaId);
    lua_pushnumber(L, floor);
    return 2;
}

// ── WorldMapBlobFrame ───────────────────────────────────────────────────────
//
// A QuestPOIFrame (CGQuestPOIFrame, 0x005905F0 for DrawQuestBlob) holds up to
// four quests and shades each one's blocks - the objective areas - for the
// objectives still open, on the map showing. WorldMapFrame turns a quest on
// when it is selected and off otherwise; game::GameHandler keeps that set.
// UpdateMouseOverTooltip answers which quest's areas are under the cursor and
// how many, and GetTooltipIndex which objective each of those is for.
namespace {

struct BlobShape {
    uint32_t questId = 0;
    int objective = -1;
    std::vector<std::pair<float, float>> uv;
};

std::vector<BlobShape> blobShapes(lua_State* L) {
    std::vector<BlobShape> out;
    auto* gh = getGameHandler(L);
    if (!gh || gh->getPlayerGuid() == 0 || !worldmap::ready(L)) return out;
    const PoiView view = poiView(L);
    if (!view.valid) return out;
    int continent = -1, zone = -1;
    worldmap::currentView(L, continent, zone);
    if (zone == -1 && continent != -2 && !view.dungeon) return out;   // continents carry none
    for (const auto& set : gh->getQuestPoiSets()) {
        if (!gh->isQuestBlobShown(set.questId)) continue;
        const uint32_t mask = questObjectiveMask(gh, set.questId);
        for (const auto& a : set.areas) {
            if (a.points.size() < 3) continue;
            if (!poiForObjectives(mask, a.objectiveIndex)) continue;
            if (!poiOnMap(L, view, a)) continue;
            BlobShape shape;
            shape.questId = set.questId;
            shape.objective = a.objectiveIndex;
            const int override = view.dungeon ? view.dungeonMapId : 0;
            for (const auto& p : a.points) {
                float u = 0.0f, v = 0.0f;
                worldmap::worldToMap(L, static_cast<float>(p.first), static_cast<float>(p.second),
                                     0.0f, a.mapId, override, u, v, nullptr, /*noClamp=*/true);
                shape.uv.emplace_back(u, v);
            }
            out.push_back(std::move(shape));
        }
    }
    return out;
}

bool pointInPolygon(const std::vector<std::pair<float, float>>& poly, float x, float y) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const float xi = poly[i].first, yi = poly[i].second;
        const float xj = poly[j].first, yj = poly[j].second;
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
    }
    return inside;
}

std::vector<int>& blobTooltipObjectives() {
    static std::vector<int> objectives;
    return objectives;
}

}  // namespace

std::vector<std::vector<std::pair<float, float>>> questBlobPolygons(lua_State* L) {
    std::vector<std::vector<std::pair<float, float>>> out;
    for (auto& s : blobShapes(L)) out.push_back(std::move(s.uv));
    return out;
}

/// WorldMapBlobFrame:UpdateMouseOverTooltip(x, y) -> questLogIndex, count, for
/// x and y as fractions of the frame.
int lua_QuestBlobFrame_UpdateMouseOverTooltip(lua_State* L) {
    const float x = static_cast<float>(luaL_optnumber(L, 2, -1));
    const float y = static_cast<float>(luaL_optnumber(L, 3, -1));
    auto& objectives = blobTooltipObjectives();
    objectives.clear();
    uint32_t questId = 0;
    for (const auto& s : blobShapes(L)) {
        if (questId != 0 && s.questId != questId) continue;
        if (!pointInPolygon(s.uv, x, y)) continue;
        questId = s.questId;
        objectives.push_back(s.objective);
    }
    if (questId == 0) return 0;
    lua_pushnumber(L, questLogRowOf(getGameHandler(L), questId));
    lua_pushnumber(L, static_cast<lua_Number>(objectives.size()));
    return 2;
}

/// WorldMapBlobFrame:GetNumTooltips() - how many areas the last hit found.
int lua_QuestBlobFrame_GetNumTooltips(lua_State* L) {
    const auto& objectives = blobTooltipObjectives();
    if (objectives.empty()) return luaReturnNil(L);
    lua_pushnumber(L, static_cast<lua_Number>(objectives.size()));
    return 1;
}

/// WorldMapBlobFrame:GetTooltipIndex(i) - the objective index of the i-th.
int lua_QuestBlobFrame_GetTooltipIndex(lua_State* L) {
    const int i = static_cast<int>(luaL_optnumber(L, 2, 0));
    const auto& objectives = blobTooltipObjectives();
    if (i < 1 || i > static_cast<int>(objectives.size())) return luaReturnNil(L);
    lua_pushnumber(L, objectives[static_cast<size_t>(i - 1)]);
    return 1;
}

/// The QuestPOI set, the only binding of each of these names.
void registerQuestPoiLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction fn; } api[] = {
        {"QuestMapUpdateAllQuests", lua_QuestMapUpdateAllQuests},
        {"QuestPOIUpdateIcons", lua_QuestPOIUpdateIcons},
        {"QuestPOIGetIconInfo", lua_QuestPOIGetIconInfo},
        {"QuestPOIGetQuestIDByVisibleIndex", lua_QuestPOIGetQuestIDByVisibleIndex},
        {"GetQuestWorldMapAreaID", lua_GetQuestWorldMapAreaID},
    };
    for (const auto& a : api) {
        lua_pushcfunction(L, a.fn);
        lua_setglobal(L, a.name);
    }
}

/// GetQuestPOILeaderBoard(objectiveIndex, questId) → the objective's text and
/// counts, the same as the quest log's version - except that this one is given
/// a quest id where that one takes a log index. Aliasing the two would look
/// right and read the wrong quest, so the id is turned into an index here.
static int lua_GetQuestPOILeaderBoard(lua_State* L) {
    // GetQuestPOILeaderBoard(questPOIIndex, questLogIndex) - 0x005E6650.
    //
    // The first number is a POI block's objective index, as the blob frame's
    // GetTooltipIndex hands it out: 0-3 the kill and object counts, 4-9 the
    // items. The second is a quest log index, the same as
    // GetQuestLogLeaderBoard's - WorldMapQuestPOI_SetTooltip passes the one it
    // has. The line answered is the one for that objective.
    auto* gh = getGameHandler(L);
    const int poiIndex = static_cast<int>(luaL_checknumber(L, 1));
    const int logIndex = static_cast<int>(luaL_optnumber(L, 2, 0));
    const auto* qp = (gh && logIndex > 0) ? questAtRow(gh, logIndex) : nullptr;
    if (!qp) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }
    const std::vector<game::QuestObjective> lines = game::questObjectives(*gh, *qp);
    int line = 0, found = -1;
    for (size_t i = 0; i < qp->killObjectives.size(); ++i) {
        const auto& kill = qp->killObjectives[i];
        if (!game::isQuestObjective(kill.npcOrGoId, kill.required)) continue;
        if (static_cast<int>(i) == poiIndex) found = line;
        ++line;
    }
    for (size_t i = 0; i < qp->itemObjectives.size(); ++i) {
        const auto& item = qp->itemObjectives[i];
        if (!game::isQuestObjective(static_cast<int32_t>(item.itemId), item.required)) continue;
        if (static_cast<int>(4 + i) == poiIndex) found = line;
        ++line;
    }
    if (found < 0 || found >= static_cast<int>(lines.size())) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }
    const game::QuestObjective& o = lines[static_cast<size_t>(found)];
    lua_pushstring(L, o.text.c_str());
    lua_pushstring(L, o.type);
    if (o.finished) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 3;
}

/// The objective's line, its kind and whether it is done - through the one
/// builder the map window's quest list uses too (game/quest_objectives.hpp).
static int lua_GetQuestLogLeaderBoard(lua_State* L) {
    auto* gh = getGameHandler(L);
    int objIdx = static_cast<int>(luaL_checknumber(L, 1));
    int questIdx = static_cast<int>(luaL_optnumber(L, 2,
        gh ? gh->getSelectedQuestLogIndex() : 0));
    if (!gh || questIdx < 1 || objIdx < 1) { return luaReturnNil(L); }
    const auto* qp = questAtRow(gh, questIdx);
    if (!qp) { return luaReturnNil(L); }
    const std::vector<game::QuestObjective> lines = game::questObjectives(*gh, *qp);
    if (objIdx > static_cast<int>(lines.size())) {
        lua_pushnil(L);
        return 1;
    }
    const game::QuestObjective& line = lines[objIdx - 1];
    lua_pushstring(L, line.text.c_str());
    lua_pushstring(L, line.type);
    lua_pushboolean(L, line.finished ? 1 : 0);
    return 3;
}

// ExpandQuestHeader(index) / CollapseQuestHeader(index) - fold a zone away.
//
// The index is a display index naming a header row, and zero means every header
// at once, which is what the log's own "collapse all" does. Anything that is
// not a header is ignored rather than treated as row zero.
static int setQuestHeaderCollapsed(lua_State* L, bool collapse) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const auto rows = questRows(gh);
    if (index == 0) {
        collapsedQuestGroups().clear();
        if (collapse) {
            for (const auto& row : rows) {
                if (row.isHeader) collapsedQuestGroups().insert(row.group);
            }
        }
    } else if (index >= 1 && index <= static_cast<int>(rows.size()) &&
               rows[index - 1].isHeader) {
        if (collapse) collapsedQuestGroups().insert(rows[index - 1].group);
        else          collapsedQuestGroups().erase(rows[index - 1].group);
    }
    // The list the frame is walking just changed length underneath it, so it
    // has to be told to walk it again - nothing else fires for this.
    gh->fireAddonEvent("QUEST_LOG_UPDATE", {});
    return 0;
}
static int lua_ExpandQuestHeader(lua_State* L) { return setQuestHeaderCollapsed(L, false); }
static int lua_CollapseQuestHeader(lua_State* L) { return setQuestHeaderCollapsed(L, true); }

// GetQuestLogSpecialItemInfo(questLogIndex) -> link, texture, charges
//
// Answering nil meant no button was ever built, so a quest that gives you
// something to use looked like one that does not.
static int lua_GetQuestLogSpecialItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const QuestSpecialItem item = questSpecialItemAt(gh, index);
    if (!item.itemId) { return luaReturnNil(L); }

    const auto* info = gh->getItemInfo(item.itemId);
    // The name is what goes in the link, and without it there is no link to
    // return. The query was sent when the quest was read; until it answers,
    // there is honestly nothing to draw.
    if (!info || info->name.empty()) { return luaReturnNil(L); }

    const uint32_t quality = info->quality < 8 ? info->quality : 1u;
    const std::string link = game::itemChatLink(item.itemId, quality, info->name);
    lua_pushstring(L, link.c_str());                                        // 1: link

    // A nil texture is an empty slot to the interface, and the button draws
    // its background art instead of the item.
    const std::string icon = info->displayInfoId
        ? gh->getItemIconPath(info->displayInfoId) : std::string();
    lua_pushstring(L, icon.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                   : icon.c_str());                 // 2: texture
    // WatchFrameItem_OnUpdate compares this against the count it drew with and
    // rebuilds the whole frame when it changes, so it has to be stable.
    lua_pushnumber(L, item.count > 0 ? item.count : 1);             // 3: charges
    return 3;
}

static int lua_UseQuestLogSpecialItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const QuestSpecialItem item = questSpecialItemAt(gh, index);
    if (!item.itemId) return 0;
    if (item.bag == 0) gh->useItemBySlot(item.slot - 1);
    else               gh->useItemInBag(item.bag - 1, item.slot - 1);
    return 0;
}

/// The player's skills in the order the list shows them: by name.
///
/// They live in an unordered_map, and the skill list used to be read out of it
/// by counting to the asked-for position. That order is arbitrary - the list
/// came out in no order a player could recognise - and worse, it is not fixed:
/// learning a skill inserts, inserting can rehash, and rehashing reorders a
/// list already drawn under a selection held as an index. The skill being
/// looked at would quietly become a different one.
///
/// By name, with the id breaking ties, so the answer is the same every time it
/// is asked and reads like a list rather than a spill. Ties matter: two skills
/// can share a name before the name cache has resolved either.
static std::vector<uint32_t> skillOrder(game::GameHandler* gh) {
    std::vector<uint32_t> ids;
    if (!gh) return ids;
    const auto& skills = gh->getPlayerSkills();
    ids.reserve(skills.size());
    for (const auto& [id, skill] : skills) ids.push_back(id);
    std::sort(ids.begin(), ids.end(), [gh](uint32_t a, uint32_t b) {
        const std::string &na = gh->getSkillName(a), &nb = gh->getSkillName(b);
        if (na != nb) return na < nb;
        return a < b;
    });
    return ids;
}

/// One drawn line of the skills tab: a heading, or a skill under one.
///
/// The tab is a flat list with a heading row every so often, exactly like the
/// reputation panel - GetSkillLineInfo answers `header` and `isExpanded` for
/// each row, and a closed heading takes its skills out of the list rather than
/// hiding them in place, because GetNumSkillLines is what the scroll frame
/// counts.
///
/// The grouping was previously reported as absent on the reasoning that "the
/// guard is in the data, not in the frame". The data was there the whole time:
/// getSkillCategory already answered for every skill, and SkillLineCategory.dbc
/// names the eight headings and gives the order to draw them in.
struct SkillRow {
    uint32_t skillId = 0;      ///< 0 for a heading
    uint32_t categoryId = 0;
    bool isHeader = false;
};

static std::vector<SkillRow> skillRows(game::GameHandler* gh) {
    std::vector<SkillRow> rows;
    if (!gh) return rows;

    // Category 12 is "Not Displayed", and it means it. A skill the file does
    // not categorise at all is kept rather than dropped: it is something the
    // player has, and losing it silently is worse than filing it last.
    constexpr uint32_t kNotDisplayed = 12;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> grouped;
    for (uint32_t id : skillOrder(gh)) {
        const uint32_t cat = gh->getSkillCategory(id);
        if (cat == kNotDisplayed) continue;
        grouped[{gh->getSkillCategorySortIndex(cat), cat}].push_back(id);
    }

    for (const auto& [key, ids] : grouped) {
        const uint32_t cat = key.second;
        // A category with no name is one this build's file does not describe;
        // its skills are still listed, just without a heading over them.
        if (cat != 0 && !gh->getSkillCategoryName(cat).empty()) {
            rows.push_back({0, cat, true});
            if (gh->isSkillCategoryCollapsed(cat)) continue;
        }
        for (uint32_t id : ids) rows.push_back({id, cat, false});
    }
    return rows;
}

// --- The stable ---
//
// Everything here was already tracked: the pets, their levels, how many slots
// the player has bought, and the three commands that list, store and retrieve.
// Only the questions the original interface asks were missing, so its stable
// opened on nothing however many pets were in it.
//
// Slot 0 is the pet that is out, and 1 upwards are the ones stabled. That is
// the interface's numbering, not this client's - the pets arrive in one list
// with a flag saying which is active.
namespace {

/// An achievement's artwork, as a path.
///
/// Achievement.dbc names an icon by SpellIcon.dbc id and the loader has been
/// caching that id all along; the two bindings that answer an icon were pushing
/// a constant path and the raw id respectively, so every achievement in the
/// panel wore the same picture. The resolver was already here - the spellbook's
/// tabs use it for the icons SkillLine.dbc names the same way.
///
/// The generic one stays as the fallback: eighty-seven of the 1817 rows name no
/// icon at all, and a blank texture reads as a missing file rather than as a
/// row without artwork.
std::string achievementIconPath(game::GameHandler* gh, uint32_t achievementId) {
    static const std::string kGeneric = "Interface\\Icons\\Achievement_General";
    if (!gh) return kGeneric;
    const uint32_t iconId = gh->getAchievementIconId(achievementId);
    if (iconId == 0) return kGeneric;
    std::string path = gh->getIconPath(iconId);
    return path.empty() ? kGeneric : path;
}

/// The stabled pets, in the order they arrived, with the active one left out.
std::vector<const game::GameHandler::StabledPet*> stabledOnly(game::GameHandler* gh) {
    std::vector<const game::GameHandler::StabledPet*> out;
    if (!gh) return out;
    for (const auto& p : gh->getStabledPets()) {
        if (!p.isActive) out.push_back(&p);
    }
    return out;
}

/// The pet that is currently out, or null if none is.
const game::GameHandler::StabledPet* activePet(game::GameHandler* gh) {
    if (!gh) return nullptr;
    for (const auto& p : gh->getStabledPets()) {
        if (p.isActive) return &p;
    }
    return nullptr;
}

/// Which pet the player has clicked, as the client keeps it (0xC0F6D0): a
/// stabled pet's number, kSelectedCurrent for the pet that is out, or 0 for
/// none. Kept by number, not slot, because every stable or unstable sends the
/// list again; and each list resets it to none (0x5A11A0), which is what makes
/// PetStable_Update pick the current pet, or the first, afresh.
constexpr uint32_t kSelectedCurrent = 0xFFFFFFFFu;
uint32_t& selectedStablePet(game::GameHandler* gh) {
    static uint32_t number = 0;
    static uint32_t listSeen = 0;
    if (gh && gh->getStableListGeneration() != listSeen) {
        listSeen = gh->getStableListGeneration();
        number = 0;
    }
    return number;
}

/// GetSelectedStablePet's numbering (0x5A0E40, plus one): 0 the pet that is
/// out, 1.. a stabled one, -1 none.
int selectedStableSlot(game::GameHandler* gh) {
    const uint32_t sel = selectedStablePet(gh);
    if (sel == kSelectedCurrent) return 0;
    if (sel == 0) return -1;
    const auto stabled = stabledOnly(gh);
    for (size_t i = 0; i < stabled.size(); ++i) {
        if (stabled[i]->petNumber == sel) return static_cast<int>(i) + 1;
    }
    return -1;
}

/// The stable entry behind a slot: 0 the current one, 1.. the stabled ones.
const game::GameHandler::StabledPet* stableEntry(game::GameHandler* gh, int slot) {
    if (slot == 0) return activePet(gh);
    const auto stabled = stabledOnly(gh);
    if (slot >= 1 && slot <= static_cast<int>(stabled.size())) return stabled[static_cast<size_t>(slot - 1)];
    return nullptr;
}

/// A stabled pet's family, from its template - asked for when unknown, with
/// PET_STABLE_UPDATE raised when it arrives (0x67B6A0 with 0x5A0E80).
const game::CreatureFamilyInfo* stableFamily(game::GameHandler* gh, uint32_t entry) {
    if (!gh || entry == 0) return nullptr;
    const auto& cache = gh->getCreatureInfoCache();
    auto it = cache.find(entry);
    if (it == cache.end()) {
        gh->queryCreatureInfo(entry, 0);
        return nullptr;
    }
    return gh->petDbc().family(it->second.family);
}

/// The pet talent tree a family trains, by TalentTab's PetTalentMask.
std::string petTreeName(game::GameHandler* gh, const game::CreatureFamilyInfo* fam) {
    if (!gh || !fam || fam->petTalentType < 0) return {};
    gh->loadTalentDbc();
    const uint32_t bit = 1u << static_cast<uint32_t>(fam->petTalentType);
    for (const auto& [id, tab] : gh->getAllTalentTabs()) {
        if (tab.petTalentMask & bit) return tab.name;
    }
    return {};
}

/// The player's own hunter pet's family, or null for any other pet.
const game::CreatureFamilyInfo* hunterPetFamily(game::GameHandler* gh) {
    if (!gh || !gh->isHunterPet()) return nullptr;
    return gh->petDbc().family(gh->petCreatureFamily());
}

} // namespace

// GetStablePetInfo(slot) → icon, name, level, family, talent (0x5A1330)
//
// Slot 0 is the pet that is out, 1.. the stabled ones. The icon and the two
// names come from the pet's CreatureFamily row, which the client finds through
// the pet's creature template; until that is known the slot answers
// nil, nil, 0 and reads as empty, and the template's arrival redraws it. The
// talent tree is the fifth return only for a family that trains one.
static int lua_GetStablePetInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto* pet = gh ? stableEntry(gh, slot) : nullptr;
    const auto* fam = pet ? stableFamily(gh, pet->entry) : nullptr;
    if (!pet || !fam) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnumber(L, 0);
        return 3;
    }
    lua_pushstring(L, fam->icon.c_str());
    lua_pushstring(L, pet->name.c_str());
    lua_pushnumber(L, pet->level);
    lua_pushstring(L, fam->name.c_str());
    const std::string tree = petTreeName(gh, fam);
    if (tree.empty()) return 4;
    lua_pushstring(L, tree.c_str());
    return 5;
}

static int lua_GetNumStablePets(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(stabledOnly(getGameHandler(L)).size()));
    return 1;
}

static int lua_GetNumStableSlots(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getStableSlots() : 0);
    return 1;
}

static int lua_GetSelectedStablePet(lua_State* L) {
    lua_pushnumber(L, selectedStableSlot(getGameHandler(L)));
    return 1;
}

// ClickStablePet(slot) (0x5A1CA0): with no stable pet on the cursor, select
// the slot and answer true (the frame redraws on that). With one, the drop:
// the pet that is out onto an empty slot stables it (CMSG_STABLE_PET), onto an
// occupied one swaps (CMSG_STABLE_SWAP_PET); a stabled pet onto the current
// slot unstables it (CMSG_UNSTABLE_PET), or swaps when a pet is out. The
// cursor clears and the answer is nil; the server's list redraws.
static int lua_ClickStablePet(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) return 0;
    const int carried = cursorStableSlot();
    if (carried < 0) {
        if (slot == 0) selectedStablePet(gh) = kSelectedCurrent;
        else if (const auto* p = stableEntry(gh, slot)) selectedStablePet(gh) = p->petNumber;
        else selectedStablePet(gh) = 0;
        gh->fireAddonEvent("PET_STABLE_UPDATE_PAPERDOLL", {});
        lua_pushboolean(L, 1);
        return 1;
    }
    const auto* clicked = stableEntry(gh, slot);
    const auto* held = stableEntry(gh, carried);
    const auto r = game::pet::stableClick(carried, slot, clicked != nullptr,
                                          clicked ? clicked->petNumber : 0,
                                          held ? held->petNumber : 0, held != nullptr,
                                          gh->getStableSlots(), gh->hasPet());
    switch (r.op) {
        case game::pet::StableClick::Stable:   gh->stablePet(0); break;
        case game::pet::StableClick::Swap:     gh->swapStablePet(r.petNumber); break;
        case game::pet::StableClick::Unstable: gh->unstablePet(r.petNumber); break;
        default: break;
    }
    clearCursor(L);
    lua_pushnil(L);
    return 1;
}

// GetPetIcon / GetPetFoodTypes / GetPetTalentTree: the player's hunter pet's
// family icon (0x5D3CE0), diet names (0x5D3BD0) and pet talent tree's name
// (0x5D3D80), from CreatureFamily. Any other pet - a warlock's demon, a
// ghoul, a water elemental - has none of them.
static int lua_GetPetIcon(lua_State* L) {
    const auto* fam = hunterPetFamily(getGameHandler(L));
    if (!fam || fam->icon.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, fam->icon.c_str());
    return 1;
}
static int lua_GetPetTalentTree(lua_State* L) {
    auto* gh = getGameHandler(L);
    const std::string tree = petTreeName(gh, hunterPetFamily(gh));
    if (tree.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, tree.c_str());
    return 1;
}
static int lua_GetPetFoodTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* fam = hunterPetFamily(gh);
    if (!fam) return 0;
    const auto names = gh->petDbc().foodNames(fam->petFoodMask);
    for (const auto& n : names) lua_pushstring(L, n.c_str());
    return static_cast<int>(names.size());
}

/// SetPetStablePaperdoll(model) (0x5A1530): the selected pet in the preview -
/// the pet that is out from its own unit when it is a hunter's pet in view,
/// otherwise the template of the stable entry, by its first display.
static int lua_SetPetStablePaperdoll(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* tree = getWidgetTree(L);
    if (!gh || !tree || !lua_istable(L, 1)) {
        return luaL_error(L, "Usage: SetPetStablePaperdoll(characterModel)");
    }
    lua_getfield(L, 1, "__wid");
    const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    auto* w = tree->get(id);
    if (!w) return 0;
    const int slot = selectedStableSlot(gh);
    uint32_t display = 0;
    if (slot == 0) {
        if (gh->hasPet() && gh->isHunterPet()) {
            if (auto* u = gh->getUnitByGuid(gh->getPetGuid())) display = u->getDisplayId();
        }
        if (display == 0) {
            if (const auto* cur = activePet(gh)) display = gh->getCreatureDisplayIdForEntry(cur->entry);
        }
    } else if (const auto* p = stableEntry(gh, slot)) {
        display = gh->getCreatureDisplayIdForEntry(p->entry);
    }
    w->modelDisplayId = display;
    return 0;
}

/// PickupStablePet(slot) (0x5A0FE0 -> 0x520C60): a pet whose family is known
/// goes onto the cursor with its family icon; ClickStablePet on another slot
/// is the drop.
static int lua_PickupStablePet(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    clearCursor(L);
    const auto* pet = gh ? stableEntry(gh, slot) : nullptr;
    const auto* fam = pet ? stableFamily(gh, pet->entry) : nullptr;
    if (!fam || fam->icon.empty()) return 0;
    pickUpStablePet(L, slot, fam->icon);
    return 0;
}

/// GetStablePetFoodTypes(slot) (0x5A16A0): the slot's diet names, one return
/// each. Nothing for an unknown family - and the frame formats the list
/// without testing it, so a known pet always has at least one.
static int lua_GetStablePetFoodTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto* pet = gh ? stableEntry(gh, slot) : nullptr;
    const auto* fam = pet ? stableFamily(gh, pet->entry) : nullptr;
    if (!fam) return 0;
    const auto names = gh->petDbc().foodNames(fam->petFoodMask);
    for (const auto& n : names) lua_pushstring(L, n.c_str());
    return static_cast<int>(names.size());
}

static int lua_ClosePetStables(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->closeStableWindow();
    return 0;
}

static int lua_IsAtStableMaster(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isStableWindowOpen() ? 1 : 0);
    return 1;
}

// GetNextStableSlotCost() → what the next slot costs, in copper.
// StablePet() / UnstablePet(slot) (0x5A19C0 / 0x5A1AC0): the API forms of
// the two moves, which the stable frame itself does by dragging.
static int lua_StablePet(lua_State* L) {
    if (auto* gh = getGameHandler(L); gh && gh->isStableWindowOpen()) gh->stablePet(0);
    return 0;
}
static int lua_UnstablePet(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || gh->hasPet() || !gh->isStableWindowOpen()) return 0;
    if (const auto* p = stableEntry(gh, slot); p && slot >= 1) gh->unstablePet(p->petNumber);
    return 0;
}

static int lua_GetNextStableSlotCost(lua_State* L) {
    // StableSlotPrices.dbc's row for the next slot (0x5A14D0): 5s, 50s, 50g
    // and 150g in 3.3.5; 0 past the fourth.
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->petDbc().stableSlotPrice(gh->getStableSlots() + 1u) : 0);
    return 1;
}

static int lua_GetNumSkillLines(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    // The drawn rows, not the skills: headings are rows too, and a closed one
    // takes its skills out of the list entirely.
    lua_pushnumber(L, static_cast<double>(skillRows(gh).size()));
    return 1;
}

/// Open or close the heading at a drawn-row index. Shared by the two verbs,
/// which differ only in the boolean and must resolve the index the same way.
static int skillHeaderSetCollapsed(lua_State* L, bool collapsed) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || index < 1) return 0;
    const auto rows = skillRows(gh);
    if (index > static_cast<int>(rows.size())) return 0;
    const auto& row = rows[static_cast<size_t>(index) - 1];
    if (row.isHeader) gh->setSkillCategoryCollapsed(row.categoryId, collapsed);
    return 0;
}

// GetSkillLineInfo(index) → skillName, isHeader, isExpanded, skillRank, numTempPoints, skillModifier, skillMaxRank, isAbandonable, stepCost, rankCost, minLevel, skillCostType
static int lua_GetSkillLineInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    // optnumber, not checknumber: a nil index is a question this can answer
    // with nil, and raising instead takes down whatever file asked. SkillFrame
    // calls SkillDetailFrame_SetStatusBar with no selection during its own
    // load, which is exactly that question.
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    // An index with no skill behind it answers as an empty skill rather than a
    // single nil. SkillFrame_UpdateSkills passes GetSelectedSkill() straight in
    // and adds two of the results together on the next line, and before the
    // server has sent any skills there is nothing selected - so a bare nil
    // there is arithmetic on nothing and the file is lost. An empty row is the
    // truthful answer and it costs no more than a blank line in the list.
    // Short-circuit rather than a ternary: binding a reference across both arms
    // would copy the whole skill map on every call.
    const auto rows = gh ? skillRows(gh) : std::vector<SkillRow>{};
    if (!gh || index < 1 || index > static_cast<int>(rows.size())) {
        lua_pushstring(L, "");                          // 1: skillName
        lua_pushboolean(L, 0);                          // 2: isHeader
        lua_pushboolean(L, 1);                          // 3: isExpanded
        for (int i = 4; i <= 7; ++i) lua_pushnumber(L, 0);   // rank, temp, mod, max
        lua_pushboolean(L, 0);                          // 8: isAbandonable
        // stepCost and rankCost nil for the same reason they are nil below:
        // zero is true in Lua, so a zero here sends the empty row down the
        // "Learn <skill>" branch. The fix went in on the real path and this
        // fallback kept its zeros, which is how a fix half-lands.
        lua_pushnil(L);                                 // 9: stepCost
        lua_pushnil(L);                                 // 10: rankCost
        lua_pushnumber(L, 0);                           // 11: minLevel
        lua_pushnumber(L, 0);                           // 12: skillCostType
        lua_pushstring(L, "");                          // 13: skillDescription
        return 13;
    }
    const auto& row = rows[static_cast<size_t>(index) - 1];
    if (row.isHeader) {
        // A heading carries its name and its open/closed state and nothing
        // else. The costs stay nil for the same reason they are nil below -
        // zero is true in Lua and would send the heading down the
        // "Learn <skill>" branch.
        lua_pushstring(L, gh->getSkillCategoryName(row.categoryId).c_str());
        lua_pushboolean(L, 1);                          // 2: isHeader
        lua_pushboolean(L, gh->isSkillCategoryCollapsed(row.categoryId) ? 0 : 1);
        for (int i = 4; i <= 7; ++i) lua_pushnumber(L, 0);
        lua_pushboolean(L, 0);                          // 8: isAbandonable
        lua_pushnil(L);                                 // 9: stepCost
        lua_pushnil(L);                                 // 10: rankCost
        lua_pushnumber(L, 0);                           // 11: minLevel
        lua_pushnumber(L, 0);                           // 12: skillCostType
        lua_pushstring(L, "");                          // 13: skillDescription
        return 13;
    }
    const auto& skills = gh->getPlayerSkills();
    const auto found = skills.find(row.skillId);
    if (found == skills.end()) { return luaReturnNil(L); }
    const auto& skill = found->second;
    std::string name = gh->getSkillName(skill.skillId);
    if (name.empty()) name = "Skill " + std::to_string(skill.skillId);

    lua_pushstring(L, name.c_str());                    // 1: skillName
    lua_pushboolean(L, 0);                              // 2: isHeader
    lua_pushboolean(L, 1);                              // 3: isExpanded
    lua_pushnumber(L, skill.effectiveValue());           // 4: skillRank
    lua_pushnumber(L, skill.bonusTemp);                  // 5: numTempPoints
    lua_pushnumber(L, skill.bonusPerm);                  // 6: skillModifier
    lua_pushnumber(L, skill.maxValue);                   // 7: skillMaxRank
    lua_pushboolean(L, 0);                              // 8: isAbandonable
    // Nil, not zero, and this is the whole of the "Learn Mounts" mystery.
    //
    // SkillFrame_SetStatusBar branches on these three in order:
    //
    //     if ( stepCost ) then          -- a skill that must be bought
    //         statusBarName:SetFormattedText(LEARN_SKILL_TEMPLATE, skillName)
    //     elseif ( rankCost or numTempPoints > 0 ) then   -- trainable
    //     else                                            -- an ordinary skill
    //
    // Zero is *true* in Lua, so the first branch always won and every row in
    // the skills window was titled "Learn <skill>" - First Aid, Axes, Cooking,
    // everything - as though none of them were known.
    //
    // No purchase cost is tracked here, and nil is how the client says a skill
    // has none. With both nil and no temporary points, the third branch runs
    // and the row is simply the skill's name, which is what it should have
    // been reading all along.
    lua_pushnil(L);                                     // 9: stepCost
    lua_pushnil(L);                                     // 10: rankCost
    lua_pushnumber(L, 0);                               // 11: minLevel
    lua_pushnumber(L, 0);                               // 12: skillCostType
    // The sentence the detail panel prints under the selected skill.
    //
    // Returning twelve values left it nil, and SkillDetailFrame_SetStatusBar
    // feeds it straight into SetFormattedText(SKILL_DESCRIPTION, type, desc).
    // string.format raises on a nil %s, so the guarded SetFormattedText fell
    // back to writing the format string itself - the lower half of the skills
    // window showed "%s %s" where the description belongs.
    lua_pushstring(L, gh->getSkillDescription(skill.skillId).c_str());  // 13
    return 13;
}

// --- Friends/Ignore API ---


/// Whose talents a talent binding is being asked about.
///
/// Every talent binding takes an `inspect` flag that this client ignored, so
/// the inspect talent tab enumerated the viewer's own class tabs and read the
/// viewer's own ranks - the wrong tree under the target's name, rather than an
/// empty one. Zero means the inspect result has no class yet, in which case
/// falling back to the player's is the only thing left to do.
static uint8_t talentClassId(game::GameHandler* gh, bool inspect) {
    if (inspect && gh) {
        if (const auto* r = gh->getInspectResult()) {
            if (r->classId) return r->classId;
        }
    }
    return gh ? gh->getPlayerClass() : 0;
}

/// A class's talent tabs, in the order the interface numbers them.
///
/// Which tab is tab 1 is the whole of this: the interface asks for tabs by
/// index and expects them in orderIndex order, and a tab list built any other
/// way silently renames every tab. Four places worked it out for themselves -
/// GetNumTalentTabs, GetTalentTabInfo, the talent lookup and resolveTalentId -
/// and they agreed, which is luck rather than design in a file where the index
/// conventions have bitten before.
static std::vector<const game::GameHandler::TalentTabEntry*> classTalentTabs(
        game::GameHandler* gh, uint8_t classId) {
    std::vector<const game::GameHandler::TalentTabEntry*> tabs;
    if (!gh || classId == 0) return tabs;
    const uint32_t classMask = 1u << (classId - 1);
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) tabs.push_back(&tab);
    }
    std::sort(tabs.begin(), tabs.end(),
              [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    return tabs;
}

// Declared in lua_api_helpers.hpp; see TalentSource.
TalentSource talentSource(game::GameHandler* gh, bool inspect, int talentGroup) {
    static const std::unordered_map<uint32_t, uint8_t> kNoTalents;
    TalentSource src;
    src.ranks = &kNoTalents;
    if (!gh) return src;
    src.classId = talentClassId(gh, inspect);
    if (inspect) {
        src.inspect = true;
        // Only the inspected player's active group arrives with the inspect
        // result, so that is what any group of theirs reads as.
        if (const auto* r = gh->getInspectResult()) src.ranks = &r->talentRanks;
        return src;
    }
    const uint8_t active = gh->getActiveTalentSpec();
    uint8_t group = active;
    if (talentGroup >= 1 && talentGroup <= gh->getNumTalentGroups())
        group = static_cast<uint8_t>(talentGroup - 1);
    src.ranks = &gh->getLearnedTalents(group);
    src.previewable = (group == active);
    return src;
}

/// The `pet` argument: the pet's tree rather than the player's.
static bool wantsPetTalents(lua_State* L, int index) {
    return lua_toboolean(L, index) != 0;
}

// ── The pet's talent tree (TalentInfo.cpp; docs/re/pets.md) ────────────────
//
// One tree, the TalentTab whose PetTalentMask has the bit of the pet family's
// PetTalentType (0x5C64D0), for the family SMSG_PET_SPELLS named (0x5C9DF0).
// Its talents are every Talent.dbc row of that tab; a row whose CategoryMask
// leaves the family out answers as an empty button (0x5C5A20). Ranks and the
// unspent count are the pet's SMSG_TALENTS_INFO (type 1). The preview is the
// pet's own, kept apart from the player's.

/// The pet's family row, or null when the pet has no talents.
static const game::CreatureFamilyInfo* petTalentFamily(game::GameHandler* gh) {
    if (!gh || !gh->hasPet() || gh->getPetFamily() == 0) return nullptr;
    const auto* fam = gh->petDbc().family(gh->getPetFamily());
    return (fam && fam->petTalentType >= 0) ? fam : nullptr;
}

/// The pet's talent tabs, in TalentTab order (one in 3.3.5).
static std::vector<const game::GameHandler::TalentTabEntry*> petTalentTabs(game::GameHandler* gh) {
    std::vector<const game::GameHandler::TalentTabEntry*> tabs;
    const auto* fam = petTalentFamily(gh);
    if (!fam) return tabs;
    gh->loadTalentDbc();
    const uint32_t bit = 1u << static_cast<uint32_t>(fam->petTalentType);
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.petTalentMask & bit) tabs.push_back(&tab);
    }
    std::sort(tabs.begin(), tabs.end(), [](const auto* a, const auto* b) { return a->tabId < b->tabId; });
    return tabs;
}

/// The talents of a pet tab, by row, column and id - the id breaking the tie
/// between two family-specific talents that share a square.
static std::vector<const game::GameHandler::TalentEntry*> petTabTalents(game::GameHandler* gh, int tabIndex) {
    std::vector<const game::GameHandler::TalentEntry*> out;
    const auto tabs = petTalentTabs(gh);
    if (tabIndex < 1 || tabIndex > static_cast<int>(tabs.size())) return out;
    const uint32_t tabId = tabs[static_cast<size_t>(tabIndex - 1)]->tabId;
    for (const auto& [id, entry] : gh->getAllTalents()) {
        if (entry.tabId == tabId) out.push_back(&entry);
    }
    std::sort(out.begin(), out.end(), [](const auto* a, const auto* b) {
        if (a->row != b->row) return a->row < b->row;
        if (a->column != b->column) return a->column < b->column;
        return a->talentId < b->talentId;
    });
    return out;
}

const game::TalentEntry* petTalentAt(game::GameHandler* gh, int tabIndex, int talentIndex) {
    const auto talents = petTabTalents(gh, tabIndex);
    if (talentIndex < 1 || talentIndex > static_cast<int>(talents.size())) return nullptr;
    return talents[static_cast<size_t>(talentIndex - 1)];
}

/// Whether the pet's family is offered this talent (0x5C5A20).
bool petTalentShown(game::GameHandler* gh, const game::TalentEntry* talent) {
    const auto* fam = petTalentFamily(gh);
    return fam && talent &&
           game::pet::petTalentForFamily(talent->categoryMask[0], talent->categoryMask[1], fam->categoryEnumId);
}

int petTalentRank(game::GameHandler* gh, uint32_t talentId) {
    if (!gh) return 0;
    const auto& ranks = gh->getPetTalents();
    auto it = ranks.find(talentId);
    return it != ranks.end() ? it->second : 0;
}

/// Points staged in the pet's preview, and the pet talent packet a Learn is
/// waiting on: the staged points stay drawn until the server's answer lands,
/// so the tree does not drop back to the old ranks for a round trip.
static std::unordered_map<uint32_t, int>& petPreviewPoints() {
    static std::unordered_map<uint32_t, int> points;
    return points;
}
static bool& petPreviewSent() { static bool sent = false; return sent; }
static uint32_t& petPreviewGeneration() { static uint32_t g = 0; return g; }
static uint64_t& petPreviewPet() { static uint64_t g = 0; return g; }

static void settlePetPreview(game::GameHandler* gh) {
    if (!gh) return;
    // Another pet, or none: its staged points mean nothing now.
    if (petPreviewPet() != gh->getPetGuid()) {
        petPreviewPoints().clear();
        petPreviewSent() = false;
        petPreviewPet() = gh->getPetGuid();
    }
    if (petPreviewSent() && gh->getPetTalentGeneration() != petPreviewGeneration()) {
        petPreviewPoints().clear();
        petPreviewSent() = false;
    }
}

static int petPreviewTotal() {
    int total = 0;
    for (const auto& [id, n] : petPreviewPoints()) { (void)id; total += n; }
    return total;
}

static int petPreviewOf(uint32_t talentId) {
    auto it = petPreviewPoints().find(talentId);
    return it != petPreviewPoints().end() ? it->second : 0;
}

static int petGetTalentTabInfo(lua_State* L, game::GameHandler* gh, int tabIndex) {
    settlePetPreview(gh);
    const auto tabs = petTalentTabs(gh);
    if (tabIndex < 1 || tabIndex > static_cast<int>(tabs.size())) {
        lua_pushnil(L); lua_pushnil(L); lua_pushinteger(L, 0); lua_pushnil(L); lua_pushinteger(L, 0);
        return 5;
    }
    const auto* tab = tabs[static_cast<size_t>(tabIndex - 1)];
    int spent = 0, preview = 0;
    for (const auto* talent : petTabTalents(gh, tabIndex)) {
        spent += petTalentRank(gh, talent->talentId);
        preview += petPreviewOf(talent->talentId);
    }
    const std::string icon = tab->spellIconId ? gh->getIconPath(tab->spellIconId) : std::string{};
    lua_pushstring(L, tab->name.c_str());
    if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
    lua_pushinteger(L, spent);
    lua_pushstring(L, tab->backgroundFile.c_str());
    lua_pushinteger(L, preview);
    return 5;
}

static int petGetTalentInfo(lua_State* L, game::GameHandler* gh, int tabIndex, int talentIndex) {
    settlePetPreview(gh);
    const auto* talent = petTalentAt(gh, tabIndex, talentIndex);
    if (!talent || !petTalentShown(gh, talent)) {
        // The row 0x5C7800 answers for a talent the family is not offered.
        lua_pushnil(L); lua_pushnil(L);
        lua_pushnumber(L, 1); lua_pushnumber(L, 1);
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        lua_pushnil(L);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 10;
    }
    const int rank = petTalentRank(gh, talent->talentId);
    const int previewRank = rank + petPreviewOf(talent->talentId);
    const uint32_t shownSpell = talent->rankSpells[rank > 0 ? rank - 1 : 0];
    std::string name = gh->getSpellName(talent->rankSpells[0]);
    const std::string icon = gh->getSpellIconPath(shownSpell ? shownSpell : talent->rankSpells[0]);
    if (name.empty()) lua_pushnil(L); else lua_pushstring(L, name.c_str());
    if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
    lua_pushnumber(L, talent->row + 1);
    lua_pushnumber(L, talent->column + 1);
    lua_pushnumber(L, rank);
    lua_pushnumber(L, talent->maxRank);
    if (talent->flags & 1u) lua_pushboolean(L, 1); else lua_pushnil(L);
    // A pet talent has no required spell to know (0x5C7800 asks that only of
    // the player's), so both answers are yes; the arrows are
    // GetTalentPrereqs' business.
    lua_pushboolean(L, 1);
    lua_pushnumber(L, previewRank);
    lua_pushboolean(L, 1);
    return 10;
}

static int petGetTalentPrereqs(lua_State* L, game::GameHandler* gh, int tabIndex, int talentIndex) {
    settlePetPreview(gh);
    const auto* talent = petTalentAt(gh, tabIndex, talentIndex);
    if (!talent) return 0;
    int pushed = 0;
    for (int p = 0; p < 3; ++p) {
        const uint32_t prereqId = talent->prereqTalent[p];
        if (prereqId == 0) continue;
        const auto* prereq = gh->getTalentEntry(prereqId);
        if (!prereq) continue;
        const int needed = static_cast<int>(talent->prereqRank[p]) + 1;
        const int have = petTalentRank(gh, prereqId);
        lua_pushnumber(L, prereq->row + 1);
        lua_pushnumber(L, prereq->column + 1);
        if (have >= needed) lua_pushboolean(L, 1); else lua_pushnil(L);
        if (have + petPreviewOf(prereqId) >= needed) lua_pushboolean(L, 1); else lua_pushnil(L);
        pushed += 4;
    }
    return pushed;
}

static void petAddPreview(lua_State* L, game::GameHandler* gh, int tabIndex, int talentIndex, int delta) {
    settlePetPreview(gh);
    const auto* talent = petTalentAt(gh, tabIndex, talentIndex);
    if (!talent || !petTalentShown(gh, talent) || petPreviewSent()) return;
    const int have = petTalentRank(gh, talent->talentId);
    int& staged = petPreviewPoints()[talent->talentId];
    const int before = staged;
    int upper = static_cast<int>(talent->maxRank) - have;
    if (delta > 0) {
        const int pool = static_cast<int>(gh->getPetUnspentTalents()) - petPreviewTotal();
        upper = std::min(upper, staged + std::max(0, pool));
    }
    staged = std::clamp(staged + delta, 0, std::max(0, upper));
    const int change = staged - before;
    if (staged == 0) petPreviewPoints().erase(talent->talentId);
    if (change != 0) {
        // (talentIndex, tabIndex, groupIndex, points) - 0x5C9590.
        gh->fireAddonEvent("PREVIEW_PET_TALENT_POINTS_CHANGED",
                           {std::to_string(talentIndex), std::to_string(tabIndex), "1",
                            std::to_string(change)});
    }
    (void)L;
}

static void petResetPreview(game::GameHandler* gh) {
    settlePetPreview(gh);
    const int total = petPreviewTotal();
    petPreviewPoints().clear();
    petPreviewSent() = false;
    if (gh && total != 0) {
        gh->fireAddonEvent("PREVIEW_PET_TALENT_POINTS_CHANGED", {"0", "0", "1", std::to_string(-total)});
    }
}

static void petLearnPreview(game::GameHandler* gh) {
    settlePetPreview(gh);
    if (!gh || petPreviewPoints().empty() || petPreviewSent()) return;
    // In unlock order - tier, then column, never ahead of a staged
    // prerequisite - each talent once at its final rank, counted from zero,
    // as AzerothCore's LearnPetTalent takes it.
    std::vector<const game::TalentEntry*> staged;
    for (const auto& [id, n] : petPreviewPoints()) {
        if (n <= 0) continue;
        if (const auto* talent = gh->getTalentEntry(id)) staged.push_back(talent);
    }
    std::sort(staged.begin(), staged.end(), [](const auto* a, const auto* b) {
        if (a->row != b->row) return a->row < b->row;
        if (a->column != b->column) return a->column < b->column;
        return a->talentId < b->talentId;
    });
    std::vector<std::pair<uint32_t, uint32_t>> request;
    for (const auto* talent : staged) {
        const int finalRank = petTalentRank(gh, talent->talentId) + petPreviewOf(talent->talentId);
        if (finalRank > 0) request.emplace_back(talent->talentId, static_cast<uint32_t>(finalRank - 1));
    }
    if (request.empty()) return;
    gh->learnPreviewPetTalents(request);
    petPreviewSent() = true;
    petPreviewGeneration() = gh->getPetTalentGeneration();
}

static int lua_GetNumTalentTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (wantsPetTalents(L, 2)) {
        // No inspected pet's tree (0x5C5CC0).
        lua_pushnumber(L, lua_toboolean(L, 1) ? 0 : static_cast<double>(petTalentTabs(gh).size()));
        return 1;
    }
    if (!gh) { return luaReturnZero(L); }
    // Count tabs matching the class in question
    uint8_t classId = talentClassId(gh, lua_toboolean(L, 1) != 0);
    uint32_t classMask = (classId > 0) ? (1u << (classId - 1)) : 0;
    int count = 0;
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) count++;
    }
    lua_pushnumber(L, count);
    return 1;
}

/// Points staged in the talent preview but not yet learned, defined below with
/// the functions that change it.
static std::unordered_map<uint32_t, int>& previewPoints();
/// Retires staged points the server has since learned; defined with them.
static void settlePreview(game::GameHandler* gh);

// GetTalentTabInfo(tabIndex) → name, iconTexture, pointsSpent, background
static int lua_GetTalentTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIndex = static_cast<int>(luaL_checknumber(L, 1)); // 1-indexed
    settlePreview(gh);
    if (!gh || tabIndex < 1) {
        return luaReturnNil(L);
    }
    if (wantsPetTalents(L, 3)) {
        if (lua_toboolean(L, 2)) return luaReturnNil(L);
        return petGetTalentTabInfo(L, gh, tabIndex);
    }
    const bool inspect = lua_toboolean(L, 2) != 0;
    // Argument 4 is the talent group (TalentFrame.talentGroup): the inactive
    // spec's tab totals its own points, not the active spec's.
    const TalentSource src =
        talentSource(gh, inspect, static_cast<int>(luaL_optnumber(L, 4, 0)));
    const auto classTabs = classTalentTabs(gh, src.classId);
    if (tabIndex > static_cast<int>(classTabs.size())) {
        return luaReturnNil(L);
    }
    const auto* tab = classTabs[tabIndex - 1];
    // Count points spent in this tab
    int pointsSpent = 0;
    for (const auto& [talentId, rank] : *src.ranks) {
        const auto* entry = gh->getTalentEntry(talentId);
        if (entry && entry->tabId == tab->tabId) pointsSpent += rank;
    }
    // Points staged in the preview but not yet learned, for this tab. The
    // talent frame adds this to the spent count without checking it -
    //     local displayPointsSpent = pointsSpent + previewPointsSpent;
    // - in a loop over every tab, so leaving it out took the whole frame down
    // as it opened rather than merely showing the wrong total.
    // Staged points are the viewer part-way through spending their own; they
    // have no meaning on someone else's tree.
    int previewSpent = 0;
    if (src.previewable) {
        for (const auto& [talentId, staged] : previewPoints()) {
            const auto* entry = gh->getTalentEntry(talentId);
            if (entry && entry->tabId == tab->tabId) previewSpent += staged;
        }
    }

    // The tree's own icon, from TalentTab.dbc's SpellIconID. The talent frame
    // puts the icon of the spec's primary tree on the spec tab, and a nil here
    // left that tab with nothing to draw.
    const std::string icon = tab->spellIconId ? gh->getIconPath(tab->spellIconId)
                                              : std::string{};

    lua_pushstring(L, tab->name.c_str());              // 1: name
    if (icon.empty()) lua_pushnil(L);                   // 2: iconTexture
    else lua_pushstring(L, icon.c_str());
    lua_pushnumber(L, pointsSpent);                     // 3: pointsSpent
    lua_pushstring(L, tab->backgroundFile.c_str());     // 4: background
    lua_pushnumber(L, previewSpent);                    // 5: previewPointsSpent
    return 5;
}

// GetNumTalents(tabIndex) → count
static int lua_GetNumTalents(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIndex = static_cast<int>(luaL_checknumber(L, 1));
    // The counts belong to the same family as the readers below and were
    // missed with them: a pet tab whose talents all answer nil still asked how
    // many to lay out, and got the player's tree's count.
    if (wantsPetTalents(L, 3)) {
        lua_pushnumber(L, lua_toboolean(L, 2) ? 0 : static_cast<double>(petTabTalents(gh, tabIndex).size()));
        return 1;
    }
    if (!gh || tabIndex < 1) { return luaReturnZero(L); }
    uint8_t classId = talentClassId(gh, lua_toboolean(L, 2) != 0);
    const auto classTabs = classTalentTabs(gh, classId);
    if (tabIndex > static_cast<int>(classTabs.size())) {
        return luaReturnZero(L);
    }
    uint32_t targetTabId = classTabs[tabIndex - 1]->tabId;
    int count = 0;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (entry.tabId == targetTabId) count++;
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetTalentInfo(tabIndex, talentIndex) → name, iconTexture, tier, column, rank, maxRank, isExceptional, available
/// Which quest the confirmation is about. Set when the log asks to abandon one
/// and read back by the popup that confirms it, because the two are separate
/// calls with the player's answer in between.
static uint32_t& pendingAbandonQuest() {
    static uint32_t questId = 0;
    return questId;
}

/// The talent at a tab and index, by the ordering every talent function has to
/// agree on: the player's own tabs in their order index, then the tab's talents
/// by row and column.
///
/// Shared rather than repeated, because two copies that drift disagree about
/// which talent is fourth - and the prerequisite lines are drawn between
/// positions, so a disagreement points an arrow at the wrong button rather than
/// failing outright.
// Not static: the tooltip setters in lua_engine.cpp ask the same question, and
// two copies of this would have to keep the same tab ordering and the same
// row/column sort forever or the tooltip would describe a different talent from
// the one under the cursor. Declared in lua_api_helpers.hpp.

const game::TalentEntry* talentAt(game::GameHandler* gh,
                                  int tabIndex, int talentIndex,
                                  uint8_t classIdOverride) {
    if (!gh || tabIndex < 1 || talentIndex < 1) return nullptr;
    const uint8_t classId = classIdOverride ? classIdOverride : gh->getPlayerClass();
    const auto classTabs = classTalentTabs(gh, classId);
    if (tabIndex > static_cast<int>(classTabs.size())) return nullptr;

    const uint32_t targetTabId = classTabs[tabIndex - 1]->tabId;
    std::vector<const game::GameHandler::TalentEntry*> tabTalents;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (entry.tabId == targetTabId) tabTalents.push_back(&entry);
    }
    std::sort(tabTalents.begin(), tabTalents.end(),
        [](const auto* a, const auto* b) {
            return (a->row != b->row) ? a->row < b->row : a->column < b->column;
        });
    if (talentIndex > static_cast<int>(tabTalents.size())) return nullptr;
    return tabTalents[talentIndex - 1];
}


/// Whether every prerequisite of a talent is satisfied, optionally counting
/// points staged in the preview but not yet learned.
static bool talentPrereqsMet(game::GameHandler* gh,
                             const game::GameHandler::TalentEntry* talent,
                             bool withPreview, const TalentSource& src) {
    if (!gh || !talent) return false;
    for (int p = 0; p < 3; ++p) {
        const uint32_t prereqId = talent->prereqTalent[p];
        if (prereqId == 0) continue;
        // Counted from zero in the DBC, so the rank asked for is one more.
        const int needed = static_cast<int>(talent->prereqRank[p]) + 1;
        // The ranks of the tree being drawn - the inspected player's, or the
        // group asked for - not the viewer's active spec.
        int have = src.rank(prereqId);
        if (withPreview && src.previewable) {
            const auto staged = previewPoints().find(prereqId);
            if (staged != previewPoints().end()) have += staged->second;
        }
        if (have < needed) return false;
    }
    return true;
}

static int lua_GetTalentInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tabIndex = static_cast<int>(luaL_checknumber(L, 1));
    const int talentIndex = static_cast<int>(luaL_checknumber(L, 2));
    settlePreview(gh);
    const bool inspect = lua_toboolean(L, 3) != 0;
    if (wantsPetTalents(L, 4)) {
        if (inspect || !gh) { for (int i = 0; i < 10; i++) lua_pushnil(L); return 10; }
        return petGetTalentInfo(L, gh, tabIndex, talentIndex);
    }
    // Argument 5 is the talent group, as GetTalentTabInfo's 4 is.
    const TalentSource src =
        talentSource(gh, inspect, static_cast<int>(luaL_optnumber(L, 5, 0)));
    const auto* talent = talentAt(gh, tabIndex, talentIndex, src.classId);
    // Ten values, not eight. The frame reads previewRank into the rank it
    // displays and then compares it against maxRank - as nil that is an error
    // rather than a blank, and it happens the moment points are staged, which
    // is how talents are spent at all.
    if (!talent) {
        for (int i = 0; i < 10; i++) lua_pushnil(L);
        return 10;
    }
    const int rank = src.rank(talent->talentId);
    const auto staged = previewPoints().find(talent->talentId);
    const int previewRank = !src.previewable
        ? rank
        : rank + (staged == previewPoints().end() ? 0 : staged->second);

    std::string name = gh->getSpellName(talent->rankSpells[0]);
    if (name.empty()) name = "Talent " + std::to_string(talent->talentId);
    // A nil texture is an empty slot to the interface, so every talent button
    // was drawn blank.
    const std::string icon = gh->getSpellIconPath(talent->rankSpells[0]);

    lua_pushstring(L, name.c_str());          // 1: name
    if (icon.empty()) lua_pushnil(L);          // 2: iconTexture
    else lua_pushstring(L, icon.c_str());
    lua_pushnumber(L, talent->row + 1);        // 3: tier (1-indexed)
    lua_pushnumber(L, talent->column + 1);     // 4: column (1-indexed)
    lua_pushnumber(L, rank);                   // 5: rank
    lua_pushnumber(L, talent->maxRank);        // 6: maxRank
    // 7: isExceptional - Talent.dbc Flags bit 1 (0x5C7800), true or nil.
    if (talent->flags & 1u) lua_pushboolean(L, 1); else lua_pushnil(L);
    // Was hardcoded true, which drew every talent as learnable however deep in
    // a chain it sat.
    lua_pushboolean(L, talentPrereqsMet(gh, talent, /*withPreview=*/false, src) ? 1 : 0);
    lua_pushnumber(L, previewRank);            // 9: previewRank
    lua_pushboolean(L, talentPrereqsMet(gh, talent, /*withPreview=*/true, src) ? 1 : 0);
    return 10;
}

// ---- Trade skill window ------------------------------------------------
//
// Every function reads the model SpellHandler keeps (stock_trade_skill.hpp),
// which follows Wow.exe's TradeSkillFrame.cpp: one list of rows - headers and
// recipes, shown ones first - indexed 1-based by every call, and a selection
// that is the recipe rather than its row.

static game::SpellHandler* tradeSkillHandler(lua_State* L) {
    auto* gh = getGameHandler(L);
    return gh ? gh->getSpellHandler() : nullptr;
}

/// The index argument the stock functions demand ("Usage: ...(index)").
static int tradeSkillIndexArg(lua_State* L, int arg, const char* usage) {
    if (!lua_isnumber(L, arg)) return luaL_error(L, "%s", usage);
    return static_cast<int>(lua_tonumber(L, arg));
}

/// The recipe at a row, and its Spell.dbc entry, or nulls for a header or an
/// index past the end.
struct TradeSkillPick {
    const game::stock_trade_skill::Recipe* recipe = nullptr;
    const game::GameHandler::SpellNameEntry* spell = nullptr;
    const game::SpellHandler::TradeSpellColumns* columns = nullptr;
};
static TradeSkillPick tradeSkillPick(game::GameHandler* gh, int index) {
    TradeSkillPick p;
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    if (!sh) return p;
    p.recipe = sh->tradeSkill().recipeAt(index);
    if (!p.recipe) return p;
    gh->loadSpellNameCache();
    auto it = gh->spellNameCacheRef().find(static_cast<uint32_t>(p.recipe->spellId));
    if (it != gh->spellNameCacheRef().end()) p.spell = &it->second;
    p.columns = sh->tradeSpellColumns(static_cast<uint32_t>(p.recipe->spellId));
    return p;
}

/// The item a recipe makes (effect 0's item), 0 for an enchant.
static uint32_t tradeSkillMadeItem(const TradeSkillPick& p) {
    if (!p.recipe || !p.recipe->createsItem) return 0;
    if (p.columns) return p.columns->itemType[0];
    return p.spell ? p.spell->createdItemId : 0;
}

uint32_t spellAtTradeSkillRow(game::GameHandler* gh, int index) {
    auto p = tradeSkillPick(gh, index);
    return p.recipe ? static_cast<uint32_t>(p.recipe->spellId) : 0;
}

uint32_t itemMadeAtTradeSkillRow(game::GameHandler* gh, int index) {
    return tradeSkillMadeItem(tradeSkillPick(gh, index));
}

uint32_t reagentAtTradeSkillRow(game::GameHandler* gh, int index, int reagent) {
    auto p = tradeSkillPick(gh, index);
    if (!p.spell) return 0;
    int seen = 0;
    for (const auto& r : p.spell->reagents) {
        if (r.itemId == 0) continue;
        if (++seen == reagent) return r.itemId;
    }
    return 0;
}

/// The trainer panel's own selection and filters. The client has no opinion
/// about either - they are what the player last clicked - so they live here
/// rather than being invented on every read.
static int& trainerSelection() {
    static int selected = 0;
    return selected;
}
/// The three type filters, seeded rather than empty.
///
/// Seeded with the stock defaults, which blizzard_trainerui.lua states as
/// TRAINER_FILTER_AVAILABLE = 1, TRAINER_FILTER_UNAVAILABLE = 1,
/// TRAINER_FILTER_USED = 0: what can be learned now and what cannot yet both
/// show (the red rows are how a player sees what the next levels bring), and
/// only what is already known is hidden. Unavailable used to start off.
///
/// Seeded rather than left unset because three readers consult this map and all
/// three have to agree what an absent entry means: the list, the dropdown's
/// ticks, and the setter deciding whether a call is a change. With the three
/// names always present there is no absent entry to disagree about, and an
/// unknown name still reads as showing.
///
/// The player's own changes outlive it - this is where the session's filter
/// lives, so a tick turned on stays on until the client restarts.
static std::unordered_map<std::string, bool>& trainerFilters() {
    static std::unordered_map<std::string, bool> filters{
        {"available", true},
        {"unavailable", true},
        {"used", false},
    };
    return filters;
}

/// What the trainer panel's search box holds, folded to lower case.
///
/// Beside the type filter rather than inside it because it is the same kind of
/// thing - a rule the panel applies to the list the trainer sent, kept here
/// because every binding that takes a service index has to agree about which
/// service index 3 is. 3.3.5's trainer has no search box; this client's has
/// one, and shownTrainerServices is where the two filters meet.
static std::string& trainerSearch() {
    static std::string search;
    return search;
}

/// Case-insensitively, because a player types "linen" and the spell is called
/// "Brown Linen Robe".
static std::string foldedForSearch(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

/// The word the trainer panel colours a service by, and the name of the filter
/// that hides it.
///
/// "used" wins the moment the spell is known rather than only when the packet
/// said so: the list is a snapshot from when the trainer was opened, and
/// learning something fires TRAINER_UPDATE without a fresh one arriving.
static const char* trainerServiceCategory(game::GameHandler* gh,
                                          const game::TrainerSpell& sp) {
    const auto* spells = gh ? gh->getSpellHandler() : nullptr;
    if (spells && spells->hasKnownSpell(sp.spellId))
        return "used";
    // The same snapshot problem the other way: a next rank arrives
    // unavailable because its ReqAbility (the chain nodes) is not known yet,
    // and learning the rank before it does not resend the list. Re-checked
    // here with the server's own tests (Trainer::GetDefaultSpellState: each
    // ReqAbility known, level, skill rank) - but only for a row that names a
    // ReqAbility, since a row unavailable for its class or race carries
    // nothing this side could re-check.
    if (sp.state == 1 && spells) {
        const uint32_t req[3] = {sp.chainNode1, sp.chainNode2, sp.chainNode3};
        bool namesAbility = false, abilitiesKnown = true;
        for (uint32_t r : req) {
            if (r == 0) continue;
            namesAbility = true;
            if (!spells->hasKnownSpell(r)) abilitiesKnown = false;
        }
        bool skillOk = sp.reqSkill == 0;
        if (!skillOk) {
            const auto& skills = gh->getPlayerSkills();
            auto it = skills.find(sp.reqSkill);
            skillOk = it != skills.end() && it->second.value >= sp.reqSkillValue;
        }
        if (namesAbility && abilitiesKnown && skillOk && gh->getPlayerLevel() >= sp.reqLevel)
            return "available";
    }
    return sp.state == 0 ? "available" : sp.state == 2 ? "used" : "unavailable";
}

/// Whether the filter dropdown is currently letting this category through.
/// The three the panel names are always in the map; anything else shows.
static bool trainerCategoryShowing(const char* category) {
    const auto& filters = trainerFilters();
    auto it = filters.find(category);
    return it == filters.end() || it->second;
}

/// The trainer's list as the panel sees it - the services the filters leave.
///
/// The filter was stored and read back and consulted by nothing, so ticking a
/// box in the dropdown changed the tick and not the list. It has to be applied
/// here rather than at each reader, because every one of the nine bindings that
/// takes a service index has to agree about which service index 3 is: filtering
/// in one of them and not the rest would train a different spell than the row
/// that was clicked.
static std::vector<const game::TrainerSpell*> shownTrainerServices(game::GameHandler* gh) {
    std::vector<const game::TrainerSpell*> shown;
    if (!gh) return shown;
    const std::string& search = trainerSearch();
    for (const auto& sp : gh->getTrainerSpells().spells) {
        if (!trainerCategoryShowing(trainerServiceCategory(gh, sp))) continue;
        // The name only when there is something to match, so the common case
        // costs no lookups at all.
        if (!search.empty() &&
            foldedForSearch(gh->getSpellName(sp.spellId)).find(search) ==
                std::string::npos) {
            continue;
        }
        shown.push_back(&sp);
    }
    return shown;
}

/// The nth shown service, counting from one, or null.
static const game::TrainerSpell* shownTrainerService(game::GameHandler* gh, int index) {
    const auto shown = shownTrainerServices(gh);
    if (index < 1 || index > static_cast<int>(shown.size())) return nullptr;
    return shown[index - 1];
}

/// (tabIndex, talentIndex) → talent id, or 0.
///
/// The same ordering GetTalentInfo reports by - tabs in orderIndex, talents by
/// row then column - because the interface identifies a talent by its position
/// in that listing and nothing else.
static uint32_t resolveTalentId(game::GameHandler* gh, int tabIndex, int talentIndex) {
    if (!gh || tabIndex < 1 || talentIndex < 1) return 0;
    const uint8_t classId = gh->getPlayerClass();
    const auto classTabs = classTalentTabs(gh, classId);
    if (tabIndex > static_cast<int>(classTabs.size())) return 0;

    std::vector<const game::GameHandler::TalentEntry*> tabTalents;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (entry.tabId == classTabs[tabIndex - 1]->tabId) tabTalents.push_back(&entry);
    }
    std::sort(tabTalents.begin(), tabTalents.end(),
              [](const auto* a, const auto* b) {
                  return (a->row != b->row) ? a->row < b->row : a->column < b->column;
              });
    if (talentIndex > static_cast<int>(tabTalents.size())) return 0;
    return tabTalents[talentIndex - 1]->talentId;
}

/// The points staged but not yet sent, per talent.
///
/// WotLK's talent frame does not spend a point when one is clicked: it adds to
/// a preview, redraws from the preview, and sends the lot when Learn is
/// pressed. Without somewhere to hold that, clicking a talent did nothing at
/// all. Keyed by talent id so it survives a tab change, and retired point by
/// point as the server confirms each rank (see settlePreview).
static std::unordered_map<uint32_t, int>& previewPoints() {
    static std::unordered_map<uint32_t, int> points;
    return points;
}

/// Staged points that Learn has already sent, per talent, until the server
/// answers for them.
///
/// Learn used to drop the whole preview the moment it sent the requests. The
/// answers take a round trip and arrive one rank at a time, and every one of
/// them redraws the frame - so in between, the talents fell back to their old
/// ranks, the counter jumped back up to the full unspent total, and the
/// Learn/Reset bar greyed out, then it all came back rank by rank. That is the
/// window "disappearing for a second" after Learn. Holding each sent point as
/// staged until its rank is actually taught keeps the talents and tree totals
/// steady: learned goes up by exactly what staged comes down by.
struct CommittedPreview {
    struct Pending {
        int targetRank = 0;  // the rank Learn asked for, counted from one
        int remaining = 0;   // staged points still waiting for their rank
    };
    std::unordered_map<uint32_t, Pending> talents;
    std::chrono::steady_clock::time_point sentAt{};
    // The rank (SMSG_LEARNED_SPELL) lands a moment before the new unspent
    // total (SMSG_TALENTS_INFO). Points let go on the rank are still counted
    // as spent until the total catches up - see unchargedPreviewPoints - or the
    // "Talent Points" counter ticked up by one and back on every rank.
    int unspentAtSend = 0;
    int retiredTotal = 0;
};
static CommittedPreview& committedPreview() {
    static CommittedPreview committed;
    return committed;
}

/// Long past any round trip: whatever has not arrived was refused.
static bool committedPreviewExpired() {
    return std::chrono::steady_clock::now() - committedPreview().sentAt >
           std::chrono::seconds(5);
}

static void settlePreview(game::GameHandler* gh) {
    auto& committed = committedPreview();
    if (!gh || committed.talents.empty()) return;
    // A refused rank goes back to being an ordinary unstaged talent rather
    // than a point the frame holds for ever.
    const bool expired = committedPreviewExpired();
    auto& staged = previewPoints();
    for (auto it = committed.talents.begin(); it != committed.talents.end();) {
        auto& pending = it->second;
        const int have = gh->getTalentRank(it->first);
        const int stillWaiting =
            expired ? 0 : std::min(pending.remaining, std::max(0, pending.targetRank - have));
        const int retire = pending.remaining - stillWaiting;
        if (retire > 0) {
            // Only ranks actually taught are waiting to be charged.
            if (!expired) committed.retiredTotal += retire;
            pending.remaining = stillWaiting;
            auto s = staged.find(it->first);
            if (s != staged.end()) {
                s->second -= retire;
                if (s->second <= 0) staged.erase(s);
            }
        }
        if (pending.remaining == 0) it = committed.talents.erase(it);
        else ++it;
    }
}

/// Points already taught by the server but not yet taken off its unspent total.
static int unchargedPreviewPoints(game::GameHandler* gh) {
    auto& committed = committedPreview();
    if (!gh || committed.retiredTotal <= 0 || committedPreviewExpired()) return 0;
    const int unspentNow = gh->getUnspentTalentPoints(gh->getActiveTalentSpec());
    const int charged = std::max(0, committed.unspentAtSend - unspentNow);
    return std::max(0, committed.retiredTotal - charged);
}

// AddPreviewTalentPoints(tabIndex, talentIndex, delta, pet, group)
static int lua_AddPreviewTalentPoints(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tab   = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int delta = static_cast<int>(luaL_optnumber(L, 3, 1));
    // The pet tree again: staging a preview point there resolved against the
    // player's trees, and LearnPreviewTalents would then have spent it.
    if (wantsPetTalents(L, 4)) {
        petAddPreview(L, gh, tab, index, delta);
        return 0;
    }
    settlePreview(gh);
    const uint32_t id = resolveTalentId(gh, tab, index);
    if (!gh || id == 0) return 0;

    const uint8_t have = gh->getTalentRank(id);
    uint8_t maxRank = 0;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (talentId == id) { maxRank = entry.maxRank; break; }
    }
    int& staged = previewPoints()[id];
    // Bounded by what is already learned and the talent's own cap: a preview
    // that goes past either is one the server will refuse, and the frame draws
    // straight from these numbers.
    int upper = static_cast<int>(maxRank) - have;

    // And bounded by the points the player actually has. This was missing, so
    // a preview could stage more than were available and go on staging them:
    // GetUnspentTalentPoints subtracts the staged total and floors the answer
    // at zero, which hid the overspend instead of stopping it - the counter sat
    // at 0 while every further click still took.
    //
    // Counted against every staged point rather than this talent's, because the
    // pool is shared across all three trees, and only when adding: taking a
    // staged point back is always allowed however far past the pool it went.
    if (delta > 0) {
        int stagedTotal = 0;
        for (const auto& [tid, n] : previewPoints()) { (void)tid; stagedTotal += n; }
        stagedTotal += unchargedPreviewPoints(gh);
        const int unspent =
            gh->getUnspentTalentPoints(gh->getActiveTalentSpec()) - stagedTotal;
        upper = std::min(upper, staged + std::max(0, unspent));
    }
    // Not below what Learn has already sent for it: those ranks are on their
    // way, and taking them back here would not stop the server teaching them.
    int lower = 0;
    if (const auto c = committedPreview().talents.find(id);
        c != committedPreview().talents.end()) {
        lower = c->second.remaining;
    }
    const int wanted = std::clamp(staged + delta, lower, std::max(lower, upper));
    staged = wanted;
    if (staged == 0) previewPoints().erase(id);
    // What makes a staged point appear: the talent frame refreshes on this
    // event, and reads the rank back through GetTalentInfo's preview value.
    gh->fireAddonEvent("PREVIEW_TALENT_POINTS_CHANGED", {});
    return 0;
}

// GetTalentPrereqs(tab, index) → tier, column, isLearnable, isPreviewLearnable
//                                 repeated once per prerequisite
//
// The frame draws a line from each prerequisite to the talent that needs it, so
// the positions here have to be the same ones GetTalentInfo reports - 1-indexed
// row and column, from the same ordering.
static int lua_GetTalentPrereqs(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tabIndex = static_cast<int>(luaL_checknumber(L, 1));
    const int talentIndex = static_cast<int>(luaL_checknumber(L, 2));
    if (wantsPetTalents(L, 4)) {
        if (lua_toboolean(L, 3) || !gh) return 0;
        return petGetTalentPrereqs(L, gh, tabIndex, talentIndex);
    }
    settlePreview(gh);
    // (tab, index, inspect, pet, talentGroup) - talentframebase.lua:195. The
    // inspected player's class and ranks, or the group asked for: the
    // viewer's class put the arrows of the wrong tree on an inspect tab.
    const TalentSource src = talentSource(gh, lua_toboolean(L, 3) != 0,
                                          static_cast<int>(luaL_optnumber(L, 5, 0)));
    const auto* talent = talentAt(gh, tabIndex, talentIndex, src.classId);
    if (!talent) return 0;

    int pushed = 0;
    for (int p = 0; p < 3; ++p) {
        const uint32_t prereqId = talent->prereqTalent[p];
        if (prereqId == 0) continue;
        const auto* prereq = gh->getTalentEntry(prereqId);
        if (!prereq) continue;

        // The DBC counts ranks from zero, so the rank a prerequisite asks for
        // is one more than the number stored beside it.
        const int needed = static_cast<int>(talent->prereqRank[p]) + 1;
        const int have = src.rank(prereqId);
        const auto staged = previewPoints().find(prereqId);
        const int previewHave = have + (!src.previewable || staged == previewPoints().end()
                                            ? 0 : staged->second);

        lua_pushnumber(L, prereq->row + 1);
        lua_pushnumber(L, prereq->column + 1);
        lua_pushboolean(L, have >= needed ? 1 : 0);
        lua_pushboolean(L, previewHave >= needed ? 1 : 0);
        pushed += 4;
    }
    return pushed;
}

// GetGroupPreviewTalentPointsSpent(pet, group) → points staged
static int lua_GetGroupPreviewTalentPointsSpent(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (wantsPetTalents(L, 1)) {
        settlePetPreview(gh);
        lua_pushnumber(L, petPreviewTotal());
        return 1;
    }
    settlePreview(gh);
    int total = 0;
    for (const auto& [id, n] : previewPoints()) { (void)id; total += n; }
    // Plus points a Learn has had taught but the server has not yet taken off
    // the unspent total: the frame shows unspent minus this, and that line
    // should not step up and back down between the two packets.
    total += unchargedPreviewPoints(gh);
    lua_pushnumber(L, total);
    return 1;
}

// ResetGroupPreviewTalentPoints(pet, group)
static int lua_ResetGroupPreviewTalentPoints(lua_State* L) {
    if (wantsPetTalents(L, 1)) {
        petResetPreview(getGameHandler(L));
        return 0;
    }
    previewPoints().clear();
    // Ranks already sent still arrive and show as learned when they do; there
    // is nothing left to hold for them.
    committedPreview().talents.clear();
    committedPreview().retiredTotal = 0;
    // The frame redraws on this and on nothing else, so clearing the staged
    // points without it leaves them on screen after they are gone.
    if (auto* gh = getGameHandler(L)) {
        gh->fireAddonEvent("PREVIEW_TALENT_POINTS_CHANGED", {});
    }
    return 0;
}

// What still has to be sent for one staged talent: the ranks above what is
// learned and above what an earlier Learn already has on its way.
struct PreviewSend {
    const game::GameHandler::TalentEntry* talent;
    int fromRank;   // first rank to request, counted from one
    int toRank;     // last rank to request
    int staged;     // the talent's whole staged count, all of it now sent
};

// `sends` in the order the tree unlocks, which is the order the server checks.
//
// AzerothCore handles each request on its own against the talents already
// learned: a talent in tier N needs N*5 points in its tree, and one with a
// prerequisite needs that prerequisite's rank. The staged points came out
// of a hash map, so a deep talent was routinely asked for before the tier
// points beneath it - and refused, silently, with the server answering the
// unchanged talents. A Learn of eleven points would teach the shallow ones
// and quietly drop the rest. Tier by tier, then column, and never before a
// staged prerequisite (a prerequisite can sit in the same tier).
static std::vector<PreviewSend> inUnlockOrder(std::vector<PreviewSend> sends) {
    std::sort(sends.begin(), sends.end(), [](const PreviewSend& a, const PreviewSend& b) {
        if (a.talent->row != b.talent->row) return a.talent->row < b.talent->row;
        if (a.talent->column != b.talent->column) return a.talent->column < b.talent->column;
        return a.talent->talentId < b.talent->talentId;
    });
    std::vector<PreviewSend> ordered;
    ordered.reserve(sends.size());
    std::vector<bool> done(sends.size(), false);
    const auto waitsOnPrereq = [&](const PreviewSend& s) {
        for (uint32_t prereqId : s.talent->prereqTalent) {
            if (prereqId == 0) continue;
            for (size_t j = 0; j < sends.size(); ++j) {
                if (!done[j] && sends[j].talent->talentId == prereqId) return true;
            }
        }
        return false;
    };
    while (ordered.size() < sends.size()) {
        size_t pick = sends.size();
        for (size_t i = 0; i < sends.size(); ++i) {
            if (!done[i] && !waitsOnPrereq(sends[i])) { pick = i; break; }
        }
        // A prerequisite loop cannot come out of the DBC; if one did, sending
        // in tier order is still the best guess.
        if (pick == sends.size()) {
            for (size_t i = 0; i < sends.size(); ++i) {
                if (!done[i]) { pick = i; break; }
            }
        }
        done[pick] = true;
        ordered.push_back(sends[pick]);
    }
    return ordered;
}

// LearnPreviewTalents(pet) - commit the staged points
static int lua_LearnPreviewTalents(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    if (wantsPetTalents(L, 1)) {
        petLearnPreview(gh);
        return 0;
    }
    settlePreview(gh);
    auto& committed = committedPreview();
    // A fresh baseline unless an earlier Learn is still being answered: its
    // count of taught-but-uncharged points is measured from its own baseline.
    if (committed.talents.empty() && unchargedPreviewPoints(gh) == 0) {
        committed.unspentAtSend = gh->getUnspentTalentPoints(gh->getActiveTalentSpec());
        committed.retiredTotal = 0;
    }

    std::vector<PreviewSend> sends;
    for (const auto& [id, n] : previewPoints()) {
        const auto* talent = gh->getTalentEntry(id);
        if (!talent || n <= 0) continue;
        const int have = gh->getTalentRank(id);
        // An earlier Learn's ranks still on their way are part of the staged
        // count (settlePreview has already let go of the ones taught), and
        // are not asked for twice.
        int alreadyAsked = have;
        if (const auto c = committed.talents.find(id); c != committed.talents.end()) {
            alreadyAsked = std::max(have, c->second.targetRank);
        }
        const int fromRank = alreadyAsked + 1;
        const int toRank = have + n;
        if (fromRank <= toRank) sends.push_back({talent, fromRank, toRank, n});
    }

    const std::vector<PreviewSend> ordered = inUnlockOrder(std::move(sends));
    // One CMSG_LEARN_PREVIEW_TALENTS, as the stock Learn button sends: each
    // talent once at its final rank, in unlock order. AzerothCore's
    // HandleLearnPreviewTalents runs Player::LearnTalent on each pair in turn,
    // which charges (rank - current + 1) points, so a final rank is a whole
    // multi-rank step - and answers with one SMSG_TALENTS_INFO rather than one
    // per rank requested.
    std::vector<std::pair<uint32_t, uint32_t>> request;
    request.reserve(ordered.size());
    for (const PreviewSend& s : ordered)
        request.emplace_back(s.talent->talentId, static_cast<uint32_t>(s.toRank));
    gh->learnPreviewTalents(request);
    for (const PreviewSend& s : ordered) {
        // Held as staged until the server teaches them - see CommittedPreview.
        auto& pending = committed.talents[s.talent->talentId];
        pending.targetRank = s.toRank;
        pending.remaining = s.staged;
    }
    if (!ordered.empty()) committed.sentAt = std::chrono::steady_clock::now();
    return 0;
}

// LearnTalent(tabIndex, talentIndex, pet, group) - spend one point directly
static int lua_LearnTalent(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Pet talents are not modelled, and this is the argument that made that
    // dangerous rather than merely blank: clicking in a pet tree resolved the
    // same tab and index against the *player's* trees and spent a point there.
    if (wantsPetTalents(L, 3)) {
        // CMSG_PET_LEARN_TALENT for the next rank, counted from zero (0x5C80E0).
        const auto* talent = petTalentAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)),
                                         static_cast<int>(luaL_optnumber(L, 2, 0)));
        if (gh && talent && petTalentShown(gh, talent)) {
            const int have = petTalentRank(gh, talent->talentId);
            if (have < talent->maxRank) gh->learnPetTalent(talent->talentId, static_cast<uint32_t>(have));
        }
        return 0;
    }
    const uint32_t id = resolveTalentId(gh, static_cast<int>(luaL_optnumber(L, 1, 0)),
                                            static_cast<int>(luaL_optnumber(L, 2, 0)));
    if (gh && id != 0) gh->learnTalent(id, gh->getTalentRank(id) + 1);
    return 0;
}

// GetUnspentTalentPoints(inspect, pet, group) → points
static int lua_GetUnspentTalentPoints(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    if (wantsPetTalents(L, 2)) {
        // The pet's own count (0x5C6310 -> 0x5C5FC0 +0x38); none inspected.
        lua_pushnumber(L, (lua_toboolean(L, 1) || !petTalentFamily(gh)) ? 0 : gh->getPetUnspentTalents());
        return 1;
    }
    // inspect: the target's unspent count, which arrives with their talents
    // (talentframebase.lua:525 asks with TalentFrame.inspect). The viewer's
    // own was answered before.
    if (lua_toboolean(L, 1)) {
        const auto* r = gh->getInspectResult();
        lua_pushnumber(L, r ? static_cast<double>(r->unspentTalents) : 0.0);
        return 1;
    }
    const int group = static_cast<int>(luaL_optnumber(L, 3, 0));
    const uint8_t spec = (group >= 1 && group <= 2) ? static_cast<uint8_t>(group - 1)
                                                   : gh->getActiveTalentSpec();
    // The raw unspent total, before any staged preview points.
    //
    // The frame subtracts the preview itself: TalentFrame_UpdateTalentPoints is
    // `GetUnspentTalentPoints() - GetGroupPreviewTalentPointsSpent()`. This
    // binding used to subtract the staged points as well, so every point
    // clicked came off the counter twice - stage one and the "Talent Points"
    // line dropped by two. The overspend that subtraction was meant to stop is
    // already stopped in AddPreviewTalentPoints, which bounds each stage by the
    // handler's own raw unspent minus what is already staged, so nothing here
    // needs to guard it. blizzard_talentui reads this raw too, to decide
    // whether there are points left to commit - which must count the staged
    // ones, or the Learn controls vanish the moment the last point is staged.
    const int points = gh->getUnspentTalentPoints(spec);
    lua_pushnumber(L, points);
    return 1;
}

// GetNumTalentGroups(inspect, pet) → how many specs the player has
static int lua_GetNumTalentGroups(lua_State* L) {
    auto* gh = getGameHandler(L);
    // The pet's groups. Pet talents are not modelled here - every other talent
    // binding answers nothing for the pet tree - and the count has to agree
    // with them: answering the player's 1 for the pet made the talent frame
    // build a "petspec1" tab for every class, so two spec tabs stood on the
    // frame's right edge (read as dual spec), each drawn with a portrait
    // because one group means "show the unit's face". Zero is what the client
    // answers with no talented pet, and it hides that tab.
    if (wantsPetTalents(L, 2)) {
        // One group for a pet whose family has a tree, none otherwise
        // (0x5C5DF0) - which is what shows or hides the pet's spec tab.
        lua_pushnumber(L, (!lua_toboolean(L, 1) && !petTalentTabs(gh).empty()) ? 1 : 0);
        return 1;
    }
    // Two only once the second is actually bought: the frame draws a spec tab
    // per group, and reporting two unconditionally offers one that is not there.
    int groups = 1;
    if (gh && lua_toboolean(L, 1)) {
        // The inspected player's spec count comes with their talents; the
        // viewer's own says nothing about how many specs the target bought.
        if (const auto* r = gh->getInspectResult()) {
            if (r->talentGroups > 0) groups = r->talentGroups;
        }
        lua_pushnumber(L, groups);
        return 1;
    }
    // The server's count from SMSG_TALENTS_INFO, not a guess from what the
    // second slot happens to hold.
    if (gh) groups = std::clamp<int>(gh->getNumTalentGroups(), 1, 2);
    lua_pushnumber(L, groups);
    return 1;
}

// SetActiveTalentGroup(group)
static int lua_SetActiveTalentGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int group = static_cast<int>(luaL_optnumber(L, 1, 1));
    if (gh && group >= 1 && group <= 2) {
        gh->switchTalentSpec(static_cast<uint8_t>(group - 1));
    }
    return 0;
}

// GetTalentLink(tabIndex, talentIndex, inspect, pet, talentGroup) → hyperlink
// (blizzard_talentui.lua:438, inspecttalentframe.lua:6)
static int lua_GetTalentLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (wantsPetTalents(L, 4)) {
        const auto* talent = lua_toboolean(L, 3) ? nullptr
            : petTalentAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)), static_cast<int>(luaL_optnumber(L, 2, 0)));
        if (!gh || !talent || !petTalentShown(gh, talent)) { lua_pushnil(L); return 1; }
        int rank = petTalentRank(gh, talent->talentId);
        if (lua_toboolean(L, 6)) rank += petPreviewOf(talent->talentId);
        std::string name = gh->getSpellName(talent->rankSpells[0]);
        if (name.empty()) name = "Talent";
        const std::string link = "|cff4e96f7|Htalent:" + std::to_string(talent->talentId) + ":" +
                                 std::to_string(rank - 1) + "|h[" + name + "]|h|r";
        lua_pushstring(L, link.c_str());
        return 1;
    }
    // The tree and ranks the frame is drawing: an inspected player's link is
    // their talent at their rank, and the inactive spec's is that spec's.
    const TalentSource src = talentSource(gh, lua_toboolean(L, 3) != 0,
                                          static_cast<int>(luaL_optnumber(L, 5, 0)));
    const auto* talent = talentAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)),
                                  static_cast<int>(luaL_optnumber(L, 2, 0)), src.classId);
    if (!gh || !talent) { lua_pushnil(L); return 1; }
    const uint32_t id = talent->talentId;
    std::string name = gh->getSpellName(talent->rankSpells[0]);
    if (name.empty()) name = "Talent";
    // The link's rank is counted from zero, -1 for none (the stock
    // |Htalent:id:rank| form, which AzerothCore's Hyperlinks.cpp checks the
    // same way). This sent the 1-based count, so a link always read one
    // rank higher than what was learned.
    const std::string link = "|cff4e96f7|Htalent:" + std::to_string(id) + ":" +
                             std::to_string(src.rank(id) - 1) + "|h[" + name +
                             "]|h|r";
    lua_pushstring(L, link.c_str());
    return 1;
}

static int lua_GetActiveTalentGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    // The pet has one group, always the first (0x5C5810).
    if (!lua_toboolean(L, 1) && wantsPetTalents(L, 2)) { lua_pushnumber(L, 1); return 1; }
    if (gh && lua_toboolean(L, 1)) {
        // Which of the target's specs the talents just read belong to. The
        // inspect frame keeps this as its talentGroup and passes it back into
        // every other talent call.
        const auto* r = gh->getInspectResult();
        lua_pushnumber(L, r ? (r->activeTalentGroup + 1) : 1);
        return 1;
    }
    lua_pushnumber(L, gh ? (gh->getActiveTalentSpec() + 1) : 1);
    return 1;
}


namespace {
/// A template because the dialog carries its rewards in a vector and the quest
/// log in a fixed array, and both are padded the same way.
template <typename Container>
int countRewards(const Container* v) {
    if (!v) return 0;
    int n = 0;
    for (const auto& r : *v) if (r.itemId != 0) ++n;
    return n;
}
}  // namespace

// --- The quest log's own reward panel ---
//
// QuestInfo draws rewards for the quest log as well as for the quest giver, and
// asks a different set of functions for each. The giver's were written earlier;
// these read the same fields out of the log entry the player has selected.

/// The quest log entry the panel is showing, or null.
static const game::GameHandler::QuestLogEntry* selectedLogEntry(game::GameHandler* gh) {
    if (!gh) return nullptr;
    // The selected index is a display index - the log is grouped under zone
    // headers and those are rows too - so it is resolved through the row list
    // rather than subscripting getQuestLog(). Every reward binding reads the
    // selected quest through here, so getting it wrong shows one quest's
    // rewards beside another's text.
    return questAtRow(gh, gh->getSelectedQuestLogIndex());
}

// GetQuestLogRewardSpell() → texture, name, isTradeskillSpell, isSpellLearned
//
// Nil for every quest, and questinfo.lua gates the whole reward-spell row on
// `if ( GetQuestLogRewardSpell() )` - so a quest that teaches a recipe or hands
// out a buff showed no sign of it, and the tooltip setter beside it had nothing
// to describe. The spell is in SMSG_QUEST_QUERY_RESPONSE at the field between
// two the reward parser already reads.
//
// The three flags decide only which line is printed under the icon:
// REWARD_TRADESKILL_SPELL for a recipe, REWARD_AURA for something not yet
// known, REWARD_SPELL otherwise.
static int lua_GetQuestLogRewardSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    const auto* q = selectedLogEntry(gh);
    const uint32_t spellId = q ? q->rewardSpellId : 0u;
    if (spellId == 0) return luaReturnNil(L);
    const std::string name = gh->getSpellName(spellId);
    if (name.empty()) return luaReturnNil(L);
    std::string icon = gh->getSpellIconPath(spellId);
    lua_pushstring(L, icon.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                   : icon.c_str());
    lua_pushstring(L, name.c_str());
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0);
    lua_pushboolean(L, gh->getKnownSpells().count(spellId) > 0 ? 1 : 0);
    return 4;
}

/// One reward, described the way every reward button reads it.
template <typename Container>
static int pushRewardAt(lua_State* L, game::GameHandler* gh,
                        const Container& list, int index) {
    if (!gh || index < 1) { return luaReturnNil(L); }
    int seen = 0;
    for (const auto& r : list) {
        if (r.itemId == 0) continue;
        if (++seen != index) continue;
        const auto* info = gh->getItemInfo(r.itemId);
        lua_pushstring(L, info ? info->name.c_str() : "");
        lua_pushstring(L, gh->getItemIconPath(
            info && info->displayInfoId ? info->displayInfoId : r.displayInfoId).c_str());
        lua_pushnumber(L, r.count);
        lua_pushnumber(L, info ? info->quality : 1);
        lua_pushboolean(L, 1);
        return 5;
    }
    return luaReturnNil(L);
}

// GetSuggestedGroupNum() / GetQuestLogGroupNum() → how many players a quest
// suggests bringing
//
// QuestInfo assigns one of these and then tests `groupNum > 0`, so an absent
// answer is an error rather than a quest that suggests nothing - and QuestInfo
// draws for the quest giver and the quest log both.
//
// The giver's packet carries the number. The log's does not, and zero is what
// stops the line being drawn at all.
// GetDailyQuestsCompleted() / GetMaxDailyQuests() → the daily allowance
//
// The quest log tests the first against zero before drawing the line at all, so
// an absent answer is an error rather than a hidden line. Nothing here counts
// dailies, and zero completed is what keeps the line hidden - which is the same
// thing the count would do if it were counting and found none.
static int lua_GetDailyQuestsCompleted(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// Twenty-five is the allowance in this era. Only ever shown beside the count
// above, which stays at zero, so it is a label rather than a limit here.
static int lua_GetMaxDailyQuests(lua_State* L) {
    lua_pushnumber(L, 25);
    return 1;
}

static int lua_GetSuggestedGroupNum(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool open = gh && gh->isQuestDetailsOpen();
    lua_pushnumber(L, open ? gh->getQuestDetails().suggestedPlayers : 0);
    return 1;
}

static int lua_GetQuestLogGroupNum(lua_State* L) {
    const auto* q = selectedLogEntry(getGameHandler(L));
    lua_pushnumber(L, q ? q->suggestedPlayers : 0);
    return 1;
}

static int lua_GetNumQuestLogRewards(lua_State* L) {
    const auto* q = selectedLogEntry(getGameHandler(L));
    lua_pushnumber(L, q ? countRewards(&q->rewardItems) : 0);
    return 1;
}

static int lua_GetNumQuestLogChoices(lua_State* L) {
    const auto* q = selectedLogEntry(getGameHandler(L));
    lua_pushnumber(L, q ? countRewards(&q->rewardChoiceItems) : 0);
    return 1;
}

static int lua_GetQuestLogRewardInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* q = selectedLogEntry(gh);
    if (!q) { return luaReturnNil(L); }
    return pushRewardAt(L, gh, q->rewardItems,
                        static_cast<int>(luaL_optnumber(L, 1, 0)));
}

static int lua_GetQuestLogChoiceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* q = selectedLogEntry(gh);
    if (!q) { return luaReturnNil(L); }
    return pushRewardAt(L, gh, q->rewardChoiceItems,
                        static_cast<int>(luaL_optnumber(L, 1, 0)));
}

// GetQuestLogItemLink(type, index) → a reward of the selected quest, as a link.
//
// The quest log's own version of GetQuestItemLink, over the same two lists
// GetQuestLogRewardInfo and GetQuestLogChoiceInfo read. Shift-clicking a
// reward in the log put nothing in chat before.
static int lua_GetQuestLogItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* q = selectedLogEntry(gh);
    const char* type = luaL_optstring(L, 1, "reward");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || !q || index < 1) { return luaReturnNil(L); }

    // The two lists are fixed arrays of different lengths, so they are walked
    // by a template rather than bound to one reference.
    const auto linkAt = [&](const auto& list) -> int {
        int seen = 0;
        for (const auto& r : list) {
            if (r.itemId == 0) continue;
            if (++seen != index) continue;
            const auto* info = gh->getItemInfo(r.itemId);
            const std::string name = info ? info->name : "";
            if (name.empty()) break;
            lua_pushstring(L, game::buildItemLink(r.itemId,
                                                  info ? info->quality : 1u,
                                                  name).c_str());
            return 1;
        }
        return luaReturnNil(L);
    };
    return (std::string(type) == "choice") ? linkAt(q->rewardChoiceItems)
                                           : linkAt(q->rewardItems);
}

// Nil for the same reason GetQuestSpellLink is: nothing this client parses
// says a quest gives a spell, so there is no spell to name.
static int lua_GetQuestLogSpellLink(lua_State* L) {
    return luaReturnNil(L);
}

// The server sends one field for both, and its sign says which: AzerothCore's
// GetRewOrReqMoney returns the reward when it is positive and the *cost* when
// it is negative, and the quest query serializer writes that single int32. So a
// quest that charges the player was answering this with a negative reward, and
// the reward panel drew a money frame with a negative amount in it.
static int lua_GetQuestLogRewardMoney(lua_State* L) {
    const auto* q = selectedLogEntry(getGameHandler(L));
    const int32_t money = q ? q->rewardMoney : 0;
    lua_pushnumber(L, money > 0 ? money : 0);
    return 1;
}

// ...and the other half of that field, which answered zero for every quest.
//
// Named with an index by the world map and without one by the quest info
// panel, which is how most of the quest log accessors are asked: the selected
// entry is the default, not the only answer.
static int lua_GetQuestLogRequiredMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    const game::GameHandler::QuestLogEntry* q = nullptr;
    if (gh && lua_isnumber(L, 1)) {
        q = questAtRow(gh, static_cast<int>(lua_tonumber(L, 1)));
    } else {
        q = selectedLogEntry(gh);
    }
    const int32_t money = q ? q->rewardMoney : 0;
    lua_pushnumber(L, money < 0 ? -money : 0);
    return 1;
}

// --- The quest giver's own dialog ---
//
// WoW's quest-giver functions do not name which quest they mean: GetTitleText
// and the reward accessors serve the dialog and the quest log both, and answer
// for whichever is in front. A dialog is in front while it is open - the
// reward panel over the progress panel over the offer, in the order the server
// walks a player through them - and the log's selection answers otherwise.

namespace {

struct QuestSource {
    const std::string* title = nullptr;
    const std::vector<game::QuestRewardItem>* rewards = nullptr;
    const std::vector<game::QuestRewardItem>* choices = nullptr;
    uint32_t money = 0;
    uint32_t xp = 0;
    uint32_t honor = 0;
    uint32_t talents = 0;
    uint32_t arenaPoints = 0;
};

QuestSource currentQuestSource(game::GameHandler* gh) {
    QuestSource s;
    if (!gh) return s;
    if (gh->isQuestOfferRewardOpen()) {
        const auto& d = gh->getQuestOfferReward();
        s = {&d.title, &d.fixedRewards, &d.choiceRewards, d.rewardMoney, d.rewardXp,
             d.rewardHonor, d.rewardTalents, d.rewardArenaPoints};
    } else if (gh->isQuestRequestItemsOpen()) {
        const auto& d = gh->getQuestRequestItems();
        s.title = &d.title;
    } else if (gh->isQuestDetailsOpen()) {
        const auto& d = gh->getQuestDetails();
        s = {&d.title, &d.rewardItems, &d.rewardChoiceItems, d.rewardMoney, d.rewardXp};
    }
    return s;
}

/// Counts only the filled slots: the server pads a reward list to a fixed
/// width, and a row drawn for an empty one is a blank button the player can
/// click.

int pushQuestText(lua_State* L, const std::string* s) {
    if (!s || s->empty()) { lua_pushstring(L, ""); return 1; }
    // The binding is the first point that has the player to resolve against;
    // the parser only ever turned $b into a line break. See forPlayer.
    lua_pushstring(L, forPlayer(L, *s).c_str());
    return 1;
}

}  // namespace

// GetTitleText() → the quest's name, whichever panel is showing it
static int lua_GetTitleText(lua_State* L) {
    return pushQuestText(L, currentQuestSource(getGameHandler(L)).title);
}

// GetQuestText() → what the quest giver says when offering it
static int lua_GetQuestText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isQuestDetailsOpen()) { lua_pushstring(L, ""); return 1; }
    return pushQuestText(L, &gh->getQuestDetails().details);
}

// GetObjectiveText() → what the quest asks for
static int lua_GetObjectiveText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isQuestDetailsOpen()) { lua_pushstring(L, ""); return 1; }
    return pushQuestText(L, &gh->getQuestDetails().objectives);
}

// GetProgressText() → what is said while the quest is still unfinished
static int lua_GetProgressText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isQuestRequestItemsOpen()) { lua_pushstring(L, ""); return 1; }
    return pushQuestText(L, &gh->getQuestRequestItems().completionText);
}

// GetRewardText() → what is said on handing it in
static int lua_GetRewardText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isQuestOfferRewardOpen()) { lua_pushstring(L, ""); return 1; }
    return pushQuestText(L, &gh->getQuestOfferReward().rewardText);
}

// GetGossipText() → the body of the gossip window
static int lua_GetGossipText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isGossipWindowOpen()) { lua_pushstring(L, ""); return 1; }
    return pushQuestText(L, &gh->getNpcText(gh->getCurrentGossip().titleTextId));
}

// GetRewardMoney() → coin the quest pays out
static int lua_GetRewardMoney(lua_State* L) {
    lua_pushnumber(L, currentQuestSource(getGameHandler(L)).money);
    return 1;
}

// GetRewardXP() → experience the quest pays out
static int lua_GetRewardXP(lua_State* L) {
    lua_pushnumber(L, currentQuestSource(getGameHandler(L)).xp);
    return 1;
}
// The offer panel's honour, talents and arena points - the quest-giver twins of
// GetQuestLogReward*, read from the offer packet (which carries all three after
// the money and XP) rather than the query. Were hard zero.
static int lua_GetRewardHonor(lua_State* L) {
    lua_pushnumber(L, currentQuestSource(getGameHandler(L)).honor);
    return 1;
}
static int lua_GetRewardTalents(lua_State* L) {
    lua_pushnumber(L, currentQuestSource(getGameHandler(L)).talents);
    return 1;
}
static int lua_GetRewardArenaPoints(lua_State* L) {
    lua_pushnumber(L, currentQuestSource(getGameHandler(L)).arenaPoints);
    return 1;
}

// GetQuestMoneyToGet() → coin the player has to *hand over*, which is the
// opposite of the reward and a different field entirely. Some quests ask for
// money; showing the reward here would tell the player they are being paid
// when they are being charged.
static int lua_GetQuestMoneyToGet(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isQuestRequestItemsOpen()) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getQuestRequestItems().requiredMoney);
    return 1;
}

// IsQuestCompletable() → whether the turn-in button should work
static int lua_IsQuestCompletable(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool ok = gh && gh->isQuestRequestItemsOpen() &&
                    gh->getQuestRequestItems().isCompletable();
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// GetQuestReward(choiceIndex) → takes the reward and closes the dialog
static int lua_GetQuestReward(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    // One-based, and zero means the quest offered nothing to choose between.
    const int choice = static_cast<int>(luaL_optnumber(L, 1, 0));
    gh->chooseQuestReward(choice > 0 ? static_cast<uint32_t>(choice - 1) : 0);
    return 0;
}

// CloseQuest() → puts the dialog away without answering it
static int lua_CloseQuest(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    // Without announcing. This is QuestFrame telling the client it has closed
    // - it runs from QuestFrame_OnHide - and answering with QUEST_FINISHED,
    // which is the event that hides QuestFrame, closes it a second time from
    // inside its own closing.
    if (gh->isQuestOfferRewardOpen())       gh->closeQuestOfferReward(false);
    else if (gh->isQuestRequestItemsOpen()) gh->closeQuestRequestItems(false);
    else                                    gh->declineQuest(false);
    return 0;
}

// GetQuestItemInfo(type, index) → name, texture, count, quality, isUsable
//
// type is "choice" for the rewards a player picks between and "reward" for the
// ones always given.
static int lua_GetQuestItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* type = luaL_optstring(L, 1, "reward");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    const QuestSource s = currentQuestSource(gh);

    // Three lists, not two. "required" is what the *progress* page asks for -
    // the items a quest wants handed in - and it was falling through to the
    // reward list, so turning in a quest showed what it would pay rather than
    // what it wanted.
    const std::string want(type);
    const std::vector<game::QuestRewardItem>* list = nullptr;
    if (want == "choice")        list = s.choices;
    else if (want == "required") list = gh ? &gh->getQuestRequestItems().requiredItems : nullptr;
    else                         list = s.rewards;
    if (!gh || !list || index < 1) { return luaReturnNil(L); }

    int seen = 0;
    for (const auto& r : *list) {
        if (r.itemId == 0) continue;
        if (++seen != index) continue;
        const auto* info = gh->getItemInfo(r.itemId);
        lua_pushstring(L, info ? info->name.c_str() : "");
        lua_pushstring(L, gh->getItemIconPath(
            info && info->displayInfoId ? info->displayInfoId : r.displayInfoId).c_str());
        lua_pushnumber(L, r.count);
        lua_pushnumber(L, info ? info->quality : 1);
        lua_pushboolean(L, 1);
        return 5;
    }
    return luaReturnNil(L);
}

// GetQuestItemLink(type, index) → the reward as a link, for shift-clicking it
// into chat.
//
// The same two lists GetQuestItemInfo walks, answered as a link rather than as
// pieces. Nil when there is no such reward, which is what the click handler
// checks before doing anything with it.
static int lua_GetQuestItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* type = luaL_optstring(L, 1, "reward");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    const QuestSource s = currentQuestSource(gh);

    const auto* list = (std::string(type) == "choice") ? s.choices : s.rewards;
    if (!gh || !list || index < 1) { return luaReturnNil(L); }

    int seen = 0;
    for (const auto& r : *list) {
        if (r.itemId == 0) continue;
        if (++seen != index) continue;
        const auto* info = gh->getItemInfo(r.itemId);
        const std::string name = info ? info->name : "";
        if (name.empty()) { return luaReturnNil(L); }
        lua_pushstring(L, game::buildItemLink(r.itemId,
                                              info ? info->quality : 1u,
                                              name).c_str());
        return 1;
    }
    return luaReturnNil(L);
}

// GetQuestSpellLink(...) - the spell a quest gives, as a link.
//
// Nil, and deliberately: no quest packet this client parses carries a reward
// spell, so there is nothing to name. The click handler passes whatever it
// gets to HandleModifiedItemClick, which does nothing with nil - where a
// made-up link would put a spell in someone's chat that the quest does not
// give.
static int lua_GetQuestSpellLink(lua_State* L) {
    return luaReturnNil(L);
}

void registerQuestLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetNumQuestLogEntries",   lua_GetNumQuestLogEntries},
                // Whether a quest has been finished, ever.
                //
                // The client asks the server for this list on entering the
                // world - CMSG_QUERY_QUESTS_COMPLETED - parses the reply into
                // completedQuests_ and keeps it there. Nothing could read it:
                // the two names that ask, which every quest addon uses and
                // which is how a quest giver knows to grey an offer out, were
                // not bound at all.
                {"IsQuestFlaggedCompleted", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            lua_pushboolean(L, (gh && id && gh->isQuestCompleted(id)) ? 1 : 0);
            return 1;
        }},
                // GetQuestsCompleted([table]) - the whole set at once, keyed by
                // quest id, which is the shape the caller indexes. Fills the
                // table it is given, as WoW does, so a caller reusing one does
                // not allocate per call.
                {"GetQuestsCompleted", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!lua_istable(L, 1)) lua_newtable(L);
            else                    lua_pushvalue(L, 1);
            if (gh) {
                const auto& done = gh->getCompletedQuests();
                // Lua promises twenty free slots; this pushes two at a time and
                // pops them, so the depth never grows with the set's size.
                for (uint32_t id : done) {
                    lua_pushnumber(L, static_cast<lua_Number>(id));
                    lua_pushboolean(L, 1);
                    lua_rawset(L, -3);
                }
            }
            return 1;
        }},
                {"GetQuestLogTimeLeft",     lua_GetQuestLogTimeLeft},
                {"IsCurrentQuestFailed",    lua_IsCurrentQuestFailed},
                {"GetQuestLogRewardSpell",  lua_GetQuestLogRewardSpell},
                {"GetRewardSpell",          lua_GetQuestRewardSpell},
                {"GetRewardTitle",          lua_GetQuestRewardTitle},
                {"GetQuestLogRewardTitle",  lua_GetQuestRewardTitle},
                {"ProcessQuestLogRewardFactions", lua_ProcessQuestLogRewardFactions},
                {"GetNumQuestLogRewardFactions",  lua_GetNumQuestLogRewardFactions},
                {"GetQuestLogRewardFactionInfo",  lua_GetQuestLogRewardFactionInfo},
                {"GetFactionInfoByID",      lua_GetFactionInfoByID},
                {"GetQuestWatchIndex",      lua_GetQuestWatchIndex},
                {"SortQuestWatches",        lua_SortQuestWatches},
                {"ShiftQuestWatches",       lua_ShiftQuestWatches},
                {"GetQuestSortIndex",       lua_GetQuestSortIndex},
                {"IsQuestLogSpecialItemInRange", lua_IsQuestLogSpecialItemInRange},
                {"UseQuestLogSpecialItem",  lua_UseQuestLogSpecialItem},
                {"GetQuestLogSpecialItemCooldown", lua_GetQuestLogSpecialItemCooldown},
                {"GetQuestTimers",          lua_GetQuestTimers},
                {"GetQuestIndexForTimer",   lua_GetQuestIndexForTimer},
                {"GetQuestLogTitle",        lua_GetQuestLogTitle},
                // IsUnitOnQuest(questIndex, unit) - whether that unit is also
                // on the quest, which the log prints as "[2]" beside an entry
                // to say how many group mates share it.
                //
                // Only answerable for the player: the server does not tell a
                // client what its party members' quest logs hold, and nothing
                // here tracks them. False for everyone else is the truthful
                // answer and is what leaves the counter hidden, rather than
                // claiming a number nobody can stand behind.
                {"IsUnitOnQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            const char* uid = luaL_optstring(L, 2, "player");
            std::string u(uid);
            for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!gh || u != "player" || index < 1) { lua_pushboolean(L, 0); return 1; }
            lua_pushboolean(L, questAtRow(gh, index) != nullptr ? 1 : 0);
            return 1;
        }},
                {"GetQuestLogQuestText",    lua_GetQuestLogQuestText},
                // GetQuestLogCompletionText(index) → what to do now it is done.
                //
                // An empty string rather than nothing when there is none,
                // because the world map hovers a finished quest's pin and
                // writes AddLine("- "..GetQuestLogCompletionText(i)) without
                // checking. Concatenating nil raises, so the pin would have
                // taken the map down; an empty string draws a bare dash.
                //
                // The world map's quest list puts this straight into the row's
                // objectives line for any quest that is complete, so answering
                // "" left the row blank where "Return to Marshal Dughan"
                // belongs. It is the fifth string in SMSG_QUEST_QUERY_RESPONSE
                // and was being walked past.
                //
                // A quest whose server sent no such line falls back to its own
                // objectives sentence rather than to nothing. Both callers put
                // this string on screen unconditionally - the tracker as the
                // one line under a completed quest's title, the world map in
                // the pin's tooltip - so an empty answer is a bare dash where a
                // sentence belongs, which is what completed quests in the
                // tracker looked like.
                //
                // The objectives sentence and not a phrase of this client's
                // own: "Bring Mor'Ladim's Skull to Sven Yorgen." says where to
                // take it, which is what this line is for, and it is the
                // server's own words. A stand-in reading "Quest completed" was
                // tried and is wrong - the tracker draws this line for any
                // quest with nothing countable in it, complete or not, so that
                // phrase announced completion on quests that were still open.
                {"GetQuestLogCompletionText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const game::GameHandler::QuestLogEntry* q = nullptr;
            if (gh && lua_isnumber(L, 1)) {
                q = questAtRow(gh, static_cast<int>(lua_tonumber(L, 1)));
            } else {
                q = selectedLogEntry(gh);
            }
            if (!q) { lua_pushstring(L, ""); return 1; }
            if (!q->completionText.empty()) {
                lua_pushstring(L, q->completionText.c_str());
                return 1;
            }
            // Only where the interface actually draws this line: a quest the
            // server has called complete, or one with nothing countable in it,
            // which the tracker treats as complete itself. Anything else keeps
            // the empty answer the API promises.
            const int objectives = game::questObjectiveCount(*q);
            const bool drawnAsComplete = q->complete || objectives == 0;
            if (!drawnAsComplete) { lua_pushstring(L, ""); return 1; }

            // Said once per quest that lands here, because two different
            // things put a quest in this branch and only the log can tell them
            // apart: the server calling it complete, or this client having no
            // countable objective for it - which the tracker then reads as
            // complete on its own, marker and all, whether or not it is.
            static std::set<uint32_t> announced;
            if (announced.insert(q->questId).second) {
                LOG_WARNING("Quest ", q->questId, " (", q->title, ") is drawn as "
                            "complete with no completed line of its own: complete=",
                            q->complete, " objectives=", objectives,
                            " - falling back to its objectives text, '",
                            q->objectives, "'");
            }
            lua_pushstring(L, q->objectives.c_str());
            return 1;
        }},
                // The words on a book or a plaque, which this client parses out
                // of SMSG_ITEM_TEXT_QUERY_RESPONSE and kept to itself. This
                // answered with an empty string on the reasoning that nothing
                // opened the window anyway; ITEM_TEXT_READY is fired now, so it
                // does open, and empty is the difference between a book and a
                // blank page.
                //
                // Still a string and never nil: the page is drawn as
                // "\n"..ItemTextGetText()..creator, and a nil there takes the
                // whole window down rather than leaving it empty.
                {"ItemTextGetText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushstring(L, ""); return 1; }
            // Two things arrive on this frame and they come by different
            // opcodes: a letter's body, which is one page, and a book, which
            // chains through nextPageId and is collected whole before anyone
            // reads it. A book wins while one is open, because a letter cannot
            // be open at the same time.
            const auto& pages = gh->getBookPages();
            if (!pages.empty()) {
                const size_t page = static_cast<size_t>(bookPage());
                lua_pushstring(L, page < pages.size() ? pages[page].text.c_str()
                                                      : pages.back().text.c_str());
                return 1;
            }
            lua_pushstring(L, gh->getItemText().c_str());
            return 1;
        }},
                // ItemTextGetItem() - the heading over the page. The page
                // text response carries the words and the id of the page
                // after it and nothing about what they belong to, so this is
                // kept from wherever the book was opened: the game object's
                // name out of its query cache, or the item's out of the bag.
                // It used to answer nil and the frame headed every book, sign
                // and plaque with nothing.
                {"ItemTextGetItem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const std::string title = gh ? gh->getBookTitle() : std::string();
            if (title.empty()) return luaReturnNil(L);
            lua_pushstring(L, title.c_str());
            return 1;
        }},
                // A book has no author; a letter copied out of the mail has
                // its sender as the item's creator (ITEM_FIELD_CREATOR), which
                // itemtextframe.lua prints as "From <name>". Nil drops the line.
                {"ItemTextGetCreator",  [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const std::string who = gh ? gh->getItemTextCreator() : std::string();
            if (who.empty()) return luaReturnNil(L);
            lua_pushstring(L, who.c_str());
            return 1;
        }},
                // The material is carried, and the claim that it was not stood
                // beside the line discarding it: the item query's four
                // post-description words are PageText, LanguageID,
                // PageMaterial and StartQuest, and all three parsers read the
                // third into nothing. A game object's is in its template two
                // words behind the page id.
                //
                // It names one of PageTextMaterial.dbc's seven backings, which
                // is what itemtextframe wants - it picks both the frame's
                // texture and the text colour by that word, so every letter
                // and tablet was drawn on parchment.
                {"ItemTextGetMaterial", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnNil(L);
            const std::string& name = gh->getPageTextMaterialName(gh->getBookMaterial());
            // Nil rather than an empty string for an unknown id: the frame
            // tests `if ( not material )` and substitutes Parchment itself.
            if (name.empty()) return luaReturnNil(L);
            lua_pushstring(L, name.c_str());
            return 1;
        }},
                // A letter is one page and its buttons stay hidden. A book is
                // as many as its chain has, all of them already fetched, so
                // turning one is a move through what is in hand rather than a
                // request - and the frame redraws on ITEM_TEXT_READY, which is
                // what makes the move visible.
                {"ItemTextGetPage", [](lua_State* L) -> int {
            lua_pushnumber(L, bookPage() + 1);
            return 1;
        }},
                {"ItemTextHasNextPage", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const size_t pages = gh ? gh->getBookPages().size() : 0;
            lua_pushboolean(L, static_cast<size_t>(bookPage() + 1) < pages ? 1 : 0);
            return 1;
        }},
                {"ItemTextPrevPage", [](lua_State* L) -> int {
            if (bookPage() > 0) { --bookPage(); fireItemTextReady(L); }
            return 0;
        }},
                {"ItemTextNextPage", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const size_t pages = gh ? gh->getBookPages().size() : 0;
            if (static_cast<size_t>(bookPage() + 1) < pages) {
                ++bookPage();
                fireItemTextReady(L);
            }
            return 0;
        }},
                // Closing is a state change this client owns, and the frame
                // calls it on hide as well as from its close button.
                {"CloseItemText", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) {
                gh->closeItemText();
                // The pages go with the book, and so does the place in it -
                // left behind, the next book opens at whatever page the last
                // one was left on and shows nothing until it is paged back.
                gh->clearBook();
            }
            bookPage() = 0;
            return 0;
        }},
                {"IsQuestComplete",         lua_IsQuestComplete},
                {"SelectQuestLogEntry",     lua_SelectQuestLogEntry},
                {"GetQuestLogSelection",    lua_GetQuestLogSelection},
                {"GetQuestLogPushable",     lua_GetQuestLogPushable},
                {"QuestLogPushQuest",       lua_QuestLogPushQuest},
                {"GetNumQuestWatches",      lua_GetNumQuestWatches},
                {"GetQuestIndexForWatch",   lua_GetQuestIndexForWatch},
                {"AddQuestWatch",           lua_AddQuestWatch},
                {"RemoveQuestWatch",        lua_RemoveQuestWatch},
                {"IsQuestWatched",          lua_IsQuestWatched},
                {"GetQuestLink",            lua_GetQuestLink},
                {"GetNumQuestLeaderBoards", lua_GetNumQuestLeaderBoards},
                {"GetQuestLogLeaderBoard",  lua_GetQuestLogLeaderBoard},
                {"GetQuestPOILeaderBoard",  lua_GetQuestPOILeaderBoard},
                {"ExpandQuestHeader",       lua_ExpandQuestHeader},
                {"CollapseQuestHeader",     lua_CollapseQuestHeader},
                {"GetQuestLogSpecialItemInfo", lua_GetQuestLogSpecialItemInfo},
                {"GetNumSkillLines",        lua_GetNumSkillLines},
                {"GetStablePetInfo",       lua_GetStablePetInfo},
                {"GetNumStablePets",       lua_GetNumStablePets},
                {"GetNumStableSlots",      lua_GetNumStableSlots},
                {"GetSelectedStablePet",   lua_GetSelectedStablePet},
                {"ClickStablePet",         lua_ClickStablePet},
                {"GetPetIcon",             lua_GetPetIcon},
                {"GetPetTalentTree",       lua_GetPetTalentTree},
                {"GetPetFoodTypes",        lua_GetPetFoodTypes},
                {"GetStablePetFoodTypes",  lua_GetStablePetFoodTypes},
                {"SetPetStablePaperdoll",  lua_SetPetStablePaperdoll},
                {"PickupStablePet",        lua_PickupStablePet},
                {"ClosePetStables",        lua_ClosePetStables},
                {"IsAtStableMaster",       lua_IsAtStableMaster},
                {"GetNextStableSlotCost",  lua_GetNextStableSlotCost},
        {"StablePet",              lua_StablePet},
        {"UnstablePet",            lua_UnstablePet},
                {"GetSkillLineInfo",        lua_GetSkillLineInfo},
                // Opening and closing a heading. The index is a position in
                // the drawn rows, and a click on a skill row rather than a
                // heading is ignored rather than refused - the tab calls these
                // from the row label template whatever the row turns out to be.
                {"ExpandSkillHeader",   [](lua_State* L) -> int {
            return skillHeaderSetCollapsed(L, false);
        }},
                {"CollapseSkillHeader", [](lua_State* L) -> int {
            return skillHeaderSetCollapsed(L, true);
        }},
                {"GetNumTalentTabs",        lua_GetNumTalentTabs},
                {"GetTalentTabInfo",        lua_GetTalentTabInfo},
                {"GetNumTalents",           lua_GetNumTalents},
                {"GetTalentInfo",           lua_GetTalentInfo},
                {"GetTalentPrereqs",        lua_GetTalentPrereqs},
                {"AddPreviewTalentPoints",  lua_AddPreviewTalentPoints},
                {"GetGroupPreviewTalentPointsSpent", lua_GetGroupPreviewTalentPointsSpent},
                {"ResetGroupPreviewTalentPoints",    lua_ResetGroupPreviewTalentPoints},
                {"LearnPreviewTalents",     lua_LearnPreviewTalents},
                {"LearnTalent",             lua_LearnTalent},
                {"GetUnspentTalentPoints",  lua_GetUnspentTalentPoints},
                {"GetNumTalentGroups",      lua_GetNumTalentGroups},
                {"SetActiveTalentGroup",    lua_SetActiveTalentGroup},
                {"GetTalentLink",           lua_GetTalentLink},
                {"GetActiveTalentGroup",    lua_GetActiveTalentGroup},
                {"AcceptQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->acceptQuest();
            return 0;
        }},
                {"DeclineQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Announcing: the button was pressed, and the frame has not hidden
            // itself yet. QUEST_FINISHED is what hides it.
            if (gh) gh->declineQuest(true);
            return 0;
        }},
                // The other party member's quest, not the one on the table.
                // StaticPopup "QUEST_ACCEPT" calls this from OnAccept, so it
                // has to exist before QUEST_ACCEPT_CONFIRM is fired at all:
                // the popup would come up and raise on the yes.
                {"ConfirmAcceptQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->acceptSharedQuest();
            return 0;
        }},
                {"CompleteQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->completeQuest();
            return 0;
        }},
                // Abandoning is two steps, not one: the quest log marks which
                // quest it means, a confirmation shows its name, and only then
                // is it abandoned. AbandonQuest therefore takes no argument in
                // the interface, and requiring one raised a Lua error on every
                // attempt - the id is accepted when given so anything already
                // passing one keeps working.
                {"SetAbandonQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto* q = selectedLogEntry(gh);
            pendingAbandonQuest() = q ? q->questId : 0;
            return 0;
        }},
                {"GetAbandonQuestName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = pendingAbandonQuest();
            if (gh && id != 0) {
                for (const auto& q : gh->getQuestLog()) {
                    if (q.questId == id) {
                        lua_pushstring(L, forPlayer(L, q.title).c_str());
                        return 1;
                    }
                }
            }
            lua_pushstring(L, "");
            return 1;
        }},
                // What is destroyed along with the quest. Nothing here knows
                // which items a quest would take back, and an invented list
                // would warn about items the player keeps.
                // What abandoning the quest destroys, or nil for nothing -
                // and nil is the load-bearing part. Both callers do
                // `if ( items )` to choose between two popups, and an empty
                // string is true in Lua, so answering "" always took the
                // ABANDON_QUEST_WITH_ITEMS branch and showed "you will lose:"
                // with nothing after the colon.
                //
                // The quest's source item is the one the server takes back, and
                // this client has stored it since the reward parser learned to
                // read it - it even queries the item info for it on arrival.
                // Items gathered for the quest are not named: which of those
                // the server destroys depends on flags this client does not
                // parse, and listing the wrong ones is worse than listing none.
                {"GetAbandonQuestItems", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = pendingAbandonQuest();
            if (!gh || id == 0) { lua_pushnil(L); return 1; }
            for (const auto& q : gh->getQuestLog()) {
                if (q.questId != id) continue;
                if (q.sourceItemId == 0) break;
                const auto* info = gh->getItemInfo(q.sourceItemId);
                if (!info || !info->valid || info->name.empty()) break;
                lua_pushstring(L, info->name.c_str());
                return 1;
            }
            lua_pushnil(L);
            return 1;
        }},
                {"AbandonQuest", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const uint32_t questId = lua_isnoneornil(L, 1)
                ? pendingAbandonQuest()
                : static_cast<uint32_t>(luaL_checknumber(L, 1));
            if (questId != 0) gh->abandonQuest(questId);
            pendingAbandonQuest() = 0;
            return 0;
        }},
                // Both of these answer for the quest giver's dialog while one
                // is open and for the log's selection otherwise, which is the
                // same rule the text accessors follow.
                {"GetNumQuestRewards", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            if (const auto* v = currentQuestSource(gh).rewards) {
                lua_pushnumber(L, countRewards(v));
                return 1;
            }
            const auto* q = questAtLogRow(gh, gh->getSelectedQuestLogIndex());
            if (!q) { return luaReturnZero(L); }
            lua_pushnumber(L, countRewards(&q->rewardItems));
            return 1;
        }},
                {"GetNumQuestChoices", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            if (const auto* v = currentQuestSource(gh).choices) {
                lua_pushnumber(L, countRewards(v));
                return 1;
            }
            const auto* q = questAtLogRow(gh, gh->getSelectedQuestLogIndex());
            if (!q) { return luaReturnZero(L); }
            lua_pushnumber(L, countRewards(&q->rewardChoiceItems));
            return 1;
        }},
                {"GetSuggestedGroupNum", lua_GetSuggestedGroupNum},
                {"GetDailyQuestsCompleted", lua_GetDailyQuestsCompleted},
                {"GetMaxDailyQuests",    lua_GetMaxDailyQuests},
                {"GetQuestLogGroupNum",  lua_GetQuestLogGroupNum},
                {"GetNumQuestLogRewards", lua_GetNumQuestLogRewards},
                {"GetNumQuestLogChoices", lua_GetNumQuestLogChoices},
                {"GetQuestLogRewardInfo", lua_GetQuestLogRewardInfo},
                {"GetQuestLogChoiceInfo", lua_GetQuestLogChoiceInfo},
                {"GetQuestLogItemLink",     lua_GetQuestLogItemLink},
                {"GetQuestLogSpellLink",    lua_GetQuestLogSpellLink},
                {"GetQuestLogRewardMoney", lua_GetQuestLogRewardMoney},
                {"GetQuestLogRequiredMoney", lua_GetQuestLogRequiredMoney},
                {"GetTitleText",         lua_GetTitleText},
                {"GetQuestText",         lua_GetQuestText},
                {"GetObjectiveText",     lua_GetObjectiveText},
                {"GetProgressText",      lua_GetProgressText},
                {"GetRewardText",        lua_GetRewardText},
                {"GetGossipText",        lua_GetGossipText},
                {"GetQuestItemInfo",     lua_GetQuestItemInfo},
                // How many items a quest wants handed in.
                // QuestFrameProgressItems_Update reads it straight into
                // `numRequiredItems > 0`, and comparing nil against a number
                // raises - so opening the progress page of any quest that
                // takes items took the page down.
                {"GetNumQuestItems", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            const auto& req = gh->getQuestRequestItems().requiredItems;
            int n = 0;
            for (const auto& r : req) if (r.itemId != 0) ++n;
            lua_pushnumber(L, n);
            return 1;
        }},
                // The parchment behind the quest text, and nil is right for a
                // different reason than the one that used to be written here.
                //
                // That reason was "the only material this client has art for",
                // and it is not: Data/interface/itemtextframe carries bronze,
                // marble, silver, stone and valentine, and QuestFrame_GetMaterial
                // builds exactly those names - Interface\ItemTextFrame\ItemText-
                // <material>-TopLeft and its three corners. Parchment has no
                // files because it is the frame's own art. The same set backs
                // ItemTextGetMaterial, which does answer.
                //
                // What is missing is a source, not a texture. A book's material
                // is on the item or the game object that opened it;
                // SMSG_QUESTGIVER_QUEST_DETAILS carries no such field, and the
                // quest giver's own is not the quest's. Answering from the
                // giver would be a guess dressed as data, and the caller's
                // substitution of Parchment is what nearly every quest wants.
                {"GetQuestBackgroundMaterial", [](lua_State* L) -> int { return luaReturnNil(L); }},
                // Whether the quest is flagged PvP, and whether the giver
                // opened it without being asked. Neither is parsed from the
                // quest packets here, and false is what the interface does
                // with an absent answer anyway.
                // QuestFlagsPVP() - whether holding this quest forces the PvP
                // flag on. The accept button branches on it and puts up
                // CONFIRM_ACCEPT_PVP_QUEST instead of accepting outright, so a
                // flat false took a quest that flags you for PvP without
                // asking. QUEST_FLAGS_FLAGS_PVP is 0x2000 in AzerothCore's
                // QuestDef.h; the offer packet carried it and the parser threw
                // it away. Zero before Wrath, where the offer has no flags
                // field for it to be in.
                {"QuestFlagsPVP", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            constexpr uint32_t kQuestFlagsPvp = 0x00002000u;
            lua_pushboolean(L, gh && (gh->getQuestDetails().questFlags & kQuestFlagsPvp) ? 1 : 0);
            return 1;
        }},
                // QuestGetAutoAccept() stays false, and that is the answer
                // rather than a gap: QUEST_FLAGS_AUTO_ACCEPT is 0x80000 and
                // AzerothCore's own note beside it says no 3.3.5a quest
                // carries it. The flag is parsed now, so if one ever does this
                // can read it from the same place.
                {"QuestGetAutoAccept", [](lua_State* L) -> int { lua_pushboolean(L, 0); return 1; }},
                // Called when a reward is confirmed without one being picked.
                // Wow.exe's (call at 0x58c3a5) raises ERR_QUEST_MUST_CHOOSE, an error
                // frame line with igQuestFailed, from the client: no packet
                // is involved.
                {"QuestChooseRewardError", [](lua_State* L) -> int {
                    if (auto* gh = getGameHandler(L)) {
                        gh->addUIError("You must choose a reward.");
                        gh->playUiMessageSound("ERR_QUEST_MUST_CHOOSE");
                    }
                    return 0;
                }},
                {"GetQuestItemLink",        lua_GetQuestItemLink},
                {"GetQuestSpellLink",       lua_GetQuestSpellLink},
                {"GetQuestMoneyToGet",   lua_GetQuestMoneyToGet},
                {"GetRewardMoney",       lua_GetRewardMoney},
                {"GetRewardXP",          lua_GetRewardXP},
                {"GetRewardHonor",              lua_GetRewardHonor},
                {"GetRewardArenaPoints",        lua_GetRewardArenaPoints},
                {"GetRewardTalents",            lua_GetRewardTalents},
                {"GetQuestLogRewardHonor",       lua_GetQuestLogRewardHonor},
                {"GetQuestLogRewardArenaPoints", lua_GetQuestLogRewardArenaPoints},
                {"GetQuestLogRewardTalents",     lua_GetQuestLogRewardTalents},
                {"GetQuestLogRewardXP",          lua_GetQuestLogRewardXP},
                {"IsQuestCompletable",   lua_IsQuestCompletable},
                {"GetQuestReward",       lua_GetQuestReward},
                {"CloseQuest",           lua_CloseQuest},
                // RemoveGlyphFromSocket(socket) - the Accept on the "remove
                // this glyph?" popup, which is the only way to clear a socket.
                // Unbound, the dialog appeared and answering it raised, so a
                // glyph could be put in and never taken out.
                //
                // FrameXML counts sockets from one and the server from zero.
                {"RemoveGlyphFromSocket", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int socket = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (gh && socket >= 1 && socket <= game::GameHandler::MAX_GLYPH_SLOTS)
                gh->removeGlyphFromSocket(static_cast<uint32_t>(socket - 1));
            return 0;
        }},
                {"GetNumGlyphSockets", [](lua_State* L) -> int {
            lua_pushnumber(L, game::GameHandler::MAX_GLYPH_SLOTS);
            return 1;
        }},
                {"GetGlyphSocketInfo", [](lua_State* L) -> int {
            // GetGlyphSocketInfo(index [, talentGroup]) → enabled, glyphType, glyphSpellID, icon
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            int spec = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1 || index > game::GameHandler::MAX_GLYPH_SLOTS) {
                lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushnil(L); lua_pushnil(L);
                return 4;
            }
            const auto& glyphs = (spec >= 1 && spec <= 2)
                ? gh->getGlyphs(static_cast<uint8_t>(spec - 1)) : gh->getGlyphs();
            uint16_t glyphId = glyphs[index - 1];
            // The socket's type is its GlyphSlot.dbc row's TypeFlags (0 major,
            // 1 minor), the row being the slot id the server put in
            // PLAYER_FIELD_GLYPH_SLOTS_1. Stock 3.3.5a is major, minor, minor,
            // major, minor, major - which is why the frame's socket art is
            // laid out that way - not the three-and-three assumed here.
            // FrameXML's numbering: GLYPHTYPE_MAJOR 1, GLYPHTYPE_MINOR 2.
            const int32_t typeFlags = gh->glyphSocketTypeFlags(index);
            const int glyphType = typeFlags < 0 ? ((index == 1 || index == 4 || index == 6) ? 1 : 2)
                                                : (typeFlags == 1 ? 2 : 1);
            // Locked until the level unlocks it: PLAYER_GLYPHS_ENABLED, set by
            // AzerothCore's Player::InitGlyphsForLevel. glyphui draws the lock
            // art for enabled = false (blizzard_glyphui.lua 79-89).
            lua_pushboolean(L, gh->glyphSocketEnabled(index) ? 1 : 0);  // enabled
            lua_pushnumber(L, glyphType);       // glyphType (1=major, 2=minor)
            if (glyphId != 0) {
                // The *spell*, not the properties id the server sent. Every
                // caller treats this as a spell - the glyph panel builds its
                // link from it and the tooltip describes it - and pushing the
                // properties id gave them a number that means something else.
                // GlyphProperties.dbc field 1 is the spell; checked by
                // resolving it for all 362 rows, which come out named "Glyph
                // of Moonfire" and the like.
                gh->ensureGlyphPropertiesLoaded();
                const uint32_t spellId = gh->getGlyphSpellId(glyphId);
                lua_pushnumber(L, spellId ? spellId : glyphId);   // glyphSpellID
                // ...and its own icon rather than a warrior glyph for everyone.
                std::string icon = spellId ? gh->getSpellIconPath(spellId) : std::string();
                lua_pushstring(L, icon.empty()
                    ? "Interface\\Icons\\INV_Misc_QuestionMark" : icon.c_str());
            } else {
                lua_pushnil(L);
                lua_pushnil(L);
            }
            return 4;
        }},
                // ---- Trade skills ---------------------------------------
                //
                // Wow.exe 0x5DA120..0x5DE620 (the table at 0xAD0E80); see
                // docs/re/professions.md for each.
                {"GetNumTradeSkills", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            lua_pushnumber(L, sh ? static_cast<double>(sh->tradeSkill().numVisible()) : 0.0);
            return 1;
        }},
                // name, type, numAvailable, isExpanded, altVerb (0x5DB2A0).
                {"GetTradeSkillInfo", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillInfo(index)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            const auto* row = sh ? sh->tradeSkill().rowAt(i) : nullptr;
            if (!row) {
                lua_pushnil(L); lua_pushnil(L); lua_pushnumber(L, 0); lua_pushnil(L); lua_pushnil(L);
                return 5;
            }
            const auto& m = sh->tradeSkill();
            if (row->isHeader()) {
                std::string name;
                if (row->itemClass == -1 && row->itemSubClass == -1) {
                    // The line's alternate verb names the recipes that make
                    // nothing ("Enchant").
                    if (const auto* info = sh->tradeSkillLineInfo(m.skillLine)) name = info->alternateVerb;
                } else {
                    name = m.headerName(row->itemClass, row->itemSubClass);
                }
                lua_pushstring(L, name.c_str());
                lua_pushstring(L, "header");
                lua_pushnumber(L, 0);
                if (m.rowExpanded(static_cast<size_t>(i - 1))) lua_pushnumber(L, 1); else lua_pushnil(L);
                lua_pushnil(L);
                return 5;
            }
            auto p = tradeSkillPick(gh, i);
            lua_pushstring(L, p.recipe ? p.recipe->name.c_str() : "");
            lua_pushstring(L, game::stock_trade_skill::difficultyName(row->difficulty));
            lua_pushnumber(L, row->numAvailable);
            lua_pushnil(L);
            // altVerb: the line's, for a recipe whose first effect makes no
            // item - "Enchant" - else nil, which reads as "Create".
            const uint32_t effect0 = p.columns ? p.columns->effect[0] : (p.spell ? p.spell->effectIds[0] : 0);
            const auto* info = sh->tradeSkillLineInfo(m.skillLine);
            if (!game::stock_trade_skill::isCreateItemEffect(effect0) && info)
                lua_pushstring(L, info->alternateVerb.c_str());
            else
                lua_pushnil(L);
            return 5;
        }},
                {"GetTradeSkillIcon", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillIcon(index)");
            auto* gh = getGameHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!p.recipe) return luaReturnNil(L);
            std::string icon;
            if (const uint32_t made = tradeSkillMadeItem(p)) {
                gh->ensureItemInfo(made);
                if (const auto* info = gh->getItemInfo(made); info && info->valid && info->displayInfoId)
                    icon = gh->getItemIconPath(info->displayInfoId);
            } else {
                icon = gh->getSpellIconPath(static_cast<uint32_t>(p.recipe->spellId));
            }
            if (icon.empty()) return luaReturnNil(L);
            lua_pushstring(L, icon.c_str());
            return 1;
        }},
                {"GetTradeSkillDescription", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillDescription(index)");
            auto* gh = getGameHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!p.spell || p.spell->description.empty()) return luaReturnNil(L);
            const uint32_t id = static_cast<uint32_t>(p.recipe->spellId);
            lua_pushstring(L, gh->formatSpellDescription(id, p.spell->description).c_str());
            return 1;
        }},
                {"GetTradeSkillNumReagents", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillNumReagents(index)");
            auto p = tradeSkillPick(getGameHandler(L), i);
            int n = 0;
            if (p.spell)
                for (const auto& r : p.spell->reagents) if (r.itemId != 0) ++n;
            lua_pushnumber(L, n);
            return 1;
        }},
                // name, texture, needed, have (0x5DE400); the name and the
                // texture are nil until the item's reply.
                {"GetTradeSkillReagentInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
                return luaL_error(L, "Usage: GetTradeSkillReagentInfo(index, reagentIndex)");
            auto* gh = getGameHandler(L);
            auto p = tradeSkillPick(gh, static_cast<int>(lua_tonumber(L, 1)));
            const int n = static_cast<int>(lua_tonumber(L, 2));
            if (p.spell) {
                int seen = 0;
                for (const auto& r : p.spell->reagents) {
                    if (r.itemId == 0 || ++seen != n) continue;
                    gh->ensureItemInfo(r.itemId);
                    const auto* info = gh->getItemInfo(r.itemId);
                    if (info && info->valid) {
                        lua_pushstring(L, info->name.c_str());
                        const std::string icon = gh->getItemIconPath(info->displayInfoId);
                        if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
                    } else {
                        lua_pushnil(L);
                        lua_pushnil(L);
                    }
                    lua_pushnumber(L, r.count);
                    lua_pushnumber(L, gh->countItemInBags(r.itemId));
                    return 4;
                }
            }
            lua_pushnil(L); lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
            return 4;
        }},
                // 0x5DB6E0: the made count's range from effect 0's points.
                {"GetTradeSkillNumMade", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillNumMade(index)");
            auto p = tradeSkillPick(getGameHandler(L), i);
            int lo = 1, hi = 1;
            if (p.columns) {
                lo = p.columns->basePoints[0] + (p.columns->dieSides[0] != 0 ? 1 : 0);
                hi = p.columns->basePoints[0] + p.columns->dieSides[0];
                if (lo < 1) lo = 1;
                if (hi < 1) hi = 1;
            }
            lua_pushnumber(L, lo);
            lua_pushnumber(L, hi);
            return 2;
        }},
                // name, rank, maxRank (0x5DB810); "UNKNOWN", 0, 0 when closed.
                {"GetTradeSkillLine", [](lua_State* L) -> int {
            std::string name = "UNKNOWN";
            int rank = 0, maxRank = 0;
            if (auto* sh = tradeSkillHandler(L)) sh->tradeSkillLine(name, rank, maxRank);
            lua_pushstring(L, name.c_str());
            lua_pushnumber(L, rank);
            lua_pushnumber(L, maxRank);
            return 3;
        }},
                // DoTradeSkill(index [, repeat]) (0x5DAC00): never more than
                // the bags make; a run of this recipe already going takes the
                // new count without a second cast.
                {"DoTradeSkill", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: DoTradeSkill(index [, repeat])");
            auto* gh = getGameHandler(L);
            auto* sh = gh ? gh->getSpellHandler() : nullptr;
            int count = 1;
            if (lua_isnumber(L, 2)) count = static_cast<int>(std::lround(lua_tonumber(L, 2)));
            const auto* row = sh ? sh->tradeSkill().rowAt(i) : nullptr;
            // A header or no row: the stock client casts nothing. Said, so a
            // Create that does nothing can be told from one never pressed.
            if (!row || row->isHeader()) {
                LOG_WARNING("DoTradeSkill: no recipe at row ", i);
                return 0;
            }
            count = game::stock_trade_skill::Model::clampCount(count, row->numAvailable);
            gh->startCraftQueue(static_cast<uint32_t>(row->spellId), count);
            return 0;
        }},
                {"CloseTradeSkill", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->closeCraftingWindow();
            return 0;
        }},
                {"SelectTradeSkill", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: SelectTradeSkill(index)");
            if (auto* sh = tradeSkillHandler(L)) sh->tradeSkillMutable().select(i);
            return 0;
        }},
                {"GetTradeSkillSelectionIndex", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            lua_pushnumber(L, sh ? sh->tradeSkill().selectionIndex() : 0);
            return 1;
        }},
                {"GetFirstTradeSkill", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            lua_pushnumber(L, sh ? sh->tradeSkill().firstRecipe() : 0);
            return 1;
        }},
                // Seconds left on the recipe's cooldown, nil when none (0x5DA9A0).
                {"GetTradeSkillCooldown", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillCooldown(index)");
            auto* gh = getGameHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!p.recipe) return luaReturnNil(L);
            const float left = gh->getSpellCooldown(static_cast<uint32_t>(p.recipe->spellId));
            if (left <= 0.0f) return luaReturnNil(L);
            lua_pushnumber(L, left);
            return 1;
        }},
                // Pairs of name and "there" (0x5DE620): the spell focus (always
                // shown as there), the tools carried, the tool categories.
                {"GetTradeSkillTools", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillTools(index)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!sh || !p.recipe) return 0;
            const auto tools = sh->tradeSkillTools(static_cast<uint32_t>(p.recipe->spellId));
            for (const auto& [name, have] : tools) {
                lua_pushstring(L, name.c_str());
                if (have) lua_pushnumber(L, 1); else lua_pushnil(L);
            }
            return static_cast<int>(tools.size() * 2);
        }},
                // The item a recipe makes, or an enchant link for one that
                // makes nothing (0x5DBA70).
                {"GetTradeSkillItemLink", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillItemLink(index)");
            auto* gh = getGameHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!p.recipe || !p.spell) return 0;
            if (const uint32_t made = tradeSkillMadeItem(p)) {
                gh->ensureItemInfo(made);
                const auto* info = gh->getItemInfo(made);
                if (!info || !info->valid) return 0;
                lua_pushstring(L, game::buildItemLink(made, info->quality, info->name).c_str());
                return 1;
            }
            char buf[512];
            std::snprintf(buf, sizeof(buf), "|cffffd000|Henchant:%d|h[%s]|h|r", p.recipe->spellId,
                          p.spell->name.c_str());
            lua_pushstring(L, buf);
            return 1;
        }},
                // "|Henchant:id|h[Line: Recipe]|h" for a trade spell (0x5DB920).
                {"GetTradeSkillRecipeLink", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillRecipeLink(index)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            auto p = tradeSkillPick(gh, i);
            if (!sh || !p.recipe || !p.spell ||
                !(p.spell->attr & game::stock_trade_skill::kAttrTradeSpell))
                return 0;
            gh->loadSkillLineAbilityDbc();
            const auto& lines = gh->spellToSkillLineRef();
            auto lit = lines.find(static_cast<uint32_t>(p.recipe->spellId));
            const auto* info = lit != lines.end() ? sh->tradeSkillLineInfo(lit->second) : nullptr;
            char buf[512];
            if (info)
                std::snprintf(buf, sizeof(buf), "|cffffd000|Henchant:%d|h[%s: %s]|h|r", p.recipe->spellId,
                              info->name.c_str(), p.spell->name.c_str());
            else
                std::snprintf(buf, sizeof(buf), "|cffffd000|Henchant:%d|h[%s]|h|r", p.recipe->spellId,
                              p.spell->name.c_str());
            lua_pushstring(L, buf);
            return 1;
        }},
                {"GetTradeSkillReagentItemLink", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
                return luaL_error(L, "Usage: GetTradeReagentSkillItemLink(index, reagentIndex)");
            auto* gh = getGameHandler(L);
            const uint32_t item = reagentAtTradeSkillRow(gh, static_cast<int>(lua_tonumber(L, 1)),
                                                         static_cast<int>(lua_tonumber(L, 2)));
            if (!item) return luaReturnNil(L);
            gh->ensureItemInfo(item);
            const auto* info = gh->getItemInfo(item);
            if (!info || !info->valid) return luaReturnNil(L);
            lua_pushstring(L, game::buildItemLink(item, info->quality, info->name).c_str());
            return 1;
        }},
                // The player's own list as a "trade:" link (0x5DB060).
                {"GetTradeSkillListLink", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            const std::string link = sh ? sh->tradeSkillListLink() : std::string{};
            if (link.empty()) return luaReturnNil(L);
            lua_pushstring(L, link.c_str());
            return 1;
        }},
                // 1 and the owner's name for a linked list, else nil, nil.
                {"IsTradeSkillLinked", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh || !sh->tradeSkill().linked) { lua_pushnil(L); lua_pushnil(L); return 2; }
            lua_pushnumber(L, 1);
            const std::string name = gh->getCachedPlayerName(sh->tradeSkill().linkedGuid);
            if (name.empty()) lua_pushnil(L); else lua_pushstring(L, name.c_str());
            return 2;
        }},
                {"NoPlayTime",                [](lua_State* L) -> int { lua_pushboolean(L, 0); return 1; }},
                {"PartialPlayTime",           [](lua_State* L) -> int { lua_pushboolean(L, 0); return 1; }},
                {"GetBillingTimeRested",      [](lua_State* L) -> int { lua_pushnumber(L, 0); return 1; }},
                // The two dropdowns (0x5DAA70 / 0x5DBF10): the headers' names
                // in their order, and the slot names of what is listed.
                {"GetTradeSkillSubClasses", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            if (!sh) return 0;
            const auto& m = sh->tradeSkill();
            int n = 0;
            for (const auto& h : m.headers()) {
                std::string name;
                if (h.itemClass == -1 && h.itemSubClass == -1) {
                    if (const auto* info = sh->tradeSkillLineInfo(m.skillLine)) name = info->alternateVerb;
                } else {
                    name = m.headerName(h.itemClass, h.itemSubClass);
                }
                lua_pushstring(L, name.c_str());
                ++n;
            }
            return n;
        }},
                {"GetTradeSkillInvSlots", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            if (!sh) return 0;
            const uint32_t bits = sh->tradeSkill().listedSlotBits();
            int n = 0;
            for (int b = 0; b < game::stock_trade_skill::kNumSlotBits; ++b) {
                if (!(bits & (1u << b))) continue;
                const char* key = game::stock_trade_skill::slotBitKey(b);
                lua_getglobal(L, key);
                if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, key); }
                ++n;
            }
            return n;
        }},
                {"GetTradeSkillSubClassFilter", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillSubClassFilter(index)");
            auto* sh = tradeSkillHandler(L);
            if (!sh) return luaReturnNil(L);
            if (i - 1 >= static_cast<int>(sh->tradeSkill().headers().size()))
                return luaL_error(L, "Bad sub class in GetTradeSkillSubClassFilter");
            if (sh->tradeSkill().subClassFilterOn(i)) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                {"GetTradeSkillInvSlotFilter", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: GetTradeSkillInvSlotFilter(index)");
            auto* sh = tradeSkillHandler(L);
            if (!sh) return luaReturnNil(L);
            if (sh->tradeSkill().invSlotFilterOn(i)) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                {"SetTradeSkillSubClassFilter", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: SetTradeSkillSubClassFilter(index [, on\\off, exclusive])");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            const int i = static_cast<int>(lua_tonumber(L, 1));
            auto& m = sh->tradeSkillMutable();
            if (i - 1 >= 0) {
                if (i - 1 >= static_cast<int>(m.headers().size()))
                    return luaL_error(L, "Bad sub class in SetTradeSkillSubClassFilter");
                if (!lua_isnumber(L, 2))
                    return luaL_error(L, "Missing on//off parameter in SetTradeSkillSubClassFilter");
            }
            const bool on = lua_tonumber(L, 2) != 0;
            const bool exclusive = lua_isnumber(L, 3) && lua_tonumber(L, 3) != 0;
            m.setSubClassFilter(i, on, exclusive);
            gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                {"SetTradeSkillInvSlotFilter", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: SetTradeSkillInvSlotFilter(index [, on\\off, exclusive])");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            const int i = static_cast<int>(lua_tonumber(L, 1));
            auto& m = sh->tradeSkillMutable();
            if (i - 1 >= 0) {
                if (m.listedSlotBit(i - 1) < 0)
                    return luaL_error(L, "Bad inv slot in SetTradeSkillInvSlotFilter");
                if (!lua_isnumber(L, 2))
                    return luaL_error(L, "Missing on//off parameter in SetTradeSkillInvSlotFilter");
            }
            const bool on = lua_tonumber(L, 2) != 0;
            const bool exclusive = lua_isnumber(L, 3) && lua_tonumber(L, 3) != 0;
            m.setInvSlotFilter(i, on, exclusive);
            gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                // The search box: a name (TRADE_SKILL_FILTER_UPDATE on a
                // change, 0x5DCF60) or a level range (TRADE_SKILL_UPDATE).
                {"SetTradeSkillItemNameFilter", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            const char* text = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
            if (sh->tradeSkillMutable().setNameFilter(text ? text : ""))
                gh->fireAddonEvent("TRADE_SKILL_FILTER_UPDATE", {});
            return 0;
        }},
                {"GetTradeSkillItemNameFilter", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            if (!sh || sh->tradeSkill().nameFilter.empty()) return luaReturnNil(L);
            lua_pushstring(L, sh->tradeSkill().nameFilter.c_str());
            return 1;
        }},
                {"SetTradeSkillItemLevelFilter", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
                return luaL_error(L, "Usage: SetTradeSkillItemLevelFilter(minLevel, maxLevel)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            if (sh->tradeSkillMutable().setLevelFilter(static_cast<int>(lua_tonumber(L, 1)),
                                                       static_cast<int>(lua_tonumber(L, 2))))
                gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                {"GetTradeSkillItemLevelFilter", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            lua_pushnumber(L, sh ? sh->tradeSkill().minLevel : 0);
            lua_pushnumber(L, sh ? sh->tradeSkill().maxLevel : 0);
            return 2;
        }},
                // "Have Materials" and "Skill Ups": TRADE_SKILL_UPDATE on a
                // change (0x5DD3E0 / 0x5DD420).
                {"TradeSkillOnlyShowMakeable", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (gh && sh && sh->tradeSkillMutable().setOnlyMakeable(lua_toboolean(L, 1) != 0))
                gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                {"TradeSkillOnlyShowSkillUps", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (gh && sh && sh->tradeSkillMutable().setOnlySkillUps(lua_toboolean(L, 1) != 0))
                gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                // By row, 0 for every header (0x5DD460 / 0x5DD500).
                {"CollapseTradeSkillSubClass", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: CollapseTradeSkillSubClass(index)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            if (!sh->tradeSkillMutable().setExpanded(i, false))
                return luaL_error(L, "Bad sub class in CollapseTradeSkillSubClass");
            gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                {"ExpandTradeSkillSubClass", [](lua_State* L) -> int {
            const int i = tradeSkillIndexArg(L, 1, "Usage: ExpandTradeSkillSubClass(index)");
            auto* gh = getGameHandler(L);
            auto* sh = tradeSkillHandler(L);
            if (!gh || !sh) return 0;
            if (!sh->tradeSkillMutable().setExpanded(i, true))
                return luaL_error(L, "Bad skill line in ExpandTradeSkillSubClass");
            gh->fireAddonEvent("TRADE_SKILL_UPDATE", {});
            return 0;
        }},
                // What is left of the selected recipe's run, 1 when it is not
                // running (0x5DA270).
                {"GetTradeskillRepeatCount", [](lua_State* L) -> int {
            auto* sh = tradeSkillHandler(L);
            lua_pushnumber(L, sh ? sh->tradeSkill().repeatCount() : 1);
            return 1;
        }},
                {"StopTradeSkillRepeat", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->cancelCraftQueue();
            return 0;
        }},
                // ---- Trainer -------------------------------------------
                //
                // The client has parsed the trainer list all along - spell,
                // cost, required level and skill, and whether it is already
                // known - and fires TRAINER_SHOW when it arrives. None of it
                // had a way into the interface, so the panel opened blank at
                // every trainer in the game.
                {"GetNumTrainerServices", [](lua_State* L) -> int {
            // What the filters leave, not what the trainer sent.
            lua_pushnumber(L, static_cast<double>(
                shownTrainerServices(getGameHandler(L)).size()));
            return 1;
        }},
                // GetTrainerServiceInfo(i) → name, rank, category, expanded
                {"GetTrainerServiceInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh) return luaReturnNil(L);
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) return luaReturnNil(L);
            const auto& sp = *svc;
            std::string name = gh->getSpellName(sp.spellId);
            if (name.empty()) name = "Spell " + std::to_string(sp.spellId);
            lua_pushstring(L, name.c_str());
            // The rank, from the same place the name came from. The trainer
            // list on the wire carries neither - both are looked up by spell id
            // - so "this list does not carry it" was true of the packet and
            // not of the client, which has had getSpellRank all along.
            //
            // It is what tells two rows apart. The panel writes it in
            // parentheses after the name, so a trainer offering Fireball at
            // three ranks listed Fireball three times, identically. An empty
            // string still hides the line, which is right for a spell that has
            // no rank.
            lua_pushstring(L, gh->getSpellRank(sp.spellId).c_str());   // rank
            // The three words the panel colours by: green, grey, and already
            // trained.
            //
            // "used" wins the moment the spell is known, not only when the
            // trainer packet said so. The packet is a snapshot from when the
            // list was opened; learning a service adds it to the known set and
            // fires TRAINER_UPDATE, but the cached state stayed at 0. So the
            // panel redrew the just-learned skill as still available - green,
            // trainable, its cost still shown - and only a close and reopen,
            // which fetches a fresh list, corrected it. Asking the known set
            // makes the live redraw right.
            lua_pushstring(L, trainerServiceCategory(gh, sp));
            lua_pushboolean(L, 1);   // expanded: the list here is flat
            return 4;
        }},
                // Three costs: coin, talent points, and a profession slot.
                // The trainer reads the third bare - `if ( cpCost2 > 0 )` - so
                // one value made selecting anything a trainer offers an error.
                //
                // The second and third were zero on the reasoning that nothing
                // in the trainer list says which service is a profession. The
                // list says exactly that and has all along: profDialog and
                // profButton are the packet's two point costs, and AzerothCore
                // fills the second with `primaryProfessionFirstRank ? 1 : 0`
                // - the one thing it is for. Parsed since the parser was
                // written, stored, and read by nothing.
                //
                // What it buys: learning a first profession rank now raises
                // CONFIRM_PROFESSION, and the Train button greys when both
                // slots are already spent. Both come free from the frame once
                // the number is real, because UnitCharacterPoints already
                // answers the free-slot count it is compared against.
                {"GetTrainerServiceCost", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            lua_pushnumber(L, svc ? svc->spellCost  : 0);
            lua_pushnumber(L, svc ? svc->profDialog : 0);
            lua_pushnumber(L, svc ? svc->profButton : 0);
            return 3;
        }},
                {"GetTrainerServiceLevelReq", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                lua_pushnumber(L, 0);
                return 1;
            }
            lua_pushnumber(L, svc->reqLevel);
            return 1;
        }},
                // GetTrainerServiceSkillReq(i) → skill name, required value
                {"GetTrainerServiceSkillReq", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                return luaReturnNil(L);
            }
            const auto& sp = *svc;
            if (sp.reqSkill == 0) return luaReturnNil(L);
            const std::string skill = gh->getSkillLineName(sp.reqSkill);
            lua_pushstring(L, skill.empty() ? "Skill" : skill.c_str());
            lua_pushnumber(L, sp.reqSkillValue);
            // Whether the player already meets it. The trainer window picks
            // between TRAINER_REQ_SKILL_RANK and its _RED twin on this, so
            // leaving it nil painted every requirement red - including the
            // ones already satisfied, next to a spell the player could train.
            const auto& skills = gh->getPlayerSkills();
            const auto it = skills.find(sp.reqSkill);
            const bool met = it != skills.end() &&
                             it->second.effectiveValue() >= sp.reqSkillValue;
            lua_pushboolean(L, met ? 1 : 0);
            return 3;
        }},
                {"GetTrainerServiceIcon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                return luaReturnNil(L);
            }
            const std::string icon = gh->getSpellIconPath(svc->spellId);
            lua_pushstring(L, icon.empty()
                ? "Interface\\Icons\\INV_Misc_QuestionMark" : icon.c_str());
            return 1;
        }},
                {"GetTrainerServiceDescription", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                return luaReturnNil(L);
            }
            const uint32_t id = svc->spellId;
            const std::string desc =
                gh->formatSpellDescription(id, gh->getSpellDescription(id));
            lua_pushstring(L, desc.c_str());
            return 1;
        }},
                {"GetTrainerServiceSkillLine", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                return luaReturnNil(L);
            }
            const std::string skill = gh->getSkillLineName(svc->reqSkill);
            lua_pushstring(L, skill.empty() ? "" : skill.c_str());
            return 1;
        }},
                // The prerequisite chain, which this list carries as up to
                // three spell ids.
                {"GetTrainerServiceNumAbilityReq", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                lua_pushnumber(L, 0);
                return 1;
            }
            const auto& sp = *svc;
            int n = 0;
            if (sp.chainNode1) ++n;
            if (sp.chainNode2) ++n;
            if (sp.chainNode3) ++n;
            lua_pushnumber(L, n);
            return 1;
        }},
                // GetTrainerServiceAbilityReq(i, n) → name, hasIt
                {"GetTrainerServiceAbilityReq", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            const int n = static_cast<int>(luaL_optnumber(L, 2, 1));
            const auto* svc = shownTrainerService(gh, i);
            if (!svc) {
                return luaReturnNil(L);
            }
            const auto& sp = *svc;
            const uint32_t chain[3] = {sp.chainNode1, sp.chainNode2, sp.chainNode3};
            if (n < 1 || n > 3 || chain[n - 1] == 0) return luaReturnNil(L);
            const uint32_t req = chain[n - 1];
            std::string name = gh->getSpellName(req);
            if (name.empty()) name = "Ability";
            lua_pushstring(L, name.c_str());
            lua_pushboolean(L, gh->getKnownSpells().count(req) ? 1 : 0);
            return 2;
        }},
                // GetTrainerServiceStepReq(i) → step, met.
                //
                // Nil, not zero. The trainer window does `if ( step ) then`
                // and zero is truthy in Lua, so answering 0 claimed every
                // service had a step requirement - and with `met` missing it
                // took the red branch, printing a bogus unmet requirement of
                // "0" beside every spell on the list.
                //
                // No step requirement is tracked here, and saying so is both
                // true and what removes the line.
                {"GetTrainerServiceStepReq", [](lua_State* L) -> int {
            return luaReturnNil(L);
        }},
                {"GetTrainerServiceItemLink", [](lua_State* L) -> int {
            return luaReturnNil(L);
        }},
                {"GetTrainerGreetingText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const std::string& g = gh ? gh->getTrainerSpells().greeting
                                      : std::string();
            lua_pushstring(L, g.c_str());
            return 1;
        }},
                {"IsTradeskillTrainer", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->getTrainerSpells().trainerType == 2);
            return 1;
        }},
                {"BuyTrainerService", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
            // Through the filtered list like every other index, and this is
            // the one where getting it wrong buys a different spell than the
            // row that was clicked.
            const auto* svc = shownTrainerService(gh, i);
            if (svc) gh->trainSpell(svc->spellId);
            return 0;
        }},
                {"CloseTrainer", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->closeTrainer();
            return 0;
        }},
                // Selection and the type filter are the panel's own state; the
                // client has no opinion about either, so they are kept here
                // rather than pretended away - the panel reads back what it set.
                {"SelectTrainerService", [](lua_State* L) -> int {
            trainerSelection() = static_cast<int>(luaL_optnumber(L, 1, 0));
            return 0;
        }},
                {"GetTrainerSelectionIndex", [](lua_State* L) -> int {
            lua_pushnumber(L, trainerSelection());
            return 1;
        }},
                {"SetTrainerServiceTypeFilter", [](lua_State* L) -> int {
            const char* which = luaL_optstring(L, 1, "");
            // A number is read as a number. The dropdown clears a category
            // with SetTrainerServiceTypeFilter(value, 0), and lua_toboolean
            // answers true for 0 - only nil and false are false in Lua - so
            // every attempt to hide one set it instead, and no filter on this
            // panel had ever turned anything off.
            const bool show = lua_isnumber(L, 2) ? (lua_tonumber(L, 2) != 0)
                                                 : (lua_toboolean(L, 2) != 0);
            auto& filters = trainerFilters();
            const auto it = filters.find(which);
            // Read back the same way GetTrainerServiceTypeFilter answers, so
            // "no change" here means the dropdown's tick would not move either.
            const bool was = (it == filters.end()) || it->second;
            filters[which] = show;
            if (was == show) return 0;

            // Changing the filter has to redraw the list, and nothing was
            // asking it to.
            //
            // The filter dropdown's OnClick sets the filter and then resets
            // the scroll bar to zero, which reaches ClassTrainerFrame_Update
            // through the scroll frame's OnVerticalScroll - but only when the
            // value moves. Ticking a category while the list is already at the
            // top moves nothing, so the rows the tick had just excluded stayed
            // on screen until something else redrew them, which is the state a
            // freshly opened trainer is always in.
            //
            // Raised as TRAINER_UPDATE rather than by calling the panel:
            // ClassTrainerFrame_OnLoad registers for it and answers by
            // reselecting and updating, so this is the redraw path the panel
            // already has, and it works for whichever interface is drawing.
            if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("TRAINER_UPDATE", {});
            return 0;
        }},
                // The search box this client's trainer panel has and 3.3.5's
                // does not. Named alongside the type filter it works with, and
                // it redraws the same way - see SetTrainerServiceTypeFilter for
                // why the panel has to be told rather than left to notice.
                {"SetTrainerServiceSearch", [](lua_State* L) -> int {
            std::string text = foldedForSearch(luaL_optstring(L, 1, ""));
            if (text == trainerSearch()) return 0;
            trainerSearch() = text;
            if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("TRAINER_UPDATE", {});
            return 0;
        }},
                {"GetTrainerServiceSearch", [](lua_State* L) -> int {
            lua_pushstring(L, trainerSearch().c_str());
            return 1;
        }},
                {"GetTrainerServiceTypeFilter", [](lua_State* L) -> int {
            const char* which = luaL_optstring(L, 1, "");
            auto it = trainerFilters().find(which);
            // The three the panel asks about are always set; a name it does
            // not use reads as showing rather than as hidden.
            lua_pushboolean(L, it == trainerFilters().end() ? 1 : (it->second ? 1 : 0));
            return 1;
        }},
                // Inert, and this one really is - unlike the three other
                // Collapse/Expand pairs, which all turned out to have their
                // grouping sitting in data the client already held.
                //
                // Blizzard's name says the heading is a skill line, and the
                // only skill line in the packet is ReqSkillLine. Across
                // AzerothCore's whole npc_trainer table - 880 trainers, 4934
                // rows - not one trainer has more than a single distinct
                // non-zero value, and 816 have none at all. So grouping by it
                // yields one heading over the entire list, or none, which is
                // strictly worse than the flat list it would replace.
                //
                // Checked against the table rather than argued from the packet,
                // because the packet plainly *could* carry several and the
                // question is whether any server sends them.
                {"CollapseTrainerSkillLine", [](lua_State* L) -> int { (void)L; return 0; }},
                {"ExpandTrainerSkillLine",   [](lua_State* L) -> int { (void)L; return 0; }},
                // GetGlyphLink(socket [, talentGroup]) → hyperlink, or nil
                {"GetGlyphLink", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            const int spec  = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1 || index > game::GameHandler::MAX_GLYPH_SLOTS) {
                lua_pushnil(L);
                return 1;
            }
            const auto& glyphs = (spec >= 1 && spec <= 2)
                ? gh->getGlyphs(static_cast<uint8_t>(spec - 1)) : gh->getGlyphs();
            const uint16_t glyphId = glyphs[index - 1];
            if (glyphId == 0) { lua_pushnil(L); return 1; }
            // The name is the glyph's spell's: glyphId is a GlyphProperties
            // id, which as a spell id named some unrelated spell.
            gh->ensureGlyphPropertiesLoaded();
            std::string name = gh->getSpellName(gh->getGlyphSpellId(glyphId));
            if (name.empty()) name = "Glyph";
            const std::string link = "|cff66bbff|Hglyph:" + std::to_string(index) +
                                     ":" + std::to_string(glyphId) + "|h[" + name +
                                     "]|h|r";
            lua_pushstring(L, link.c_str());
            return 1;
        }},
                // GlyphMatchesSocket(socket) → whether the glyph being placed
                // - one used and waiting (USE_GLYPH), or one on the cursor -
                // fits that socket: its GlyphProperties TypeFlags equal the
                // socket's GlyphSlot TypeFlags, the test AzerothCore's
                // Spell::EffectApplyGlyph makes before applying it. Also false
                // for a locked socket, which the server refuses outright.
                {"GlyphMatchesSocket", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int socket = static_cast<int>(luaL_optnumber(L, 1, 0));
            bool match = false;
            if (gh && socket >= 1 && socket <= game::GameHandler::MAX_GLYPH_SLOTS &&
                gh->glyphSocketEnabled(socket)) {
                uint32_t itemId = gh->getPendingGlyphItemId();
                if (itemId == 0) itemId = cursorItemId();
                const uint32_t props = itemId ? gh->glyphPropertiesForItem(itemId) : 0;
                if (props != 0) {
                    gh->ensureGlyphPropertiesLoaded();
                    const int32_t glyphType = gh->getGlyphTypeFlags(props);
                    match = glyphType >= 0 && glyphType == gh->glyphSocketTypeFlags(socket);
                }
            }
            lua_pushboolean(L, match ? 1 : 0);
            return 1;
        }},
                
                // SetCursor(art) - the pointer's own image, which this client
                // does not change.
                {"SetCursor", [](lua_State* L) -> int { (void)L; return 0; }},
                // How long until the daily quests reset, which the quest log
                // prints in a tooltip through SecondsToTime. A nil there is
                // arithmetic on nothing rather than a blank line.
                //
                // The server's own figure, asked for once on entering the
                // world: SMSG_QUERY_TIME_RESPONSE's second word is
                // GetNextDailyQuestsResetTime() minus now. This used to count
                // to the next local midnight, under a comment saying the real
                // reset was a realm setting nothing sends - it is sent, in the
                // reply to a packet this client already knew how to ask for,
                // and it was parsed and dropped.
                //
                // Midnight remains the answer until the reply lands, and for
                // any realm that does not send one. Zero would be wrong in a
                // way a wrong number is not: SecondsToTime renders it as "the
                // reset is now", every time the tooltip is opened.
                {"GetQuestResetTime", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) {
                if (const uint32_t left = gh->getSecondsUntilDailyReset(); left > 0) {
                    lua_pushnumber(L, left);
                    return 1;
                }
            }
            const time_t now = time(nullptr);
            const std::tm t = core::localTime(now);
            const int secondsIntoDay = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
            lua_pushnumber(L, 86400 - secondsIntoDay);
            return 1;
        }},
                // The cursor changes at a vendor: the hand that sells, and the
                // hammer that repairs. This client draws its own cursor and
                // has no art switched by name, so these do nothing - which is
                // what they did before, silently, as unknown globals.
                //
                // Bound rather than left to the fallback because the fallback
                // is what makes a typo look like a working call, and because a
                // window that calls nothing undefined is the measure of
                // whether it can be handed over.
                {"ShowMerchantSellCursor", [](lua_State* L) -> int { (void)L; return 0; }},
                // The merchant's repair-an-item button toggles between these
                // two and reads the state back through InRepairMode. All three
                // were stubs, so the button never latched and the per-item
                // repair the bags and the paperdoll gate on it was dead.
                {"ShowRepairCursor",       [](lua_State* L) -> int {
            (void)L; repairCursorUp() = true; return 0; }},
                {"HideRepairCursor",       [](lua_State* L) -> int {
            (void)L; repairCursorUp() = false; return 0; }},
                // GetNumCompletedAchievements() → total, completed.
                //
                // Two values, and the total comes first. This returned one -
                // the *earned* count in the total's place - so the summary bar
                // was scaled to the number earned, and `completed` was nil.
                // AchievementFrameSummaryCategoriesStatusBar_Update then does
                // SetText(completed.."/"..total), and concatenating nil raises,
                // so opening the achievements panel took its own update down.
                {"GetNumCompletedAchievements", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
            // What the achievement tab lists: every category under
            // Achievements, for this character's side - not statistics, which
            // nothing completes, and not the other faction's twins. The whole
            // DBC (1817 rows, a third of them statistics or the other side's)
            // made the summary bar read a fraction of what it should.
            gh->ensureAchievementCategoriesLoaded();
            const auto& earnedSet = gh->getEarnedAchievements();
            size_t total = 0, earned = 0;
            for (uint32_t cat : gh->getAchievementCategoryOrder()) {
                for (uint32_t a : gh->getCategoryAchievements(cat)) {
                    ++total;
                    if (earnedSet.count(a)) ++earned;
                }
            }
            if (total == 0) earned = earnedSet.size();
            // Never fewer total than earned: if the DBC did not load, saying
            // "3 of 0" is worse than saying the total is what we have seen.
            lua_pushnumber(L, static_cast<lua_Number>(total ? total : earned));
            lua_pushnumber(L, static_cast<lua_Number>(earned));
            return 2;
        }},
                // GetCategoryList() → the achievement category ids, as a table.
                {"GetCategoryList", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_newtable(L);
            if (!gh) return 1;
            gh->ensureAchievementCategoriesLoaded();
            int n = 0;
            for (uint32_t id : gh->getAchievementCategoryOrder()) {
                lua_pushnumber(L, ++n);
                lua_pushnumber(L, id);
                lua_settable(L, -3);
            }
            return 1;
        }},
                // GetStatisticsCategoryList() → the same for the other tab.
                //
                // The Statistics tab reads this out of a table built when the
                // panel loads - STAT_FUNCTIONS.categoryAccessor - and then calls
                // it: `local cats = achievementFunctions.categoryAccessor()`.
                // Absent, that is a call on a nil field, so opening Statistics
                // raised rather than showing an empty tab. The list it wants is
                // the half GetCategoryList used to return along with everything
                // else, which put rows nothing can complete under Achievements.
                {"GetStatisticsCategoryList", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_newtable(L);
            if (!gh) return 1;
            gh->ensureAchievementCategoriesLoaded();
            int n = 0;
            for (uint32_t id : gh->getStatisticCategoryOrder()) {
                lua_pushnumber(L, ++n);
                lua_pushnumber(L, id);
                lua_settable(L, -3);
            }
            return 1;
        }},
                // GetCategoryInfo(id) → name, parentCategoryID, flags
                {"GetCategoryInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh) return luaReturnNil(L);
            gh->ensureAchievementCategoriesLoaded();
            const auto* info = gh->getAchievementCategoryInfo(id);
            if (!info) return luaReturnNil(L);
            lua_pushstring(L, info->name.c_str());
            lua_pushnumber(L, info->parentId);
            lua_pushnumber(L, 0);
            return 3;
        }},
                // GetCategoryNumAchievements(id) → total, completed, incomplete
                {"GetCategoryNumAchievements", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = lua_isnumber(L, 1) ? static_cast<uint32_t>(lua_tonumber(L, 1)) : 0u;
            if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
            gh->ensureAchievementCategoriesLoaded();
            const auto& earned = gh->getEarnedAchievements();
            // "summary" (the comparison summary id) means every achievement
            // category together; luaL_optnumber used to throw on the string.
            if (lua_type(L, 1) == LUA_TSTRING && !lua_isnumber(L, 1)) {
                size_t total = 0, doneAll = 0;
                for (uint32_t cat : gh->getAchievementCategoryOrder()) {
                    for (uint32_t a : gh->getCategoryAchievements(cat)) {
                        ++total;
                        if (earned.count(a)) ++doneAll;
                    }
                }
                lua_pushnumber(L, static_cast<lua_Number>(total));
                lua_pushnumber(L, static_cast<lua_Number>(doneAll));
                lua_pushnumber(L, static_cast<lua_Number>(total - doneAll));
                return 3;
            }
            const auto& ids = gh->getCategoryAchievements(id);
            uint32_t done = 0;
            for (uint32_t a : ids) if (earned.count(a)) ++done;
            lua_pushnumber(L, static_cast<lua_Number>(ids.size()));
            lua_pushnumber(L, done);
            lua_pushnumber(L, static_cast<lua_Number>(ids.size() - done));
            return 3;
        }},
                {"GetAchievementCategory", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh) return luaReturnNil(L);
            gh->ensureAchievementCategoriesLoaded();
            lua_pushnumber(L, gh->getAchievementCategory(id));
            return 1;
        }},
                {"GetAchievementNumCriteria", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            gh->ensureAchievementCriteriaLoaded();
            lua_pushnumber(L, static_cast<lua_Number>(gh->getAchievementCriteria(id).size()));
            return 1;
        }},
                // GetAchievementCriteriaInfo(achievementID, index) →
                //   description, criteriaType, completed, quantity,
                //   reqQuantity, charName, flags, assetID, quantityString,
                //   criteriaID
                {"GetAchievementCriteriaInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id  = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            const int  idx = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || idx < 1) return luaReturnNil(L);
            gh->ensureAchievementCriteriaLoaded();
            const auto& list = gh->getAchievementCriteria(id);
            if (idx > static_cast<int>(list.size())) return luaReturnNil(L);
            const auto& c = list[static_cast<size_t>(idx) - 1];
            // Per-criterion progress is tracked after all: SMSG_ALL_ACHIEVEMENT_DATA
            // carries a counter per criterion id, and this reported all-or-none
            // from the achievement's earned flag while that sat unread beside
            // it. A half-finished achievement now shows which of its criteria
            // are done and how far the rest have got.
            const auto& progress = gh->getCriteriaProgress();
            const auto pit = progress.find(c.id);
            const uint32_t have = (pit != progress.end())
                ? static_cast<uint32_t>(pit->second) : 0u;
            const bool earned = gh->getEarnedAchievements().count(id) > 0;
            // Earned wins over the counter: the server stops counting once an
            // achievement is complete, so a finished one can carry a criterion
            // still short of its quantity.
            const bool done = earned || (c.quantity > 0 && have >= c.quantity);
            const uint32_t shown = earned ? c.quantity : have;
            lua_pushstring(L, c.description.c_str());          // 1: description
            lua_pushnumber(L, c.type);                         // 2: criteriaType
            lua_pushboolean(L, done ? 1 : 0);                  // 3: completed
            lua_pushnumber(L, shown);                          // 4: quantity
            lua_pushnumber(L, c.quantity);                     // 5: reqQuantity
            lua_pushnil(L);                                    // 6: charName
            // The panel reads bit one of this to decide between a progress
            // bar and a tick, so zero drew every counted criterion as a tick.
            lua_pushnumber(L, c.flags);                        // 7: flags
            lua_pushnumber(L, c.assetId);                      // 8: assetID
            lua_pushstring(L, std::to_string(shown).c_str());  // 9: quantityString
            lua_pushnumber(L, c.id);                           // 10: criteriaID
            return 10;
        }},
                // GetLatestCompletedAchievements() → the most recent earned
                // ids, newest first. The summary page fills its top rows from
                // these. The earn date is a WoW PackedTime - year in the low
                // sixteen bits, then day, then month - so it does not sort as
                // a plain integer and has to be unpacked first.
                {"GetLatestCompletedAchievements", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            std::vector<std::pair<uint32_t, uint32_t>> byDate;  // sortKey, id
            for (uint32_t id : gh->getEarnedAchievements()) {
                const game::WowDate d =
                    game::unpackWowPackedTime(gh->getAchievementDate(id));
                byDate.emplace_back(
                    (static_cast<uint32_t>(d.yearSince2000) << 9) |
                    (static_cast<uint32_t>(d.month) << 5) |
                    static_cast<uint32_t>(d.day), id);
            }
            std::sort(byDate.begin(), byDate.end(),
                      [](const auto& a, const auto& b) {
                          if (a.first != b.first) return a.first > b.first;
                          return a.second > b.second;  // stable, and a total order
                      });
            const size_t n = std::min<size_t>(byDate.size(), 5);
            for (size_t i = 0; i < n; ++i) lua_pushnumber(L, byDate[i].second);
            return static_cast<int>(n);
        }},
                // GetAchievementInfoFromCriteria(criteriaID) → the achievement
                // that criterion belongs to, in GetAchievementInfo's shape.
                {"GetAchievementInfoFromCriteria", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto critId = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || critId == 0) return luaReturnNil(L);
            gh->ensureAchievementCriteriaLoaded();
            // Nothing indexes criteria by their own id - the panel asks this
            // once per link clicked, not per frame, so the walk is cheaper
            // than a second map kept in step with the first.
            for (const auto& [achId, list] : gh->getAchievementCriteriaMap()) {
                for (const auto& c : list) {
                    if (c.id != critId) continue;
                    // Ten, the same shape as GetAchievementInfo: the panel
                    // unpacks it into the same ten names and puts the last one
                    // on a button as its texture.
                    const bool done = gh->getEarnedAchievements().count(achId) > 0;
                    const uint32_t date = gh->getAchievementDate(achId);
                    lua_pushnumber(L, achId);                                    // 1: id
                    lua_pushstring(L, gh->getAchievementName(achId).c_str());    // 2: name
                    lua_pushnumber(L, gh->getAchievementPoints(achId));          // 3: points
                    lua_pushboolean(L, done ? 1 : 0);                            // 4: completed
                    const game::WowDate on = game::unpackWowPackedTime(date);
                    lua_pushnumber(L, done ? on.month : 0);                      // 5: month
                    lua_pushnumber(L, done ? on.day : 0);                        // 6: day
                    // SHORTDATE formats the year "%02d", so it wants the short
                    // form rather than a four-digit one.
                    lua_pushnumber(L, done ? on.yearSince2000 : 0);              // 7: year
                    lua_pushstring(L, gh->getAchievementDescription(achId).c_str()); // 8
                    lua_pushnumber(L, gh->getAchievementFlags(achId));            // 9: flags
                    // A path, not the id. The panel puts this straight on a
                    // button as its texture, and a number is not a texture in
                    // 3.3.5 - the icon id was already cached, and the resolver
                    // that turns one into a path was already here for the
                    // spellbook's tabs.
                    lua_pushstring(L, achievementIconPath(gh, achId).c_str());   // 10: icon
                    return 10;
                }
            }
            return luaReturnNil(L);
        }},
                // The two ends of an achievement chain, off Achievement.dbc's
                // Supercedes column. Answering nil for both made every chained
                // achievement look like a single one: the panel shows the
                // expand arrow only when there is a previous step
                // (blizzard_achievementui.lua:828), and builds the list of
                // finished steps by walking GetPreviousAchievement until it
                // stops. So "Level 20" showed nothing of the "Level 10" behind
                // it, and neither did any cooking, fishing or battleground
                // series.
                {"GetPreviousAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return luaReturnNil(L);
            gh->ensureAchievementCategoriesLoaded();
            const uint32_t prev = gh->getAchievementSupercedes(id);
            if (prev == 0) return luaReturnNil(L);
            lua_pushnumber(L, prev);
            return 1;
        }},
                // ...and forwards, with whether that next step is already done
                // - AchievementFrameSummaryAchievement_OnClick walks
                // `newID, completed = GetNextAchievement(nextID)` to find the
                // furthest one earned, so the second value is what stops it.
                {"GetNextAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return luaReturnNil(L);
            gh->ensureAchievementCategoriesLoaded();
            const uint32_t next = gh->getAchievementSupercededBy(id);
            if (next == 0) return luaReturnNil(L);
            lua_pushnumber(L, next);
            lua_pushboolean(L, gh->getEarnedAchievements().count(next) > 0 ? 1 : 0);
            return 2;
        }},
                // GetStatistic(id) → the statistic's value, as a string.
                // Statistics count criteria progress, and only whether an
                // achievement was earned is tracked here, so this reports the
                // dash WoW itself shows for a statistic with no value.
                // GetStatistic(achievementId) → the counter, as a string.
                //
                // A statistic is an achievement with one criterion and no
                // quantity to reach, so its value is simply that criterion's
                // counter - the same counter GetAchievementCriteriaInfo reads
                // for a half-finished achievement, which was already tracked
                // while this answered "--" for every row. "--" is still the
                // answer when nothing has been counted, because that is what
                // the real client shows for a statistic never triggered.
                {"GetStatistic", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) { lua_pushstring(L, "--"); return 1; }
            gh->ensureAchievementCriteriaLoaded();
            const auto& list = gh->getAchievementCriteria(id);
            if (list.empty()) { lua_pushstring(L, "--"); return 1; }
            // A statistic can count over several criteria - one per
            // battleground, one per dungeon boss - and its flags say how they
            // combine (DBCEnums.h): ACHIEVEMENT_FLAG_SUMM 0x08 adds them,
            // _MAX_USED 0x10 takes the largest. Otherwise it is the one that
            // has a value. Reading criterion one alone showed "--" for every
            // statistic whose first criterion had not moved.
            const auto& progress = gh->getCriteriaProgress();
            const uint32_t flags = gh->getAchievementFlags(id);
            bool any = false;
            uint64_t sum = 0, best = 0, first = 0;
            for (const auto& c : list) {
                const auto pit = progress.find(c.id);
                if (pit == progress.end()) continue;
                const auto v = static_cast<uint64_t>(pit->second);
                if (!any) first = v;
                any = true;
                sum += v;
                best = std::max(best, v);
            }
            if (!any) { lua_pushstring(L, "--"); return 1; }
            const uint64_t value = (flags & 0x08u) ? sum : (flags & 0x10u) ? best : first;
            lua_pushstring(L, std::to_string(value).c_str());
            return 1;
        }},
                // The comparison tab, which used to say the server is never
                // asked for another player's achievements. It is: inspecting a
                // player on Wrath sends CMSG_QUERY_INSPECT_ACHIEVEMENTS beside
                // the inspect itself, and the reply has been parsed into a set
                // per guid the whole time. What was missing was naming whose
                // set to read.
                {"SetAchievementComparisonUnit", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "");
            if (!gh || !uid || !*uid) return 0;
            std::string unit(uid);
            toLowerInPlace(unit);
            gh->setAchievementComparisonGuid(resolveUnitGuid(gh, unit));
            return 0;
        }},
                {"ClearAchievementComparisonUnit", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->setAchievementComparisonGuid(0);
            return 0;
        }},
                // GetAchievementComparisonInfo(id) → completed, month, day, year.
                //
                // Nil when nobody is being compared, which is what the tab reads
                // as "no data yet" - distinct from a false that would claim the
                // other player has not earned it.
                {"GetAchievementComparisonInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return luaReturnNil(L);
            const auto* set = gh->getInspectedPlayerAchievements(
                gh->getAchievementComparisonGuid());
            if (!set) return luaReturnNil(L);
            const auto it = set->find(id);
            if (it == set->end()) {
                lua_pushboolean(L, 0);
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                return 4;
            }
            const game::WowDate on = game::unpackWowPackedTime(it->second);
            lua_pushboolean(L, 1);
            lua_pushnumber(L, on.month);
            lua_pushnumber(L, on.day);
            lua_pushnumber(L, on.yearSince2000);
            return 4;
        }},
                // Their statistics, which the reply does carry counters for and
                // this client walks past to reach the end of the packet. Left
                // as the dash until those are kept: a zero would read as a
                // player who has never done the thing.
                {"GetComparisonStatistic",         [](lua_State* L) -> int { lua_pushstring(L, "--"); return 1; }},
                {"GetComparisonAchievementPoints", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            const auto* set = gh->getInspectedPlayerAchievements(
                gh->getAchievementComparisonGuid());
            if (!set) { lua_pushnumber(L, 0); return 1; }
            gh->ensureAchievementCategoriesLoaded();
            uint32_t total = 0;
            for (const auto& [id, when] : *set) {
                (void)when;
                total += gh->getAchievementPoints(id);
            }
            lua_pushnumber(L, total);
            return 1;
        }},
                {"GetTotalAchievementPoints", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getTotalAchievementPoints() : 0);
            return 1;
        }},
                {"GetAchievementLink", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return luaReturnNil(L);
            gh->ensureAchievementNamesLoaded();
            const std::string& name = gh->getAchievementName(id);
            if (name.empty()) return luaReturnNil(L);
            std::string link = "|cffffff00|Hachievement:" + std::to_string(id) +
                               ":0:0:0:0:0:0:0:0:0|h[" + name + "]|h|r";
            lua_pushstring(L, link.c_str());
            return 1;
        }},
                // ---- Achievement tracking (client-side, like the quest tracker)
                {"IsTrackedAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            lua_pushboolean(L, gh && gh->getTrackedAchievements().count(id) ? 1 : 0);
            return 1;
        }},
                {"AddTrackedAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (gh && id) { gh->setAchievementTracked(id, true); gh->fireAddonEvent("TRACKED_ACHIEVEMENT_UPDATE", {std::to_string(id)}); }
            return 0;
        }},
                {"RemoveTrackedAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (gh && id) { gh->setAchievementTracked(id, false); gh->fireAddonEvent("TRACKED_ACHIEVEMENT_UPDATE", {std::to_string(id)}); }
            return 0;
        }},
                {"GetNumTrackedAchievements", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<lua_Number>(gh->getTrackedAchievements().size()) : 0);
            return 1;
        }},
                {"GetAchievementInfo", [](lua_State* L) -> int {
            // GetAchievementInfo(id) → id, name, points, completed, month, day, year, description, flags, icon, rewardText, isGuildAch
            auto* gh = getGameHandler(L);
            uint32_t id = static_cast<uint32_t>(luaL_checknumber(L, 1));
            if (!gh) { return luaReturnNil(L); }
            // The other form the panel uses: GetAchievementInfo(category,
            // index) walks a category's achievements by position. Only the
            // by-id form existed, so every row the achievements panel asked
            // for was read as an achievement id and answered with the wrong
            // achievement, or with nothing.
            if (!lua_isnoneornil(L, 2)) {
                const int idx = static_cast<int>(luaL_optnumber(L, 2, 0));
                gh->ensureAchievementCategoriesLoaded();
                const auto& ids = gh->getCategoryAchievements(id);
                if (idx < 1 || idx > static_cast<int>(ids.size())) return luaReturnNil(L);
                id = ids[static_cast<size_t>(idx) - 1];
            }
            const std::string& name = gh->getAchievementName(id);
            if (name.empty()) { return luaReturnNil(L); }
            bool completed = gh->getEarnedAchievements().count(id) > 0;
            uint32_t date = gh->getAchievementDate(id);
            uint32_t points = gh->getAchievementPoints(id);
            const std::string& desc = gh->getAchievementDescription(id);
            const game::WowDate earned = game::unpackWowPackedTime(date);
            int month = completed ? earned.month : 0;
            int day   = completed ? earned.day : 0;
            // Short form: SHORTDATE is "%2$d/%1$02d/%3$02d".
            int year  = completed ? earned.yearSince2000 : 0;
            lua_pushnumber(L, id);                 // 1: id
            lua_pushstring(L, name.c_str());       // 2: name
            lua_pushnumber(L, points);             // 3: points
            lua_pushboolean(L, completed ? 1 : 0); // 4: completed
            lua_pushnumber(L, month);              // 5: month
            lua_pushnumber(L, day);                // 6: day
            lua_pushnumber(L, year);               // 7: year
            lua_pushstring(L, desc.c_str());       // 8: description
            lua_pushnumber(L, gh->getAchievementFlags(id)); // 9: flags
            lua_pushstring(L, achievementIconPath(gh, id).c_str()); // 10: icon
            // 11: rewardText - Achievement.dbc's Reward column, which the
            // panel prints under the achievement ("Reward: Black War Bear").
            lua_pushstring(L, gh->getAchievementRewardText(id).c_str());
            lua_pushboolean(L, 0);                 // 12: isGuildAchievement
            return 12;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
