#pragma once

// Which game objects the mouse may find, and which of those it may use.
//
// The stock 3.3.5a client answers two questions about every object, and the
// pick, the cursor and the tooltip all follow from them:
//
//   highlight - may the pointer find it at all? An object that may not is not
//               there as far as the mouse is concerned: the world pick skips it
//               (CGWorldFrame's model pick, 0x4F9550, keeps only objects whose
//               virtual at +0xA4 says yes), so the ray goes on to whatever is
//               behind it, and it gets no tooltip, no highlight and no cursor.
//               This is what keeps an instance portal's purple aura, a
//               decorative column or a transport from wearing a name.
//   interact  - may it be used? Decides the cursor. An object that may be found
//               but not used - a signpost, an anvil, a pool of fish - is named
//               under the plain pointer and nothing happens when it is clicked.
//
// Both come from CGGameObject_C's per-type handler, picked by GameObjectType
// when the object is created (0x714250). The handler's IsInteractable (vtable
// +0x18, 0x7112A0 for most types) is the use rule; its CanHighlight (vtable
// +0x0C) is, for most types, the same question - and for a handful of types
// something else. CGGameObject_C::CanHighlight (0x70F580) then adds the quest
// condition on top. Written out from the binary below, type by type.
//
// Pure, so it can be tested without the client behind it; the picker gathers
// the inputs.

#include <cstdint>

namespace wowee {
namespace ui {

/// GAMEOBJECT_FLAGS bits the rule reads.
constexpr uint32_t kGoFlagInUse = 0x01;
constexpr uint32_t kGoFlagInteractCond = 0x04;
constexpr uint32_t kGoFlagNotSelectable = 0x10;
/// Low half of GAMEOBJECT_DYNAMIC.
constexpr uint32_t kGoDynActivate = 0x01;
constexpr uint32_t kGoDynNoInteract = 0x04;

/// Everything the stock client looks at, gathered by the caller.
struct GameObjectAffordanceInput {
    /// GameObjectType. From the cache record, or GAMEOBJECT_BYTES_1 when the
    /// record has not arrived - the client creates the handler from the
    /// descriptor, not the cache.
    uint32_t type = 0;
    bool typeKnown = false;
    /// The cache record's data fields, or null while it has not arrived. Stock
    /// reads them through the record too (CGGameObject_C+0x1A4), so a missing
    /// record answers the way the binary does: nothing that needs a field.
    const uint32_t* data = nullptr;

    uint32_t flags = 0;          ///< GAMEOBJECT_FLAGS.
    uint32_t dynamicFlags = 0;   ///< Low 16 bits of GAMEOBJECT_DYNAMIC.

    /// Whether the object regards the player as an enemy - reaction Hated or
    /// Hostile. Stock asks the object's creator if it is a unit in view, else
    /// GAMEOBJECT_FACTION, else calls it neutral (0x70EDD0).
    bool hostile = false;
    /// GAMEOBJECT_FIELD_CREATED_BY is the player.
    bool createdByPlayer = false;
    /// The object is the player's UNIT_FIELD_CHANNEL_OBJECT - while fishing,
    /// their own bobber. The only thing a bobber is asked (0x712030).
    bool playersChannelObject = false;
    /// The quest the type names in its data, if any, is in the player's log.
    /// Only read when that quest is not zero.
    bool questInLog = false;
};

struct GameObjectAffordance {
    bool highlight = false;
    bool interact = false;
};

/// The data field holding a type's quest, or -1. The binary's type table
/// (0xA36F30) lists each type's fields by meaning; meaning 20 is the quest.
constexpr int gameObjectQuestField(uint32_t type) {
    switch (type) {
        case 3:  return 8;   // CHEST
        case 5:  return 5;   // GENERIC
        case 8:  return 4;   // SPELL_FOCUS
        case 10: return 1;   // GOOBER
        default: return -1;
    }
}

/// The handler's IsInteractable: vtable +0x18.
///
/// 0x7112A0 for the ordinary types; chairs (0x711CE0) and the fishing bobber
/// (0x712030) wrap it; every other type answers no (0x427A90).
constexpr bool gameObjectInteractable(const GameObjectAffordanceInput& in) {
    switch (in.type) {
        // The ordinary handler, and the two that only narrow it.
        case 0: case 1: case 2: case 3: case 4: case 6: case 7: case 9:
        case 10: case 12: case 13: case 17: case 18: case 19: case 22:
        case 23: case 24: case 26: case 27: case 32: case 34:
            break;
        default:
            return false;
    }

    // A bobber is only ever its caster's to reel in.
    if (in.type == 17 && !in.playersChannelObject) return false;

    if (in.type == 6) {
        // A trap is the other way round: only an enemy's can be worked, and
        // only one with a lock to disarm. (Stock also wants a Lock.dbc row
        // behind the id, and for a player's trap a PvP test on its owner.)
        if (!in.data || in.data[0] == 0) return false;
        if (!in.hostile) return false;
    } else if (in.hostile) {
        return false;
    }

    // The server's switches. NO_INTERACT in the dynamic flags; in use or not
    // selectable in the flags; and an object flagged INTERACT_COND is usable
    // only while the server has activated it for this player, which is how a
    // quest object is closed to everyone not on the quest.
    if (in.dynamicFlags & kGoDynNoInteract) return false;
    if (in.flags & (kGoFlagInUse | kGoFlagNotSelectable)) return false;
    if ((in.flags & kGoFlagInteractCond) && !(in.dynamicFlags & kGoDynActivate)) return false;

    // A chair may be its creator's alone (data2, onlyCreatorUse).
    if (in.type == 7 && in.data && in.data[2] != 0 && !in.createdByPlayer) return false;
    return true;
}

/// What the mouse may do with this object.
constexpr GameObjectAffordance gameObjectAffordance(const GameObjectAffordanceInput& in) {
    GameObjectAffordance out;
    // Nothing to go on: offer it, rather than hide something that may well be
    // a door. Stock always knows the type; this client briefly may not.
    if (!in.typeKnown) {
        out.highlight = out.interact = true;
        return out;
    }

    out.interact = gameObjectInteractable(in);

    // The handler's CanHighlight: vtable +0x0C.
    switch (in.type) {
        case 5:   // GENERIC: its "highlight" field, data1 (0x70FF80).
            out.highlight = in.data && in.data[1] != 0;
            break;
        case 29:  // CAPTURE_POINT: its "highlight" field, data19 (0x70FF80).
            out.highlight = in.data && in.data[19] != 0;
            break;
        case 8:   // SPELL_FOCUS - the anvil and forge are named, not used.
        case 16:  // DUEL_ARBITER
        case 25:  // FISHINGHOLE - a pool is named, and fished, not clicked.
        case 30:  // AURA_GENERATOR
        case 33:  // DESTRUCTIBLE_BUILDING (0x70DF30: unless it is the vehicle
                  // being driven, which this client does not ask)
            out.highlight = true;
            break;
        case 11:  // TRANSPORT
        case 14:  // MAP_OBJECT
        case 15:  // MO_TRANSPORT
        case 31:  // DUNGEON_DIFFICULTY
        case 35:  // TRAPDOOR
            out.highlight = false;
            break;
        default:  // Everything else may be found exactly when it may be used.
            out.highlight = out.interact;
            break;
    }

    // ...and the quest. An object naming a quest is there for the pointer only
    // while that quest is in the log, in whatever state (0x70F580).
    if (out.highlight) {
        const int qf = gameObjectQuestField(in.type);
        if (qf >= 0 && in.data && static_cast<int32_t>(in.data[qf]) > 0 && !in.questInLog) {
            out.highlight = false;
        }
    }

    // Nothing is used that cannot be found first.
    out.interact = out.interact && out.highlight;
    return out;
}

} // namespace ui
} // namespace wowee
