// Server packets through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/entity.hpp"
#include "game/game_utils.hpp"
#include "game/packed_time.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using replay::Session;
using replay::UpdateObject;
using wowee::game::UF;

namespace {

std::shared_ptr<wowee::game::Unit> playerUnit(Session& s) {
    return std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
}

constexpr uint32_t kAutoShot = 75;
constexpr uint64_t kMob = 0xF130000000000123ull;
constexpr uint64_t kOtherHunter = 0x0000000000000077ull;

// A hunter in the world with a mob to shoot.
void huntMob(Session& s) {
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().create(kMob, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 8}}));
}

// The castCount CMSG_CAST_SPELL carried on the latest press, its first byte.
uint8_t lastCastCount(const Session& s) {
    auto casts = s.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL);
    REQUIRE_FALSE(casts.empty());
    return casts.back().getData().at(0);
}

// Spell::SendCastResult: castCount, spellId, result.
wowee::network::Packet castFailed(uint8_t castCount, uint32_t spellId, uint8_t result) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CAST_FAILED));
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt8(result);
    return p;
}

// Player::SendAutoRepeatCancel: the shooter's packed guid, to everyone nearby.
wowee::network::Packet autoRepeatCancelled(uint64_t shooter) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CANCEL_AUTO_REPEAT));
    p.writePackedGuid(shooter);
    return p;
}

constexpr uint8_t kNotReady = 67;
constexpr uint8_t kOutOfRange = 97;

}  // namespace

TEST_CASE("the session speaks WotLK", "[replay]") {
    Session s;
    CHECK(wowee::game::isActiveExpansion("wotlk"));
    CHECK_FALSE(wowee::game::isPreWotlk());
}

TEST_CASE("the player's create block makes the player's unit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 181);
    CHECK(unit->getMaxHealth() == 181);
    CHECK(unit->getLevel() == 9);
}

TEST_CASE("a values block changes only the fields it carries", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 65}}));

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 65);
    CHECK(unit->getMaxHealth() == 181);
}

// AzerothCore's Player::SetRestState and RemoveRestState set and clear
// PLAYER_FLAGS_RESTING (0x20).
TEST_CASE("resting follows the resting player flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::PLAYER_FLAGS, 0x20}});
    CHECK(s.handler().isPlayerResting());

    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerResting());
}

// Player::SendInitialPacketsBeforeAddToMap: the packed game time, the speed,
// and a zero added in 3.1.2.
TEST_CASE("the login time sets the game clock", "[replay]") {
    Session s;
    wowee::game::WowDate now;
    now.hour = 13;
    now.minute = 30;
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LOGIN_SETTIMESPEED));
    p.writeUInt32(wowee::game::packWowPackedTime(now));
    p.writeFloat(0.01666667f);
    p.writeUInt32(0);
    s.receive(p);

    CHECK(s.handler().getGameTime() == Catch::Approx(13.5f).margin(0.01f));
}

TEST_CASE("a refused Auto Shot press stops the repeat it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    REQUIRE(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(castFailed(lastCastCount(s), kAutoShot, kOutOfRange));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

// Auto Shot is a toggle in 3.3.5a: UseAction on the repeating spell sends
// CMSG_CANCEL_AUTO_REPEAT_SPELL instead of casting it again (0x80DA80 ->
// 0x807560(1)). docs/re/input-timing.md.
TEST_CASE("a second Auto Shot press turns the repeat off", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    REQUIRE(s.handler().getAutoRepeatSpellId() == kAutoShot);
    s.handler().castSpell(kAutoShot, kMob);
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

// Unit::InterruptSpell sends it for whoever stopped shooting.
TEST_CASE("only the player's own auto repeat cancel stops Auto Shot", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);

    s.receive(autoRepeatCancelled(kOtherHunter));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(autoRepeatCancelled(Session::kPlayerGuid));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

constexpr uint32_t kArcaneShot = 3044;

TEST_CASE("a refused cast takes back the GCD it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    REQUIRE(s.handler().isGCDActive());

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kOutOfRange));
    CHECK_FALSE(s.handler().isGCDActive());
}

TEST_CASE("a press refused during the GCD leaves the GCD running", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    s.handler().castSpell(kArcaneShot, kMob);

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kNotReady));
    CHECK(s.handler().isGCDActive());
}

namespace {

constexpr uint64_t kPet = 0xF140000000000051ull;

// Player::PetSpellInitialize up to the spell count: guid, family, duration,
// react and command states, flags, the ten action bar slots.
wowee::network::Packet petSpells(uint64_t pet) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_PET_SPELLS));
    p.writeUInt64(pet);
    p.writeUInt16(0);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt8(1);
    p.writeUInt16(0);
    for (int i = 0; i < 10; ++i) p.writeUInt32(0);
    p.writeUInt8(0);
    return p;
}

bool petCanBeAbandoned(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerUnitLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeAbandoned()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

namespace {

constexpr uint64_t kWolf = 0xF130000000000321ull;

// Unit::SendAttackStateUpdate for one swing of a single physical sub-damage.
wowee::network::Packet swing(uint64_t attacker, uint64_t victim, uint32_t damage, uint8_t victimState,
                             uint32_t hitInfo = 0x2 /* HITINFO_AFFECTS_VICTIM */, uint32_t blocked = 0) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ATTACKERSTATEUPDATE));
    p.writeUInt32(hitInfo);
    p.writePackedGuid(attacker);
    p.writePackedGuid(victim);
    p.writeUInt32(damage);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt32(1);  // physical
    p.writeFloat(static_cast<float>(damage));
    p.writeUInt32(damage);
    p.writeUInt8(victimState);
    p.writeUInt32(0);
    p.writeUInt32(0);
    if (hitInfo & 0x2000) p.writeUInt32(blocked);  // HITINFO_BLOCK
    return p;
}

std::vector<wowee::game::CombatTextEntry::Type> combatTextTypes(Session& s) {
    std::vector<wowee::game::CombatTextEntry::Type> types;
    for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
    return types;
}

}  // namespace

// Pet::CreateBaseAtTamed and Pet::LoadPetFromDB set UNIT_CAN_BE_ABANDONED
// (0x02) in UNIT_FIELD_BYTES_2 byte 2 on a hunter's beast, and nothing else
// sets it.
TEST_CASE("PetCanBeAbandoned is true for a tamed pet only", "[replay]") {
    for (const bool tamed : {true, false}) {
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
        s.receive(UpdateObject().create(
            kPet, wowee::game::ObjectType::UNIT,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, tamed ? 0x03u << 16 : 0u},
             // The player made it (0x5D3780 checks CREATEDBY too).
             {UF::UNIT_FIELD_CREATEDBY_LO, static_cast<uint32_t>(Session::kPlayerGuid)}}));
        s.receive(petSpells(kPet));
        REQUIRE(s.handler().getPetGuid() == kPet);

        CHECK(petCanBeAbandoned(s) == tamed);
    }
}

// WorldPackets::Misc::Weather: the WeatherState, its intensity, and whether
// it changes at once. Weather::GetWeatherState picks the state.
TEST_CASE("weather states read as what the server means", "[replay]") {
    using wowee::game::WeatherKind;
    struct Case { uint32_t state; WeatherKind kind; bool raining; bool snowing; };
    const Case cases[] = {
        {0, WeatherKind::NONE, false, false},
        {1, WeatherKind::FOG, false, false},
        {3, WeatherKind::RAIN, true, false},
        {4, WeatherKind::RAIN, true, false},
        {5, WeatherKind::RAIN, true, false},
        {6, WeatherKind::SNOW, false, true},
        {7, WeatherKind::SNOW, false, true},
        {8, WeatherKind::SNOW, false, true},
        {22, WeatherKind::SANDSTORM, false, false},
        {41, WeatherKind::SANDSTORM, false, false},
        {42, WeatherKind::SANDSTORM, false, false},
        {86, WeatherKind::THUNDERSTORM, true, false},
        {90, WeatherKind::RAIN, true, false},
        {106, WeatherKind::SNOW, false, true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.state);
        Session s;
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
        p.writeUInt32(c.state);
        p.writeFloat(0.5f);
        p.writeUInt8(0);
        s.receive(p);

        CHECK(s.handler().getWeatherKind() == c.kind);
        CHECK(s.handler().isRaining() == c.raining);
        CHECK(s.handler().isSnowing() == c.snowing);
    }
}

namespace {

wowee::network::Packet weather(uint32_t state, float intensity) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
    p.writeUInt32(state);
    p.writeFloat(intensity);
    p.writeUInt8(0);
    return p;
}

std::vector<std::string> chatLines(Session& s) {
    std::vector<std::string> lines;
    for (const auto& m : s.handler().getChatHistory()) lines.push_back(m.message);
    return lines;
}

}  // namespace

TEST_CASE("weather changes post nothing to chat, as in the stock client", "[replay]") {
    Session s;
    s.receive(weather(3, 0.3f));
    s.receive(weather(5, 0.9f));
    s.receive(weather(0, 0.0f));
    s.receive(weather(1, 0.5f));
    s.receive(weather(0, 0.0f));
    CHECK(chatLines(s).empty());
}

TEST_CASE("faint rain is not rain", "[replay]") {
    Session s;
    s.receive(weather(4, 0.01f));
    CHECK_FALSE(s.handler().isRaining());
}

// AuraEffect::HandleFeignDeath sets UNIT_FLAG2_FEIGN_DEATH and
// UNIT_DYNFLAG_DEAD together, and removes them together.
TEST_CASE("Feign Death is not a death", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_DEAD") == 0);
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

TEST_CASE("the dead flag at zero health is still a death", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK(s.handler().isPlayerDead());
}

// vmangos's Unit::SetFeignDeath sets the dead flag alone.
TEST_CASE("the dead flag at full health is Feign Death without its flags2 bit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

TEST_CASE("killed while feigning stays dead when the feign ends", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

// A player created while feigning, as after a relog or a teleport mid-feign.
TEST_CASE("a player created feigning is alive, and stays so when it ends", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300},
                   {UF::UNIT_FIELD_MAXHEALTH, 300},
                   {UF::UNIT_FIELD_FLAGS_2, 0x1},
                   {UF::UNIT_DYNAMIC_FLAGS, 0x20}});
    CHECK_FALSE(s.handler().isPlayerDead());
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

TEST_CASE("a wolf's hit shows its damage", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 12, 1));
    REQUIRE(s.handler().getCombatText().size() == 1);
    CHECK(s.handler().getCombatText()[0].type == T::MELEE_DAMAGE);
    CHECK(s.handler().getCombatText()[0].amount == 12);
}

TEST_CASE("a dodged swing shows a dodge", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(Session::kPlayerGuid, kWolf, 0, 2));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::DODGE});
}

TEST_CASE("a parried wolf shows a parry", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 0, 3));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::PARRY});
}

// Unit::CalcDamageInfo sends an evade as a miss with VICTIMSTATE_EVADES.
TEST_CASE("an evading wolf shows an evade, not a miss", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(Session::kPlayerGuid, kWolf, 0, 6, 0x2 | 0x10 | 0x200000));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::EVADE});
}

TEST_CASE("a partly blocked hit shows its damage and the block", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 7, 1, 0x2 | 0x2000, 5));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::MELEE_DAMAGE, T::BLOCK});
}

TEST_CASE("a fully blocked hit shows only the block", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 0, 5, 0x2 | 0x2000, 12));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::BLOCK});
}

namespace {

constexpr uint32_t kRaptorStrike = 2973;

// Spell::SendSpellStart for the player's own spell on no particular target.
wowee::network::Packet spellStart(uint8_t castCount, uint32_t spellId, uint32_t castTimeMs) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_START));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt32(0);  // cast flags
    p.writeUInt32(castTimeMs);
    p.writeUInt32(0);  // target flags: none
    return p;
}

}  // namespace

// Spell::SendCastResult writes the press's castCount; the server's own casts,
// such as each Auto Shot after the first, have none and send 0.
TEST_CASE("a press never goes out with castCount 0", "[replay]") {
    Session s;
    huntMob(s);
    for (int i = 0; i < 255; ++i) s.handler().castSpell(kArcaneShot, kMob);
    s.handler().castSpell(kAutoShot, kMob);
    CHECK(lastCastCount(s) != 0);

    s.receive(castFailed(0, kAutoShot, kOutOfRange));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);
}

TEST_CASE("a refused second press leaves the queued swing ability lit", "[replay]") {
    Session s;
    huntMob(s);
    auto& cache = s.handler().spellNameCacheRef();
    cache[kRaptorStrike].attr = 0x4;  // SPELL_ATTR0_ON_NEXT_SWING
    s.handler().spellNameCacheLoadedRef() = true;

    s.handler().castSpell(kRaptorStrike, kMob);
    const uint8_t queued = lastCastCount(s);
    s.receive(spellStart(queued, kRaptorStrike, 0));
    REQUIRE(s.handler().getActiveActionSpellId() == kRaptorStrike);

    s.handler().castSpell(kRaptorStrike, kMob);
    s.receive(castFailed(lastCastCount(s), kRaptorStrike, kNotReady));
    CHECK(s.handler().getActiveActionSpellId() == kRaptorStrike);

    s.receive(castFailed(queued, kRaptorStrike, kOutOfRange));
    CHECK(s.handler().getActiveActionSpellId() == 0);
}

namespace {

bool petCanBeRenamed(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeRenamed()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

// Pet::LoadPetFromDB sets UNIT_CAN_BE_RENAMED (0x01) beside
// UNIT_CAN_BE_ABANDONED only on a pet never named, and
// WorldSession::HandlePetRename clears it.
TEST_CASE("PetCanBeRenamed follows the pet's rename flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, 0x03u << 16},
                                     {UF::UNIT_FIELD_CREATEDBY_LO, static_cast<uint32_t>(Session::kPlayerGuid)}}));
    s.receive(petSpells(kPet));
    CHECK(petCanBeRenamed(s));

    s.receive(UpdateObject().values(kPet, {{UF::UNIT_FIELD_BYTES_2, 0x02u << 16}}));
    CHECK_FALSE(petCanBeRenamed(s));
}

namespace {

// WorldObject::SendSpellMiss: the spell, the caster's guid whole, an unknown
// byte, one target, its guid whole, and how it missed (SpellMissInfo).
wowee::network::Packet spellMiss(uint64_t caster, uint64_t victim, uint32_t spellId, uint8_t missInfo) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELLLOGMISS));
    p.writeUInt32(spellId);
    p.writeUInt64(caster);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt64(victim);
    p.writeUInt8(missInfo);
    return p;
}

std::vector<wowee::game::CombatTextEntry::Type> spellMissText(uint64_t caster, uint64_t victim,
                                                               uint8_t missInfo) {
    Session s;
    huntMob(s);
    // Resists and immunities show only on the target (fctSpellMechanics).
    s.handler().setTarget(kMob);
    s.receive(spellMiss(caster, victim, kArcaneShot, missInfo));
    std::vector<wowee::game::CombatTextEntry::Type> types;
    for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
    return types;
}

}  // namespace

namespace {

// SpellMissInfo, as every server family numbers it.
const std::vector<std::pair<uint8_t, wowee::game::CombatTextEntry::Type>>& missTypes() {
    using T = wowee::game::CombatTextEntry;
    static const std::vector<std::pair<uint8_t, T::Type>> table = {
        {1, T::MISS},   {2, T::RESIST}, {3, T::DODGE},   {4, T::PARRY},  {5, T::BLOCK},
        {6, T::EVADE},  {7, T::IMMUNE}, {8, T::IMMUNE},  {9, T::DEFLECT}, {10, T::ABSORB},
    };
    return table;
}

}  // namespace

TEST_CASE("a spell miss shows what the server says happened", "[replay]") {
    for (const auto& [missInfo, type] : missTypes()) {
        CAPTURE(missInfo);
        // The player's own misses, against the target, where every kind shows.
        CHECK(spellMissText(Session::kPlayerGuid, kMob, missInfo) ==
              std::vector<wowee::game::CombatTextEntry::Type>{type});
    }
}

namespace {

// Spell::SendSpellGo with no hits and one miss: caster and unit packed, the
// cast count, spell, flags and time, no hit targets, one miss target whole
// with its SpellMissInfo, and an empty target mask.
wowee::network::Packet spellGoMiss(uint64_t victim, uint32_t spellId, uint8_t missInfo) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_GO));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(1);
    p.writeUInt32(spellId);
    p.writeUInt32(0);
    p.writeUInt32(0);
    p.writeUInt8(0);
    p.writeUInt8(1);
    p.writeUInt64(victim);
    p.writeUInt8(missInfo);
    p.writeUInt32(0);
    return p;
}

}  // namespace

TEST_CASE("a spell go miss shows what the server says happened", "[replay]") {
    for (const auto& [missInfo, type] : missTypes()) {
        CAPTURE(missInfo);
        Session s;
        huntMob(s);
        s.handler().setTarget(kMob);
        s.receive(spellGoMiss(kMob, kArcaneShot, missInfo));
        std::vector<wowee::game::CombatTextEntry::Type> types;
        for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
        CHECK(types == std::vector<wowee::game::CombatTextEntry::Type>{type});
    }
}

// SMSG_LOGOUT_COMPLETE after a plain /logout: the stock client leaves the
// world but keeps the session, and asks for the character list again for
// character select. This client used to drop the connection and go back to
// the login screen.
TEST_CASE("a logout returns to character select on the same session", "[replay]") {
    Session s;
    s.socket().setConnected(true);
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 9}});
    REQUIRE(s.handler().getEntityManager().getEntity(Session::kPlayerGuid));

    bool called = false, exiting = true;
    s.handler().setLogoutCompleteCallback([&](bool e) { called = true; exiting = e; });
    s.receive(wowee::network::Packet(
        wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LOGOUT_COMPLETE)));
    REQUIRE(called);
    CHECK_FALSE(exiting);

    // What the application does with exiting == false.
    s.handler().returnToCharacterSelect();
    CHECK(s.handler().isConnected());
    CHECK(s.handler().getState() == wowee::game::WorldState::CHAR_LIST_REQUESTED);
    CHECK(s.sent().of(wowee::game::Opcode::CMSG_CHAR_ENUM).size() == 1);
    CHECK_FALSE(s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
}

TEST_CASE("a logout whose session has gone ends at the login screen", "[replay]") {
    Session s;  // the test socket says it is not connected
    s.handler().returnToCharacterSelect();
    CHECK(s.handler().getState() == wowee::game::WorldState::DISCONNECTED);
    CHECK(s.sent().of(wowee::game::Opcode::CMSG_CHAR_ENUM).empty());
}

namespace {

constexpr uint32_t kPowerWordShield = 48066;
constexpr uint64_t kPartyMember = 0x0000000000000088ull;

// Spell::SendSpellStart for an instant: caster and unit packed, the cast
// count, spell, flags, a cast time of 0 and a unit target.
wowee::network::Packet instantStart(uint64_t caster, uint32_t spellId, uint64_t target) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_START));
    p.writePackedGuid(caster);
    p.writePackedGuid(caster);
    p.writeUInt8(1);
    p.writeUInt32(spellId);
    p.writeUInt32(0x2);
    p.writeUInt32(0);
    p.writeUInt32(0x2);  // TARGET_FLAG_UNIT
    p.writePackedGuid(target);
    return p;
}

// Spell::SendSpellGo: one hit target, no misses, a unit target.
wowee::network::Packet instantGo(uint64_t caster, uint32_t spellId, uint64_t target) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_GO));
    p.writePackedGuid(caster);
    p.writePackedGuid(caster);
    p.writeUInt8(1);
    p.writeUInt32(spellId);
    p.writeUInt32(0x100);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt64(target);
    p.writeUInt8(0);
    p.writeUInt32(0x2);  // TARGET_FLAG_UNIT
    p.writePackedGuid(target);
    return p;
}

struct AnimCall {
    uint64_t guid;
    bool start;
    bool hardStop;
    uint32_t spellId;
};

}  // namespace

// The cast animation is the spell's own kits' (0x73B140 plays the cast kit's
// AnimID), so every call that can play one has to name the spell. An instant
// is no current cast and its SPELL_START clears the prediction, so the
// animation path used to find no spell for it and guess from the target:
// Power Word: Shield on a party member released in SpellCastDirected.
TEST_CASE("an instant cast on someone else names its spell to the cast animation", "[replay]") {
    for (const uint64_t caster : {Session::kPlayerGuid, kOtherHunter}) {
        CAPTURE(caster);
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 80}});
        s.receive(UpdateObject().create(kPartyMember, wowee::game::ObjectType::PLAYER,
                                        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}}));
        if (caster != Session::kPlayerGuid)
            s.receive(UpdateObject().create(caster, wowee::game::ObjectType::PLAYER,
                                            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}}));
        std::vector<AnimCall> calls;
        s.handler().setSpellCastAnimCallback(
            [&](uint64_t guid, bool start, bool isChannel, wowee::game::SpellCastType,
                uint32_t spellId) { calls.push_back({guid, start, !start && isChannel, spellId}); });
        if (caster == Session::kPlayerGuid) s.handler().castSpell(kPowerWordShield, kPartyMember);
        s.receive(instantStart(caster, kPowerWordShield, kPartyMember));
        s.receive(instantGo(caster, kPowerWordShield, kPartyMember));
        size_t named = 0;
        for (const auto& c : calls) {
            if (c.guid != caster || c.hardStop) continue;
            CHECK(c.spellId == kPowerWordShield);
            ++named;
        }
        CHECK(named > 0);
    }
}
