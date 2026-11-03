// GameError routing and GlobalStrings formatting (game/game_error_table.hpp,
// game/global_strings.hpp). The kinds are Wow.exe 12340's GameError table at
// 0xAC8AF0, which CGGameUI::DisplayError (0x5216F0) reads to decide where a
// message goes: 0 a system chat line, 1 the yellow info text, 2 the red error.
#include <catch_amalgamated.hpp>

#include "game/game_error_table.hpp"
#include "game/global_strings.hpp"

using namespace wowee::game;

TEST_CASE("the GameError table has the stock row count and order") {
    STATIC_REQUIRE(sizeof(kGameErrorTable) / sizeof(kGameErrorTable[0]) == 730);
    CHECK(std::string_view(kGameErrorTable[0].key) == "ERR_INV_FULL");
    CHECK(std::string_view(kGameErrorTable[0x92].key) == "ERR_QUEST_ACCEPTED_S");
    CHECK(std::string_view(kGameErrorTable[0x109].key) == "ERR_QUEST_ADD_ITEM_SII");
}

TEST_CASE("messages go where the stock client sends them") {
    // Quest progress is the yellow centre text.
    CHECK(gameMessageKind("ERR_QUEST_ADD_KILL_SII") == GameMessageKind::Info);
    CHECK(gameMessageKind("ERR_QUEST_ADD_ITEM_SII") == GameMessageKind::Info);
    CHECK(gameMessageKind("ERR_QUEST_OBJECTIVE_COMPLETE_S") == GameMessageKind::Info);
    // Accepting, failing and sharing quests are chat lines.
    CHECK(gameMessageKind("ERR_QUEST_ACCEPTED_S") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_QUEST_FAILED_S") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_QUEST_PUSH_SUCCESS_S") == GameMessageKind::System);
    // Trade: the request and its start in chat, the end in the centre.
    CHECK(gameMessageKind("ERR_TRADE_REQUEST_S") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_INITIATE_TRADE_S") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_TRADE_COMPLETE") == GameMessageKind::Info);
    CHECK(gameMessageKind("ERR_MAIL_SENT") == GameMessageKind::Info);
    // Group and loot notices are chat lines; refusals are errors.
    CHECK(gameMessageKind("ERR_SET_LOOT_GROUP") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_GROUP_DISBANDED") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_RAID_LEADER_READY_CHECK_START_S") == GameMessageKind::System);
    CHECK(gameMessageKind("ERR_INV_FULL") == GameMessageKind::Error);
    CHECK(gameMessageKind("ERR_TOO_FAR_TO_INTERACT") == GameMessageKind::Error);
    // Not a GameError: an error, the kind a caller raising one expects.
    CHECK(gameMessageKind("NOT_A_REAL_KEY") == GameMessageKind::Error);
}

TEST_CASE("GlobalStrings formats fill as the client's printf does") {
    CHECK(formatGlobalString("%s: %d/%d", {"Tough Wolf Meat", "3", "8"}) == "Tough Wolf Meat: 3/8");
    CHECK(formatGlobalString("%s slain: %d/%d", {"Kobold Vermin", "1", "10"}) ==
          "Kobold Vermin slain: 1/10");
    CHECK(formatGlobalString("Quest accepted: %s", {"Kobold Camp Cleanup"}) ==
          "Quest accepted: Kobold Camp Cleanup");
    // Positional arguments (CHAT_INVITE_NOTICE).
    CHECK(formatGlobalString("%2$s has invited you to join the channel '%1$s'.", {"Trade", "Bob"}) ==
          "Bob has invited you to join the channel 'Trade'.");
    CHECK(formatGlobalString("100%% done", {}) == "100% done");
    CHECK(formatGlobalString("%.1f yards", {"5.0"}) == "5.0 yards");
    CHECK(formatGlobalString("%s and %s", {"one"}) == "one and ");
}
