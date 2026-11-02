#pragma once

// The stock 3.3.5a (build 12340) rules for dying, the ghost world, corpses,
// resurrection and loot rolls, as numbers and pure functions. Each one names
// the Wow.exe function it was read from; the prose is
// docs/re/death-ghost-loot.md.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::stock_death {

// ---- Player field bits these rules read ----
/// PLAYER_FLAGS (0x96).
inline constexpr uint32_t kPlayerFlagGhost = 0x10;
inline constexpr uint32_t kPlayerFlagOutOfBounds = 0x4000;  // IsOutOfBounds (0x6123c0)
/// PLAYER_FIELD_BYTES (0x4AD) byte 0.
inline constexpr uint8_t kFieldByteReleaseTimer = 0x08;
inline constexpr uint8_t kFieldByteNoReleaseWindow = 0x10;
/// PLAYER_FIELD_BYTES2 (0x4CD) byte 3.
inline constexpr uint8_t kBytes2InvisibilityGlow = 0x40;

/// The release countdown a death starts with (0x6dc070).
inline constexpr int32_t kReleaseTimerMs = 360000;

/// What 0x6dc070 does at the moment of death, as the delay before the
/// automatic release: 0 with NO_RELEASE_WINDOW (release at once), six minutes
/// with RELEASE_TIMER unless the player is out of bounds, else -1 for no
/// timer at all (instances and battlegrounds, where the server sends neither).
[[nodiscard]] inline int32_t releaseDelayMs(uint8_t fieldBytes0, uint32_t playerFlags) {
    if (fieldBytes0 & kFieldByteNoReleaseWindow) return 0;
    if ((fieldBytes0 & kFieldByteReleaseTimer) && !(playerFlags & kPlayerFlagOutOfBounds)) {
        return kReleaseTimerMs;
    }
    return -1;
}

/// GetReleaseTimeRemaining (0x516210): whole seconds, truncated; -1 when the
/// death has no timer; 0 once it has run out.
[[nodiscard]] inline int releaseSecondsRemaining(int32_t delayMs, uint64_t deathMs, uint64_t nowMs) {
    if (delayMs < 0) return -1;
    const int64_t left = static_cast<int64_t>(deathMs) + delayMs - static_cast<int64_t>(nowMs);
    if (left <= 0) return 0;
    return static_cast<int>(left / 1000);
}

// ---- The screen effect (0x4f88b0) ----
inline constexpr uint32_t kAuraScreenEffect = 260;  // SPELL_AURA_SCREEN_EFFECT
inline constexpr uint32_t kScreenEffectGhost = 1;   // "Ghost Screen Effect"
inline constexpr uint32_t kScreenEffectInvisGlow = 81;  // "Invis Screen Effect"

/// Which ScreenEffect.dbc row the local player's world is drawn and heard
/// with. The last aura (in slot order) carrying SCREEN_EFFECT wins with its
/// misc value; without one a ghost gets row 1 and an invisibility glow row
/// 81; otherwise none (0). A corpse that has not released has no effect.
///
/// `auraScreenEffects` holds the misc value of every SCREEN_EFFECT aura on
/// the player, in slot order.
[[nodiscard]] inline uint32_t screenEffectId(const std::vector<uint32_t>& auraScreenEffects,
                                             uint32_t playerFlags, uint8_t bytes2Byte3) {
    if (!auraScreenEffects.empty()) return auraScreenEffects.back();
    if (playerFlags & kPlayerFlagGhost) return kScreenEffectGhost;
    if (bytes2Byte3 & kBytes2InvisibilityGlow) return kScreenEffectInvisGlow;
    return 0;
}

/// ScreenEffect.dbc's Effect column (0x4f7020): which full-screen pass.
enum class ScreenEffectKind : uint32_t { Glow = 0, Death = 1, Nether = 2, Special = 3 };

/// A ScreenEffect.dbc row, the columns the client reads.
struct ScreenEffectRow {
    uint32_t id = 0;
    uint32_t effect = 0;          ///< ScreenEffectKind
    int32_t lightParamsSlot = -1; ///< Light.dbc slot override (0x7ecec0); -1 none, >7 none
    uint32_t soundAmbienceId = 0; ///< top music/ambience slot (0x4c8fa0)
    uint32_t zoneMusicId = 0;
};

/// The Light.dbc slot the row forces (0x7ecec0): -1 when none or out of range.
[[nodiscard]] inline int32_t lightSlotOverride(const ScreenEffectRow& row) {
    if (row.id == 0) return -1;
    if (row.lightParamsSlot < 0 || row.lightParamsSlot > 7) return -1;
    return row.lightParamsSlot;
}
/// The death LightParams slot (DayNight 0xd38b5c's set): the ghost row's.
inline constexpr int32_t kLightSlotDeath = 4;

// ---- The corpse ----
/// 0x51f5c0: in range when the squared 3D distance is under 1600 (40 yd,
/// 0xac8a78). The event is CORPSE_IN_INSTANCE rather than CORPSE_IN_RANGE
/// when the map the corpse lies on is not the map the position is on (a
/// corpse inside an instance, reached at its entrance).
inline constexpr float kCorpseRangeSq = 1600.0f;
[[nodiscard]] inline bool corpseInRange(float distSq) { return distSq < kCorpseRangeSq; }

// ---- SMSG_RESURRECT_REQUEST (0x6dbd00) ----
/// After the guid, the name length and the name (that many bytes, its
/// terminator included), two bytes: the first is ResurrectHasSickness, the
/// second ResurrectHasTimer. Whatever follows is not read.
struct ResurrectOffer {
    bool sickness = false;
    bool timer = false;
};
[[nodiscard]] inline ResurrectOffer resurrectOffer(uint8_t first, uint8_t second) {
    return {first != 0, second != 0};
}

// ---- Loot rolls (LootRoll.cpp) ----
/// At most four GroupLootFrames are up; a fifth START_LOOT_ROLL waits until
/// one closes (0x6fb2c0, 0x6fb180).
inline constexpr int kMaxShownLootRolls = 4;

/// Expand a GlobalStrings format the way the stock client's printf does for
/// these lines: %s and %d take the next argument, %N$s / %N$d the Nth.
[[nodiscard]] inline std::string formatGlobal(const std::string& fmt,
                                              const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        const char c = fmt[i];
        if (c != '%' || i + 1 >= fmt.size()) { out += c; continue; }
        size_t j = i + 1;
        if (fmt[j] == '%') { out += '%'; i = j; continue; }
        size_t pos = 0;
        size_t k = j;
        while (k < fmt.size() && fmt[k] >= '0' && fmt[k] <= '9') { pos = pos * 10 + (fmt[k] - '0'); ++k; }
        if (k < fmt.size() && fmt[k] == '$' && pos > 0) {
            j = k + 1;
        } else {
            pos = 0;
        }
        if (j < fmt.size() && (fmt[j] == 's' || fmt[j] == 'd')) {
            const size_t idx = pos > 0 ? pos - 1 : next++;
            if (idx < args.size()) out += args[idx];
            i = j;
        } else {
            out += c;
        }
    }
    return out;
}

/// SMSG_LOOT_ROLL's chat line (0x6fa840). The vote is sent with roll number
/// 128 (negative as the signed byte the client reads it as) for a choice not
/// yet rolled; a pass is that with type 0, need is number 0 type 0. Returns
/// the GlobalStrings key and its arguments; an empty key means no line
/// (showLootSpam off prints only the player's own automatic passes).
struct LootRollLine {
    std::string key;
    std::vector<std::string> args;
};
[[nodiscard]] inline LootRollLine lootRollVoteLine(bool self, int8_t rollNumber, uint8_t rollType,
                                                   bool autoPass, bool female, bool verbose,
                                                   const std::string& name, const std::string& item) {
    if (!verbose && !(self && autoPass)) return {};
    const std::string num = std::to_string(static_cast<int>(rollNumber));
    auto rolled = [&](uint8_t type) -> LootRollLine {
        if (type == 2) return {"LOOT_ROLL_ROLLED_GREED", {num, item, name}};
        if (type == 3) return {"LOOT_ROLL_ROLLED_DE", {num, item, name}};
        return {"LOOT_ROLL_ROLLED_NEED", {num, item, name}};
    };
    if (rollType == 2 && rollNumber < 0) {
        return self ? LootRollLine{"LOOT_ROLL_GREED_SELF", {item}}
                    : LootRollLine{"LOOT_ROLL_GREED", {name, item}};
    }
    if (rollType == 3) {
        if (rollNumber < 0) {
            return self ? LootRollLine{"LOOT_ROLL_DISENCHANT_SELF", {item}}
                        : LootRollLine{"LOOT_ROLL_DISENCHANT", {name, item}};
        }
        return rolled(rollType);
    }
    if (rollNumber < 0) {
        if (self) {
            return {autoPass ? "LOOT_ROLL_PASSED_SELF_AUTO" : "LOOT_ROLL_PASSED_SELF", {item}};
        }
        if (autoPass) {
            return {female ? "LOOT_ROLL_PASSED_AUTO_FEMALE" : "LOOT_ROLL_PASSED_AUTO", {name, item}};
        }
        return {"LOOT_ROLL_PASSED", {name, item}};
    }
    if (rollType == 0) {
        return self ? LootRollLine{"LOOT_ROLL_NEED_SELF", {item}}
                    : LootRollLine{"LOOT_ROLL_NEED", {name, item}};
    }
    return rolled(rollType);
}

/// SMSG_LOOT_ROLL_WON's chat line (0x6fb700).
[[nodiscard]] inline LootRollLine lootRollWonLine(bool self, uint8_t rollNumber, uint8_t rollType,
                                                  bool verbose, const std::string& name,
                                                  const std::string& item) {
    const std::string num = std::to_string(static_cast<int>(rollNumber));
    if (verbose) {
        return self ? LootRollLine{"LOOT_ROLL_YOU_WON", {item}}
                    : LootRollLine{"LOOT_ROLL_WON", {name, item}};
    }
    const char* kind = rollType == 1 ? "NEED" : rollType == 2 ? "GREED" : rollType == 3 ? "DE" : nullptr;
    if (!kind) return {};
    if (self) return {std::string("LOOT_ROLL_YOU_WON_NO_SPAM_") + kind, {num, item}};
    return {std::string("LOOT_ROLL_WON_NO_SPAM_") + kind, {name, num, item}};
}

/// The English GlobalStrings for the lines above and the loot money, for
/// when the interface data is not there to ask.
[[nodiscard]] inline const char* englishGlobal(const std::string& key) {
    struct Entry { const char* key; const char* text; };
    static constexpr Entry kTable[] = {
        {"LOOT_ROLL_ALL_PASSED", "Everyone passed on: %s"},
        {"LOOT_ROLL_DISENCHANT", "%s has selected Disenchant for: %s"},
        {"LOOT_ROLL_DISENCHANT_SELF", "You have selected Disenchant for: %s"},
        {"LOOT_ROLL_GREED", "%s has selected Greed for: %s"},
        {"LOOT_ROLL_GREED_SELF", "You have selected Greed for: %s"},
        {"LOOT_ROLL_NEED", "%s has selected Need for: %s"},
        {"LOOT_ROLL_NEED_SELF", "You have selected Need for: %s"},
        {"LOOT_ROLL_PASSED", "%s passed on: %s"},
        {"LOOT_ROLL_PASSED_AUTO", "%s automatically passed on: %s because he cannot loot that item."},
        {"LOOT_ROLL_PASSED_AUTO_FEMALE", "%s automatically passed on: %s because she cannot loot that item."},
        {"LOOT_ROLL_PASSED_SELF", "You passed on: %s"},
        {"LOOT_ROLL_PASSED_SELF_AUTO", "You automatically passed on: %s because you cannot loot that item."},
        {"LOOT_ROLL_ROLLED_DE", "Disenchant Roll - %d for %s by %s"},
        {"LOOT_ROLL_ROLLED_GREED", "Greed Roll - %d for %s by %s"},
        {"LOOT_ROLL_ROLLED_NEED", "Need Roll - %d for %s by %s"},
        {"LOOT_ROLL_WON", "%s won: %s"},
        {"LOOT_ROLL_WON_NO_SPAM_DE", "%1$s won: %3$s |cff818181(Disenchant - %2$d)|r"},
        {"LOOT_ROLL_WON_NO_SPAM_GREED", "%1$s won: %3$s |cff818181(Greed - %2$d)|r"},
        {"LOOT_ROLL_WON_NO_SPAM_NEED", "%1$s won: %3$s |cff818181(Need - %2$d)|r"},
        {"LOOT_ROLL_YOU_WON", "You won: %s"},
        {"LOOT_ROLL_YOU_WON_NO_SPAM_DE", "You won: %2$s |cff818181(Disenchant - %1$d)|r"},
        {"LOOT_ROLL_YOU_WON_NO_SPAM_GREED", "You won: %2$s |cff818181(Greed - %1$d)|r"},
        {"LOOT_ROLL_YOU_WON_NO_SPAM_NEED", "You won: %2$s |cff818181(Need - %1$d)|r"},
        {"YOU_LOOT_MONEY", "You loot %s"},
        {"LOOT_MONEY_SPLIT", "Your share of the loot is %s."},
        {"GOLD_AMOUNT", "%d Gold"},
        {"SILVER_AMOUNT", "%d Silver"},
        {"COPPER_AMOUNT", "%d Copper"},
    };
    for (const auto& e : kTable) {
        if (key == e.key) return e.text;
    }
    return "";
}

/// The money text of 0x7e7d80: the coins that are not zero, highest first,
/// as GOLD_AMOUNT / SILVER_AMOUNT / COPPER_AMOUNT joined with ", ". `lookup`
/// answers a GlobalStrings key.
template <typename Lookup>
[[nodiscard]] std::string moneyText(uint32_t copper, Lookup&& lookup) {
    const uint32_t parts[3] = {copper / 10000, (copper / 100) % 100, copper % 100};
    static const char* const keys[3] = {"GOLD_AMOUNT", "SILVER_AMOUNT", "COPPER_AMOUNT"};
    std::string out;
    for (int i = 0; i < 3; ++i) {
        if (parts[i] == 0) continue;
        if (!out.empty()) out += ", ";
        out += formatGlobal(lookup(keys[i]), {std::to_string(parts[i])});
    }
    return out;
}

/// SMSG_LOOT_MONEY_NOTIFY (0x6ceca0): CHAT_MSG_MONEY, YOU_LOOT_MONEY when the
/// player took it alone, LOOT_MONEY_SPLIT for a share.
template <typename Lookup>
[[nodiscard]] std::string lootMoneyLine(uint32_t copper, bool soleLooter, Lookup&& lookup) {
    return formatGlobal(lookup(soleLooter ? "YOU_LOOT_MONEY" : "LOOT_MONEY_SPLIT"),
                        {moneyText(copper, lookup)});
}

}  // namespace wowee::game::stock_death
