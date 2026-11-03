#pragma once

// How a pet action is packed into the CMSG_PET_ACTION `action` field.
//
// It is not a flat list of ids. The field is a pair: the high byte says what
// kind of thing this is, and the low 24 bits say which one. The kinds reuse the
// same small numbers, so 1 means "follow" as a command and "defensive" as a
// reaction, and the two are only told apart by the byte above them.
//
// Four callers each invented their own reading of this - a raw 1..6 with no
// type byte at all, the type and the action swapped, and a dismiss that packed
// action 0 and so told the pet to stay. The bar slots the server sends down are
// already packed correctly and can be sent straight back; everything the client
// builds itself goes through packPetAction.

#include <cstdint>

namespace wowee {
namespace game {
namespace pet {

/// High byte: what kind of action this is.
enum class ActionType : uint8_t {
    Decide   = 0x00,
    Passive  = 0x01,
    Reaction = 0x06,  ///< Passive / defensive / aggressive stance.
    Command  = 0x07,  ///< Stay / follow / attack / dismiss / move-to.
    Disabled = 0x81,  ///< Castable spell, autocast off.
    Enabled  = 0xC1,  ///< Castable spell, autocast on.
};

/// Low 24 bits when the type is Command.
enum Command : uint32_t {
    kStay    = 0,
    kFollow  = 1,
    kAttack  = 2,
    kAbandon = 3,  ///< Dismiss. Not present in the default bar - built by hand.
    kMoveTo  = 4,
};

/// Low 24 bits when the type is Reaction.
enum Reaction : uint32_t {
    kPassive    = 0,
    kDefensive  = 1,
    kAggressive = 2,
};

constexpr uint32_t packPetAction(ActionType type, uint32_t action) {
    return (static_cast<uint32_t>(type) << 24) | (action & 0x00FFFFFFu);
}

constexpr uint32_t petActionId(uint32_t packed) { return packed & 0x00FFFFFFu; }
constexpr ActionType petActionType(uint32_t packed) {
    return static_cast<ActionType>((packed >> 24) & 0xFFu);
}

/// Whether a packed slot holds a castable spell rather than a built-in command
/// or stance. The spell types are the ones that carry an autocast bit.
constexpr bool isPetSpellAction(uint32_t packed) {
    const ActionType t = petActionType(packed);
    return t != ActionType::Command && t != ActionType::Reaction;
}


// ---------------------------------------------------------------------------
// The rest of this file is the stock client's pet bar and stable bookkeeping
// (Wow.exe 12340, PetInfo.cpp / StableInfo.cpp; docs/re/pets.md), kept free of
// the game handler so it can be tested on its own.
// ---------------------------------------------------------------------------

/// The slot kind the client switches on: the type byte without its two
/// autocast bits (`>> 24 & 0x3F`). ACT_PASSIVE, ACT_DISABLED and ACT_ENABLED
/// all come out as 1.
constexpr uint8_t slotKind(uint32_t packed) { return static_cast<uint8_t>((packed >> 24) & 0x3Fu); }

/// A spell slot as GetPetActionInfo, CastPetAction and the cooldown reader see
/// it: kind 1, or 8..17 (the kinds a vehicle's or possessed unit's bar uses).
constexpr bool isSpellKind(uint8_t kind) { return kind == 1 || (kind >= 8 && kind <= 0x11); }

/// The two autocast bits of a slot (0x5D4EC0): 0x80 in the type byte means
/// the spell can autocast at all, 0x40 that it is on.
constexpr uint32_t kSlotAutocastAllowed = 0x80000000u;
constexpr uint32_t kSlotAutocastOn      = 0x40000000u;
constexpr bool slotAutocastAllowed(uint32_t packed) { return (packed & kSlotAutocastAllowed) != 0; }
constexpr bool slotAutocastOn(uint32_t packed) { return (packed & kSlotAutocastOn) != 0; }

/// A slot holding nothing: the spell kind with no spell (0x5D6020's test).
constexpr bool isEmptySlot(uint32_t packed) {
    return (packed & 0x3F000000u) == 0x01000000u ? (packed & 0x00FFFFFFu) == 0 : packed == 0;
}

/// GetPetActionInfo's token names (0x5D4EC0): "PET_ACTION_%s" from the table
/// at 0xA1A8C0 for a command and "PET_MODE_%s" for a stance, with the texture
/// global "PET_%s_TEXTURE" of the same word.
inline const char* commandWord(uint32_t id) {
    static const char* const kWords[] = {"WAIT", "FOLLOW", "ATTACK", "DISMISS"};
    return id < 4 ? kWords[id] : nullptr;
}
inline const char* reactionWord(uint32_t id) {
    static const char* const kWords[] = {"PASSIVE", "DEFENSIVE", "AGGRESSIVE"};
    return id < 3 ? kWords[id] : nullptr;
}

/// The pet's command and stance as the client keeps them (0xC234C8): the
/// uint32 SMSG_PET_SPELLS and SMSG_PET_MODE both carry - react in the low
/// byte, command in the next, flags above. Reading the command as the low
/// byte swapped the two.
constexpr uint8_t modeReact(uint32_t mode) { return static_cast<uint8_t>(mode & 0xFFu); }
constexpr uint8_t modeCommand(uint32_t mode) { return static_cast<uint8_t>((mode >> 8) & 0xFFu); }

/// GetPetActionInfo's isActive for a built-in slot (0x5D4EC0): a stance is
/// checked when it is the pet's stance; a command when it is the pet's
/// command, except Attack, which is checked while the pet is attacking (the
/// pet's UNIT_FLAG_PET_IN_COMBAT, or a PetAttack the client has just sent).
constexpr bool builtinActive(uint32_t packed, uint8_t react, uint8_t command, bool attacking) {
    const uint8_t kind = slotKind(packed);
    const uint32_t id = packed & 0x00FFFFFFu;
    if (kind == 6) return id == react;
    if (kind == 7) return id == kAttack ? attacking : id == command;
    return false;
}

/// Happiness, from PetPersonality.dbc row 1 (0x71F390/0x71F400): the face is
/// the last threshold the pet's happiness power has reached, and the damage
/// it is worth is the row's multiplier for that face. Thresholds 0, 333000,
/// 666000 and damage 75%, 100%, 125% in the 3.3.5 file - thirds of the
/// 1050000 maximum would be 350000 and 700000.
struct HappinessRow {
    uint32_t threshold[3] = {0, 333000, 666000};
    float damage[3] = {0.75f, 1.0f, 1.25f};
};
constexpr int happinessIndex(uint32_t happiness, const HappinessRow& row) {
    int i = 1;
    while (i < 3 && happiness >= row.threshold[i]) ++i;
    return i - 1;
}

/// One CMSG_PET_SET_ACTION pair.
struct SlotWrite {
    uint32_t slot = 0;
    uint32_t value = 0;
};

/// The result of dropping an action on a pet bar slot (0x5D6020).
struct SlotDrop {
    bool accepted = false;
    /// What to send, in order: the slot the displaced action moved to (if it
    /// moved), then the target slot.
    SlotWrite writes[2];
    int writeCount = 0;
    /// What the cursor picks up afterwards: the action that was in the slot,
    /// when it had nowhere else to go. Zero for nothing.
    uint32_t toCursor = 0;
};

/// Drop `action` on `slots[target]` the way the client does:
///  - the same action again is nothing;
///  - an action already elsewhere on the bar moves, and that slot takes what
///    the target held;
///  - a command or stance in the target cannot be dropped off the bar, so it
///    moves to the first empty slot, and with no empty slot the drop is
///    refused;
///  - otherwise what the target held goes to the cursor.
/// The caller refuses passive spells beforehand (Attributes 0x40).
inline SlotDrop dropOnSlot(uint32_t (&slots)[10], uint32_t target, uint32_t action) {
    SlotDrop r;
    if (target >= 10 || action == slots[target]) return r;
    const uint32_t old = slots[target];
    int moved = -1;
    if (!isEmptySlot(action)) {
        for (uint32_t u = 0; u < 10; ++u) {
            if (u != target && (slots[u] & 0x3FFFFFFFu) == (action & 0x3FFFFFFFu)) {
                slots[u] = 0;
                moved = static_cast<int>(u);
                break;
            }
        }
    }
    if (moved < 0) {
        const uint8_t kind = slotKind(old);
        if (kind == 6 || kind == 7) {
            for (uint32_t u = 0; u < 10; ++u) {
                const uint8_t k = slotKind(slots[u]);
                if (k != 6 && k != 7 && (slots[u] & 0x00FFFFFFu) == 0) { moved = static_cast<int>(u); break; }
            }
            if (moved < 0) return r;
        }
    }
    if (moved >= 0) slots[moved] = old;
    slots[target] = action;
    r.accepted = true;
    if (moved >= 0) r.writes[r.writeCount++] = {static_cast<uint32_t>(moved), slots[moved]};
    r.writes[r.writeCount++] = {target, slots[target]};
    if (moved < 0) r.toCursor = old;
    return r;
}

/// What a click on a stable slot does (ClickStablePet, 0x5A1CA0). Slot 0 is
/// the pet that is out, 1..4 the stabled ones; `cursorSlot` is the stable slot
/// the cursor is carrying (PickupStablePet), or -1 for none.
enum class StableClick { Select, Drop, Stable, Swap, Unstable };
struct StableClickResult {
    StableClick op = StableClick::Select;
    uint32_t petNumber = 0;   ///< the stabled pet a swap or unstable names
};
inline StableClickResult stableClick(int cursorSlot, int clicked, bool clickedOccupied,
                                     uint32_t clickedNumber, uint32_t carriedNumber,
                                     bool carriedOccupied, int numSlots, bool petOut) {
    StableClickResult r;
    if (cursorSlot < 0) return r;                       // select
    r.op = StableClick::Drop;                           // and the cursor clears
    if (cursorSlot == clicked) return r;
    if (cursorSlot == 0 && clicked >= 1) {
        // The pet that is out onto a stable slot.
        if (clickedOccupied) { r.op = StableClick::Swap; r.petNumber = clickedNumber; }
        else if (clicked <= numSlots) { r.op = StableClick::Stable; }
        return r;
    }
    if (clicked == 0 && cursorSlot >= 1 && carriedOccupied) {
        // A stabled pet onto the current slot.
        r.op = petOut ? StableClick::Swap : StableClick::Unstable;
        r.petNumber = carriedNumber;
    }
    return r;
}

/// Whether a pet talent shows for a family (0x5C5A20): a talent with no
/// category mask is every family's; one with a mask needs the family's
/// CategoryEnumID bit (CreatureFamily column 9; Talent columns 21-22).
constexpr bool petTalentForFamily(uint32_t mask0, uint32_t mask1, int32_t familyCategory) {
    if (mask0 == 0 && mask1 == 0) return true;
    if (familyCategory < 0) return true;
    const uint32_t word = static_cast<uint32_t>(familyCategory) >> 5;
    if (word > 1) return false;
    const uint32_t bit = 1u << (static_cast<uint32_t>(familyCategory) & 31u);
    return ((word == 0 ? mask0 : mask1) & bit) != 0;
}

} // namespace pet
} // namespace game
} // namespace wowee
