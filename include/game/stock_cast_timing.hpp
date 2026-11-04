#pragma once

// The timing rules of the stock 3.3.5a client (build 12340) for a press that
// becomes a cast, kept apart from the handlers so they can be tested.
// docs/re/input-timing.md has the addresses and the measurements.

#include <cmath>
#include <cstdint>

namespace wowee::game::stock_cast {

/// The global cooldown the client starts by itself when it sends a cast
/// (0x805D70, called from the send 0x80AC90).
///
/// Spell.dbc StartRecoveryCategory and StartRecoveryTime name it; a spell
/// with neither (Divine Protection, Avenging Wrath, Auto Shot) starts none,
/// and one with a category but no time starts none either. The common case -
/// category 133, 1500 ms, a magic or unclassed spell (DmgClass not melee 2 or
/// ranged 3) that is neither SPELL_ATTR0_RANGED (0x2) nor SPELL_ATTR0_ABILITY
/// (0x10) - is scaled by the caster's UNIT_MOD_CAST_SPEED: round(1500 * speed
/// - 0.5) held to 1000..1500 ms. The rounding is the x87 default, to nearest
/// even. The SPELLMOD_GLOBAL_COOLDOWN (21) talents the client applies first
/// are not modelled here.
inline uint32_t predictedGcdMs(uint32_t startRecoveryCategory, uint32_t startRecoveryTimeMs,
                               uint32_t dmgClass, uint32_t attributes, float modCastSpeed) {
    if (startRecoveryCategory == 0 && startRecoveryTimeMs == 0) return 0;
    uint32_t ms = startRecoveryTimeMs;
    if (startRecoveryCategory == 133 && startRecoveryTimeMs == 1500 && dmgClass != 2 &&
        dmgClass != 3 && (attributes & 0x12u) == 0) {
        // A missing field reads 0; the client's own default is 1.0.
        const float speed = (modCastSpeed > 0.0f && std::isfinite(modCastSpeed)) ? modCastSpeed : 1.0f;
        const double scaled = std::nearbyint(static_cast<double>(static_cast<float>(ms) * speed) - 0.5);
        const long v = static_cast<long>(scaled);
        ms = static_cast<uint32_t>(v < 1000 ? 1000 : (v > 1500 ? 1500 : v));
    }
    return ms;
}

/// Whether a cast failure is shown (0x808200): the same spell refused for the
/// same reason within 3 s of its previous refusal says nothing - no text, no
/// voice. Each refusal restarts the 3 s, so a held or mashed key reports once
/// per burst. Shared by the client's own refusals and SMSG_CAST_FAILED.
struct CastErrorThrottle {
    uint32_t lastSpell = 0;
    uint32_t lastResult = 0xFFFFFFFFu;
    int64_t lastMs = 0;
    bool any = false;

    bool shouldShow(uint32_t spellId, uint32_t result, int64_t nowMs) {
        const bool repeat = any && spellId == lastSpell && result == lastResult &&
                            nowMs - lastMs < 3000;
        lastSpell = spellId;
        lastResult = result;
        lastMs = nowMs;
        any = true;
        return !repeat;
    }
};

/// The game error a SPELL_FAILED_NOT_READY (67) or ITEM_NOT_READY (45) is
/// shown as (0x807F10 -> 0x5216F0): the table names, not the SPELL_FAILED_*
/// text ("Not yet recovered" is never what the stock client prints for them).
/// Spell.dbc Category 4 and 9 are potions, 10 and 11 food and drink;
/// SPELL_ATTR0_ABILITY (0x10) makes a spell an ability.
inline const char* notReadyErrorName(uint32_t result, uint32_t category, uint32_t attributes) {
    if (result != 67 && result != 45) return nullptr;
    if (category == 4 || category == 9) return "ERR_POTION_COOLDOWN";
    if (category == 10 || category == 11) return "ERR_FOOD_COOLDOWN";
    if (result == 45) return "ERR_ITEM_COOLDOWN";
    return (attributes & 0x10u) ? "ERR_ABILITY_COOLDOWN" : "ERR_SPELL_COOLDOWN";
}

/// English text of the names above, for when the interface's GlobalStrings
/// are not loaded.
inline const char* notReadyErrorText(const char* name) {
    if (!name) return nullptr;
    const auto is = [&](const char* s) {
        const char* a = name;
        while (*a && *a == *s) { ++a; ++s; }
        return *a == *s;
    };
    if (is("ERR_POTION_COOLDOWN")) return "You cannot drink any more yet.";
    if (is("ERR_FOOD_COOLDOWN")) return "You are too full to eat more now.";
    if (is("ERR_ITEM_COOLDOWN")) return "Item is not ready yet.";
    if (is("ERR_ABILITY_COOLDOWN")) return "Ability is not ready yet.";
    if (is("ERR_SPELL_COOLDOWN")) return "Spell is not ready yet.";
    return nullptr;
}

/// A world click (CGInputControl 0x5FA450 -> 0x5F9600 on the button's
/// release): never once the button has been held 800 ms; otherwise a click
/// when the pointer stayed put (under 8 units of travel), or, when it moved,
/// only a quick one under 200 ms.
inline bool isWorldClick(double heldMs, bool moved) {
    if (heldMs >= 800.0) return false;
    if (!moved) return true;
    return heldMs < 200.0;
}

} // namespace wowee::game::stock_cast
