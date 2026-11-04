#pragma once

// How the stock 3.3.5a client (build 12340) ends a cast: which
// UNIT_SPELLCAST_* event it fires, when the bar's STOP goes with it, which
// cancels send CMSG_CANCEL_CAST, and the list of the player's own casts in
// flight those all work from. Kept apart from SpellHandler so the rules can be
// tested; docs/re/combat-targeting-movement.md section 3 ("Casts ending") has
// the addresses and the event sequences they make.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee::game::stock_cast {

/// The SPELL_FAILED_* results 0x7FECC0 sorts by. 187 is not a result the
/// server sends: the client passes it for a cast that went off.
inline constexpr uint8_t kResultCharmed = 24;
inline constexpr uint8_t kResultDontReport = 27;
inline constexpr uint8_t kResultInterrupted = 40;
inline constexpr uint8_t kResultInterruptedCombat = 41;
inline constexpr uint8_t kResultNeedAmmo = 52;
inline constexpr uint8_t kResultNeedAmmoPouch = 53;
inline constexpr uint8_t kResultNeedExoticAmmo = 54;
inline constexpr uint8_t kResultSpellInProgress = 105;
inline constexpr uint8_t kResultSucceeded = 187;

/// The event 0x7FECC0 fires for a cast that ends with `result`.
enum class CastEndEvent : uint8_t { Succeeded, FailedQuiet, Interrupted, Failed };

[[nodiscard]] constexpr CastEndEvent castEndEvent(uint8_t result) {
    if (result == kResultSucceeded) return CastEndEvent::Succeeded;
    if (result == kResultDontReport || result == kResultSpellInProgress || result == kResultCharmed)
        return CastEndEvent::FailedQuiet;
    if (result == kResultInterrupted || result == kResultInterruptedCombat)
        return CastEndEvent::Interrupted;
    return CastEndEvent::Failed;
}

[[nodiscard]] constexpr const char* castEndEventName(CastEndEvent e) {
    switch (e) {
    case CastEndEvent::Succeeded: return "UNIT_SPELLCAST_SUCCEEDED";
    case CastEndEvent::FailedQuiet: return "UNIT_SPELLCAST_FAILED_QUIET";
    case CastEndEvent::Interrupted: return "UNIT_SPELLCAST_INTERRUPTED";
    case CastEndEvent::Failed: break;
    }
    return "UNIT_SPELLCAST_FAILED";
}

/// One cast as the client names it: the cast count of the press (0 for a cast
/// the server started itself) and the spell.
struct CastRef {
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    friend constexpr bool operator==(const CastRef&, const CastRef&) = default;
};

/// A unit's own cast: CGUnit_C +0xA5C (count) and +0xA60 (spell), set when the
/// cast starts (for the player already at the send, 0x80AC90 -> 0x805330), and
/// +0xA6C, the spell the cast bar shows, set by SMSG_SPELL_START.
struct UnitCast {
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    uint32_t barSpellId = 0;
};

/// What 0x7FECC0 does for `cast` ending with `result` on a unit whose own cast
/// is `unit`.
struct CastEndSteps {
    bool endsCast = false;   // 0x7FEC00: the unit's cast and its pose are put away
    CastEndEvent event = CastEndEvent::Failed;
    bool stop = false;       // 0x726200 and UNIT_SPELLCAST_STOP: the bar showed it
};

/// 0x7FECC0. The event always fires, with the cast's own count; the unit's
/// cast ends only when it is this one (count and spell), and STOP follows only
/// when the bar shows it. Both compare against the count as it was on entry
/// (0x72E430 zeroes it with the spell). So a cast the client ended itself and
/// the server then reports again fires its event again but no second STOP.
[[nodiscard]] inline CastEndSteps castEndSteps(UnitCast& unit, CastRef cast, uint8_t result) {
    CastEndSteps s;
    const uint8_t count = unit.castCount;
    s.endsCast = cast.spellId != 0 && unit.spellId == cast.spellId && count == cast.castCount;
    if (s.endsCast) {
        unit.spellId = 0;
        unit.castCount = 0;
    }
    s.event = castEndEvent(result);
    s.stop = cast.spellId != 0 && unit.barSpellId == cast.spellId && count == cast.castCount;
    if (s.stop) unit.barSpellId = 0;
    return s;
}

/// 0x806200: a cancel sends CMSG_CANCEL_CAST only for a spell with a cast time
/// (the caster's vtable +0x148, the cast time after the spell mods) or one
/// that waits for the next swing (SPELL_ATTR0 0x4 | 0x400). Auto Shot (cast
/// time -1000000), an instant, a wand's Shoot are put away without a word to
/// the server.
[[nodiscard]] constexpr bool cancelSendsPacket(int32_t castTimeMs, uint32_t attributes) {
    return castTimeMs >= 1 || (attributes & 0x404u) != 0;
}

/// SMSG_CAST_FAILED and SMSG_SPELL_FAILURE play Resistances.dbc's
/// FizzleSoundID for the spell's school (0x4CFF30): the first of the
/// SchoolMask's bits 0-6 that names a row. -1 for none.
[[nodiscard]] constexpr int fizzleSchool(uint32_t schoolMask) {
    for (int i = 0; i < 7; ++i)
        if (schoolMask & (1u << i)) return i;
    return -1;
}

/// 0x808200 plays it for every cast failure but SPELL_FAILED_DONT_REPORT.
[[nodiscard]] constexpr bool castFailurePlaysFizzle(uint8_t result) {
    return result != kResultDontReport;
}

/// SMSG_SPELL_FAILURE (0x809C70) turns off the player's auto-repeat for a
/// shot that failed for want of ammunition (0x807560).
[[nodiscard]] constexpr bool spellFailureStopsAutoRepeat(uint8_t result) {
    return result == kResultNeedAmmo || result == kResultNeedAmmoPouch ||
           result == kResultNeedExoticAmmo;
}

/// The player's casts in flight (0xAF5254): one record per press from the
/// send (0x80AC90, 0x805010) until its SMSG_SPELL_GO, its failure, or a cancel
/// (0x805100). An Auto Shot's or Shoot's press is never answered with a GO -
/// every shot after it is the server's own, count 0 - so its record stays for
/// as long as the repeat does. The server's reports find a record by count,
/// spell and caster (0x805180); one that finds none makes a fresh one and
/// does the same with it.
class PendingCasts {
public:
    static constexpr size_t kMax = 32;

    void add(CastRef cast) {
        if (cast.spellId == 0) return;
        remove(cast);
        if (list_.size() >= kMax) list_.erase(list_.begin());
        list_.push_back(cast);
    }
    [[nodiscard]] bool contains(CastRef cast) const {
        return std::find(list_.begin(), list_.end(), cast) != list_.end();
    }
    bool remove(CastRef cast) {
        auto it = std::find(list_.begin(), list_.end(), cast);
        if (it == list_.end()) return false;
        list_.erase(it);
        return true;
    }
    /// A copy to walk while records are removed, as 0x806620 walks its list.
    [[nodiscard]] std::vector<CastRef> snapshot() const { return list_; }
    [[nodiscard]] size_t size() const { return list_.size(); }
    void clear() { list_.clear(); }

private:
    std::vector<CastRef> list_;
};

} // namespace wowee::game::stock_cast
