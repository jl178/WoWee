// lua_spell_api.cpp - Spell info, casting, auras, and targeting Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/shapeshift_forms.hpp"
#include "addons/lua_api_helpers.hpp"
#include "game/item_text.hpp"
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include <set>

namespace wowee::addons {

namespace {

/// The highest-rank known spell with this name, or 0 when the player knows
/// none by that name.
///
/// This is what `/cast Frostbolt` means: the name alone picks the best rank the
/// player has. Two bindings resolved it with their own copy of the walk, and
/// picking the wrong one is silent - the spell goes off, it is simply the
/// rank-one version.
///
/// Rank arrives from the DBC as text, "Rank 4", so the number is parsed out of
/// it. A spell with no rank string, which is most of them, sorts as rank zero:
/// right, because it is the only one of its name.
uint32_t highestKnownRankByName(game::GameHandler* gh, const std::string& name) {
    std::string wanted(name);
    toLowerInPlace(wanted);

    uint32_t bestId = 0;
    int bestRank = -1;
    for (uint32_t sid : gh->getKnownSpells()) {
        std::string known = gh->getSpellName(sid);
        toLowerInPlace(known);
        if (known != wanted) continue;

        int rank = 0;
        const std::string& rankText = gh->getSpellRank(sid);
        if (!rankText.empty()) {
            std::string lowered = rankText;
            toLowerInPlace(lowered);
            if (lowered.rfind("rank ", 0) == 0) {
                try { rank = std::stoi(lowered.substr(5)); } catch (...) {}
            }
        }
        if (rank > bestRank) { bestRank = rank; bestId = sid; }
    }
    return bestId;
}

}  // namespace


// ---- Finishing a spell that is waiting for a target ----
//
// A spell cast with no target leaves the cursor holding it until something is
// clicked; these are what the unit and party frames call when that click lands
// on them. This client resolves a target at cast time instead - there is no
// pending spell on the cursor to complete - so each answers false: nothing was
// waiting, so nothing was consumed, and the frame falls through to its ordinary
// click handling rather than swallowing it.
static int lua_SpellTargetUnit(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_SpellTargetItem(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// Dropping what the cursor holds onto a unit, which opens a trade. The cursor
// carries no item here, so there is nothing to drop.
static int lua_DropItemOnUnit(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// The totem bar's per-slot spell choice, which needs the multi-cast action bar
// this client does not model.
static int lua_SetMultiCastSpell(lua_State* L) { (void)L; return 0; }


// --- Totems ---
//
// Four slots, each holding a spell and how long it has left. The interface
// wants the moment one was placed rather than its age, on the same clock
// GetTime() reports - a bar that is drawn from start and duration cannot use a
// figure measured from a different zero.

// GetTotemInfo(slot) → haveTotem, name, startTime, duration, icon
static int lua_GetTotemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "");
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 5;
    }
    const auto& totem = gh->getTotemSlot(slot - 1);
    const bool active = totem.active();
    const double durationSec = totem.durationMs / 1000.0;
    const double remainingSec = totem.remainingMs() / 1000.0;

    lua_pushboolean(L, active ? 1 : 0);
    lua_pushstring(L, active ? gh->getSpellName(totem.spellId).c_str() : "");
    // Placed at however long ago it has already run for.
    lua_pushnumber(L, active ? luaGetTimeNow() - (durationSec - remainingSec) : 0.0);
    lua_pushnumber(L, active ? durationSec : 0.0);
    if (active) {
        const std::string icon = gh->getSpellIconPath(totem.spellId);
        if (!icon.empty()) lua_pushstring(L, icon.c_str()); else lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    return 5;
}

// GetTotemTimeLeft(slot) → seconds
static int lua_GetTotemTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getTotemSlot(slot - 1).remainingMs() / 1000.0);
    return 1;
}

static int lua_IsSpellInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* spellNameOrId = luaL_checkstring(L, 1);
    const char* uid = luaL_optstring(L, 2, "target");

    // Resolve spell ID
    uint32_t spellId = 0;
    if (spellNameOrId[0] >= '0' && spellNameOrId[0] <= '9') {
        spellId = static_cast<uint32_t>(strtoul(spellNameOrId, nullptr, 10));
    } else {
        // The rank that would actually be cast, not the first one found. A
        // range check exists to say whether pressing the button will work, so
        // it has to be asked of the same spell /cast would pick; where ranks
        // differ in range, answering about rank one greys out a button that
        // would have reached.
        spellId = highestKnownRankByName(gh, spellNameOrId);
    }
    if (spellId == 0) { return luaReturnNil(L); }

    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    return pushSpellRangeAnswer(L, gh, spellId, resolveUnitGuid(gh, uidStr));
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)

/// Whether the player's class can remove a debuff of this dispel type.
///
/// The "RAID" filter FrameXML passes on party frames means "only what I can do
/// something about", and it is what showDispelDebuffs turns on. Without it the
/// filter string was accepted and ignored, so the frames showed every debuff
/// however little the healer could do - and answering the CVar without this
/// would have claimed a filter that does not filter.
///
/// Dispel types are Spell.dbc's: 1 Magic, 2 Curse, 3 Disease, 4 Poison. The
/// class table is 3.3.5's, and it is about what the class can remove from a
/// *friendly* target rather than what it can purge from an enemy.
static bool classCanDispel(uint8_t classId, uint8_t dispelType) {
    if (dispelType == 0) return false;
    constexpr uint8_t kMagic = 1, kCurse = 2, kDisease = 3, kPoison = 4;
    switch (classId) {
        case 2:  // Paladin - Cleanse
            return dispelType == kMagic || dispelType == kPoison || dispelType == kDisease;
        case 5:  // Priest - Dispel Magic, Abolish Disease
            return dispelType == kMagic || dispelType == kDisease;
        case 7:  // Shaman - Cure Toxins, Cleanse Spirit at 51
            return dispelType == kCurse || dispelType == kPoison || dispelType == kDisease;
        case 8:  // Mage - Remove Curse
            return dispelType == kCurse;
        case 11: // Druid - Remove Curse, Abolish Poison
            return dispelType == kCurse || dispelType == kPoison;
        case 6:  // Death Knight - the pet's Leech, and unholy's disease removal
            return dispelType == kDisease;
        case 9:  // Warlock - the Felhunter's Devour Magic
            return dispelType == kMagic;
        default: // Warrior, Hunter, Rogue: nothing
            return false;
    }
}

static int lua_UnitAura(lua_State* L, bool wantBuff) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    int index = static_cast<int>(luaL_optnumber(L, 2, 1));
    if (index < 1) { return luaReturnNil(L); }

    std::string uidStr(uid);
    toLowerInPlace(uidStr);

    const std::vector<game::AuraSlot>* auras = nullptr;
    if (uidStr == "player")      auras = &gh->getPlayerAuras();
    else if (uidStr == "target") auras = &gh->getTargetAuras();
    else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid != 0) auras = gh->getUnitAuras(guid);
    }
    if (!auras) { return luaReturnNil(L); }

    // "RAID" on a debuff means only what this character can remove. The index
    // counts into the filtered list, so the test has to come before the count
    // - filtering after it would answer the wrong aura for every index past
    // the first one skipped.
    const char* filterArg = luaL_optstring(L, 3, "");
    bool dispellableOnly = false;
    if (!wantBuff && filterArg) {
        std::string f(filterArg);
        for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        dispellableOnly = f.find("RAID") != std::string::npos;
    }
    const uint8_t playerClass = gh->getPlayerClass();

    // Filter to buffs or debuffs and find the Nth one
    int found = 0;
    for (const auto& aura : *auras) {
        if (aura.isEmpty() || aura.spellId == 0) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        if (dispellableOnly &&
            !classCanDispel(playerClass, gh->getSpellDispelType(aura.spellId))) {
            continue;
        }
        found++;
        if (found == index) {
            // Return: name, rank, icon, count, debuffType, duration, expirationTime, ...spellId
            std::string name = gh->getSpellName(aura.spellId);
            if (name.empty()) {
                // An aura this client's Spell.dbc has no row for draws as
                // "Unknown" with no icon, and nothing said which one it was.
                // A realm's own spells are the usual cause - a server can put
                // any id on a unit, and only a patched client knows its name.
                static std::set<uint32_t> saidNoName;
                if (saidNoName.insert(aura.spellId).second) {
                    LOG_WARNING("Aura spell ", aura.spellId, " on ", uidStr,
                                " has no Spell.dbc row, so it shows as 'Unknown' with no "
                                "icon (", (aura.flags & 0x80) ? "debuff" : "buff",
                                ", flags 0x", std::hex, static_cast<int>(aura.flags), std::dec,
                                ", caster 0x", std::hex, aura.casterGuid, std::dec, ")");
                }
            }
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
            lua_pushstring(L, "");           // rank
            std::string iconPath = gh->getSpellIconPath(aura.spellId);
            if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
            else lua_pushnil(L);             // icon texture path
            lua_pushnumber(L, aura.charges); // count
            // debuffType: resolve from Spell.dbc dispel type
            {
                uint8_t dt = gh->getSpellDispelType(aura.spellId);
                switch (dt) {
                    case 1:  lua_pushstring(L, "Magic");  break;
                    case 2:  lua_pushstring(L, "Curse");  break;
                    case 3:  lua_pushstring(L, "Disease"); break;
                    case 4:  lua_pushstring(L, "Poison"); break;
                    default: lua_pushnil(L);              break;
                }
            }
            lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
            // expirationTime: GetTime() + remaining seconds (so addons can compute countdown)
            if (aura.durationMs > 0) {
                uint64_t auraNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remMs = aura.getRemainingMs(auraNowMs);
                lua_pushnumber(L, luaGetTimeNow() + remMs / 1000.0);
            } else {
                lua_pushnumber(L, 0);  // permanent aura
            }
            // caster: return unit ID string if caster is known
            if (aura.casterGuid != 0) {
                if (aura.casterGuid == gh->getPlayerGuid())
                    lua_pushstring(L, "player");
                else if (aura.casterGuid == gh->getTargetGuid())
                    lua_pushstring(L, "target");
                else if (aura.casterGuid == gh->getFocusGuid())
                    lua_pushstring(L, "focus");
                else if (aura.casterGuid == gh->getPetGuid())
                    lua_pushstring(L, "pet");
                else {
                    char cBuf[32];
                    snprintf(cBuf, sizeof(cBuf), "0x%016llX", (unsigned long long)aura.casterGuid);
                    lua_pushstring(L, cBuf);
                }
            } else {
                lua_pushnil(L);
            }
            // isStealable: what the target frame's Stealable border reads
            // (targetframe.lua shows it for a buff when this is true and the
            // target is not the player). The 3.3.5 client answers true for a
            // helpful Magic aura on a unit the player can attack, and only for
            // a player who can steal one - which is a mage with Spellsteal, so
            // nobody else ever sees the border.
            {
                constexpr uint32_t kSpellsteal = 30449;
                bool stealable = false;
                if (!isDebuff && gh->getSpellDispelType(aura.spellId) == 1 &&
                    gh->getKnownSpells().count(kSpellsteal) != 0) {
                    const uint64_t unitGuid = resolveUnitGuid(gh, uidStr);
                    if (unitGuid != 0 && unitGuid != gh->getPlayerGuid()) {
                        auto ent = gh->getEntityManager().getEntity(unitGuid);
                        if (ent && ent->isUnit()) {
                            const auto* u = static_cast<const game::Unit*>(ent.get());
                            stealable = u->isHostile() ||
                                        !gh->isFriendlyFaction(u->getFactionTemplate());
                        }
                    }
                }
                lua_pushboolean(L, stealable ? 1 : 0);
            }
            lua_pushboolean(L, 0);           // shouldConsolidate
            lua_pushnumber(L, aura.spellId); // spellId
            return 11;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ── The 1.12 buff API ───────────────────────────────────────────────────────
//
// A vanilla interface does not call UnitBuff. It asks for a *slot* and then
// asks that slot four more questions, which is a different shape: UnitBuff
// answers a name and counts into a filtered list, and these answer an opaque
// number the caller hands straight back to the rest of them.
//
// buffframe.lua walks slots upward until it is told there is nothing there,
// and the terminator is -1 rather than nil. Unbound, GetPlayerBuff answered
// nil through the missing-API fallback, so `if buffIndex < 0` compared nil
// with a number on the first line of BuffButton_Update. That raised while the
// file was being read, which loses the file whole: no buff has ever appeared
// on a 1.12 interface, and BuffFrame.xml never built.

/// The player's aura in a slot, or nullptr for an empty or out-of-range one.
///
/// The slot is the index into the player's own aura array rather than a
/// position in a filtered list, so it stays meaningful after the caller has
/// stopped filtering - which is the whole point of handing it back.
static const game::AuraSlot* playerAuraAt(game::GameHandler* gh, int slot) {
    if (!gh || slot < 0) return nullptr;
    const auto& auras = gh->getPlayerAuras();
    if (static_cast<size_t>(slot) >= auras.size()) return nullptr;
    const game::AuraSlot& aura = auras[static_cast<size_t>(slot)];
    return aura.isEmpty() ? nullptr : &aura;
}

/// Whether a filter asks for debuffs. 1.12 spells that "HARMFUL"; the other
/// words it can carry - PASSIVE, CANCELABLE - do not change which half is
/// wanted, so they are not read.
static bool filterWantsHarmful(const char* filter) {
    if (!filter) return false;
    std::string upper(filter);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper.find("HARMFUL") != std::string::npos;
}

/// Milliseconds on the same clock AuraSlot::receivedAtMs was stamped from.
static uint64_t auraNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// GetPlayerBuff(index, filter) -> slot, untilCancelled
//
// index counts from zero, which is how buffframe.lua asks.
static int lua_GetPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int wanted = static_cast<int>(luaL_optnumber(L, 1, 0));
    const bool harmful = filterWantsHarmful(luaL_optstring(L, 2, "HELPFUL"));
    if (gh && wanted >= 0) {
        const auto& auras = gh->getPlayerAuras();
        int seen = 0;
        for (size_t i = 0; i < auras.size(); ++i) {
            const game::AuraSlot& aura = auras[i];
            if (aura.isEmpty()) continue;
            const bool isDebuff = (aura.flags & 0x80) != 0;
            if (isDebuff != harmful) continue;
            if (seen++ != wanted) continue;
            lua_pushnumber(L, static_cast<double>(i));
            // A buff with no duration is one the player dismisses rather than
            // waits out, and the interface draws it without a timer.
            lua_pushnumber(L, aura.durationMs > 0 ? 0 : 1);
            return 2;
        }
    }
    // -1, never nil. The caller compares this with a number before it does
    // anything else, so nil here is an error rather than an empty slot.
    lua_pushnumber(L, -1);
    lua_pushnil(L);
    return 2;
}

// GetPlayerBuffTexture(slot) -> texture
static int lua_GetPlayerBuffTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    const std::string icon = gh->getSpellIconPath(aura->spellId);
    if (icon.empty()) return luaReturnNil(L);
    lua_pushstring(L, icon.c_str());
    return 1;
}

// GetPlayerBuffTimeLeft(slot) -> seconds
static int lua_GetPlayerBuffTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    // Zero rather than nil: buffframe.lua divides and compares this, and a
    // permanent aura is honestly zero time left rather than an absent answer.
    if (!aura || aura->durationMs <= 0) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, aura->getRemainingMs(auraNowMs()) / 1000.0);
    return 1;
}

// GetPlayerBuffApplications(slot) -> count
static int lua_GetPlayerBuffApplications(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    lua_pushnumber(L, aura ? aura->charges : 0);
    return 1;
}

// GetPlayerBuffDispelType(slot) -> "Magic" | "Curse" | "Disease" | "Poison"
static int lua_GetPlayerBuffDispelType(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    switch (gh->getSpellDispelType(aura->spellId)) {
        case 1:  lua_pushstring(L, "Magic");   return 1;
        case 2:  lua_pushstring(L, "Curse");   return 1;
        case 3:  lua_pushstring(L, "Disease"); return 1;
        case 4:  lua_pushstring(L, "Poison");  return 1;
        default: break;
    }
    return luaReturnNil(L);
}

// __WoweePlayerBuffInfo(slot) -> name, debuffType, spellId, timeLeft
//
// What GameTooltip:SetPlayerBuff needs from a 1.12 slot, in one call. Not a WoW
// function: the real method is C and reads the aura directly, and the 1.12 API
// has no getter that names the spell in a slot.
static int lua_WoweePlayerBuffInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura || aura->spellId == 0) return luaReturnNil(L);
    const std::string& name = gh->getSpellName(aura->spellId);
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
    switch (gh->getSpellDispelType(aura->spellId)) {
        case 1:  lua_pushstring(L, "Magic");   break;
        case 2:  lua_pushstring(L, "Curse");   break;
        case 3:  lua_pushstring(L, "Disease"); break;
        case 4:  lua_pushstring(L, "Poison");  break;
        default: lua_pushnil(L);               break;
    }
    lua_pushnumber(L, aura->spellId);
    if (aura->durationMs > 0) lua_pushnumber(L, aura->getRemainingMs(auraNowMs()) / 1000.0);
    else                      lua_pushnil(L);
    return 4;
}

// CancelPlayerBuff(slot)
static int lua_CancelPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (aura) gh->cancelAura(aura->spellId);
    return 0;
}

static int lua_UnitBuff(lua_State* L) { return lua_UnitAura(L, true); }
static int lua_UnitDebuff(lua_State* L) { return lua_UnitAura(L, false); }

// UnitAura(unit, index, filter) - generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    std::string f(filter ? filter : "HELPFUL");
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    bool wantBuff = (f.find("HARMFUL") == std::string::npos);
    return lua_UnitAura(L, wantBuff);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid ? uid : "player");

    // Use shared GetTime() epoch for consistent timestamps
    double nowSec = luaGetTimeNow();

    // Resolve cast state for the unit
    bool isCasting = false;
    bool isChannel = false;
    uint32_t spellId = 0;
    float timeTotal = 0.0f;
    float timeRemaining = 0.0f;
    bool interruptible = true;

    if (uidStr == "player") {
        isCasting = gh->isCasting();
        isChannel = gh->isChanneling();
        spellId = gh->getCurrentCastSpellId();
        timeTotal = gh->getCastTimeTotal();
        timeRemaining = gh->getCastTimeRemaining();
        // Player interruptibility: always true for own casts (server controls actual interrupt)
        interruptible = true;
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnNil(L); }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { return luaReturnNil(L); }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { return luaReturnNil(L); }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { return luaReturnNil(L); }

    // Spell name + icon
    const std::string& name = gh->getSpellName(spellId);
    std::string iconPath = gh->getSpellIconPath(spellId);

    // Time values in milliseconds (WoW API convention)
    double startTimeMs = (nowSec - (timeTotal - timeRemaining)) * 1000.0;
    double endTimeMs   = (nowSec + timeRemaining) * 1000.0;

    // The real signatures, which are one wider than what this used to send:
    //
    //   UnitCastingInfo → name, nameSubtext, text, texture, startTime,
    //                     endTime, isTradeSkill, castID, notInterruptible
    //   UnitChannelInfo → the same without castID
    //
    // nameSubtext is the rank, and leaving it out shifted every value after it
    // by one. castingbarframe.lua destructures all nine by name, so it read the
    // icon path as the bar's text, a timestamp as the texture, and a boolean as
    // endTime - then computed (endTime - startTime) and raised, which is why no
    // cast bar was ever drawn rather than a wrong one.
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                             // nameSubtext
    // What the bar is labelled with, which is the third value and not the
    // first: castingbarframe.lua reads `text` into barText and never touches
    // `name`. An empty string there is a bar that fills up saying nothing -
    // no way to tell a Fireball from a Frostbolt, or a cast from a channel.
    //
    // The spell's own name, because that is what this is for every spell. It
    // differs only for a trade skill, where the real client puts the item
    // being made here, and this client has no such cast in flight to name.
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // text
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_QuestionMark");  // texture
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0); // isTradeSkill
    if (!wantChannel) {
        lua_pushnumber(L, spellId);                              // castID (UnitCastingInfo only)
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 8 : 9;
}

static int lua_UnitCastingInfo(lua_State* L) { return lua_UnitCastInfo(L, false); }
static int lua_UnitChannelInfo(lua_State* L) { return lua_UnitCastInfo(L, true); }

static int lua_CastSpellByName(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* name = luaL_checkstring(L, 1);
    if (!name || !*name) return 0;
    // "!Auto Shot": cast without the toggle (0x540310 goes straight to
    // 0x80DA40), so a repeat that is running is left running rather than
    // put away.
    const bool noToggle = *name == '!';
    if (noToggle) ++name;

    const uint32_t bestId = highestKnownRankByName(gh, name);
    if (noToggle && bestId != 0 && gh->getSpellHandler() &&
        gh->getSpellHandler()->getAutoRepeatSpellId() == bestId) {
        return 0;
    }
    if (bestId != 0) {
        // The same second argument, and the same branch of SECURE_ACTIONS.spell
        // reaches here: a spell named rather than numbered is cast by name, and
        // the unit clicked still says who it lands on.
        uint64_t target = 0;
        if (const char* unit = luaL_optstring(L, 2, nullptr)) {
            std::string u(unit);
            toLowerInPlace(u);
            target = resolveUnitGuid(gh, u);
        }
        if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
        gh->castSpell(bestId, target);
    }
    return 0;
}

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
    return 1;
}

// --- Spell Book Tab API ---

// GetNumSpellTabs() → count
static int lua_GetNumSpellTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getSpellBookTabs().size());
    return 1;
}

// GetSpellTabInfo(tabIndex) → name, texture, offset, numSpells,
//                             highestRankOffset, highestRankNumSpells
// tabIndex is 1-based; offset is 1-based global spell book slot
//
// Six values, and the last two are not optional. SpellBook_GetTabInfo unpacks
// all six and then, when ShowAllSpellRanks is off - which is the default -
// throws away offset and numSpells and uses the highest-rank pair instead. So
// a four-value answer leaves both nil on the ordinary path and the spellbook
// raises on the arithmetic, showing nothing at all.
//
// The comment here used to say four. Cutting the returns to match it would
// have emptied the spellbook, which is exactly what a stubbed four-value
// version did when it was tried.
static int lua_GetSpellTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(luaL_checknumber(L, 1));
    // An empty tab rather than one nil, because the caller does arithmetic on
    // what comes back without checking it.
    //
    // SpellBookFrame's own OnLoad ends with SpellBookSkillLineTab_OnClick(nil,
    // 1), and three frames down that divides numSpells by the page size. In
    // WoW tab one is General and always exists, so FrameXML never guards it -
    // here it does not exist until the spell list has arrived, and a single
    // nil made that a raise inside OnLoad, which loses the whole file.
    // SpellBookFrame.xml was failing to load outright.
    //
    // Six zeroes give a book with no pages, which is what a player with no
    // spells yet has, and the frame loads and fills in when they arrive.
    auto emptyTab = [](lua_State* s) {
        lua_pushstring(s, "");   // name
        lua_pushstring(s, "");   // texture
        lua_pushnumber(s, 0);    // offset
        lua_pushnumber(s, 0);    // numSpells
        lua_pushnumber(s, 0);    // highestRankOffset
        lua_pushnumber(s, 0);    // highestRankNumSpells
        return 6;
    };
    if (!gh || tabIdx < 1) {
        return emptyTab(L);
    }
    const auto& tabs = gh->getSpellBookTabs();
    if (tabIdx > static_cast<int>(tabs.size())) {
        return emptyTab(L);
    }
    // Compute offset: sum of spells in all preceding tabs (1-based)
    int offset = 0;
    int highestOffset = 0;
    for (int i = 0; i < tabIdx - 1; ++i) {
        offset += static_cast<int>(tabs[i].spellIds.size());
        highestOffset += static_cast<int>(tabs[i].highestRankIdx.size());
    }
    const auto& tab = tabs[tabIdx - 1];
    lua_pushstring(L, tab.name.c_str());           // name
    lua_pushstring(L, tab.texture.c_str());        // texture
    lua_pushnumber(L, offset);                     // offset (0-based for WoW compat)
    lua_pushnumber(L, tab.spellIds.size());        // numSpells
    // The highest-rank pair, which is what FrameXML actually reads: with
    // ShowAllSpellRanks off - the default - SpellBook_GetTabInfo throws away
    // the first offset and count and keeps these. Returning four values left
    // numSpells nil and the page count divided by nothing.
    //
    // A slot space of its own, as in the stock client: each tab's spells
    // with lower ranks dropped, numbered across tabs the same way. FrameXML
    // turns a slot in it back into a real one through
    // GetKnownSlotFromHighestRankSlot before asking anything about it.
    lua_pushnumber(L, highestOffset);              // highestRankOffset
    lua_pushnumber(L, tab.highestRankIdx.size());  // highestRankNumSpells
    return 6;
}

// GetSpellBookItemInfo(slot, bookType) → "SPELL", spellId
// slot is 1-based global spell book index
static int lua_GetSpellBookItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) {
        lua_pushstring(L, "SPELL");
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot; // 1-based
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            lua_pushstring(L, "SPELL");
            lua_pushnumber(L, tab.spellIds[idx - 1]);
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, 0);
    return 2;
}

// GetSpellBookItemName(slot, bookType) → name, subName
static int lua_GetSpellBookItemName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) { return luaReturnNil(L); }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            uint32_t spellId = tab.spellIds[idx - 1];
            const std::string& name = gh->getSpellName(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            // Spell.dbc's rank text: "Rank 3", "Passive", "Racial" - what
            // the book prints under the name.
            lua_pushstring(L, gh->getSpellRank(spellId).c_str()); // subName/rank
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushnil(L);
    return 1;
}


// ── The spellbook's older API ──────────────────────────────────────────────
//
// SpellBookFrame in WotLK uses both generations at once: the newer
// GetSpellBookItem* alongside GetSpellName and CastSpell, which take the same
// slot and book. The newer half was implemented and the older was not, so the
// panel could name a spell and not cast it.


/// GetSpellName(slot, bookType) → name, rank.
static int lua_GetSpellName(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Through spellIdForCall, which reads the book as well as the slot. The
    // spellbook passes SpellBookFrame.bookType to every one of these, and
    // resolving a pet slot through the player's tabs names one of the player's
    // own spells - the wrong answer wearing the shape of a right one.
    const uint32_t id = spellIdForCall(L, gh);
    if (id == 0) { return luaReturnNil(L); }
    const std::string& name = gh->getSpellName(id);
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
    // The rank text, which the book prints under the name and which tells
    // the ranks apart when ShowAllSpellRanks lists them all.
    lua_pushstring(L, gh->getSpellRank(id).c_str());
    return 2;
}

/// CastSpell(slot, bookType) - cast what is in that slot at the current
/// target, which is what clicking a spellbook entry does.
static int lua_CastSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same resolver, and here it decides what is cast: clicking a pet spell
    // resolved through the player's tabs cast whichever of the player's spells
    // sat at that index.
    const uint32_t id = spellIdForCall(L, gh);
    if (id != 0) gh->castSpell(id, gh->getTargetGuid());
    return 0;
}

/// IsPassiveSpell(slot, bookType) → whether the spell is passive, which is how
/// the book decides not to draw a cast button for it. Spell attributes are not
/// read from the DBC yet, so this answers no - which draws a button for a
/// passive rather than hiding a real one, the less wrong way round.
/// IsPassiveSpell(id or index, bookType) → whether the spell is never cast.
///
/// It answered no for everything, so the spell book drew a passive with the
/// same cast border as an ability and let it be dragged onto the action bar,
/// where it would sit doing nothing. Spell.dbc's base attribute word carries
/// it - bit 6 - beside the Ex word the client already read for
/// interruptibility.
static int lua_IsPassiveSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    // Both forms: IsPassiveSpell(spellId) and IsPassiveSpell(slot, bookType),
    // which is the one the spellbook uses - and reading a slot as an id
    // answered for whichever spell happens to carry the number 1, 2 or 3.
    // The result decides whether a spell is drawn as castable at all.
    const uint32_t id = spellIdForCall(L, gh);
    lua_pushboolean(L, gh->isSpellPassive(id) ? 1 : 0);
    return 1;
}

/// IsSelectedSpell(slot, bookType) → whether the book draws it checked.
///
/// spellbookframe.lua re-asks on CURRENT_SPELL_CAST_CHANGED,
/// UPDATE_SHAPESHIFT_FORM and TRADE_SKILL_SHOW/CLOSE (SpellButton_OnShow),
/// which names what "selected" covers: the spell being cast or channelled
/// (or auto-repeating, or queued for the next swing), the stance or form
/// the player is in, and the profession whose window is open. It answered
/// false for all of them, so no spell button ever lit.
static int lua_IsSelectedSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t id = gh ? spellIdForCall(L, gh) : 0;
    bool selected = false;
    if (id != 0) {
        selected = id == gh->getActiveActionSpellId() ||
                   id == gh->getCurrentCastSpellId() ||
                   id == gh->getAutoRepeatSpellId();
        if (!selected) {
            // A stance, form or presence: its own aura is the
            // SHAPESHIFT (36) aura the player carries while in it.
            gh->loadSpellNameCache();
            const auto& cache = gh->spellNameCacheRef();
            auto it = cache.find(id);
            if (it != cache.end()) {
                for (uint32_t aura : it->second.effectAuraIds) {
                    if (aura == 36 && gh->playerCarriesAura(id)) { selected = true; break; }
                }
            }
        }
        if (!selected && gh->isCraftingWindowOpen()) {
            auto* sh = gh->getSpellHandler();
            const uint32_t line = sh ? sh->tradeskillOpenerSkillLine(id) : 0;
            selected = line != 0 && line == gh->getCraftingSkillLine();
        }
    }
    lua_pushboolean(L, selected ? 1 : 0);
    return 1;
}

/// GetSpellAutocast(slot, bookType) → autocastable, currently autocasting.
/// Pet spells only - the player's book has no autocast, and answers nil,nil.
/// For the pet book both come from SMSG_PET_SPELLS' per-spell state byte
/// (0x80 autocastable, 0x40 on), which spellbookframe.lua turns into the
/// AutoCastable border and the rotating AutoCast shine.
static int lua_GetSpellAutocast(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* book = lua_tostring(L, 2);
    if (!gh || !book || std::string(book) != "pet") {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const uint32_t id = spellIdForCall(L, gh);
    lua_pushboolean(L, id && gh->isPetSpellAutocastable(id) ? 1 : 0);
    lua_pushboolean(L, id && gh->isPetSpellAutocast(id) ? 1 : 0);
    return 2;
}
/// GetKnownSlotFromHighestRankSlot(slot) → the slot actually known. The book
/// pages through the highest-rank slot space (GetSpellTabInfo's last pair)
/// and maps each slot back to the all-ranks one that GetSpellName, CastSpell
/// and the rest take. A slot past the end comes back as 0, which those answer
/// with nothing - passing it through unchanged would land on a real spell,
/// because the all-ranks space is the longer of the two.
static int lua_GetKnownSlotFromHighestRankSlot(lua_State* L) {
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    auto* gh = getGameHandler(L);
    if (!gh || slot < 1) { lua_pushnumber(L, slot); return 1; }
    int idx = slot;   // 1-based within the highest-rank space
    int base = 0;     // all-ranks slots before this tab
    for (const auto& tab : gh->getSpellBookTabs()) {
        const int n = static_cast<int>(tab.highestRankIdx.size());
        if (idx <= n) {
            lua_pushnumber(L, base + static_cast<int>(tab.highestRankIdx[idx - 1]) + 1);
            return 1;
        }
        idx -= n;
        base += static_cast<int>(tab.spellIds.size());
    }
    lua_pushnumber(L, 0);
    return 1;
}

/// UpdateSpells() - redraw the spell book page.
///
/// The list itself needs no rebuilding, which is why this was a no-op. But in
/// WoW the call is what makes the client announce SPELLS_CHANGED, and that
/// event is the only thing that redraws a spell button: SpellButton_OnEvent
/// answers it with SpellButton_UpdateButton, and nothing else calls that.
///
/// Six places in spellbookframe.lua call this, and every one of them is a
/// "now redraw the page" - after a skill-line tab is clicked, after a page
/// turn, after the book type changes. SpellBookFrame_ShowSpells only calls
/// Show() on buttons that are already shown, so no OnShow fires and without
/// the event the page kept whatever it was displaying before. That is the
/// whole of "the spell book does not update when the tab changes": the offset
/// moved and nothing ever re-read it.
static int lua_UpdateSpells(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    // Safe against the obvious loop: SpellBookFrame_OnEvent answers
    // SPELLS_CHANGED with SpellBookFrame_Update() and no argument, and the
    // branch that would call back here only runs when it is passed one.
    if (engine) engine->fireEvent("SPELLS_CHANGED", {});
    return 0;
}


static int lua_GetSpellDescription(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    // The shared formatter, which is what every other description path uses.
    // The local one this replaced knew $s and $o and wrote a literal "X" for
    // $d and the rest, so a duration read as "lasts X sec".
    const std::string desc =
        gh->formatSpellDescription(spellId, gh->getSpellDescription(spellId));
    lua_pushstring(L, desc.c_str());
    return 1;
}

// __WoweeAuraDescription(spellId) -> the aura tooltip's text, formatted.
//
// Not a WoW function: the real client fills the aura tooltip in C from
// Spell.dbc's ToolTip column, and the GameTooltip methods that do that here
// are written in Lua, so they need the text handed to them. The ToolTip column
// is not the Description - Fortitude's buff reads "Increases Stamina by 165."
// where its spell book entry describes casting it - and an aura with no ToolTip
// text shows no description line at all, as it does in the real client.
static int lua_WoweeAuraDescription(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    const uint32_t spellId = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
    if (spellId == 0) { lua_pushstring(L, ""); return 1; }
    const std::string desc =
        gh->formatSpellDescription(spellId, gh->getSpellAuraDescription(spellId));
    lua_pushstring(L, desc.c_str());
    return 1;
}


static int lua_GetEnchantInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t enchantId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    std::string name = gh->getEnchantName(enchantId);
    if (name.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, name.c_str());
    return 1;
}

static int lua_GetSpellCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Three values on this path too, like every other. The cooldown frame does
    // `start > 0 and duration > 0 and enable > 0`, and `and` short-circuits, so
    // a missing third value is only ever reached when the first two say a
    // cooldown is running - which this path cannot say, because it answers
    // zero. Safe by accident rather than by design, and the next person to give
    // this branch a real start time would have found out the hard way.
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    // A name, an id, or a book slot with the book beside it - the spellbook
    // asks in the last of those three.
    const uint32_t spellId = spellIdForCall(L, gh);
    float cd = gh->getSpellCooldown(spellId);
    // Also check GCD - if spell has no individual cooldown but GCD is active,
    // return the GCD timing (this is how WoW handles it). Only for a spell
    // the GCD actually holds: Spell.dbc StartRecoveryCategory 133. A passive,
    // or Auto Shot / Attack (category 0), swirled with every cast. An unknown
    // category (a layout that does not map the column) keeps the old answer.
    float gcdRem = gh->getGCDRemaining();
    float gcdTotal = gh->getGCDTotal();
    if (spellId != 0) {
        gh->loadSpellNameCache();
        const auto& cache = gh->spellNameCacheRef();
        auto it = cache.find(spellId);
        const bool passive = gh->isSpellPassive(spellId);
        const uint32_t cat = it != cache.end() ? it->second.startRecoveryCategory : 0xFFFFFFFFu;
        if (passive || (cat != 0xFFFFFFFFu && cat != 133)) gcdRem = 0.0f;
        // SPELL_ATTR0_COOLDOWN_ON_EVENT (0x02000000): unusable while its
        // effect is up and the cooldown starts when it ends (Stealth, Prowl).
        // Stock answers enable 0 meanwhile, which greys the button.
        if (it != cache.end() && (it->second.attr & 0x02000000u) &&
            gh->playerCarriesAura(spellId)) {
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 3;
        }
    }

    // WoW returns (start, duration, enabled) where remaining = start + duration - GetTime()
    double nowSec = luaGetTimeNow();

    if (cd > 0.01f) {
        // Spell-specific cooldown (longer than GCD). The pair asked for is
        // (start, duration), not (now, remaining) - the cooldown frame draws a
        // sweep of `duration` beginning at `start`, so saying it began now with
        // a length of whatever is left restarts the swirl at full every time
        // the interface asks. It asks on every ACTIONBAR_UPDATE_COOLDOWN, which
        // fires on every cast, so a five-minute cooldown looked like it kept
        // starting over. Wound back the same way the GCD branch below already
        // does it.
        double total = gh->getSpellCooldownTotal(spellId);
        if (total < cd) total = cd;   // a total never recorded falls back to what is left
        double start = nowSec - (total - cd);
        lua_pushnumber(L, start);
        lua_pushnumber(L, total);
    } else if (gcdRem > 0.01f) {
        // GCD is active - return GCD timing
        double elapsed = gcdTotal - gcdRem;
        double start = nowSec - elapsed;
        lua_pushnumber(L, start);
        lua_pushnumber(L, gcdTotal);
    } else {
        lua_pushnumber(L, 0);       // not on cooldown
        lua_pushnumber(L, 0);
    }
    lua_pushnumber(L, 1);           // enabled
    return 3;
}

static int lua_HasTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasTarget());
    return 1;
}

// TargetUnit(unitId) - set current target
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    // Not a unit token: a name, as stock TargetUnit takes one - the nearest
    // unit of that name.
    if (guid == 0) {
        const auto& mi = gh->getMovementInfo();
        float best = 1e30f;
        for (const auto& [g, e] : gh->getEntityManager().getEntities()) {
            if (!e || !e->isUnit()) continue;
            std::string n = std::static_pointer_cast<game::Unit>(e)->getName();
            toLowerInPlace(n);
            if (n.empty() || n != uidStr) continue;
            const float dx = e->getX() - mi.x, dy = e->getY() - mi.y, dz = e->getZ() - mi.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) { best = d; guid = g; }
        }
    }
    if (guid != 0) gh->setTarget(guid);
    return 0;
}

// InteractUnit(unit) - right-click a unit: talk to it, open its vendor or
// flight map (stock 3.3.5a InputControl).
static int lua_InteractUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    std::string uidStr(luaL_checkstring(L, 1));
    toLowerInPlace(uidStr);
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0 && guid != gh->getPlayerGuid()) gh->interactWithNpc(guid);
    return 0;
}

// ClearTarget() - clear current target. Answers whether there was one, which
// ToggleGameMenu reads to stop there instead of also opening the game menu.
static int lua_ClearTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool hadTarget = gh && gh->hasTarget();
    if (gh) gh->clearTarget();
    lua_pushboolean(L, hadTarget);
    return 1;
}

// FocusUnit(unitId) - set focus target
static int lua_FocusUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, nullptr);
    if (!uid || !*uid) return 0;
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setFocus(guid);
    return 0;
}

// ClearFocus() - clear focus target
static int lua_ClearFocus(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearFocus();
    return 0;
}

// AssistUnit(unitId) - target whatever the given unit is targeting, with the
// stock errors and assistAttack (0x525EB0, CombatHandler::assistUnit).
static int lua_AssistUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->getCombatHandler()) return 0;
    const char* uid = luaL_optstring(L, 1, "");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const bool namedOther = !uidStr.empty() && uidStr != "target";
    const uint64_t guid = resolveUnitOrName(gh, uidStr.empty() ? std::string("target") : uidStr);
    gh->getCombatHandler()->assistUnit(guid, namedOther);
    return 0;
}

// TargetLastTarget() - re-target previous target
static int lua_TargetLastTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetLastTarget();
    return 0;
}

// TargetNearestEnemy() - tab-target nearest enemy
// The argument is the direction, and it was being thrown away. Bindings.xml
// binds two keys to each of these - `TargetNearestEnemy()` forwards and
// `TargetNearestEnemy(1)` backwards, with a comment on that line saying so -
// so cycling backwards through targets went forwards instead, and the second
// key of every such pair did what the first did.
static bool wantsReverse(lua_State* L) {
    return lua_toboolean(L, 1) != 0;
}

// TargetNearest([reverse]) - any unit, by the same rule (mode 0, 0x525A90).
static int lua_TargetNearest(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && gh->getCombatHandler())
        gh->getCombatHandler()->targetNearest(game::TargetNearestMode::Any, wantsReverse(L));
    return 0;
}

static int lua_TargetNearestEnemy(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetEnemy(wantsReverse(L));
    return 0;
}

// TargetNearestFriend([reverse]) - target nearest friendly unit
static int lua_TargetNearestFriend(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetFriend(wantsReverse(L));
    return 0;
}

// GetRaidTargetIndex(unit) → icon index (1-8) or nil
static int lua_GetRaidTargetIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    uint8_t mark = gh->getEntityRaidMark(guid);
    if (mark == 0xFF) { return luaReturnNil(L); }
    lua_pushnumber(L, mark + 1); // WoW uses 1-indexed (1=Star, 2=Circle, ... 8=Skull)
    return 1;
}

// SetRaidTarget(unit, index) - set raid marker (1-8, or 0 to clear)
static int lua_SetRaidTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    int index = static_cast<int>(luaL_checknumber(L, 2));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    if (index >= 1 && index <= 8)
        gh->setRaidMark(guid, static_cast<uint8_t>(index - 1));
    else if (index == 0)
        gh->setRaidMark(guid, 0xFF); // clear
    return 0;
}

// GetSpellPowerCost(spellId) → {{ type=powerType, cost=manaCost, name=powerName }}

static int lua_GetSpellPowerCost(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_newtable(L); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    auto data = gh->getSpellData(spellId);
    lua_newtable(L); // outer table (array of cost entries)
    if (data.manaCost > 0) {
        lua_newtable(L); // cost entry
        lua_pushnumber(L, data.powerType);
        lua_setfield(L, -2, "type");
        const bool tenths = data.powerType == 1 || data.powerType == 6;
        lua_pushnumber(L, tenths ? data.manaCost / 10 : data.manaCost);
        lua_setfield(L, -2, "cost");
    
        lua_pushstring(L, data.powerType < 7 ? kLuaPowerNames[data.powerType] : "MANA");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, 1); // outer[1] = entry
    }
    return 1;
}

// --- GetSpellInfo / GetSpellTexture ---
// GetSpellInfo(spellIdOrName) -> name, rank, icon, castTime, minRange, maxRange, spellId
static int lua_GetSpellInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        spellId = highestKnownRankByName(gh, name);
    }

    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }

    lua_pushstring(L, name.c_str());                        // 1: name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                        // 2: rank
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);                                     // 3: icon texture path
    // The client's order, which this did not follow.
    //
    //   name, rank, icon, cost, isFunnel, powerType, castTime, minRange, maxRange
    //
    // Seven values were returned and only the first three were in the right
    // places. Everything asking for a cost got a cast time in milliseconds,
    // everything asking for a cast time got the spell id - so a spell read as
    // taking several thousand seconds to cast - and the two ranges were nil.
    // The interface unpacks all nine in one line in multicastactionbarframe,
    // and any addon reading a spell does the same.
    //
    // isFunnel is the one value with nothing behind it here. False is right
    // for every spell but a handful of warlock drains, and it is what the
    // callers branch on rather than display.
    auto spellData = gh->getSpellData(spellId);
    // Rage and runic power costs are stored in tenths, like the power itself;
    // the client reports whole points, the same scale UnitPower answers in.
    const bool tenths = spellData.powerType == 1 || spellData.powerType == 6;
    lua_pushnumber(L, tenths ? spellData.manaCost / 10 : spellData.manaCost); // 4: cost
    lua_pushboolean(L, 0);                                   // 5: isFunnel
    lua_pushnumber(L, spellData.powerType);                  // 6: powerType
    lua_pushnumber(L, spellData.castTimeMs);                 // 7: castTime (ms)
    lua_pushnumber(L, spellData.minRange);                   // 8: minRange (yards)
    lua_pushnumber(L, spellData.maxRange);                   // 9: maxRange (yards)
    return 9;
}

// GetSpellTexture(spellIdOrName) -> icon texture path string
static int lua_GetSpellTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    return 1;
}

// GetItemInfo(itemId) -> name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, vendorPrice

static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }
    const std::string link = game::spellChatLink(spellId, name);
    lua_pushstring(L, link.c_str());
    return 1;
}

/// CancelUnitBuff(unit, indexOrName [, filterOrRank]) - both forms.
///
/// The interface uses both and this took only the index, through
/// luaL_checknumber - so the two name forms raised on the spot rather than
/// cancelling anything. The possess bar cancels the aura holding the player in
/// a vehicle by name, and /cancelaura passes what was typed; both were an
/// error message where an aura should have dropped.
///
/// The third argument is a filter for the index form and a rank for the name
/// form, which is how WoW spells it. Only HELPFUL is answerable - a debuff
/// cannot be cancelled - so the filter decides nothing here beyond confirming
/// buffs are what is being counted.
static int lua_CancelUnitBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    if (uidStr != "player") return 0;   // only one's own auras can be cancelled

    const auto& auras = gh->getPlayerAuras();
    const bool byName = lua_isstring(L, 2) && !lua_isnumber(L, 2);

    if (byName) {
        std::string want(lua_tostring(L, 2));
        toLowerInPlace(want);
        for (const auto& a : auras) {
            if (a.isEmpty() || (a.flags & 0x80) != 0) continue;
            std::string have = gh->getSpellName(a.spellId);
            toLowerInPlace(have);
            if (have == want) { gh->cancelAura(a.spellId); return 0; }
        }
        return 0;
    }

    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    int buffCount = 0;
    for (const auto& a : auras) {
        if (a.isEmpty()) continue;
        if ((a.flags & 0x80) != 0) continue;   // debuffs are not cancellable
        if (++buffCount == index) { gh->cancelAura(a.spellId); return 0; }
    }
    return 0;
}

// CastSpellByID(spellId) - cast spell by numeric ID
static int lua_CastSpellByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (spellId == 0) return 0;
    // The second argument names who it is cast on, and this is the click-cast
    // path: SECURE_ACTIONS.spell does CastSpellByID(spellID, unit) with the
    // unit off the button that was clicked. Ignoring it cast on whatever was
    // targeted instead, so healing a party member through their frame hit the
    // current target.
    uint64_t target = 0;
    if (const char* unit = luaL_optstring(L, 2, nullptr)) {
        std::string u(unit);
        toLowerInPlace(u);
        target = resolveUnitGuid(gh, u);
    }
    if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
    gh->castSpell(spellId, target);
    return 0;
}

static int lua_IsUsableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }

    if (spellId == 0 || !gh->getKnownSpells().count(spellId)) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    float cd = gh->getSpellCooldown(spellId);
    bool onCooldown = (cd > 0.1f);
    bool noMana = false;
    if (!onCooldown) {
        auto spellData = gh->getSpellData(spellId);
        if (spellData.manaCost > 0) {
            auto playerEntity = gh->getEntityManager().getEntity(gh->getPlayerGuid());
            if (playerEntity) {
                auto* unit = dynamic_cast<game::Unit*>(playerEntity.get());
                if (unit && unit->getPowerByType(spellData.powerType) < spellData.manaCost) {
                    noMana = true;
                }
            }
        }
    }
    lua_pushboolean(L, (onCooldown || noMana) ? 0 : 1);
    lua_pushboolean(L, noMana ? 1 : 0);
    return 2;
}


void registerSpellLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetTotemInfo",     lua_GetTotemInfo},
                // DestroyTotem(slot) - pull one down early. The totem bar's
                // right-click, and the slot is all the request carries.
                {"DestroyTotem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            // The bar counts from one and the request from zero.
            if (gh && slot >= 1) gh->destroyTotem(slot - 1);
            return 0;
        }},
                {"GetTotemTimeLeft", lua_GetTotemTimeLeft},
                {"SpellStopCasting", [](lua_State* L) -> int {
            // 0x809EA0: the cast or channel stops; with none, a repeating
            // Auto Shot or Shoot does. 1 when something stopped, else nil.
            auto* gh = getGameHandler(L);
            bool stopped = false;
            if (gh && gh->isCasting()) {
                gh->cancelCast();
                stopped = true;
            } else if (gh && gh->getSpellHandler() &&
                       gh->getSpellHandler()->getAutoRepeatSpellId() != 0) {
                gh->getSpellHandler()->cancelAutoRepeat();
                stopped = true;
            }
            if (stopped) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                {"SpellStopTargeting", [](lua_State* L) -> int {
            // The pending spell put down (0x809E30 -> 0x806200 with
            // DONT_REPORT): a spell waiting for its point on the ground, or
            // the item cursor a stone or an oil arms - the stock client keeps
            // both in the one pending spell (0xD3F4E4). 1 when there was one,
            // which is how ToggleGameMenu knows escape was spent on it.
            auto* gh = getGameHandler(L);
            bool wasTargeting = false;
            if (gh && gh->getSpellHandler() && gh->getSpellHandler()->stopGroundTargeting())
                wasTargeting = true;
            if (gh && gh->isAwaitingItemTarget()) {
                gh->cancelItemTargeting();
                wasTargeting = true;
            }
            if (wasTargeting) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                {"SpellIsTargeting", [](lua_State* L) -> int {
            // 0x7FDCD0: whether a spell is pending (0xD3F4E4 != 0).
            auto* gh = getGameHandler(L);
            const bool targeting = gh && (gh->isAwaitingItemTarget() ||
                (gh->getSpellHandler() && gh->getSpellHandler()->isGroundTargeting()));
            if (targeting) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                {"IsSpellInRange",    lua_IsSpellInRange},
                {"UnitBuff",          lua_UnitBuff},
                {"UnitDebuff",        lua_UnitDebuff},
                {"UnitAura",          lua_UnitAuraGeneric},
                // The 1.12 names. A vanilla interface reads its buffs through
                // these and through nothing else.
                {"GetPlayerBuff",             lua_GetPlayerBuff},
                {"GetPlayerBuffTexture",      lua_GetPlayerBuffTexture},
                {"GetPlayerBuffTimeLeft",     lua_GetPlayerBuffTimeLeft},
                {"GetPlayerBuffApplications", lua_GetPlayerBuffApplications},
                {"GetPlayerBuffDispelType",   lua_GetPlayerBuffDispelType},
                {"CancelPlayerBuff",          lua_CancelPlayerBuff},
                {"__WoweePlayerBuffInfo",     lua_WoweePlayerBuffInfo},
                {"UnitCastingInfo",   lua_UnitCastingInfo},
                {"UnitChannelInfo",   lua_UnitChannelInfo},
                {"CastSpellByName",   lua_CastSpellByName},
                {"CastSpellByID",       lua_CastSpellByID},
                {"IsSpellKnown",      lua_IsSpellKnown},
                {"GetNumSpellTabs",   lua_GetNumSpellTabs},
                {"GetSpellTabInfo",   lua_GetSpellTabInfo},
                {"GetSpellBookItemInfo", lua_GetSpellBookItemInfo},
                {"GetSpellBookItemName", lua_GetSpellBookItemName},
                {"GetSpellName",      lua_GetSpellName},
                {"SpellTargetUnit",   lua_SpellTargetUnit},
                {"SpellTargetItem",   lua_SpellTargetItem},
                {"DropItemOnUnit",    lua_DropItemOnUnit},
                {"SetMultiCastSpell", lua_SetMultiCastSpell},
                {"CastSpell",         lua_CastSpell},
                {"IsPassiveSpell",    lua_IsPassiveSpell},
                // 1.12's name for the same question - renamed at 2.0, and
                // spellbookframe.lua calls it twice from
                // SpellButton_UpdateButton, which runs every time the book is
                // drawn. Unbound, opening the spellbook raised.
                {"IsSpellPassive",    lua_IsPassiveSpell},
                {"IsSelectedSpell",   lua_IsSelectedSpell},
                {"GetSpellAutocast",  lua_GetSpellAutocast},
                {"GetKnownSlotFromHighestRankSlot", lua_GetKnownSlotFromHighestRankSlot},
                {"UpdateSpells",      lua_UpdateSpells},
                {"GetSpellCooldown",  lua_GetSpellCooldown},
                {"GetSpellPowerCost", lua_GetSpellPowerCost},
                {"GetSpellDescription", lua_GetSpellDescription},
                {"__WoweeAuraDescription", lua_WoweeAuraDescription},
                {"GetEnchantInfo",     lua_GetEnchantInfo},
                {"GetSpellInfo",      lua_GetSpellInfo},
                {"GetSpellTexture",   lua_GetSpellTexture},
                {"GetSpellLink",         lua_GetSpellLink},
                {"IsUsableSpell",        lua_IsUsableSpell},
                {"CancelUnitBuff",      lua_CancelUnitBuff},
                {"HasTarget",         lua_HasTarget},
                {"TargetUnit",        lua_TargetUnit},
                {"InteractUnit",      lua_InteractUnit},
                {"ClearTarget",       lua_ClearTarget},
                {"FocusUnit",         lua_FocusUnit},
                {"ClearFocus",        lua_ClearFocus},
                {"AssistUnit",        lua_AssistUnit},
                {"TargetLastTarget",  lua_TargetLastTarget},
                {"TargetNearest",       lua_TargetNearest},
                {"TargetNearestEnemy",  lua_TargetNearestEnemy},
                {"TargetNearestFriend", lua_TargetNearestFriend},
                {"GetRaidTargetIndex",  lua_GetRaidTargetIndex},
                {"SetRaidTarget",       lua_SetRaidTarget},
                {"IsPlayerSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId) ? 1 : 0);
            return 1;
        }},
                {"IsSpellOverlayed", [](lua_State* L) -> int {
            (void)L; lua_pushboolean(L, 0); return 1; // No proc overlay tracking
        }},
                {"IsCurrentSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getCurrentCastSpellId() == spellId ? 1 : 0);
            return 1;
        }},
                {"IsAutoRepeatSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* name = luaL_optstring(L, 1, "");
            const uint32_t active = gh ? gh->getAutoRepeatSpellId() : 0;
            lua_pushboolean(L, active != 0 && gh->getSpellName(active) == name);
            return 1;
        }},
                {"CastShapeshiftForm", [](lua_State* L) -> int {
            // CastShapeshiftForm(index) - cast the spell for the given form slot
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) return 0;
            uint8_t classId = gh->getPlayerClass();
            const auto forms = game::knownShapeshiftForms(classId, gh->getKnownSpells());
            if (static_cast<size_t>(index) > forms.size()) return 0;
            gh->castSpell(forms[static_cast<size_t>(index) - 1].spellId, 0);
            return 0;
        }},
                {"CancelShapeshiftForm", [](lua_State* L) -> int {
            // Cancel current form - cast spell 0 or cancel aura
            auto* gh = getGameHandler(L);
            if (gh && gh->getShapeshiftFormId() != 0) {
                // Cancelling a form is done by re-casting the same form spell
                // For simplicity, just note that the server will handle it
            }
            return 0;
        }},
                // GetShapeshiftFormCooldown is defined in the bootstrap, over
                // GetShapeshiftFormInfo and GetSpellCooldown, because those are
                // in two different files and it needs both. A stub here would
                // be dead - the bootstrap runs after these are registered - and
                // framexml_lua_override_check exists to catch exactly that.
                {"GetShapeshiftForm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getShapeshiftFormId() : 0);
            return 1;
        }},
                {"GetNumShapeshiftForms", [](lua_State* L) -> int {
            // Return count based on player class
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            uint8_t classId = gh->getPlayerClass();
            // Only the forms this character has actually learned, from the
            // one table GetShapeshiftFormInfo and CastShapeshiftForm also read.
            // The bar walks 1..this and asks the other two about each index, so
            // a count taken from a different list describes one form and casts
            // another.
            lua_pushnumber(L, static_cast<double>(
                game::knownShapeshiftForms(classId, gh->getKnownSpells()).size()));
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
