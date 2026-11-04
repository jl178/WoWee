// The name pipeline's rules and the cache file format (game/name_pipeline.hpp,
// docs/re/names-and-caches.md).
#include <catch_amalgamated.hpp>

#include "game/name_pipeline.hpp"

#include <sstream>

namespace np = wowee::game::name_pipeline;
using wowee::game::CreatureQueryResponseData;
using wowee::game::GameObjectQueryResponseData;

TEST_CASE("a unit's name comes from the cache 0x72A000 reads for it") {
    CHECK(np::nameSourceFor(true, 0) == np::NameSource::Player);
    CHECK(np::nameSourceFor(true, 12) == np::NameSource::Player);
    CHECK(np::nameSourceFor(false, 0) == np::NameSource::Creature);
    CHECK(np::nameSourceFor(false, 12) == np::NameSource::Pet);
}

TEST_CASE("a pet name answers only while its stamp matches") {
    np::PetName p{"Fang", 1234};
    CHECK(np::petNameCurrent(p, 1234));
    CHECK_FALSE(np::petNameCurrent(p, 1300));  // renamed since
    CHECK_FALSE(np::petNameCurrent({"", 1234}, 1234));
}

TEST_CASE("an unanswered query is asked again after 10 s") {
    CHECK_FALSE(np::queryDueForResend(1000, 1000));
    CHECK_FALSE(np::queryDueForResend(1000, 10999));
    CHECK(np::queryDueForResend(1000, 11000));
}

TEST_CASE("realm cache keys are safe directory names") {
    CHECK(np::realmCacheKey("192.168.1.97", 8085, 1) == "192.168.1.97_8085_1");
    CHECK(np::realmCacheKey("a/../b c", 1, 2) == "a_.._b_c_1_2");
    CHECK(np::realmCacheKey("", 8085, 0) == "realm_8085_0");
}

TEST_CASE("the creature cache round-trips, odd characters and all") {
    std::unordered_map<uint32_t, CreatureQueryResponseData> in;
    CreatureQueryResponseData wolf;
    wolf.entry = 299;
    wolf.name = "Young Wolf";
    wolf.subName = "Tab\there\\and\nnewline";
    wolf.iconName = "";
    wolf.typeFlags = 1;
    wolf.creatureType = 1;
    wolf.family = 1;
    wolf.rank = 0;
    wolf.displayId[0] = 903;
    wolf.displayId[3] = 7;
    in[299] = wolf;
    CreatureQueryResponseData noName;  // not valid: never written
    noName.entry = 5;
    in[5] = noName;

    std::stringstream ss;
    np::writeCreatureCache(ss, 12340, in);
    std::unordered_map<uint32_t, CreatureQueryResponseData> out;
    REQUIRE(np::readCreatureCache(ss, 12340, out));
    REQUIRE(out.size() == 1);
    const auto& w = out.at(299);
    CHECK(w.name == "Young Wolf");
    CHECK(w.subName == "Tab\there\\and\nnewline");
    CHECK(w.typeFlags == 1);
    CHECK(w.creatureType == 1);
    CHECK(w.family == 1);
    CHECK(w.displayId[0] == 903);
    CHECK(w.displayId[3] == 7);
}

TEST_CASE("a cache file of another build is not read") {
    std::unordered_map<uint32_t, CreatureQueryResponseData> in;
    CreatureQueryResponseData d;
    d.entry = 1;
    d.name = "X";
    in[1] = d;
    std::stringstream ss;
    np::writeCreatureCache(ss, 8606, in);
    std::unordered_map<uint32_t, CreatureQueryResponseData> out;
    CHECK_FALSE(np::readCreatureCache(ss, 12340, out));
    CHECK(out.empty());
}

TEST_CASE("a damaged line is skipped, the rest kept") {
    std::stringstream ss;
    ss << np::cacheHeader("creature", 12340) << "\n"
       << "12\tbroken\n"
       << "13\tGood\t\t\t0\t7\t0\t1\t100\t0\t0\t0\n"
       << "x\tBad entry\t\t\t0\t7\t0\t1\t100\t0\t0\t0\n";
    std::unordered_map<uint32_t, CreatureQueryResponseData> out;
    REQUIRE(np::readCreatureCache(ss, 12340, out));
    REQUIRE(out.size() == 1);
    CHECK(out.at(13).name == "Good");
    CHECK(out.at(13).rank == 1);
}

TEST_CASE("the game object cache round-trips its data fields") {
    std::unordered_map<uint32_t, GameObjectQueryResponseData> in;
    GameObjectQueryResponseData mailbox;
    mailbox.entry = 32349;
    mailbox.name = "Mailbox";
    mailbox.type = 19;
    mailbox.displayId = 3051;
    mailbox.iconName = "Directions";
    mailbox.hasData = true;
    mailbox.data[0] = 42;
    mailbox.data[23] = 99;
    in[mailbox.entry] = mailbox;
    std::stringstream ss;
    np::writeGameObjectCache(ss, 12340, in);
    std::unordered_map<uint32_t, GameObjectQueryResponseData> out;
    REQUIRE(np::readGameObjectCache(ss, 12340, out));
    const auto& m = out.at(32349);
    CHECK(m.name == "Mailbox");
    CHECK(m.type == 19);
    CHECK(m.displayId == 3051);
    CHECK(m.iconName == "Directions");
    CHECK(m.hasData);
    CHECK(m.data[0] == 42);
    CHECK(m.data[23] == 99);
}
