// Duels as the stock 3.3.5a client knows them (game/stock_duel.hpp): the
// reaction and attack rules read off PLAYER_DUEL_ARBITER / PLAYER_DUEL_TEAM,
// and the chat countdown. docs/re/combat-targeting-movement.md section 1
// ("Duels") has the addresses.
#include <catch_amalgamated.hpp>
#include "game/stock_duel.hpp"

#include <vector>

namespace sd = wowee::game::stock_duel;

TEST_CASE("0x7251C0: the two sides of one duel are hostile") {
    constexpr uint64_t flag = 0xF110005EB0000001ull;
    const sd::DuelFields a{.arbiter = flag, .team = 1};
    const sd::DuelFields b{.arbiter = flag, .team = 2};
    REQUIRE(sd::duelReaction(a, b).has_value());
    CHECK(*sd::duelReaction(a, b) == 1);
    CHECK(sd::duelAllowsAttack(a, b));
    // The same side: neutral, and not to be attacked.
    CHECK(*sd::duelReaction(a, a) == 4);
    CHECK_FALSE(sd::duelAllowsAttack(a, a));
}

TEST_CASE("no duel until both teams are set, and only under one flag") {
    constexpr uint64_t flag = 0xF110005EB0000001ull;
    // Challenged: the arbiter is set, the teams wait for the countdown.
    CHECK_FALSE(sd::duelReaction({flag, 0}, {flag, 0}).has_value());
    CHECK_FALSE(sd::duelReaction({flag, 1}, {flag, 0}).has_value());
    // Two duels side by side.
    CHECK_FALSE(sd::duelReaction({flag, 1}, {flag + 1, 2}).has_value());
    // Over: both cleared.
    CHECK_FALSE(sd::duelReaction({0, 0}, {0, 0}).has_value());
    CHECK_FALSE(sd::duelAllowsAttack({flag, 1}, {flag + 1, 2}));
}

TEST_CASE("SMSG_DUEL_COUNTDOWN: Duel starting 3, 2, 1, a second apart") {
    sd::Countdown c;
    CHECK(c.start(3000) == std::vector<int>{3});
    CHECK(c.running());
    CHECK(c.tick(999).empty());
    CHECK(c.tick(1) == std::vector<int>{2});
    CHECK(c.tick(1000) == std::vector<int>{1});
    CHECK_FALSE(c.running());
    CHECK(c.tick(5000).empty());
}

TEST_CASE("a long frame says every number it passed") {
    sd::Countdown c;
    CHECK(c.start(3000) == std::vector<int>{3});
    CHECK(c.tick(2500) == std::vector<int>{2, 1});
    CHECK_FALSE(c.running());
}

TEST_CASE("SMSG_DUEL_COMPLETE stops the countdown") {
    sd::Countdown c;
    CHECK(c.start(3000) == std::vector<int>{3});
    c.stop();
    CHECK(c.tick(1000).empty());
    // Whole seconds only: 3500 ms counts from 3.
    CHECK(c.start(3500) == std::vector<int>{3});
}
