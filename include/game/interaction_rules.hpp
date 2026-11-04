#pragma once

// What the stock 3.3.5a client (build 12340) decides on its own when the player
// interacts with the world: how near is near enough, which packet a right-click
// on an NPC sends, which cursor the pointer wears, and when an open window is
// given up because the player walked away. docs/re/interactions.md has the
// whole of it with the addresses; this header is the part Wowee runs, kept pure
// so it can be tested without a client behind it.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace wowee {
namespace game {
namespace interaction {

// ---------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------

/// An NPC is spoken to from its combat reach plus four yards, centre to centre
/// in 3D (CGPlayer_C::OnNpcInteract 0x6DDBB0, the float at 0x9E8D2C). The
/// player's own reach plays no part. The same figure keeps a window open
/// (CGGameUI::SetInteractTarget 0x51FCE0).
constexpr float kNpcInteractPad = 4.0f;
[[nodiscard]] constexpr float npcInteractRange(float npcCombatReach) {
    return npcCombatReach + kNpcInteractPad;
}

/// The limit kept for a window opened by another player - a trade - is this
/// squared distance, stored as is (0xAC8A68, 11.11 yards).
constexpr float kPlayerInteractRangeSq = 123.45678f;

/// Melee reach and the reach a corpse is looted from (0x4F5F40): both combat
/// reaches plus 4/3, never under five yards.
constexpr float kMeleeReachPad = 4.0f / 3.0f;
constexpr float kMinMeleeReach = 5.0f;
[[nodiscard]] constexpr float meleeReach(float reachA, float reachB) {
    return std::max(reachA + reachB + kMeleeReachPad, kMinMeleeReach);
}
/// Anything looted that is not a unit (0x6D5A60): five yards.
constexpr float kNonUnitLootRange = 5.0f;

/// How far past its model's box a game object can be used, by GameObjectType:
/// the float each type handler's constructor stores at +0xC (0x7124B0 and the
/// constructors 0x714250 picks). A chair measures it from its seat slots
/// instead (0x711DB0) and a spell focus from its centre (0x70FA70).
[[nodiscard]] constexpr float gameObjectUseRange(uint32_t type) {
    switch (type) {
        case 2:                       // QUESTGIVER (0x7125C0)
        case 9:                       // TEXT (0x7126D0)
        case 24: case 26: case 27:    // FLAGSTAND, FLAGDROP, MINI_GAME (0x7127B0)
            return 5.5555553f;        // 0xA33570
        case 4:                       // BINDER (0x70C4E0)
            return 10.0f;             // 0xA33578
        case 7:                       // CHAIR (0x70C6A0)
        case 32:                      // BARBER_CHAIR (0x70C7F0)
            return 3.0f;              // 0xA3357C
        case 12:                      // AREADAMAGE (0x712590)
            return 0.0f;
        case 17:                      // FISHINGNODE (0x712700)
            return 100.0f;            // 0xA33580
        default:
            return 5.0f;              // 0xA33574
    }
}

// ---------------------------------------------------------------------------
// Game object template fields
// ---------------------------------------------------------------------------

/// Where a type keeps its lock in the template data, or -1. The type table at
/// 0xA36F30 names each data field by meaning; meaning 4 is the lock, read by
/// 0x70EF30.
[[nodiscard]] constexpr int gameObjectLockField(uint32_t type) {
    switch (type) {
        case 0: case 1: return 1;                    // DOOR, BUTTON
        case 2: case 3: case 6: case 10: case 12:
        case 13: case 24: case 26: return 0;         // QUESTGIVER .. FLAGDROP
        case 25: return 4;                           // FISHINGHOLE
        default: return -1;
    }
}

/// Where a type says it may be used from the saddle (meaning 91), or -1.
[[nodiscard]] constexpr int gameObjectAllowMountedField(uint32_t type) {
    switch (type) {
        case 2: return 8;     // QUESTGIVER
        case 9: return 3;     // TEXT
        case 10: return 17;   // GOOBER
        case 22: return 3;    // SPELLCASTER
        default: return -1;
    }
}

/// May the object be used without getting down (0x710390)? A mailbox always
/// may, a barber's chair never, anything else when its template says so.
[[nodiscard]] constexpr bool gameObjectAllowsMounted(uint32_t type, const uint32_t* data) {
    if (type == 19) return true;    // MAILBOX
    if (type == 32) return false;   // BARBER_CHAIR
    const int f = gameObjectAllowMountedField(type);
    return f >= 0 && data && data[f] != 0;
}

/// A chest whose template sets "not in combat" (meaning 62, data12) cannot be
/// opened while the player is in combat (0x710340, ERR 0x1C2).
[[nodiscard]] constexpr bool gameObjectUsableInCombat(uint32_t type, const uint32_t* data) {
    return !(type == 3 && data && data[12] != 0);
}

/// UNIT_FIELD_FLAGS bits the right-click reads (0x731260).
constexpr uint32_t kUnitFlagSkinnable = 0x04000000;
constexpr uint32_t kUnitFlagPacified = 0x00020000;

// ---------------------------------------------------------------------------
// NPC right-click
// ---------------------------------------------------------------------------

/// UNIT_NPC_FLAGS bits the client reads.
enum NpcFlag : uint32_t {
    kNpcGossip         = 0x00000001,
    kNpcQuestGiver     = 0x00000002,
    kNpcTrainer        = 0x00000010,
    kNpcVendor         = 0x00000080,
    kNpcRepair         = 0x00001000,
    kNpcFlightMaster   = 0x00002000,
    kNpcSpiritHealer   = 0x00004000,
    kNpcSpiritGuide    = 0x00008000,
    kNpcInnkeeper      = 0x00010000,
    kNpcBanker         = 0x00020000,
    kNpcPetitioner     = 0x00040000,
    kNpcTabardDesigner = 0x00080000,
    kNpcBattlemaster   = 0x00100000,
    kNpcAuctioneer     = 0x00200000,
    kNpcStableMaster   = 0x00400000,
    kNpcGuildBanker    = 0x00800000,
    kNpcSpellClick     = 0x01000000,
    kNpcMailbox        = 0x04000000,
};

/// What one right-click on a friendly NPC asks the server for. The stock
/// client sends exactly one packet, chosen by the first flag in this order
/// (0x6DDBB0); a gossip NPC is always greeted with CMSG_GOSSIP_HELLO and the
/// server's menu offers the rest.
enum class NpcHello : uint8_t {
    None,
    Gossip,          // CMSG_GOSSIP_HELLO 0x17B           (0x6D1E20)
    QuestGiver,      // CMSG_QUESTGIVER_HELLO 0x184       (0x6D1EA0)
    Taxi,            // CMSG_TAXIQUERYAVAILABLENODES 0x1AC (0x6D6320)
    Vendor,          // CMSG_LIST_INVENTORY 0x19E          (0x6DAF60)
    Trainer,         // CMSG_TRAINER_LIST 0x1B0            (0x6D1F20)
    SpiritHealer,    // CONFIRM_XP_LOSS, then CMSG_SPIRIT_HEALER_ACTIVATE (0x6DB010)
    SpiritGuide,     // area spirit healer query           (0x6DB180)
    Innkeeper,       // CONFIRM_BINDER, then CMSG_BINDER_ACTIVATE (0x6D2120)
    Banker,          // CMSG_BANKER_ACTIVATE 0x1B7         (0x6D2340)
    Petitioner,      // CMSG_PETITION_SHOWLIST 0x1BB       (0x6D2480)
    TabardDesigner,  // MSG_TABARDVENDOR_ACTIVATE 0x1F2    (0x6D2500)
    Battlemaster,    // CMSG_BATTLEMASTER_HELLO 0x2D7      (0x6D25C0)
    Auctioneer,      // MSG_AUCTION_HELLO 0x255            (0x6D2640)
    StableMaster,    // MSG_LIST_STABLED_PETS 0x26F        (0x6D26C0)
    GuildBanker,     // CMSG_GUILD_BANKER_ACTIVATE 0x3E6   (0x6D23C0)
    SpellClick,      // CMSG_SPELLCLICK 0x3F8              (0x6D2740)
    Mailbox,         // the mail window, opened locally    (0x56DA60)
};

/// Does a quest giver have something to say? QuestGiverStatus other than NONE
/// and UNAVAILABLE (0x6D1DE0).
[[nodiscard]] constexpr bool questStatusOffers(uint8_t questGiverStatus) {
    return questGiverStatus > 1;
}

[[nodiscard]] constexpr NpcHello npcHelloFor(uint32_t npcFlags, uint8_t questGiverStatus) {
    if (npcFlags & kNpcGossip) return NpcHello::Gossip;
    if (questStatusOffers(questGiverStatus)) return NpcHello::QuestGiver;
    if (npcFlags & kNpcFlightMaster) return NpcHello::Taxi;
    if (npcFlags & kNpcVendor) return NpcHello::Vendor;
    if (npcFlags & kNpcTrainer) return NpcHello::Trainer;
    if (npcFlags & kNpcSpiritHealer) return NpcHello::SpiritHealer;
    if (npcFlags & kNpcSpiritGuide) return NpcHello::SpiritGuide;
    if (npcFlags & kNpcInnkeeper) return NpcHello::Innkeeper;
    if (npcFlags & kNpcBanker) return NpcHello::Banker;
    if (npcFlags & kNpcPetitioner) return NpcHello::Petitioner;
    if (npcFlags & kNpcTabardDesigner) return NpcHello::TabardDesigner;
    if (npcFlags & kNpcBattlemaster) return NpcHello::Battlemaster;
    if (npcFlags & kNpcAuctioneer) return NpcHello::Auctioneer;
    if (npcFlags & kNpcStableMaster) return NpcHello::StableMaster;
    if (npcFlags & kNpcGuildBanker) return NpcHello::GuildBanker;
    if (npcFlags & kNpcSpellClick) return NpcHello::SpellClick;
    if (npcFlags & kNpcMailbox) return NpcHello::Mailbox;
    // A quest giver with nothing for this player still answers the hello.
    if (npcFlags & kNpcQuestGiver) return NpcHello::QuestGiver;
    return NpcHello::None;
}

// ---------------------------------------------------------------------------
// Cursors
// ---------------------------------------------------------------------------
//
// The client names its cursors (the table at 0xAD280C, 1-based, 26 names then
// the same 26 with "Unable" in front) and loads Interface\Cursor\<name>.blp.
// An empty name is the plain pointer.

/// "Unable" + name: the same thing, out of reach or not usable right now.
[[nodiscard]] inline std::string unableCursor(std::string_view name) {
    return "Unable" + std::string(name);
}

/// The file a cursor name is drawn from (0x70CEC0, 0x715F20).
[[nodiscard]] inline std::string cursorPath(std::string_view name) {
    return "Interface\\Cursor\\" + std::string(name) + ".blp";
}

/// The cursor a quest giver's status asks for (0x744640), or empty.
[[nodiscard]] inline std::string questStatusCursor(uint8_t status, bool unable) {
    std::string name;
    switch (status) {
        case 2: case 3: case 4: case 5: case 7: case 8: name = "Quest"; break;
        case 6: name = "QuestRepeatable"; break;
        case 9: case 10: name = "QuestTurnIn"; break;
        default: return {};
    }
    return unable ? unableCursor(name) : name;
}

/// Everything the unit cursor (CGWorldFrame 0x4F7A50) looks at, gathered by
/// the caller.
struct UnitCursorInput {
    /// The NPC may be spoken to: NPC flags set, selectable, not an enemy
    /// (0x729620).
    bool npcInteractable = false;
    uint32_t npcFlags = 0;
    uint8_t questGiverStatus = 0;
    /// The creature template's IconName ("Directions", "Gunner", ...).
    std::string_view iconName;
    /// Beyond npcInteractRange of the NPC.
    bool npcOutOfRange = false;

    /// A corpse with loot for the player (0x717B60).
    bool lootable = false;
    /// It can be looted from here, now (0x6D5A60): in reach, not falling, not
    /// stunned, no other loot open.
    bool canLootNow = false;
    /// The corpse being looted right now.
    bool isCurrentLoot = false;
    /// autoLootDefault, flipped by the auto-loot modifier (0x513700).
    bool autoLoot = false;

    /// UNIT_FLAG_SKINNABLE and a skinning lock (0x53BCE0).
    bool skinnable = false;
    /// CREATURE_TYPEFLAGS: 0x100 herb, 0x200 mining, 0x8000 engineering
    /// (0x715E50).
    uint32_t creatureTypeFlags = 0;
    bool skinOutOfRange = false;

    /// Something the player may attack (0x729A70).
    bool attackable = false;
    bool outOfMeleeRange = false;
    /// UNIT_FLAG_PACIFIED on the player.
    bool pacified = false;
};

[[nodiscard]] inline std::string unitCursorName(const UnitCursorInput& in) {
    auto pick = [](const char* name, bool unable) {
        return unable ? unableCursor(name) : std::string(name);
    };
    if (in.npcInteractable && in.npcFlags != 0) {
        const bool outOfRange = in.npcOutOfRange;
        // The quest status, then the template's own icon (0x715EA0).
        std::string q = questStatusCursor(in.questGiverStatus, outOfRange);
        if (!q.empty()) return q;
        if (!in.iconName.empty()) return pick(std::string(in.iconName).c_str(), outOfRange);
        const uint32_t f = in.npcFlags;
        if (f & kNpcRepair) return pick("RepairNPC", outOfRange);
        if (f & kNpcInnkeeper) return pick("Innkeeper", outOfRange);
        if (f & kNpcFlightMaster) return pick("Taxi", outOfRange);
        if (f & kNpcTrainer) return pick("Trainer", outOfRange);
        if (f & (kNpcSpiritHealer | kNpcSpiritGuide)) return pick("Speak", outOfRange);
        if (f & (kNpcBanker | kNpcGuildBanker)) return pick("Buy", outOfRange);
        if (f & (kNpcPetitioner | kNpcTabardDesigner | kNpcBattlemaster)) return pick("Speak", outOfRange);
        if (f & kNpcAuctioneer) return pick("Buy", outOfRange);
        if (f & kNpcStableMaster) return pick("Speak", outOfRange);
        if (f & kNpcVendor) return pick("Pickup", outOfRange);
        if (f & kNpcGossip) return pick("Speak", outOfRange);
        if (f & kNpcSpellClick) return pick("Interact", outOfRange);
        return {};
    }
    if (in.lootable) {
        // A corpse that cannot be looted from here wears the unable version,
        // unless it is the one already open.
        const bool unable = !in.canLootNow && !in.isCurrentLoot;
        return pick(in.autoLoot ? "LootAll" : "Pickup", unable);
    }
    if (in.skinnable) {
        if (in.creatureTypeFlags & 0x100) return pick("GatherHerbs", in.skinOutOfRange);
        if (in.creatureTypeFlags & (0x200 | 0x8000)) return pick("Mine", in.skinOutOfRange);
        return pick("Skin", in.skinOutOfRange);
    }
    if (in.attackable && !in.pacified) {
        return in.outOfMeleeRange ? std::string("UnableAttack") : std::string("Attack");
    }
    return {};
}

/// Everything the game object cursor (0x70CE10 and the type handlers' +0x14)
/// looks at.
struct GameObjectCursorInput {
    /// The type handler's IsInteractable: no cursor at all without it.
    bool interactable = false;
    uint32_t type = 0;
    /// The template's IconName, the first of the 2.0.3 strings.
    std::string_view iconName;
    /// LockType.dbc CursorName of the lock's first key, and its LockType id.
    std::string_view lockCursor;
    uint32_t lockTypeId = 0;
    /// The handler's CanUseNow (+0x1C): in reach, alive, not stunned, and so
    /// on. Out of reach the object keeps its highlight and its name and only
    /// the cursor changes.
    bool usableNow = false;
    uint8_t questGiverStatus = 0;
};

[[nodiscard]] inline std::string gameObjectCursorName(const GameObjectCursorInput& in) {
    if (!in.interactable) return {};
    const bool unable = !in.usableNow;
    if (!in.iconName.empty()) {
        return unable ? unableCursor(in.iconName) : std::string(in.iconName);
    }
    // The type handlers' own (vtable +0x14).
    auto lockOrInteract = [&]() -> std::string {
        // 0x70F9B0. Lockpicking (LockType 1) keeps its cursor even when it
        // cannot be used.
        if (!in.lockCursor.empty()) {
            if (in.lockTypeId != 1 && unable) return unableCursor(in.lockCursor);
            return std::string(in.lockCursor);
        }
        return unable ? "UnableInteract" : "Interact";
    };
    switch (in.type) {
        case 2: {  // QUESTGIVER (0x70FDE0)
            std::string q = questStatusCursor(in.questGiverStatus, unable);
            return q.empty() ? lockOrInteract() : q;
        }
        case 9: return unable ? "UnableInspect" : "Inspect";  // TEXT (0x70B540)
        case 19: return unable ? "UnableMail" : "Mail";       // MAILBOX (0x70B680)
        case 34: return unable ? "UnableBuy" : "Buy";         // GUILD_BANK (0x70B910)
        default: return lockOrInteract();
    }
}

}  // namespace interaction
}  // namespace game
}  // namespace wowee
