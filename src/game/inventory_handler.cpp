#include "game/inventory_handler.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/spell_classification.hpp"
#include "game/item_text.hpp"
#include "game/global_strings.hpp"
#include "game/stock_death_rules.hpp"
#include "game/inventory_slots.hpp"
#include "core/app_clock.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include <set>
#include "game/packet_parsers.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "game/sell_count.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include <algorithm>
#include <tuple>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace wowee {
namespace game {

// formatCopperAmount was forward-declared here, across translation units,
// to reach a file-scope copy in game_handler.cpp. It is in item_text.hpp now.

InventoryHandler::InventoryHandler(GameHandler& owner)
    : owner_(owner) {}

namespace {
constexpr uint32_t kConsumableSubclassItemEnhancement = 6;
// SpellCastTargetFlags bit set by Spell.dbc for spells cast onto another item.
constexpr uint32_t kSpellTargetFlagItem = 0x10;
constexpr uint32_t kBuybackWireSlotStart = 74;
constexpr uint32_t kBuybackWireSlotCount = 12;
constexpr uint32_t kBuybackWireSlotEnd =
    kBuybackWireSlotStart + kBuybackWireSlotCount;

/// A GlobalStrings line by name, from the interface data, else the English
/// text the stock rules carry.
std::string stockGlobal(GameHandler& owner, const char* key) {
    std::string text = globalString(owner.services().assetManager, key);
    if (text.empty()) text = stock_death::englishGlobal(key);
    return text;
}

/// The line an item arriving in the bags gets, as the stock client words it:
/// CHAT_MSG_LOOT with LOOT_ITEM_CREATED_SELF ("You create: %s.") for a made
/// item, LOOT_ITEM_SELF ("You receive loot: %s.") for a looted one and
/// LOOT_ITEM_PUSHED_SELF ("You receive item: %s.") for one handed over, the
/// _MULTIPLE forms ("%sx%d") for a stack - and nothing when the push says not
/// to show it in chat. This was a system line, "Received item: %s x3", for all
/// of them.
void addItemPushLine(GameHandler& owner, const std::string& link, uint32_t count,
                     uint32_t received, uint32_t created, uint32_t displayInChat) {
    if (!displayInChat) return;
    const bool many = count > 1;
    const char* key = created ? (many ? "LOOT_ITEM_CREATED_SELF_MULTIPLE" : "LOOT_ITEM_CREATED_SELF")
                    : received == 0 ? (many ? "LOOT_ITEM_SELF_MULTIPLE" : "LOOT_ITEM_SELF")
                                    : (many ? "LOOT_ITEM_PUSHED_SELF_MULTIPLE" : "LOOT_ITEM_PUSHED_SELF");
    std::string fmt = globalString(owner.services().assetManager, key);
    if (fmt.empty()) {
        fmt = created ? "You create: %s" : received == 0 ? "You receive loot: %s" : "You receive item: %s";
        fmt += many ? "x%d." : ".";
    }
    owner.addLocalChatLine(ChatType::LOOT, formatGlobalString(fmt, {link, std::to_string(count)}));
}

/// A loot roll's chat line, CHAT_MSG_LOOT as the stock client sends them.
void addLootRollLine(GameHandler& owner, const stock_death::LootRollLine& line) {
    if (line.key.empty()) return;
    const std::string fmt = stockGlobal(owner, line.key.c_str());
    if (fmt.empty()) return;
    owner.addLocalChatLine(ChatType::LOOT, stock_death::formatGlobal(fmt, line.args));
}

/// InventoryResult (EQUIP_ERR_*) as the GameError Wow.exe shows for it
/// (0x753BB0): SMSG_INVENTORY_CHANGE_FAILURE's code, and the one
/// SMSG_TRADE_STATUS CLOSE_WINDOW and an auction refusal carry. Null for the
/// codes it shows nothing for (0, 81, 83); a code past the table is
/// ERR_BAG_FULL, as there.
const char* inventoryResultKey(uint32_t error) {
    static const char* const kKeys[] = {
        /*  0 */ nullptr,
        /*  1 */ "ERR_CANT_EQUIP_LEVEL_I",
        /*  2 */ "ERR_CANT_EQUIP_SKILL",
        /*  3 */ "ERR_WRONG_SLOT",
        /*  4 */ "ERR_BAG_FULL",
        /*  5 */ "ERR_BAG_IN_BAG",
        /*  6 */ "ERR_TRADE_EQUIPPED_BAG",
        /*  7 */ "ERR_AMMO_ONLY",
        /*  8 */ "ERR_PROFICIENCY_NEEDED",
        /*  9 */ "ERR_NO_SLOT_AVAILABLE",
        /* 10 */ "ERR_CANT_EQUIP_EVER",
        /* 11 */ "ERR_CANT_EQUIP_EVER",
        /* 12 */ "ERR_NO_SLOT_AVAILABLE",
        /* 13 */ "ERR_2HANDED_EQUIPPED",
        /* 14 */ "ERR_2HSKILLNOTFOUND",
        /* 15 */ "ERR_WRONG_BAG_TYPE",
        /* 16 */ "ERR_WRONG_BAG_TYPE",
        /* 17 */ "ERR_ITEM_MAX_COUNT",
        /* 18 */ "ERR_NO_SLOT_AVAILABLE",
        /* 19 */ "ERR_CANT_STACK",
        /* 20 */ "ERR_NOT_EQUIPPABLE",
        /* 21 */ "ERR_CANT_SWAP",
        /* 22 */ "ERR_SLOT_EMPTY",
        /* 23 */ "ERR_ITEM_NOT_FOUND",
        /* 24 */ "ERR_DROP_BOUND_ITEM",
        /* 25 */ "ERR_OUT_OF_RANGE",
        /* 26 */ "ERR_TOO_FEW_TO_SPLIT",
        /* 27 */ "ERR_SPLIT_FAILED",
        /* 28 */ "ERR_SPELL_FAILED_REAGENTS_GENERIC",
        /* 29 */ "ERR_NOT_ENOUGH_MONEY",
        /* 30 */ "ERR_NOT_A_BAG",
        /* 31 */ "ERR_DESTROY_NONEMPTY_BAG",
        /* 32 */ "ERR_NOT_OWNER",
        /* 33 */ "ERR_ONLY_ONE_QUIVER",
        /* 34 */ "ERR_NO_BANK_SLOT",
        /* 35 */ "ERR_NO_BANK_HERE",
        /* 36 */ "ERR_ITEM_LOCKED",
        /* 37 */ "ERR_GENERIC_STUNNED",
        /* 38 */ "ERR_PLAYER_DEAD",
        /* 39 */ "ERR_CLIENT_LOCKED_OUT",
        /* 40 */ "ERR_INTERNAL_BAG_ERROR",
        /* 41 */ "ERR_ONLY_ONE_BOLT",
        /* 42 */ "ERR_ONLY_ONE_AMMO",
        /* 43 */ "ERR_CANT_WRAP_STACKABLE",
        /* 44 */ "ERR_CANT_WRAP_EQUIPPED",
        /* 45 */ "ERR_CANT_WRAP_WRAPPED",
        /* 46 */ "ERR_CANT_WRAP_BOUND",
        /* 47 */ "ERR_CANT_WRAP_UNIQUE",
        /* 48 */ "ERR_CANT_WRAP_BAGS",
        /* 49 */ "ERR_LOOT_GONE",
        /* 50 */ "ERR_INV_FULL",
        /* 51 */ "ERR_BANK_FULL",
        /* 52 */ "ERR_VENDOR_SOLD_OUT",
        /* 53 */ "ERR_BAG_FULL",
        /* 54 */ "ERR_ITEM_NOT_FOUND",
        /* 55 */ "ERR_CANT_STACK",
        /* 56 */ "ERR_BAG_FULL",
        /* 57 */ "ERR_VENDOR_SOLD_OUT",
        /* 58 */ "ERR_OBJECT_IS_BUSY",
        /* 59 */ "ERR_CANT_BE_DISENCHANTED",
        /* 60 */ "ERR_NOT_IN_COMBAT",
        /* 61 */ "ERR_NOT_WHILE_DISARMED",
        /* 62 */ "ERR_BAG_FULL",
        /* 63 */ "ERR_CANT_EQUIP_RANK",
        /* 64 */ "ERR_CANT_EQUIP_REPUTATION",
        /* 65 */ "ERR_TOO_MANY_SPECIAL_BAGS",
        /* 66 */ "ERR_LOOT_CANT_LOOT_THAT_NOW",
        /* 67 */ "ERR_ITEM_UNIQUE_EQUIPPABLE",
        /* 68 */ "ERR_VENDOR_MISSING_TURNINS",
        /* 69 */ "ERR_NOT_ENOUGH_HONOR_POINTS",
        /* 70 */ "ERR_NOT_ENOUGH_ARENA_POINTS",
        /* 71 */ "ERR_ITEM_MAX_COUNT_SOCKETED",
        /* 72 */ "ERR_MAIL_BOUND_ITEM",
        /* 73 */ "ERR_INTERNAL_BAG_ERROR",
        /* 74 */ "ERR_BAG_FULL",
        /* 75 */ "ERR_ITEM_MAX_COUNT_EQUIPPED_SOCKETED",
        /* 76 */ "ERR_ITEM_UNIQUE_EQUIPPABLE_SOCKETED",
        /* 77 */ "ERR_TOO_MUCH_GOLD",
        /* 78 */ "ERR_NOT_DURING_ARENA_MATCH",
        /* 79 */ "ERR_TRADE_BOUND_ITEM",
        /* 80 */ "ERR_CANT_EQUIP_RATING",
        /* 81 */ nullptr,
        /* 82 */ "ERR_NOT_SAME_ACCOUNT",
        /* 83 */ nullptr,
        /* 84 */ "ERR_ITEM_MAX_LIMIT_CATEGORY_COUNT_EXCEEDED_IS",
        /* 85 */ "ERR_ITEM_MAX_LIMIT_CATEGORY_SOCKETED_EXCEEDED_IS",
        /* 86 */ "ERR_SCALING_STAT_ITEM_LEVEL_EXCEEDED",
        /* 87 */ "ERR_PURCHASE_LEVEL_TOO_LOW",
        /* 88 */ "ERR_CANT_EQUIP_NEED_TALENT",
        /* 89 */ "ERR_ITEM_MAX_LIMIT_CATEGORY_EQUIPPED_EXCEEDED_IS",
        /* 90 */ "ERR_SHAPESHIFT_FORM_CANNOT_EQUIP",
        /* 91 */ "ERR_ITEM_INVENTORY_FULL_SATCHEL",
    };
    if (error < sizeof(kKeys) / sizeof(kKeys[0])) return kKeys[error];
    return "ERR_BAG_FULL";
}

void synchronizeStationaryBandageCast(GameHandler& owner) {
    // Bandages are cast through CMSG_USE_ITEM and therefore bypass
    // SpellHandler::castSpell(), which normally sends a stop before a timed cast.
    // Explicitly synchronize the stationary state so the server cannot retain a
    // stale start-forward/strafe/turn state after the character has visually stopped.
    auto& movement = owner.movementInfoRef();
    const uint32_t horizontalMask =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
    const uint32_t turnMask =
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
        static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
    movement.flags &= ~(horizontalMask | turnMask);

    owner.sendMovement(Opcode::MSG_MOVE_STOP);
    owner.sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
    owner.sendMovement(Opcode::MSG_MOVE_STOP_TURN);
    owner.sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

bool usesVisibleItemDisplayIds() {
    return false;
}

/// UNIT_INVENTORY_CHANGED for every unit token another player answers to.
///
/// The inspect paperdoll (and any addon showing another player's gear) redraws
/// its squares on this event for the unit it is showing, and nothing fired it
/// for anyone but the player - so gear that changed, or an item whose template
/// arrived after the window drew, left the square as it was drawn: blank, or a
/// question mark for good.
void fireUnitInventoryChangedFor(GameHandler& gh, uint64_t guid) {
    if (guid == 0 || guid == gh.getPlayerGuid() || !gh.addonEventCallbackRef()) return;
    auto fire = [&gh](const std::string& unit) {
        gh.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {unit});
    };
    if (guid == gh.getTargetGuid())    fire("target");
    if (guid == gh.getFocusGuid())     fire("focus");
    if (guid == gh.getMouseoverGuid()) fire("mouseover");
    // Numbered as resolveUnitGuid numbers them: others only, and in a raid
    // only the player's own subgroup.
    const auto& pd = gh.getPartyData();
    const bool raid = groupIsRaid(pd.groupType);
    int party = 0;
    for (const auto& m : pd.members) {
        if (m.guid == gh.getPlayerGuid()) continue;
        if (raid && m.subGroup != pd.subGroup) continue;
        if (++party > 4) break;
        if (m.guid == guid) fire("party" + std::to_string(party));
    }
}

constexpr int kTbcVisibleItemStride = 16;

int tbcVisibleItemBaseFallback() {
    const uint16_t invSlotHead = fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD);
    constexpr int kVisibleSlots = 19;
    const int visibleBytes = kVisibleSlots * kTbcVisibleItemStride;
    if (invSlotHead != 0xFFFF && invSlotHead >= visibleBytes) {
        return static_cast<int>(invSlotHead) - visibleBytes;
    }
    return 346;
}

bool visibleEquipmentFieldDiagEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("WOWEE_VISIBLE_EQUIP_FIELD_DIAG");
        if (!raw || raw[0] == '\0') return false;
        std::string value(raw);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value != "0" && value != "false" && value != "off";
    }();
    return enabled;
}

std::array<uint8_t, 19> inferredVisibleInventoryTypes() {
    return {
        InvType::HEAD,
        InvType::NECK,
        InvType::SHOULDERS,
        InvType::SHIRT,
        InvType::CHEST,
        InvType::WAIST,
        InvType::LEGS,
        InvType::FEET,
        InvType::WRISTS,
        InvType::HANDS,
        InvType::FINGER,
        InvType::FINGER,
        InvType::TRINKET,
        InvType::TRINKET,
        InvType::BACK,
        InvType::MAIN_HAND,
        InvType::SHIELD,
        InvType::RANGED_BOW,
        InvType::TABARD,
    };
}

/// Who an item's spell is being used on.
///
/// The item's class used to decide this, and the answer for every consumable
/// was the player. So an item whose spell is meant to land on something you
/// have selected - a treat fed to a particular creature, a quest item used on
/// an NPC - was sent at the player who used it and refused, while an item that
/// was not a consumable at all was sent with no target whatsoever.
///
/// The spell says what it wants and is already read a few lines up for the
/// item case, so it answers this too. TARGET_FLAG_UNIT is 0x02 and
/// TARGET_FLAG_UNIT_ALLY 0x100, both as AzerothCore's SpellInfo.h has them;
/// the ally flag is not sent, it only says whether the selected target is an
/// allowed one. That is what keeps a bandage used with an enemy selected on
/// the player rather than sending it at the enemy to be refused.
/// Whether an item's spell has to be aimed at somebody.
///
/// Spell.dbc has a Targets column that looks like the answer and is not:
/// measured over the shipped file it is zero for every bandage rank, so a
/// client that asks it is told no spell ever needs a target. What carries it
/// is EffectImplicitTargetA, which reads 21 - ally - for those same spells.
///
/// Look those spells up by id and not by name. Spell.dbc holds two rows per
/// bandage: the tradeskill recipe that makes one is called "Runecloth Bandage"
/// and reads effect 24, CREATE_ITEM, aimed at the caster - while the spell the
/// item actually casts is one of the twelve called "First Aid" (746, 1159,
/// 3267, 3268, 7926, 7927, 10838, 10839, 18608, 18610, 27030, 27031), effect 6
/// aimed at an ally. Reading the recipe instead says a bandage lands on the
/// caster and can never be used on anyone else, which is not true of either row.
bool spellNeedsAUnit(uint32_t implicitTargetA) {
    // Every friendly aim, not just 21: a scroll or a bandage whose spell reads
    // 45 or 57 needs somebody picked exactly as one reading 21 does, and asking
    // about the one value sent those at whatever was selected.
    return spellclass::requiresFriendlyTarget(implicitTargetA) ||
           implicitTargetA == spellclass::kImplicitTargetEnemy ||
           implicitTargetA == spellclass::kImplicitTargetAny;
}

uint64_t targetGuidForUseItem(GameHandler& owner, const ItemQueryResponseData* info,
                              uint32_t useSpellId) {
    const uint32_t aim = useSpellId != 0 ? owner.getSpellImplicitTargetA(useSpellId) : 0;

    // When the client has no basis to decide, defer to what the player picked.
    //
    // Two ways that happens, and a private core's custom content produces both
    // routinely: the item declares no use spell, or it declares one this
    // install's Spell.dbc has never heard of. Either way the aim reads zero,
    // which is indistinguishable from "aims at the caster" - so a treat meant
    // for a particular creature was sent at the player who used it, with the
    // creature selected on screen the whole time.
    //
    // Only when something is actually selected, and only when the spell is
    // genuinely unknown: a spell the client does know and that says caster is
    // still sent at the caster.
    const bool aimUnknown = useSpellId == 0 || !owner.isSpellKnownToClient(useSpellId);
    if (aimUnknown) {
        const uint64_t target = owner.getTargetGuid();
        if (target != 0 && target != owner.getPlayerGuid()) return target;
    }

    if (spellNeedsAUnit(aim)) {
        const uint64_t target = owner.getTargetGuid();
        if (target != 0 && target != owner.getPlayerGuid()) {
            bool allowed = true;
            auto entity = owner.getEntityManager().getEntity(target);
            if (auto unit = std::dynamic_pointer_cast<Unit>(entity)) {
                if (spellclass::requiresFriendlyTarget(aim))      allowed = !unit->isHostile();
                else if (aim == spellclass::kImplicitTargetEnemy) allowed = unit->isHostile();
            }
            if (allowed) return target;
        }
        // Nothing selected, or nothing this spell may be used on. The player is
        // the fallback for a friendly spell - a bandage with no target goes on
        // you - and the caller arms a cursor for the rest.
        return owner.getPlayerGuid();
    }

    if (!info || !info->valid || info->itemClass != ITEM_CLASS_CONSUMABLE) return 0;
    if (info->subClass == kConsumableSubclassItemEnhancement) return 0;
    return owner.getPlayerGuid();
}

/// The put-down sound of an item that has just landed in the player's bags.
/// The stock SMSG_ITEM_PUSH_RESULT path (0x523900) plays it for every push to
/// the local player - loot, quest reward, purchase, craft - through 0x4d0090
/// with ItemGroupSounds column 1 of the item's display. igLootCreature.wav is
/// only the stand-in for an install without the tables.
void playItemPushSound(GameHandler& owner, uint32_t displayInfoId) {
    auto* ac = owner.services().audioCoordinator;
    if (!ac || ac->playItemFoley(displayInfoId, /*pickup=*/false)) return;
    if (auto* sfx = ac->getUiSoundManager()) sfx->playLootItem();
}

/// The pickup sound of a loot slot being taken. The stock LootSlot (0x589140),
/// its bind-on-pickup confirm (0x589300) and the autoloot pass (0x588f90) play
/// the item display's ItemGroupSounds pickup sound as they send the autostore,
/// and only for a slot the player may take (slot type 0 or 4); a slot under a
/// roll, held by a master looter or locked is refused without a sound.
void playLootSlotPickupSound(GameHandler& owner, const LootItem& item) {
    if (item.lootSlotType != 0 && item.lootSlotType != 4) return;
    uint32_t displayId = item.displayInfoId;
    if (displayId == 0) {
        if (const auto* info = owner.getItemInfo(item.itemId)) displayId = info->displayInfoId;
    }
    if (auto* ac = owner.services().audioCoordinator) ac->playItemFoley(displayId, /*pickup=*/true);
}
} // namespace

// ============================================================
// Opcode Registration
// ============================================================

/// Says what was looted, once per loot.
///
/// Both paths that learn about looted money end here - the server's notify and
/// the fallback timer for servers that do not send one - so the line cannot
/// differ between them.
///
/// No sound here. The stock SMSG_LOOT_MONEY_NOTIFY handler (0x6ceca0) only
/// prints YOU_LOOT_MONEY or LOOT_MONEY_SPLIT; the clink comes from the
/// PLAYER_FIELD_COINAGE update that follows (see applyPlayerStatFields), and
/// it is LOOTWINDOWCOINSOUND whatever the amount.
///
/// The line is CHAT_MSG_MONEY, "You loot 1 Silver, 5 Copper" - the coins
/// spelled out by 0x7e7d80 - or "Your share of the loot is ..." for a share.
/// It was a system line, "Looted: 1s 5c".
void InventoryHandler::announceLootMoney(uint64_t lootGuid, uint32_t amount, bool soleLooter) {
    if (amount == 0) return;
    auto it = localLootState_.find(lootGuid);
    if (it != localLootState_.end()) {
        if (it->second.moneyTaken) return;
        it->second.moneyTaken = true;
    }
    owner_.addLocalChatLine(ChatType::MONEY,
        stock_death::lootMoneyLine(amount, soleLooter,
                                   [this](const char* key) { return stockGlobal(owner_, key); }));
    if (lootGuid != 0) recentLootMoneyAnnounceCooldowns_[lootGuid] = 1.5f;
}

void InventoryHandler::tickLootMoneyFallback(float deltaTime) {
    if (pendingLootMoneyNotifyTimer_ <= 0.0f) return;
    pendingLootMoneyNotifyTimer_ -= deltaTime;
    if (pendingLootMoneyNotifyTimer_ > 0.0f) return;
    pendingLootMoneyNotifyTimer_ = 0.0f;
    announceLootMoney(pendingLootMoneyGuid_, pendingLootMoneyAmount_);
    pendingLootMoneyGuid_ = 0;
    pendingLootMoneyAmount_ = 0;
}

void InventoryHandler::registerOpcodes(DispatchTable& table) {
    // ---- Item query response ----
    table[Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE] = [this](network::Packet& packet) { handleItemQueryResponse(packet); };
    table[Opcode::SMSG_ITEM_QUERY_MULTIPLE_RESPONSE] = [this](network::Packet& packet) { handleItemQueryResponse(packet); };

    // ---- Loot ----
    table[Opcode::SMSG_LOOT_RESPONSE] = [this](network::Packet& packet) { handleLootResponse(packet); };
    table[Opcode::SMSG_LOOT_RELEASE_RESPONSE] = [this](network::Packet& packet) { handleLootReleaseResponse(packet); };
    table[Opcode::SMSG_LOOT_REMOVED] = [this](network::Packet& packet) { handleLootRemoved(packet); };
    table[Opcode::SMSG_LOOT_ROLL] = [this](network::Packet& packet) { handleLootRoll(packet); };
    table[Opcode::SMSG_LOOT_ROLL_WON] = [this](network::Packet& packet) { handleLootRollWon(packet); };
    table[Opcode::SMSG_LOOT_MASTER_LIST] = [this](network::Packet& packet) {
        masterLootCandidates_.clear();
        if (!packet.hasRemaining(1)) return;
        uint8_t mlCount = packet.readUInt8();
        masterLootCandidates_.reserve(mlCount);
        for (uint8_t i = 0; i < mlCount; ++i) {
            if (!packet.hasRemaining(8)) break;
            masterLootCandidates_.push_back(packet.readUInt64());
        }
        // The candidate list arrives because the master looter picked an item,
        // and the menu that assigns it is built from exactly this. Parsed and
        // stored already; nothing was told it had come, so the menu never
        // opened. Both events: one opens it, one fills it, and they are the
        // same frame rather than two.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("OPEN_MASTER_LOOT_LIST", {});
            owner_.addonEventCallbackRef()("UPDATE_MASTER_LOOT_LIST", {});
        }
    };

    // ---- Loot money / misc consume ----
    table[Opcode::SMSG_LOOT_MONEY_NOTIFY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t amount = packet.readUInt32();
        // 1 when the player took it alone (YOU_LOOT_MONEY), 0 for a share.
        const bool soleLooter = !packet.hasRemaining(1) || packet.readUInt8() != 0;
        owner_.playerMoneyCopperRef() += amount;
        owner_.pendingMoneyDeltaRef() = amount;
        owner_.pendingMoneyDeltaTimerRef() = 2.0f;
        uint64_t notifyGuid = pendingLootMoneyGuid_ != 0 ? pendingLootMoneyGuid_ : currentLoot_.lootGuid;
        pendingLootMoneyGuid_ = 0;
        pendingLootMoneyAmount_ = 0;
        pendingLootMoneyNotifyTimer_ = 0.0f;
        announceLootMoney(notifyGuid, amount, soleLooter);
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_MONEY", {});
        // Both packets report the same thing and a server may send either, so
        // the coin goes on whichever arrives first.
        //
        // Only for an amount that is this corpse's whole purse, though: in a
        // group this notify carries one player's share and reaches everyone,
        // including someone standing over a different corpse of their own.
        // SMSG_LOOT_CLEAR_MONEY is the one that names the loot itself, and it
        // is what clears the coin in that case.
        if (amount == currentLoot_.gold) clearLootMoney();
    };
    table[Opcode::SMSG_LOOT_CLEAR_MONEY] = [this](network::Packet& /*packet*/) {
        clearLootMoney();
    };

    // ---- Read item (books) (moved from GameHandler) ----
    table[Opcode::SMSG_READ_ITEM_OK] = [](network::Packet& packet) {
        packet.skipAll();
    };
    table[Opcode::SMSG_READ_ITEM_FAILED] = [this](network::Packet& packet) {
        owner_.addUIError("You cannot read this item.");
        owner_.raiseUiError("You cannot read this item.");
        packet.skipAll();
    };

    // ---- Loot roll start / notifications ----
    table[Opcode::SMSG_LOOT_START_ROLL] = [this](network::Packet& packet) {
        // objectGuid(8) + mapId(4) (WotLK) + lootSlot(4) + itemId(4) +
        // randSuffix(4) + randProp(4) + itemCount(4) + countdown(4) + voteMask(1)
        //
        // itemCount - "items in stack" - was missing, so the countdown was read
        // from it and the vote mask from the countdown's first byte. The
        // countdown drives the bar that times the roll out and the mask decides
        // which of need, greed and disenchant are even offered.
        //
        // Chosen by the length rather than by the expansion: WotLK is verified
        // against Group::SendLootStartRoll, and rather than guess whether a
        // pre-WotLK realm sends the field, take it when the packet is long
        // enough to hold it.
        const bool hasMapId = isActiveExpansion("wotlk");
        const size_t baseSz = hasMapId ? 33 : 29;
        if (packet.getRemainingSize() < baseSz) return;
        const bool hasItemCount = packet.getRemainingSize() >= baseSz + 4;
        uint64_t objectGuid = packet.readUInt64();
        // A roll on another map is not this player's to see (0x6fb2c0).
        const uint32_t rollMapId = hasMapId ? packet.readUInt32() : owner_.getCurrentMapId();
        uint32_t lootSlot = packet.readUInt32();
        uint32_t itemId   = packet.readUInt32();
        /*uint32_t randSuffix =*/ packet.readUInt32();
        (void)packet.readUInt32(); // random property
        if (hasItemCount) (void)packet.readUInt32(); // items in stack
        uint32_t countdown = packet.readUInt32();
        uint8_t  voteMask  = packet.readUInt8();
        if (rollMapId != owner_.getCurrentMapId()) return;

        // Resolve item name from cache
        owner_.ensureItemInfo(itemId);
        auto* info = owner_.getItemInfo(itemId);
        std::string itemName = (info && !info->name.empty()) ? info->name : ("Item #" + std::to_string(itemId));
        uint8_t quality = info ? static_cast<uint8_t>(info->quality) : 1;

        // Its own entry, kept alongside whatever else is being rolled for.
        // A boss puts four of these up at once and there was one slot to hold
        // them, so the last to arrive answered for all of them.
        LootRollEntry& roll = beginLootRoll(objectGuid, lootSlot);
        roll.itemId = itemId;
        roll.itemName = itemName;
        roll.itemQuality = quality;
        roll.rollCountdownMs = countdown;
        roll.voteMask = voteMask;
        roll.rollStartedAt = std::chrono::steady_clock::now();
        // No chat line: the stock client says nothing when a roll starts (it
        // used to print "Loot roll started for ..."). Four windows at most;
        // a fifth waits for one to close.
        int shownRolls = 0;
        for (const auto& r : lootRolls_) if (r.shown) ++shownRolls;
        roll.shown = shownRolls < stock_death::kMaxShownLootRolls;
        // The roll id the interface will ask every later question by. Its own
        // number: the slot plus one was unique only while one roll was open,
        // and two corpses both rolling their first item shared it.
        if (roll.shown && owner_.addonEventCallbackRef()) {
            // ...and the time to roll in, which the packet carried all along
            // and this read past. GroupLootFrame_OpenNewFrame takes it as the
            // second argument and sizes the countdown bar from it; without one
            // the bar had no length and the window could not time out.
            owner_.addonEventCallbackRef()("START_LOOT_ROLL",
                                           {std::to_string(roll.rollId),
                                            std::to_string(countdown)});
        }
    };

    table[Opcode::SMSG_LOOT_ALL_PASSED] = [this](network::Packet& packet) {
        // objectGuid(8) + lootSlot(4) + itemId(4) + randSuffix(4) + randProp(4)
        if (!packet.hasRemaining(24)) return;
        const uint64_t passedGuid = packet.readUInt64();
        const uint32_t passedSlot = packet.readUInt32();
        uint32_t itemId     = packet.readUInt32();
        /*uint32_t randSuffix =*/ packet.readUInt32();
        (void)packet.readUInt32(); // random property
        owner_.ensureItemInfo(itemId);
        auto* allPassInfo = owner_.getItemInfo(itemId);
        std::string allPassName = (allPassInfo && !allPassInfo->name.empty())
            ? allPassInfo->name : ("Item #" + std::to_string(itemId));
        uint32_t allPassQuality = allPassInfo ? allPassInfo->quality : 1u;
        // LOOT_ROLL_ALL_PASSED, CHAT_MSG_LOOT (0x6fb470).
        addLootRollLine(owner_, {"LOOT_ROLL_ALL_PASSED",
                                 {buildItemLink(itemId, allPassQuality, allPassName)}});
        endLootRoll(passedGuid, passedSlot);
    };

    table[Opcode::SMSG_LOOT_ITEM_NOTIFY] = [this](network::Packet& packet) {
        // objectGuid(8) + lootSlot(1) [Classic: uint32; WotLK: uint8]
        if (!packet.hasRemaining(9)) return;
        /*uint64_t objectGuid =*/ packet.readUInt64();
        uint32_t lootSlot;
        if (isActiveExpansion("wotlk")) {
            lootSlot = packet.readUInt8();
        } else {
            if (!packet.hasRemaining(4)) return;
            lootSlot = packet.readUInt32();
        }
        // Try to resolve item name
        uint32_t itemId = 0;
        for (const auto& lootItem : currentLoot_.items) {
            if (lootItem.slotIndex == static_cast<uint8_t>(lootSlot)) {
                itemId = lootItem.itemId;
                break;
            }
        }
        if (itemId != 0) {
            auto* notifInfo = owner_.getItemInfo(itemId);
            std::string itemName = (notifInfo && !notifInfo->name.empty())
                ? notifInfo->name : ("Item #" + std::to_string(itemId));
            uint32_t notifyQuality = notifInfo ? notifInfo->quality : 1u;
            std::string itemLink2 = buildItemLink(itemId, notifyQuality, itemName);
            // Nothing said: Wow.exe reads this packet and drops it (0x6CED80).
            // The item's arrival is SMSG_ITEM_PUSH_RESULT's "You receive
            // loot:" line, and saying it here as well printed it twice.
            (void)itemLink2;
        }
    };

    table[Opcode::SMSG_LOOT_SLOT_CHANGED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t slotIdx = packet.readUInt8();
            LOG_DEBUG("SMSG_LOOT_SLOT_CHANGED: slot=", (int)slotIdx);
            // The loot frame redraws the one row from this. It carries the
            // slot, and the slot is what the event carries - the interface
            // reads arg1 to know which button to refresh, so a bare fire would
            // make it redraw the wrong one.
            //
            // Slots are zero-based on the wire and one-based in the interface.
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("LOOT_SLOT_CHANGED",
                                               {std::to_string(slotIdx + 1)});
            }
        }
    };

    // ---- Item push result ----
    table[Opcode::SMSG_ITEM_PUSH_RESULT] = [this](network::Packet& packet) {
        // guid(8)+received(4)+created(4)+displayInChat(4)+bagSlot(1)
        //   +slot(4)+itemId(4)+suffixFactor(4)+randomPropertyId(4)+count(4)
        // TBC and WotLK then add countInInventory(4); classic does not.
        if (!packet.hasRemaining(41)) return;
        uint64_t guid = packet.readUInt64();
        if (guid != owner_.getPlayerGuid()) { packet.skipAll(); return; }
        const uint32_t received      = packet.readUInt32();
        const uint32_t created       = packet.readUInt32();
        const uint32_t displayInChat = packet.readUInt32();
        const uint8_t bagSlot = packet.readUInt8();
        /*uint32_t slot          =*/ packet.readUInt32();
        uint32_t itemId = packet.readUInt32();
        /*uint32_t suffixFactor  =*/ packet.readUInt32();
        int32_t randomProp = static_cast<int32_t>(packet.readUInt32());
        uint32_t count = packet.readUInt32();

        auto* info = owner_.getItemInfo(itemId);
        // Which bag button the item landed in, in the numbering the interface
        // uses. Item::GetBagSlot answers the container's own inventory slot -
        // 19 to 22 for the four worn bags - or INVENTORY_SLOT_BAG_0, 255, for
        // the backpack. FrameXML's buttons take their ids from
        // GetInventorySlotInfo("Bag0Slot") and friends, which are 20 to 23,
        // with MainMenuBarBackpackButton declared id="0" in the XML. So the
        // worn bags are one apart and the backpack is a special case.
        const int bagButtonId = (bagSlot == 255) ? 0 : (static_cast<int>(bagSlot) + 1);

        if (!info || info->name.empty()) {
            // Item info not yet cached - defer notification
            owner_.pendingItemPushNotifsRef().push_back({.itemId = itemId, .count = count, .bagButtonId = bagButtonId,
                                                         .received = received, .created = created,
                                                         .displayInChat = displayInChat});
            owner_.ensureItemInfo(itemId);
            return;
        }
        std::string itemName = info->name;
        if (randomProp != 0) {
            std::string suffix = owner_.getRandomPropertyName(randomProp);
            if (!suffix.empty()) itemName += " " + suffix;
        }
        uint32_t quality = info->quality;
        std::string link = buildItemLink(itemId, quality, itemName);
        addItemPushLine(owner_, link, count, received, created, displayInChat);
        playItemPushSound(owner_, info->displayInfoId);
        if (owner_.addonEventCallbackRef()) {
            fireBagUpdates();
            // ITEM_PUSH(bagSlot, icon), which is what the bag button's flash
            // animation reads: mainmenubarbagbuttons compares its own GetID
            // against the first and calls ReplaceIconTexture with the second.
            // This sent the item id and the stack count instead - two numbers
            // where a bag id and a texture were meant - so the comparison
            // never matched and the animation has never once played.
            owner_.addonEventCallbackRef()("ITEM_PUSH",
                    {std::to_string(bagButtonId),
                     owner_.getItemIconPath(info->displayInfoId)});
        }
    };

    // ---- Open container ----
    table[Opcode::SMSG_OPEN_CONTAINER] = [](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t containerGuid = packet.readUInt64();
            LOG_DEBUG("SMSG_OPEN_CONTAINER: guid=0x", std::hex, containerGuid, std::dec);
        }
    };

    // ---- Sell / Buy / Inventory ----
    table[Opcode::SMSG_SELL_ITEM] = [this](network::Packet& packet) {
        if ((packet.getRemainingSize()) >= 17) {
            uint64_t vendorGuid = packet.readUInt64();
            uint64_t itemGuid = packet.readUInt64();
            uint8_t result = packet.readUInt8();
            LOG_INFO("SMSG_SELL_ITEM: vendorGuid=0x", std::hex, vendorGuid,
                     " itemGuid=0x", itemGuid, std::dec,
                     " result=", static_cast<int>(result));
            if (result == 0) {
                auto pending = pendingSellToBuyback_.find(itemGuid);
                if (pending != pendingSellToBuyback_.end()) {
                    pendingSellToBuyback_.erase(pending);
                    reconcileBuybackSlots();
                } else {
                    LOG_WARNING("Successful sale had no pending buyback entry: itemGuid=0x",
                                std::hex, itemGuid, std::dec);
                }
                // No sound here: the stock SMSG_SELL_ITEM handler plays none.
                // The sale's clink is LOOTWINDOWCOINSOUND from the coinage
                // update the server sends with it (applyPlayerStatFields).
                if (owner_.addonEventCallbackRef()) {
                    fireBagUpdates();
                    owner_.addonEventCallbackRef()("PLAYER_MONEY", {});
                }
            } else {
                auto it = pendingSellToBuyback_.find(itemGuid);
                if (it != pendingSellToBuyback_.end()) {
                    pendingSellToBuyback_.erase(it);
                }
                const auto rejected = std::find_if(
                    buybackItems_.begin(), buybackItems_.end(),
                    [itemGuid](const BuybackItem& item) {
                        return item.itemGuid == itemGuid;
                    });
                if (rejected != buybackItems_.end()) {
                    buybackItems_.erase(rejected);
                    notifyBuybackChanged();
                }
                // SellResult as Wow.exe (0x6D1540) words it, on the red error
                // line and only with the vendor at hand: 1 not found, 2 not
                // interested, 3 hates you, 4 not yours, 6 a bag not empty,
                // 7 doesn't buy, 8 must repair, 9 internal; 5 and the rest
                // say nothing.
                static const char* const kSellKeys[] = {
                    nullptr, "ERR_ITEM_NOT_FOUND", "ERR_VENDOR_NOT_INTERESTED", "ERR_VENDOR_HATES_YOU",
                    "ERR_NOT_OWNER", nullptr, "ERR_DESTROY_NONEMPTY_BAG", "ERR_VENDOR_DOESNT_BUY",
                    "ERR_MUST_REPAIR_DURABILITY", "ERR_INTERNAL_BAG_ERROR",
                };
                const char* key = result < 10 ? kSellKeys[result] : nullptr;
                if (key && owner_.getEntityManager().getEntity(vendorGuid))
                    owner_.addUIError(stockGlobal(owner_, key));
                const char* msg = key ? key : "-";
                LOG_WARNING("SMSG_SELL_ITEM error: ", (int)result, " (", msg,
                            ") itemGuid=0x", std::hex, itemGuid,
                            " vendorGuid=0x", vendorGuid, std::dec);
            }
        }
    };

    table[Opcode::SMSG_INVENTORY_CHANGE_FAILURE] = [this](network::Packet& packet) {
        if (packet.getRemainingSize() < 1) return;
        uint8_t error = packet.readUInt8();
        if (error == 0) return;

        LOG_WARNING("SMSG_INVENTORY_CHANGE_FAILURE: error=", (int)error);
        // The two item guids and the bag, then a level for the two level
        // refusals (1 equip, 87 purchase).
        uint32_t requiredLevel = 0;
        if (packet.hasRemaining(17)) {
            packet.readUInt64();
            packet.readUInt64();
            packet.readUInt8();
            if ((error == 1 || error == 87) && packet.hasRemaining(4))
                requiredLevel = packet.readUInt32();
        }
        packet.skipAll();

        // The GameError Wow.exe maps the code to (0x6E2E90 case 0x112,
        // 0x753BB0) - the red error, nothing in chat.
        if (const char* key = inventoryResultKey(error)) {
            std::vector<std::string> args;
            if (error == 1 || error == 87) args.push_back(std::to_string(requiredLevel));
            owner_.displayGameMessage(key, stockGlobal(owner_, key), args);
        }
        if (error == 1) {
            owner_.playErrorSpeech(audio::PlayerErrorSpeech::CANT_EQUIP_LEVEL);
            return;
        }

        // Character speech response for errors with a matching voice line -
        // the only sound the stock client gives these (its error table names
        // a VocalUISounds line for them, no interface sound).
        using audio::PlayerErrorSpeech;
        switch (error) {
            case 4:  owner_.playErrorSpeech(PlayerErrorSpeech::BAG_FULL); break;
            case 7:  owner_.playErrorSpeech(PlayerErrorSpeech::AMMO_ONLY); break;
            case 8:  owner_.playErrorSpeech(PlayerErrorSpeech::CANT_USE_ITEM); break;
            case 10:
            case 11: owner_.playErrorSpeech(PlayerErrorSpeech::CANT_EQUIP_EVER); break;
            case 17: owner_.playErrorSpeech(PlayerErrorSpeech::ITEM_MAX_COUNT); break;
            case 20: owner_.playErrorSpeech(PlayerErrorSpeech::NOT_EQUIPPABLE); break;
            case 24: owner_.playErrorSpeech(PlayerErrorSpeech::CANT_DROP_SOULBOUND); break;
            case 25: owner_.playErrorSpeech(PlayerErrorSpeech::OUT_OF_RANGE); break;
            case 29: owner_.playErrorSpeech(PlayerErrorSpeech::NOT_ENOUGH_MONEY); break;
            case 30: owner_.playErrorSpeech(PlayerErrorSpeech::NOT_A_BAG); break;
            case 36: owner_.playErrorSpeech(PlayerErrorSpeech::ITEM_LOCKED); break;
            case 50:
            case 51: owner_.playErrorSpeech(PlayerErrorSpeech::INVENTORY_FULL); break;
            default: break;
        }
    };

    table[Opcode::SMSG_BUY_FAILED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(13)) {
            uint64_t vendorGuid = packet.readUInt64();
            uint32_t itemIdOrSlot = packet.readUInt32();
            uint8_t errCode = packet.readUInt8();
            LOG_INFO("SMSG_BUY_FAILED: vendorGuid=0x", std::hex, vendorGuid, std::dec,
                     " item/slot=", itemIdOrSlot,
                     " err=", static_cast<int>(errCode),
                     " pendingBuybackSlot=", pendingBuybackSlot_,
                     " pendingBuybackWireSlot=", pendingBuybackWireSlot_,
                     " pendingBuyItemId=", pendingBuyItemId_,
                     " pendingBuyItemSlot=", pendingBuyItemSlot_);
            if (pendingBuybackSlot_ >= 0) {
                if (errCode == 0) {
                    // A missing slot is stale local state.  Never scan adjacent
                    // slots: another item may legitimately occupy them.
                    const auto stale = std::find_if(
                        buybackItems_.begin(), buybackItems_.end(),
                        [this](const BuybackItem& item) {
                            return item.wireSlot == pendingBuybackWireSlot_;
                        });
                    if (stale != buybackItems_.end()) {
                        buybackItems_.erase(stale);
                        notifyBuybackChanged();
                    }
                    pendingBuybackSlot_ = -1;
                    pendingBuybackWireSlot_ = 0;
                    if (currentVendorItems_.vendorGuid != 0 && owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
                        auto pkt = ListInventoryPacket::build(currentVendorItems_.vendorGuid);
                        owner_.getSocket()->send(pkt);
                    }
                    owner_.displayGameMessage("ERR_ITEM_NOT_FOUND", stockGlobal(owner_, "ERR_ITEM_NOT_FOUND"));
                    return;
                }
                pendingBuybackSlot_ = -1;
                pendingBuybackWireSlot_ = 0;
            }
            // BuyResult as Wow.exe (0x6CDBA0) words it, red errors: 1 and 7
            // sold out, 2 money, 4 the vendor hates you, 5 too far, 8 carry no
            // more, 11 rank, 12 reputation, 6 nothing, anything else not found.
            const char* key = "ERR_ITEM_NOT_FOUND";
            switch (errCode) {
                case 1: case 7: key = "ERR_VENDOR_SOLD_OUT"; break;
                case 2:  key = "ERR_NOT_ENOUGH_MONEY"; break;
                case 4:  key = "ERR_VENDOR_HATES_YOU"; break;
                case 5:  key = "ERR_VENDOR_TOO_FAR"; break;
                case 6:  key = nullptr; break;
                case 8:  key = "ERR_ITEM_MAX_COUNT"; break;
                case 11: key = "ERR_CANT_EQUIP_RANK"; break;
                case 12: key = "ERR_CANT_EQUIP_REPUTATION"; break;
                default: break;
            }
            if (key) owner_.displayGameMessage(key, stockGlobal(owner_, key));
            if (errCode == 2)
                owner_.playErrorSpeech(audio::PlayerErrorSpeech::NOT_ENOUGH_MONEY);
            else if (errCode == 8)
                owner_.playErrorSpeech(audio::PlayerErrorSpeech::INVENTORY_FULL);
        }
    };

    table[Opcode::SMSG_BUY_ITEM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(20)) {
            /*uint64_t vendorGuid =*/ packet.readUInt64();
            const uint32_t vendorSlot = packet.readUInt32();   // 1-based, as the list numbers them
            const uint32_t newCount   = packet.readUInt32();   // 0xFFFFFFFF for an unlimited item
            uint32_t itemCount  = packet.readUInt32();

            // The stock left, said now rather than at the next reopen.
            //
            // The slot and the remaining count were read off this packet and
            // dropped. For a limited item - a recipe the vendor has one of, a
            // faction reward - that meant the count beside it never changed
            // when it was bought: MERCHANT_UPDATE redrew from the list the
            // window opened with, which still had the old number, and only
            // closing and reopening the vendor pulled a fresh one. The server
            // numbers the slot the same way here and in the list, so the two
            // match directly; 0xFFFFFFFF is its way of saying unlimited, which
            // is -1 here.
            for (auto& vi : currentVendorItems_.items) {
                if (vi.slot != vendorSlot) continue;
                vi.maxCount = (newCount == 0xFFFFFFFFu)
                                  ? -1 : static_cast<int32_t>(newCount);
                break;
            }
            // Successful buyback: remove the entry from the local buyback list.
            // Without this the pending slot lingered and a later unrelated
            // SMSG_BUY_FAILED could misread it as a buyback retry.
            if (pendingBuybackSlot_ >= 0) {
                const auto bought = std::find_if(
                    buybackItems_.begin(), buybackItems_.end(),
                    [this](const BuybackItem& item) {
                        return item.wireSlot == pendingBuybackWireSlot_;
                    });
                // No "Bought back: X" chat line - the stock client prints
                // none; the item's arrival is SMSG_ITEM_PUSH_RESULT's to say.
                if (bought != buybackItems_.end()) buybackItems_.erase(bought);
                pendingBuybackSlot_ = -1;
                pendingBuybackWireSlot_ = 0;
                if (owner_.addonEventCallbackRef()) {
                    owner_.addonEventCallbackRef()("MERCHANT_UPDATE", {});
                    fireBagUpdates();
                }
                return;
            }
            // No "Purchased: X" chat line. The stock client prints none for
            // SMSG_BUY_ITEM: the purchase is announced by SMSG_ITEM_PUSH_RESULT
            // ("Received item: X", above), so this put it in chat twice.
            //
            // No sound of its own either. The purchase is heard as the coin
            // sound (the money field drops) and the item's put-down foley
            // (SMSG_ITEM_PUSH_RESULT), as in the stock client; the bag rustle
            // that played here as well is in neither.
            (void)itemCount;
            pendingBuyItemId_   = 0;
            pendingBuyItemSlot_ = 0;
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("MERCHANT_UPDATE", {});
                fireBagUpdates();
            }
        }
    };

    // ---- Vendor / Trainer ----
    table[Opcode::SMSG_LIST_INVENTORY] = [this](network::Packet& packet) { handleListInventory(packet); };
    table[Opcode::SMSG_TRAINER_LIST] = [this](network::Packet& packet) { handleTrainerList(packet); };

    // ---- Mail ----
    table[Opcode::SMSG_SHOW_MAILBOX] = [this](network::Packet& packet) { handleShowMailbox(packet); };
    table[Opcode::SMSG_MAIL_LIST_RESULT] = [this](network::Packet& packet) { handleMailListResult(packet); };
    table[Opcode::SMSG_SEND_MAIL_RESULT] = [this](network::Packet& packet) { handleSendMailResult(packet); };
    table[Opcode::SMSG_RECEIVED_MAIL] = [this](network::Packet& packet) { handleReceivedMail(packet); };
    table[Opcode::MSG_QUERY_NEXT_MAIL_TIME] = [this](network::Packet& packet) { handleQueryNextMailTime(packet); };

    // ---- Bank ----
    table[Opcode::SMSG_SHOW_BANK] = [this](network::Packet& packet) { handleShowBank(packet); };
    table[Opcode::SMSG_BUY_BANK_SLOT_RESULT] = [this](network::Packet& packet) { handleBuyBankSlotResult(packet); };

    // ---- Guild Bank ----
    table[Opcode::SMSG_GUILD_BANK_LIST] = [this](network::Packet& packet) { handleGuildBankList(packet); };
    // The gold the player may still withdraw today: one int32, -1 for the
    // guild master (GuildBankRemainingWithdrawMoney::Write; Guild::
    // _GetMemberRemainingMoney). GuildBankFrame_UpdateWithdrawMoney redraws
    // on GUILDBANK_UPDATE_WITHDRAWMONEY.
    table[Opcode::MSG_GUILD_BANK_MONEY_WITHDRAWN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        guildBankData_.moneyWithdrawRemaining = static_cast<int32_t>(packet.readUInt32());
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_WITHDRAWMONEY", {});
    };
    // The reply shares its opcode with the request: a tab id and its text.
    table[Opcode::MSG_QUERY_GUILD_BANK_TEXT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const uint8_t tabId = packet.readUInt8();
        std::string text = packet.readString();
        if (tabId < guildBankTabText_.size()) guildBankTabText_[tabId] = std::move(text);
        // The panel asked for this text and was never told it arrived, so the
        // info box kept whatever it was drawn with. Its handler reads the tab
        // back with GetGuildBankText, counting tabs from one.
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_TEXT", {std::to_string(tabId + 1)});
    };

    // ---- Auction House ----
    table[Opcode::MSG_AUCTION_HELLO] = [this](network::Packet& packet) { handleAuctionHello(packet); };
    table[Opcode::SMSG_AUCTION_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionListResult(packet); };
    table[Opcode::SMSG_AUCTION_OWNER_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionOwnerListResult(packet); };
    table[Opcode::SMSG_AUCTION_BIDDER_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionBidderListResult(packet); };
    table[Opcode::SMSG_AUCTION_COMMAND_RESULT] = [this](network::Packet& packet) { handleAuctionCommandResult(packet); };

    table[Opcode::SMSG_AUCTION_OWNER_NOTIFICATION] = [this](network::Packet& packet) {
        // SendAuctionOwnerNotification writes:
        //   auctionId(4) bid(4) unk(4) unkGuid(8) item_template(4) unk(4)
        //   unkTime(float 4) = 32 bytes.
        // Sent only when an owned auction SELLS - expiry and outbids arrive on
        // their own opcodes (SMSG_AUCTION_REMOVED_NOTIFICATION /
        // SMSG_AUCTION_BIDDER_NOTIFICATION).
        //
        // The item entry is at offset 20 and reading field 3 made every "sold"
        // line say "Item #0". That was fixed by counting six uint32s to reach
        // twenty, which lands in the right place and describes the packet
        // wrongly: the twelve bytes in the middle are one eight-byte guid and
        // one word, not three words. Written out properly here so the next
        // person to add a field does not count from a layout that is not the
        // one on the wire.
        // Wow.exe (0x5A0790): with no bidder guid the auction is over -
        // ERR_AUCTION_SOLD_S if it had a bid, ERR_AUCTION_EXPIRED_S if not,
        // chat lines naming the item once its name is known. A bidder guid
        // is a new bid on it, which only refreshes the lists.
        if (packet.hasRemaining(28)) {
            /*uint32_t auctionId =*/ packet.readUInt32();
            const uint32_t bid = packet.readUInt32();
            /*uint32_t minOutbid =*/ packet.readUInt32();
            const uint64_t bidder = packet.readUInt64();
            const uint32_t itemEntry = packet.readUInt32();
            const int32_t randomProp = static_cast<int32_t>(packet.readUInt32());
            if (bidder == 0) {
                announceAuctionItem(itemEntry, randomProp,
                                    bid ? "ERR_AUCTION_SOLD_S" : "ERR_AUCTION_EXPIRED_S");
            }
        }
        packet.skipAll();
    };

    table[Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION] = [this](network::Packet& packet) {
        // auctionHouseId(4) auctionId(4) bidderGuid(8) bidAmount(4)
        // outbidAmount(4) itemEntry(4) randomPropertyId(4). Wow.exe
        // (0x5A0480): no bid amount is an auction won (ERR_AUCTION_WON_S),
        // any other outbid (ERR_AUCTION_OUTBID_S) - chat lines naming the item.
        if (!packet.hasRemaining(32)) { packet.skipAll(); return; }
        /*uint32_t auctionHouseId =*/ packet.readUInt32();
        /*uint32_t auctionId      =*/ packet.readUInt32();
        /*uint64_t bidderGuid     =*/ packet.readUInt64();
        const uint32_t bidAmount = packet.readUInt32();
        /*uint32_t outbidAmount   =*/ packet.readUInt32();
        const uint32_t itemEntry = packet.readUInt32();
        const int32_t bidRandProp = static_cast<int32_t>(packet.readUInt32());
        announceAuctionItem(itemEntry, bidRandProp,
                            bidAmount ? "ERR_AUCTION_OUTBID_S" : "ERR_AUCTION_WON_S");
        packet.skipAll();
    };

    // To a bidder whose auction the seller took down: ERR_AUCTION_REMOVED_S
    // ("Your auction of %s has been cancelled by the seller."), Wow.exe
    // 0x5A0AC0. It is not an expiry.
    table[Opcode::SMSG_AUCTION_REMOVED_NOTIFICATION] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            /*uint32_t auctionId =*/ packet.readUInt32();
            const uint32_t itemEntry = packet.readUInt32();
            const int32_t itemRandom = static_cast<int32_t>(packet.readUInt32());
            announceAuctionItem(itemEntry, itemRandom, "ERR_AUCTION_REMOVED_S");
        }
        packet.skipAll();
    };

    // ---- Equipment Sets ----
    table[Opcode::SMSG_EQUIPMENT_SET_LIST] = [this](network::Packet& packet) { handleEquipmentSetList(packet); };
    table[Opcode::SMSG_EQUIPMENT_SET_SAVED] = [this](network::Packet& packet) {
        // index(4) then the set's guid, PACKED - the server writes it with
        // appendPackGUID, not as a plain uint64.
        //
        // Read as eight flat bytes it was neither the guid nor eight bytes:
        // a packed guid is a one-byte mask and only the non-zero bytes after
        // it, so a new set's guid came out as the mask plus whatever followed
        // the packet. The guid is what every later request names the set by -
        // updating it, deleting it, wearing it - so each one addressed a set
        // the server does not have, and the local list grew a duplicate row
        // every time a set was saved. Nothing about that reads as a parse
        // fault: a guid is a number and a wrong one is still a number.
        //
        // The old guard wanted twelve bytes for a packet whose shortest valid
        // form is five, so an all-zero guid was dropped rather than read.
        std::string setName;
        if (packet.hasRemaining(5)) {
            uint32_t setIndex = packet.readUInt32();
            uint64_t setGuid  = packet.readPackedGuid();
            bool found = false;
            for (auto& es : equipmentSets_) {
                if (es.setGuid == setGuid || es.setId == setIndex) {
                    es.setGuid = setGuid;
                    setName = es.name;
                    found = true;
                    break;
                }
            }
            for (auto& info : equipmentSetInfo_) {
                if (info.setGuid == setGuid || info.setId == setIndex) {
                    info.setGuid = setGuid;
                    break;
                }
            }
            if (!found && setGuid != 0) {
                EquipmentSet newEs;
                newEs.setGuid = setGuid;
                newEs.setId   = setIndex;
                newEs.name    = pendingSaveSetName_;
                newEs.iconName = pendingSaveSetIcon_;
                newEs.ignoreSlotMask = pendingSaveIgnoreMask_;
                for (int s = 0; s < 19; ++s)
                    newEs.itemGuids[s] = (pendingSaveIgnoreMask_ & (1u << s))
                        ? 0 : owner_.getEquipSlotGuid(s);
                equipmentSets_.push_back(std::move(newEs));
                EquipmentSetInfo newInfo;
                newInfo.setGuid = setGuid;
                newInfo.setId   = setIndex;
                newInfo.name    = pendingSaveSetName_;
                newInfo.iconName = pendingSaveSetIcon_;
                equipmentSetInfo_.push_back(std::move(newInfo));
                setName = pendingSaveSetName_;
            }
            pendingSaveSetName_.clear();
            pendingSaveSetIcon_.clear();
            LOG_INFO("SMSG_EQUIPMENT_SET_SAVED: index=", setIndex,
                     " guid=", setGuid, " name=", setName);
        }
        // No chat line: the stock client says nothing when a set is saved;
        // the equipment manager's list redraws on the event below.
        (void)setName;
        // The set list just gained a row, or an existing row gained the guid
        // the server knows it by. The equipment manager rebuilds its list only
        // on this event, so without it a set saved this session is missing
        // from the frame until the next login.
        owner_.addonEventCallbackRef()("EQUIPMENT_SETS_CHANGED", {});
    };

    // The end of a swap (CharacterHandler.cpp:1942): 0 done, anything else
    // failed (4 is a full inventory). EQUIPMENT_SWAP_FINISHED(completed,
    // setName) is what the gear manager waits on to select the set and
    // re-mark its ignored slots - fired here rather than when the request
    // went out, which told it the swap had finished before the server had
    // moved anything.
    table[Opcode::SMSG_EQUIPMENT_SET_USE_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const uint8_t result = packet.readUInt8();
        const std::string setName = std::move(pendingUseSetName_);
        pendingUseSetName_.clear();
        owner_.fireAddonEvent("EQUIPMENT_SWAP_FINISHED",
            {result == 0 ? std::string("1") : std::string(kEventNil),
             setName.empty() ? std::string(kEventNil) : setName});
        // Only a full inventory (4) is worded, as EQUIPMENT_MANAGER_BAGS_FULL
        // on the red error line (Wow.exe 0x5AF710); other failures are silent.
        if (result == 4) {
            std::string text = stockGlobal(owner_, "EQUIPMENT_MANAGER_BAGS_FULL");
            owner_.addUIError(text.empty() ? "Your bags are too full to complete the equipment swap." : text);
        }
    };

    // ---- Item text ----
    table[Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE] = [this](network::Packet& packet) { handleItemTextQueryResponse(packet); };

    // ---- Trade ----
    table[Opcode::SMSG_TRADE_STATUS] = [this](network::Packet& packet) { handleTradeStatus(packet); };
    table[Opcode::SMSG_TRADE_STATUS_EXTENDED] = [this](network::Packet& packet) { handleTradeStatusExtended(packet); };

    // ---- Trainer buy ----
    table[Opcode::SMSG_TRAINER_BUY_SUCCEEDED] = [this](network::Packet& p) { handleTrainerBuySucceeded(p); };
    table[Opcode::SMSG_TRAINER_BUY_FAILED] = [this](network::Packet& p) { handleTrainerBuyFailed(p); };
}

// ============================================================
// Loot
// ============================================================

void InventoryHandler::lootTarget(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    currentLoot_.items.clear();
    LOG_INFO("Looting target 0x", std::hex, targetGuid, std::dec);
    auto packet = LootPacket::build(targetGuid);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::lootItem(uint8_t slotIndex, bool confirmed) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    if (!confirmed) {
        // Bind-on-pickup, and the warning neither interface gave. The slot the
        // event carries is the one on screen, not the server's: uiparent.lua
        // answers it with GetLootSlotInfo, which counts the coin as a slot of
        // its own and the items after it.
        const auto& loot = owner_.getCurrentLoot();
        int display = loot.hasCoinSlot() ? 1 : 0;
        for (const auto& item : loot.items) {
            ++display;
            if (item.slotIndex != slotIndex) continue;
            const auto* info = owner_.getItemInfo(item.itemId);
            if (info && info->valid && info->bindType == 1) {
                pendingLootActive_ = true;
                pendingLootSlot_ = slotIndex;
                if (owner_.addonEventCallbackRef())
                    owner_.addonEventCallbackRef()("LOOT_BIND_CONFIRM",
                                                   {std::to_string(display)});
                return;
            }
            break;
        }
    }

    pendingLootActive_ = false;
    for (const auto& item : currentLoot_.items) {
        if (item.slotIndex == slotIndex && !item.looted) {
            playLootSlotPickupSound(owner_, item);
            break;
        }
    }
    auto packet = AutostoreLootItemPacket::build(slotIndex);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::confirmPendingLoot() {
    if (!pendingLootActive_) return;
    const uint8_t slot = pendingLootSlot_;
    pendingLootActive_ = false;
    lootItem(slot, true);
}

void InventoryHandler::lootMoney() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = LootMoneyPacket::build();
    owner_.getSocket()->send(packet);
}

void InventoryHandler::cancelTempEnchantment(uint8_t handIndex) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (handIndex > 1) return;      // only the two weapon hands carry one
    const uint16_t wireOp = wireOpcode(Opcode::CMSG_CANCEL_TEMP_ENCHANTMENT);
    if (wireOp == 0xFFFF) return;
    network::Packet packet(wireOp);
    packet.writeUInt32(handIndex);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::clearLootMoney() {
    if (!currentLoot_.goldLeft()) return;
    // Marked rather than zeroed: the coin is display slot one, and dropping it
    // would move every item button onto the item after its own.
    currentLoot_.goldLooted = true;
    auto it = localLootState_.find(currentLoot_.lootGuid);
    if (it != localLootState_.end()) it->second.data.goldLooted = true;
    // Display slot one, which is where the coin sits whenever there is one.
    if (lootWindowOpen_ && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LOOT_SLOT_CLEARED", {"1"});
    }
    // A corpse that held only money is empty now.
    if (lootWindowOpen_ && currentLoot_.nothingLeft()) closeLoot();
}

void InventoryHandler::closeLoot() {
    if (!lootWindowOpen_) return;
    const uint64_t lootGuid = currentLoot_.lootGuid;
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = LootReleasePacket::build(lootGuid);
        owner_.getSocket()->send(packet);
    }
    lootWindowOpen_ = false;
    if (owner_.lootWindowCallbackRef()) owner_.lootWindowCallbackRef()(false);
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LOOT_CLOSED", {});
    // Do NOT locally despawn a gather node on loot-close. Node lifetime is
    // server-authoritative: a mineral vein / herb can still hold charges for another
    // gatherer, and the server sends SMSG_DESTROY_OBJECT when it is truly depleted.
    // Predicting the despawn here removed the node after a single loot, so it couldn't
    // be shared or re-mined (it looked like one player "mined it all").
    currentLoot_ = LootResponseData{};
}

void InventoryHandler::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_MASTER_GIVE));
    pkt.writeUInt64(currentLoot_.lootGuid);
    pkt.writeUInt8(lootSlot);
    pkt.writeUInt64(targetGuid);
    owner_.getSocket()->send(pkt);
}

void InventoryHandler::handleLootResponse(network::Packet& packet) {
    const bool wotlkLoot = isActiveExpansion("wotlk");
    if (!LootResponseParser::parse(packet, currentLoot_, wotlkLoot)) return;
    const bool hasLoot = !currentLoot_.items.empty() || currentLoot_.gold > 0;
    LOG_INFO("SMSG_LOOT_RESPONSE: guid=0x", std::hex, currentLoot_.lootGuid, std::dec,
             " items=", currentLoot_.items.size(), " gold=", currentLoot_.gold);
    auto& lastInteractedGoGuid = owner_.lastInteractedGoGuidRef();
    const bool isLastInteractedGoLoot =
        lastInteractedGoGuid != 0 && currentLoot_.lootGuid == lastInteractedGoGuid;
    if (!hasLoot && isLastInteractedGoLoot &&
        ((owner_.isCasting() && owner_.getCurrentCastSpellId() != 0) ||
         owner_.hasPendingGameObjectLootOpen(currentLoot_.lootGuid))) {
        LOG_DEBUG("Ignoring empty SMSG_LOOT_RESPONSE during pending gather/open");
        return;
    }
    // Auto-loot as decided when the loot was asked for, else the option.
    const bool autoLootThis = latchedAutoLootSet_ ? latchedAutoLoot_ : autoLoot_;
    latchedAutoLootSet_ = false;
    lootWindowOpen_ = true;
    if (owner_.lootWindowCallbackRef()) owner_.lootWindowCallbackRef()(true);
    if (owner_.addonEventCallbackRef()) {
        // Carries whether this loot is being taken automatically. The loot
        // frame reads it on the path where it could not show itself, and
        // closes with CloseLoot(autoLoot == 0) - so an absent argument
        // compares false and tells the server the window opened when it did
        // not.
        owner_.addonEventCallbackRef()("LOOT_OPENED", {autoLootThis ? "1" : "0"});
        owner_.addonEventCallbackRef()("LOOT_READY", {});
    }
    if (currentLoot_.lootGuid == lastInteractedGoGuid) {
        lastInteractedGoGuid = 0;
    }
    owner_.clearPendingGameObjectLootOpen(currentLoot_.lootGuid);
    auto& localLoot = localLootState_[currentLoot_.lootGuid];
    localLoot.data = currentLoot_;

    for (const auto& item : currentLoot_.items) {
        owner_.queryItemInfo(item.itemId, 0);
    }

    // The coin is taken by itself only when looting automatically: the stock
    // client sends CMSG_LOOT_MONEY from its auto-loot pass (0x588F90) and
    // otherwise leaves the coin in the window for the player to click.
    if (currentLoot_.gold > 0 && autoLootThis) {
        if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
            bool suppressFallback = false;
            auto cooldownIt = recentLootMoneyAnnounceCooldowns_.find(currentLoot_.lootGuid);
            if (cooldownIt != recentLootMoneyAnnounceCooldowns_.end() && cooldownIt->second > 0.0f) {
                suppressFallback = true;
            }
            pendingLootMoneyGuid_ = suppressFallback ? 0 : currentLoot_.lootGuid;
            pendingLootMoneyAmount_ = suppressFallback ? 0 : currentLoot_.gold;
            pendingLootMoneyNotifyTimer_ = suppressFallback ? 0.0f : 0.4f;
            auto pkt = LootMoneyPacket::build();
            owner_.getSocket()->send(pkt);
            // The coin stays in the window until the server says it is taken.
            //
            // Clearing it here emptied the slot the interface had just been
            // told about: the loot frame builds its buttons on LOOT_OPENED,
            // which fires above, and the coin is display slot one with the
            // items after it. Dropping the coin out from under that renumbered
            // every item - GetLootSlotInfo(2) answered the *second* item, or
            // nothing at all - so the window went on showing a coin that had
            // been taken and an item button that did nothing when clicked.
        }
    }

    // Game-object quest containers are single-action pickups in normal play.
    // Autostore their returned items even when general creature auto-loot is off;
    // otherwise right-click opens server loot but never grants the objective item.
    const bool autoStoreGameObjectLoot = isLastInteractedGoLoot;
    if ((autoLootThis || autoStoreGameObjectLoot) &&
        owner_.getState() == WorldState::IN_WORLD && owner_.getSocket() &&
        !localLoot.itemAutoLootSent) {
        for (const auto& item : currentLoot_.items) {
            LOG_INFO("Autostoring loot slot=", static_cast<int>(item.slotIndex),
                     " item=", item.itemId, " count=", item.count,
                     " fromGuid=0x", std::hex, currentLoot_.lootGuid, std::dec,
                     " gameObject=", autoStoreGameObjectLoot);
            playLootSlotPickupSound(owner_, item);
            auto pkt = AutostoreLootItemPacket::build(item.slotIndex);
            owner_.getSocket()->send(pkt);
        }
        localLoot.itemAutoLootSent = true;
    }

    // A corpse that held only money is already empty, so close it here rather
    // than waiting for a slot to clear that will never clear. Only when there
    // are no items: an item loot's slots have not been confirmed cleared yet,
    // and releasing before the server stores them would drop them - that case
    // closes from handleLootRemoved once the last slot is gone.
    if (lootWindowOpen_ && currentLoot_.items.empty() && currentLoot_.gold == 0) {
        closeLoot();
    }
}

void InventoryHandler::handleLootReleaseResponse(network::Packet& packet) {
    (void)packet;
    const uint64_t lootGuid = currentLoot_.lootGuid;
    localLootState_.erase(lootGuid);
    lootWindowOpen_ = false;
    if (owner_.lootWindowCallbackRef()) owner_.lootWindowCallbackRef()(false);
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LOOT_CLOSED", {});
    // Node lifetime is server-authoritative - see closeLoot(). The server despawns a
    // depleted gather node via SMSG_DESTROY_OBJECT; don't predict it here or a node the
    // server keeps (still holding charges for another gatherer) disappears locally.
    currentLoot_ = LootResponseData{};
}

void InventoryHandler::handleLootRemoved(network::Packet& packet) {
    uint8_t slotIndex = packet.readUInt8();
    for (auto it = currentLoot_.items.begin(); it != currentLoot_.items.end(); ++it) {
        if (it->slotIndex == slotIndex) {
            // SMSG_ITEM_PUSH_RESULT is the authoritative inventory receipt and
            // emits the single "Received item" notification. Slot removal only
            // updates the open loot window; announcing here duplicated chat and
            // the loot sound for the same item.
            it->looted = true;
            if (owner_.addonEventCallbackRef())
                owner_.addonEventCallbackRef()("LOOT_SLOT_CLEARED", {std::to_string(slotIndex + 1)});
            break;
        }
    }
    // An emptied corpse closes itself.
    //
    // The interface's loot window opens on LOOT_OPENED and closes on
    // LOOT_CLOSED and nothing else - LOOT_SLOT_CLEARED only hides the one
    // button. Autoloot sends an autostore for every slot at once, the server
    // clears them one by one, and when the last one goes there is nothing left
    // to loot but no close was ever sent: the window sat open and empty, which
    // is what "autoloot doesn't close the bag" is. Money is grabbed and zeroed
    // in the same breath at open, so items empty with no gold left is a corpse
    // with nothing on it. Retail closes it here too, for a manual last-item
    // loot as much as an automatic one.
    if (lootWindowOpen_ && currentLoot_.nothingLeft()) {
        closeLoot();
    }
}

// ============================================================
// Loot Roll
// ============================================================

/// A new roll, with the id the interface will know it by.
///
/// Any roll already open on the same item of the same corpse is replaced: the
/// server restates a roll when its countdown is reset, and two windows for one
/// item is worse than none.
LootRollEntry& InventoryHandler::beginLootRoll(uint64_t objectGuid, uint32_t slot) {
    for (auto& roll : lootRolls_) {
        if (roll.objectGuid == objectGuid && roll.slot == slot) {
            const int keptId = roll.rollId;
            roll = LootRollEntry{};
            roll.rollId = keptId;
            roll.objectGuid = objectGuid;
            roll.slot = slot;
            return roll;
        }
    }
    LootRollEntry roll;
    roll.rollId = nextLootRollId_++;
    roll.objectGuid = objectGuid;
    roll.slot = slot;
    lootRolls_.push_back(roll);
    return lootRolls_.back();
}

/// The roll on one item of one corpse, which is how every later packet names
/// it - the roll id is this client's own and the server has never seen it.
LootRollEntry* InventoryHandler::findLootRoll(uint64_t objectGuid, uint32_t slot) {
    for (auto& roll : lootRolls_) {
        if (roll.objectGuid == objectGuid && roll.slot == slot) return &roll;
    }
    return nullptr;
}

/// Close one roll and take its window down with it.
void InventoryHandler::endLootRoll(uint64_t objectGuid, uint32_t slot) {
    for (auto it = lootRolls_.begin(); it != lootRolls_.end(); ++it) {
        if (it->objectGuid != objectGuid || it->slot != slot) continue;
        const int rollId = it->rollId;
        const bool shown = it->shown;
        lootRolls_.erase(it);
        // Named, so the window that closes is the one this roll opened. It was
        // the slot plus one here as well, which closed a different roll's
        // window whenever two were up. A roll still waiting had no window.
        if (shown && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("CANCEL_LOOT_ROLL",
                                           {std::to_string(rollId)});
        }
        showPendingLootRolls();
        return;
    }
}

void InventoryHandler::showPendingLootRolls() {
    int shownRolls = 0;
    for (const auto& r : lootRolls_) if (r.shown) ++shownRolls;
    for (auto& r : lootRolls_) {
        if (shownRolls >= stock_death::kMaxShownLootRolls) return;
        if (r.shown) continue;
        r.shown = true;
        ++shownRolls;
        // With the countdown it arrived with, as 0x6fb180 does; the time left
        // is measured from its arrival.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("START_LOOT_ROLL",
                                           {std::to_string(r.rollId),
                                            std::to_string(r.rollCountdownMs)});
        }
    }
}

void InventoryHandler::sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_ROLL));
    pkt.writeUInt64(objectGuid);
    pkt.writeUInt32(slot);
    pkt.writeUInt8(rollType);
    owner_.getSocket()->send(pkt);
    // Once we've sent any choice (pass/need/greed/disenchant), close the dialog
    // - this roll's, and only this roll's.
    endLootRoll(objectGuid, slot);
}

void InventoryHandler::handleLootRoll(network::Packet& packet) {
    // objectGuid(8) + lootSlot(4) + playerGuid(8) + itemId(4) + itemRandSuffix(4) +
    // itemRandProp(4) + rollNumber(1) + rollType(1) + autoPass(1)
    if (!packet.hasRemaining(35)) return;
    uint64_t objectGuid = packet.readUInt64();
    uint32_t lootSlot   = packet.readUInt32();
    uint64_t playerGuid = packet.readUInt64();
    const uint32_t rolledItemId = packet.readUInt32();
    /*uint32_t randSuffix =*/ packet.readUInt32();
    const int32_t rolledRandProp = static_cast<int32_t>(packet.readUInt32());
    uint8_t rollNumber  = packet.readUInt8();
    uint8_t rollType    = packet.readUInt8();
    const bool autoPass = packet.readUInt8() != 0;

    // Resolve player name
    std::string playerName;
    auto nit = owner_.getPlayerNameCache().find(playerGuid);
    if (nit != owner_.getPlayerNameCache().end()) playerName = nit->second;
    if (playerName.empty()) playerName = "Player";

    if (LootRollEntry* roll = findLootRoll(objectGuid, lootSlot)) {
        LootRollEntry::PlayerRollResult result;
        result.playerName = playerName;
        result.rollNum = rollNumber;
        result.rollType = rollType;
        roll->playerRolls.push_back(result);
    }

    // The stock line (0x6fa840), CHAT_MSG_LOOT: a choice ("has selected Need
    // for:", "passed on:") while the number is 128, the roll itself after
    // ("Need Roll - 54 for [item] by Name"). With showLootSpam off only the
    // player's own automatic passes are said. It printed "Name rolled Need -
    // 0" for every choice, the 0 being no roll at all.
    owner_.ensureItemInfo(rolledItemId);
    const auto* info = owner_.getItemInfo(rolledItemId);
    std::string itemName = (info && !info->name.empty()) ? info->name
                                                         : ("Item #" + std::to_string(rolledItemId));
    if (rolledRandProp != 0) {
        const std::string suffix = owner_.getRandomPropertyName(rolledRandProp);
        if (!suffix.empty()) itemName += " " + suffix;
    }
    const std::string link = buildItemLink(rolledItemId, info ? info->quality : 1u, itemName);
    bool female = false;
    if (auto e = owner_.getEntityManager().getEntity(playerGuid)) {
        const uint16_t b0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
        if (b0 != 0xFFFF) female = ((e->getField(b0) >> 16) & 0xFF) == 1;
    }
    const bool verbose = addons::storedCVarValue("showLootSpam", "1") != "0";
    addLootRollLine(owner_, stock_death::lootRollVoteLine(
        playerGuid == owner_.getPlayerGuid(), static_cast<int8_t>(rollNumber), rollType,
        autoPass, female, verbose, playerName, link));
}

void InventoryHandler::handleLootRollWon(network::Packet& packet) {
    // objectGuid(8) + lootSlot(4) + itemId(4) + itemSuffix(4) + itemProp(4) + playerGuid(8) + rollNumber(1) + rollType(1)
    if (!packet.hasRemaining(34)) return;
    const uint64_t wonGuid = packet.readUInt64();
    const uint32_t wonSlot = packet.readUInt32();
    uint32_t itemId     = packet.readUInt32();
    /*uint32_t randSuffix =*/ packet.readUInt32();
    int32_t wonRandProp = static_cast<int32_t>(packet.readUInt32());
    uint64_t winnerGuid = packet.readUInt64();
    uint8_t rollNumber  = packet.readUInt8();
    uint8_t rollType    = packet.readUInt8();

    std::string winnerName;
    auto nit = owner_.getPlayerNameCache().find(winnerGuid);
    if (nit != owner_.getPlayerNameCache().end()) winnerName = nit->second;
    if (winnerName.empty()) winnerName = "Player";

    owner_.ensureItemInfo(itemId);
    auto* info = owner_.getItemInfo(itemId);
    std::string itemName = (info && !info->name.empty()) ? info->name : ("Item #" + std::to_string(itemId));
    if (wonRandProp != 0) {
        std::string suffix = owner_.getRandomPropertyName(wonRandProp);
        if (!suffix.empty()) itemName += " " + suffix;
    }
    uint32_t wonQuality = info ? info->quality : 1u;
    std::string link = buildItemLink(itemId, wonQuality, itemName);

    // LOOT_ROLL_WON / LOOT_ROLL_YOU_WON, or the _NO_SPAM forms with the roll
    // when showLootSpam is off (0x6fb700), CHAT_MSG_LOOT.
    const bool verbose = addons::storedCVarValue("showLootSpam", "1") != "0";
    addLootRollLine(owner_, stock_death::lootRollWonLine(
        winnerGuid == owner_.getPlayerGuid(), rollNumber, rollType, verbose, winnerName, link));
    endLootRoll(wonGuid, wonSlot);
}

// ============================================================
// Vendor
// ============================================================

void InventoryHandler::openVendor(uint64_t npcGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = ListInventoryPacket::build(npcGuid);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::closeVendor() {
    bool wasOpen = vendorWindowOpen_;
    vendorWindowOpen_ = false;
    currentVendorItems_ = ListInventoryData{};
    // Keep buybackItems_ and pendingSellToBuyback_: buyback slots live on the
    // player server-side (wire slots 74-85) and persist across vendor windows,
    // so the local mirror must too - clearing here made the buyback list
    // vanish when the vendor was reopened. The mirror resets on world entry.
    pendingBuybackSlot_ = -1;
    pendingBuybackWireSlot_ = 0;
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
    if (wasOpen && owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MERCHANT_CLOSED", {});
}

void InventoryHandler::clearBuybackState() {
    buybackItems_.clear();
    pendingSellToBuyback_.clear();
    buybackSlotGuids_.fill(0);
    buybackPrices_.fill(0);
    buybackTimestamps_.fill(0);
    pendingBuybackSlot_ = -1;
    pendingBuybackWireSlot_ = 0;
}

void InventoryHandler::notifyBuybackChanged() {
    // Only while the window is up. The ring lives on the player and outlives
    // any one vendor, so it changes at times nothing is drawing it.
    if (!vendorWindowOpen_) return;
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("MERCHANT_UPDATE", {});
    }
}

void InventoryHandler::reconcileBuybackSlots() {
    // What the list looked like before, so this can say whether it changed.
    // The server owns the ring, and this is where a sale becomes a real
    // buyback row and where a purchase takes one away - both of which the
    // window has to be told about, and neither of which it was.
    std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> before;
    before.reserve(buybackItems_.size());
    for (const auto& item : buybackItems_) before.emplace_back(item.itemGuid, item.wireSlot, item.price);

    // Built from the ring itself, PLAYER_FIELD_VENDORBUYBACK_SLOT_1..12, in
    // ring order - the order AzerothCore's AddItemToBuyBackSlot fills it (the
    // next free slot, else the oldest by BUYBACK_TIMESTAMP) and the order the
    // stock client lists it by. The list used to be a mirror of this client's
    // own sales in the order made, with the ring consulted only to drop rows,
    // so once the ring wrapped the rows no longer lined up with the slots, and
    // anything in the ring this client had not seen sold was never listed.
    //
    // What each row says comes from the copy kept at sale time when there is
    // one (it has the name before any query answers), else from the item
    // object the server still keeps for the slot.
    std::deque<BuybackItem> rebuilt;
    for (uint32_t i = 0; i < kBuybackWireSlotCount; ++i) {
        const uint64_t guid = buybackSlotGuids_[i];
        if (guid == 0) continue;
        BuybackItem row;
        row.itemGuid = guid;
        row.wireSlot = kBuybackWireSlotStart + i;
        row.price = buybackPrices_[i];
        row.timestamp = buybackTimestamps_[i];
        const auto known = std::find_if(buybackItems_.begin(), buybackItems_.end(),
            [guid](const BuybackItem& b) { return b.itemGuid == guid; });
        const auto pending = pendingSellToBuyback_.find(guid);
        if (known != buybackItems_.end()) {
            row.item = known->item;
            row.count = known->count;
        } else if (pending != pendingSellToBuyback_.end()) {
            row.item = pending->second.item;
            row.count = pending->second.count;
        }
        auto online = owner_.onlineItemsRef().find(guid);
        if (online != owner_.onlineItemsRef().end()) {
            const auto& oi = online->second;
            if (row.item.itemId == 0) {
                row.item = buildItemDef(oi.entry, oi.stackCount, oi.curDurability,
                                        oi.maxDurability, guid, oi.flags,
                                        oi.randomPropertyId, oi.suffixFactor);
            }
            row.count = oi.stackCount > 0 ? oi.stackCount : row.count;
        }
        pendingSellToBuyback_.erase(guid);
        if (row.item.itemId == 0) continue;   // nothing known about it yet
        rebuilt.push_back(std::move(row));
    }
    // SMSG_SELL_ITEM and the player-field delta may arrive in either order.
    // A sale not yet in the ring stays listed (unmapped, so not clickable)
    // until its slot arrives - the row is what says the sale went through.
    for (const auto& b : buybackItems_) {
        if (b.wireSlot == 0 && pendingSellToBuyback_.count(b.itemGuid)) rebuilt.push_back(b);
    }
    buybackItems_ = std::move(rebuilt);

    std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> after;
    after.reserve(buybackItems_.size());
    for (const auto& item : buybackItems_) after.emplace_back(item.itemGuid, item.wireSlot, item.price);
    if (after != before) notifyBuybackChanged();
}

void InventoryHandler::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    LOG_INFO("Buy request: vendorGuid=0x", std::hex, vendorGuid, std::dec,
             " itemId=", itemId, " slot=", slot, " count=", count,
             " wire=0x", std::hex, wireOpcode(Opcode::CMSG_BUY_ITEM), std::dec);
    pendingBuyItemId_ = itemId;
    pendingBuyItemSlot_ = slot;
    network::Packet packet(wireOpcode(Opcode::CMSG_BUY_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt32(itemId);
    packet.writeUInt32(slot);
    packet.writeUInt32(count);
    const bool isWotLk = isActiveExpansion("wotlk");
    if (isWotLk) {
        packet.writeUInt8(0);
    }
    owner_.getSocket()->send(packet);
}

void InventoryHandler::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    LOG_INFO("Sell request: vendorGuid=0x", std::hex, vendorGuid,
             " itemGuid=0x", itemGuid, std::dec,
             " count=", count, " wire=0x", std::hex, wireOpcode(Opcode::CMSG_SELL_ITEM), std::dec);
    auto packet = SellItemPacket::build(vendorGuid, itemGuid, count);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::sellItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    const auto& slot = owner_.inventoryRef().getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    uint32_t sellPrice = slot.item.sellPrice;
    if (sellPrice == 0) {
        if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
            sellPrice = info->sellPrice;
        }
    }
    if (sellPrice == 0) {
        owner_.raiseUiError("Cannot sell: this item has no vendor value.");
        return;
    }

    uint64_t itemGuid = slot.item.guid;
    if (itemGuid == 0) itemGuid = owner_.backpackSlotGuidsRef()[backpackIndex];
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    LOG_DEBUG("sellItemBySlot: slot=", backpackIndex,
              " item=", slot.item.name,
              " itemGuid=0x", std::hex, itemGuid, std::dec,
              " vendorGuid=0x", std::hex, currentVendorItems_.vendorGuid, std::dec);
    if (itemGuid != 0 && currentVendorItems_.vendorGuid != 0) {
        const uint32_t count = sellCountForStack(slot.item.stackCount);
        BuybackItem sold;
        sold.itemGuid = itemGuid;
        sold.item = slot.item;
        sold.count = count;
        pendingSellToBuyback_[itemGuid] = sold;
        buybackItems_.push_back(sold);
        // Shown straight away rather than when the server's ring update lands:
        // the row is what tells the player the sale went through.
        notifyBuybackChanged();
        sellItem(currentVendorItems_.vendorGuid, itemGuid, count);
    } else if (itemGuid == 0) {
        owner_.raiseUiError("Cannot sell: item not found in inventory.");
        LOG_WARNING("Sell failed: missing item GUID for slot ", backpackIndex);
    } else {
        owner_.raiseUiError("Cannot sell: no vendor.");
    }
}

void InventoryHandler::sellItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return;
    const auto& slot = owner_.inventoryRef().getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return;

    uint32_t sellPrice = slot.item.sellPrice;
    if (sellPrice == 0) {
        if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
            sellPrice = info->sellPrice;
        }
    }
    if (sellPrice == 0) {
        owner_.raiseUiError("Cannot sell: this item has no vendor value.");
        return;
    }

    uint64_t itemGuid = slot.item.guid;
    uint64_t bagGuid = owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (itemGuid == 0 && bagGuid != 0) {
        auto it = owner_.containerContentsRef().find(bagGuid);
        if (it != owner_.containerContentsRef().end() && slotIndex < static_cast<int>(it->second.numSlots)) {
            itemGuid = it->second.slotGuids[slotIndex];
        }
    }
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    if (itemGuid != 0 && currentVendorItems_.vendorGuid != 0) {
        const uint32_t count = sellCountForStack(slot.item.stackCount);
        BuybackItem sold;
        sold.itemGuid = itemGuid;
        sold.item = slot.item;
        sold.count = count;
        pendingSellToBuyback_[itemGuid] = sold;
        buybackItems_.push_back(sold);
        // Shown straight away rather than when the server's ring update lands:
        // the row is what tells the player the sale went through.
        notifyBuybackChanged();
        sellItem(currentVendorItems_.vendorGuid, itemGuid, count);
    } else if (itemGuid == 0) {
        owner_.raiseUiError("Cannot sell: item not found.");
    } else {
        owner_.raiseUiError("Cannot sell: no vendor.");
    }
}

void InventoryHandler::buyBackItem(uint32_t buybackSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || currentVendorItems_.vendorGuid == 0) return;
    if (buybackSlot >= buybackItems_.size()) return;
    const uint32_t wireSlot = buybackItems_[buybackSlot].wireSlot;
    if (wireSlot < kBuybackWireSlotStart || wireSlot >= kBuybackWireSlotEnd) {
        LOG_WARNING("Buyback request rejected: row ", buybackSlot,
                    " has invalid wire slot ", wireSlot);
        return;
    }
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
    LOG_INFO("Buyback request: vendorGuid=0x", std::hex, currentVendorItems_.vendorGuid,
             std::dec, " uiSlot=", buybackSlot, " wireSlot=", wireSlot);
    pendingBuybackSlot_ = static_cast<int>(buybackSlot);
    pendingBuybackWireSlot_ = wireSlot;
    // Use the expansion-agnostic packet builder so the opcode resolves from
    // the active expansion's JSON mapping rather than a hardcoded WotLK value.
    owner_.getSocket()->send(BuybackItemPacket::build(currentVendorItems_.vendorGuid, wireSlot));
}

// The armour indicator and the merchant's repair buttons both redraw from an
// event rather than by polling. The optimistic repair below changes durability
// without the server having said so yet, so it has to say so itself.
void InventoryHandler::announceDurabilityChange() {
    owner_.fireAddonEvent("UPDATE_INVENTORY_ALERTS", {});
    owner_.fireAddonEvent("UPDATE_INVENTORY_DURABILITY", {});
}

void InventoryHandler::repairItem(uint64_t vendorGuid, uint64_t itemGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    uint32_t cost = estimateItemRepairCost(itemGuid);
    if (cost > 0 && owner_.getMoneyCopper() < cost) {
        owner_.addUIError("Not enough money");
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_REPAIR_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(itemGuid);
    if (!isClassicLikeExpansion()) packet.writeUInt8(0);
    owner_.getSocket()->send(packet);

    // Only do optimistic update if we verified the player can afford it
    if (cost > 0) {
        owner_.playerMoneyCopperRef() -= cost;
        auto it = owner_.onlineItemsRef().find(itemGuid);
        if (it != owner_.onlineItemsRef().end()) {
            it->second.curDurability = it->second.maxDurability;
            rebuildOnlineInventory();
            announceDurabilityChange();
        }
    }
}

void InventoryHandler::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    uint32_t totalCost = estimateRepairAllCost();

    if (!useGuildBank && totalCost > 0 && owner_.getMoneyCopper() < totalCost) {
        owner_.addUIError("Not enough money");
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_REPAIR_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(0);
    if (!isClassicLikeExpansion()) packet.writeUInt8(useGuildBank ? 1 : 0);
    owner_.getSocket()->send(packet);

    // Only do optimistic update for the player-funded path: we verified the
    // player has the gold, so server acceptance is essentially guaranteed.
    //
    // Guild-bank repair (useGuildBank=true) cannot be confirmed client-side:
    // the server silently rejects when the player has no guild, no
    // GUILD_BANK_RIGHT_REPAIR permission, or the guild bank lacks funds -
    // in all those cases Player::DurabilityRepair returns early WITHOUT
    // setting durability and WITHOUT sending an UPDATE_OBJECT, so any
    // optimistic durability bump would persist on screen until relog
    // (when the server's actual state reloads). Wait for the server's
    // UPDATE_OBJECT to confirm instead.
    if (totalCost > 0 && !useGuildBank) {
        owner_.playerMoneyCopperRef() -= totalCost;
        // Only what the server repairs - not the bank (see repairAllItemGuids).
        for (uint64_t guid : repairAllItemGuids()) {
            auto it = owner_.onlineItemsRef().find(guid);
            if (it == owner_.onlineItemsRef().end()) continue;
            auto& info = it->second;
            if (info.maxDurability > 0 && info.curDurability < info.maxDurability) {
                info.curDurability = info.maxDurability;
            }
        }
        rebuildOnlineInventory();
        announceDurabilityChange();
    }
}

bool InventoryHandler::equipWouldBindFromBackpack(int backpackIndex) const {
    const auto& inv = owner_.getInventory();
    if (backpackIndex < 0 || backpackIndex >= inv.getBackpackSize()) return false;
    const auto& slot = inv.getBackpackSlot(backpackIndex);
    return !slot.empty() && slot.item.wouldBindOnEquip();
}

bool InventoryHandler::equipWouldBindFromBag(int bagIndex, int slotIndex) const {
    const auto& inv = owner_.getInventory();
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (slotIndex < 0 || slotIndex >= inv.getBagSize(bagIndex)) return false;
    const auto& slot = inv.getBagSlot(bagIndex, slotIndex);
    return !slot.empty() && slot.item.wouldBindOnEquip();
}

// Equip a specific item into a specific equipment slot, rather than letting the
// server pick. CMSG_AUTOEQUIP_ITEM_SLOT has been in the opcode enum with
// nothing building it, so /equipslot could name a slot and never reach one.
//
// The wire is the item's guid and a one-byte destination - ItemPackets.cpp
// reads exactly that - and the destination is the server's own 0-based
// equipment slot, which is what this client's EquipSlot enum counts in too.
// The interface counts from one, so the caller takes the one off.
void InventoryHandler::equipItemToSlot(uint64_t itemGuid, uint8_t equipSlot) {
    if (itemGuid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Refused above the equipment slots rather than sent: AzerothCore drops the
    // packet outright when the destination is not an equipment position, and a
    // dropped request is indistinguishable from one that was never built.
    if (equipSlot >= static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT)) return;
    const uint32_t wire = wireOpcode(Opcode::CMSG_AUTOEQUIP_ITEM_SLOT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt64(itemGuid);
    pkt.writeUInt8(equipSlot);
    owner_.getSocket()->send(pkt);
}

bool InventoryHandler::isProjectileItem(uint32_t itemId) const {
    const auto* info = itemId ? owner_.getItemInfo(itemId) : nullptr;
    return info && info->valid && info->itemClass == ITEM_CLASS_PROJECTILE;
}

uint32_t InventoryHandler::getAmmoItemId() const {
    return owner_.getAmmoItemId();
}

// CMSG_SET_AMMO is the item entry and nothing else: ammunition stays in the
// bags (a quiver or an ammo pouch, usually) and PLAYER_AMMO_ID names which of
// it the ranged weapon shoots. Wowee never sent it, so a hunter could not
// load arrows at all - not by dragging them to the ammo slot, not by
// right-clicking them. Wow.exe sends it from three places: PickupInventoryItem
// on the ammo slot (0x5E85D0), AutoEquipCursorItem (0x6DFC40) and, through
// the latter, UseContainerItem on a projectile (0x6DFEE0).
void InventoryHandler::setAmmo(uint32_t itemId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t wire = wireOpcode(Opcode::CMSG_SET_AMMO);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt32(itemId);
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sending CMSG_SET_AMMO item=", itemId);
}

bool InventoryHandler::itemAtWire(uint8_t wireBag, uint8_t wireSlot, ItemDef& out,
                                  uint64_t& guid) const {
    const auto& inv = owner_.inventoryRef();
    const ItemSlot* sl = nullptr;
    guid = 0;
    const int slot = wireSlot;
    if (wireBag == slots::kNoContainer) {
        const slots::BankLayout bank = slots::bankLayout();
        if (slot < Inventory::NUM_EQUIP_SLOTS) {
            sl = &inv.getEquipSlot(static_cast<EquipSlot>(slot));
            guid = owner_.equipSlotGuidsRef()[static_cast<size_t>(slot)];
        } else if (slot < slots::kBackpackFirst + inv.getBackpackSize()) {
            const int i = slot - slots::kBackpackFirst;
            sl = &inv.getBackpackSlot(i);
            guid = owner_.backpackSlotGuidsRef()[static_cast<size_t>(i)];
        } else if (slot >= slots::kBankGeneralFirst &&
                   slot < slots::kBankGeneralFirst + bank.generalCount) {
            sl = &inv.getBankSlot(slot - slots::kBankGeneralFirst);
        } else if (slot >= bank.bagFirst && slot < bank.bagFirst + bank.bagCount) {
            sl = &inv.getBankBagItem(slot - bank.bagFirst);
        } else if (slot >= bank.keyringFirst && slot < bank.keyringFirst + inv.getKeyringSize()) {
            sl = &inv.getKeyringSlot(slot - bank.keyringFirst);
        }
    } else if (wireBag >= slots::kWornBagFirst &&
               wireBag < slots::kWornBagFirst + Inventory::NUM_BAG_SLOTS) {
        const int b = wireBag - slots::kWornBagFirst;
        if (slot < inv.getBagSize(b)) {
            sl = &inv.getBagSlot(b, slot);
            guid = owner_.getBagItemGuid(b, slot);
        }
    } else {
        const slots::BankLayout bank = slots::bankLayout();
        const int b = static_cast<int>(wireBag) - bank.bagFirst;
        if (b >= 0 && b < bank.bagCount && slot < inv.getBankBagSize(b))
            sl = &inv.getBankBagSlot(b, slot);
    }
    if (!sl || sl->empty()) return false;
    out = sl->item;
    if (guid == 0) guid = sl->item.guid;
    if (guid == 0) guid = owner_.resolveOnlineItemGuid(sl->item.itemId);
    return true;
}

// The body of AutoEquipCursorItem (Wow.exe 0x6DFC40), which UseContainerItem
// reaches for anything with an InventoryType (0x5D8650 -> 0x6DFEE0): a
// projectile is loaded with CMSG_SET_AMMO, an unbound bind-on-equip item asks
// AUTOEQUIP_BIND_CONFIRM first, and everything else is CMSG_AUTOEQUIP_ITEM
// naming where the item is. The server picks the slot - the empty ring or
// trinket slot before the first, the off hand emptied for a two-hander
// (AzerothCore Player::FindEquipSlot / HandleAutoEquipItemOpcode).
void InventoryHandler::autoEquipFromWire(uint8_t wireBag, uint8_t wireSlot, bool confirmed) {
    ItemDef item;
    uint64_t guid = 0;
    if (!itemAtWire(wireBag, wireSlot, item, guid)) return;

    if (isProjectileItem(item.itemId)) {
        setAmmo(item.itemId);
        return;
    }

    const bool unbound = guid != 0 ? !owner_.isItemSoulbound(guid) : !item.soulbound;
    const auto* info = owner_.getItemInfo(item.itemId);
    const uint32_t bindType = (info && info->valid) ? info->bindType : item.bindType;
    if (!confirmed && bindType == 2 && unbound) {
        pendingEquip_ = PendingEquip{.active = true, .srcBag = wireBag, .srcSlot = wireSlot};
        // The auto-equip form of the prompt: right-clicking an item rather
        // than dropping it on a slot. FrameXML hides whichever of the two is
        // already up before showing the other, so firing the wrong one leaves
        // both on screen.
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("AUTOEQUIP_BIND_CONFIRM", {std::to_string(wireSlot)});
        return;
    }

    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AutoEquipItemPacket::build(wireBag, wireSlot);
        owner_.getSocket()->send(packet);
    }
}

bool InventoryHandler::swapWithBindCheck(uint8_t srcBag, uint8_t srcSlot,
                                         uint8_t dstBag, uint8_t dstSlot) {
    // Which of the two items lands in an equipment slot: the one moving there,
    // or - dragging a worn item onto a bag square - the one it is swapped
    // with (Wow.exe 0x6DF890 tests both directions).
    auto isEquipPos = [](uint8_t bag, uint8_t slot) {
        return bag == slots::kNoContainer && slot < slots::kWornBagFirst;
    };
    auto wouldBind = [&](uint8_t bag, uint8_t slot) {
        ItemDef item;
        uint64_t guid = 0;
        if (!itemAtWire(bag, slot, item, guid)) return false;
        const auto* info = owner_.getItemInfo(item.itemId);
        const uint32_t bindType = (info && info->valid) ? info->bindType : item.bindType;
        const bool unbound = guid != 0 ? !owner_.isItemSoulbound(guid) : !item.soulbound;
        return bindType == 2 && unbound;
    };
    const bool asks = (isEquipPos(dstBag, dstSlot) && wouldBind(srcBag, srcSlot)) ||
                      (isEquipPos(srcBag, srcSlot) && wouldBind(dstBag, dstSlot));
    if (asks) {
        pendingEquip_ = PendingEquip{.active = true, .srcBag = srcBag, .srcSlot = srcSlot,
                                     .swap = true, .dstBag = dstBag, .dstSlot = dstSlot};
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("EQUIP_BIND_CONFIRM", {std::to_string(dstSlot)});
        return true;
    }
    swapContainerItems(srcBag, srcSlot, dstBag, dstSlot);
    return false;
}

void InventoryHandler::autoEquipItemBySlot(int backpackIndex, bool confirmed) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    autoEquipFromWire(slots::kNoContainer,
                      static_cast<uint8_t>(slots::backpackWireSlot(backpackIndex)), confirmed);
}

void InventoryHandler::autoEquipItemInBag(int bagIndex, int slotIndex, bool confirmed) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return;
    autoEquipFromWire(static_cast<uint8_t>(slots::wornBagContainer(bagIndex)),
                      static_cast<uint8_t>(slotIndex), confirmed);
}

void InventoryHandler::equipPendingItem() {
    const PendingEquip pending = pendingEquip_;
    pendingEquip_ = PendingEquip{};
    if (!pending.active) return;
    if (pending.swap) swapContainerItems(pending.srcBag, pending.srcSlot, pending.dstBag, pending.dstSlot);
    else              autoEquipFromWire(pending.srcBag, pending.srcSlot, true);
}

void InventoryHandler::cancelPendingEquip() {
    pendingEquip_ = PendingEquip{};
}

// Dispatches CMSG_USE_ITEM for an item already located at (wowBag, wowSlot).
// Spells that enchant another item (sharpening stones, weightstones, weapon oils)
// cannot be sent immediately: they need the target item's GUID, so the use is
// parked and completed by completeItemUseOnItem() once the player picks a target.
void InventoryHandler::confirmBindOnUse() {
    const PendingUse pending = pendingUse_;
    pendingUse_ = PendingUse{};
    if (!pending.active) return;
    dispatchUseItem(pending.wowBag, pending.wowSlot, pending.itemGuid, pending.item, true);
}

void InventoryHandler::dispatchUseItem(uint8_t wowBag, uint8_t wowSlot, uint64_t itemGuid,
                                       const ItemDef& item, bool confirmed,
                                       uint64_t unitTarget) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    // Bind-on-use, and not yet bound. Same warning as the equip prompt, one
    // step earlier: using the item is what binds it.
    if (!confirmed && item.bindType == 3 && !item.soulbound) {
        pendingUse_ = PendingUse{.active = true, .wowBag = wowBag, .wowSlot = wowSlot, .itemGuid = itemGuid, .item = item};
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("USE_BIND_CONFIRM", {});
        return;
    }

    if (itemGuid == 0) {
        LOG_WARNING("useItem: itemGuid=0 for item='", item.name, "' entry=", item.itemId,
                    " - cannot use");
        owner_.raiseUiError("Cannot use that item right now.");
        return;
    }

    const auto* itemInfo = owner_.getItemInfo(item.itemId);
    uint32_t useSpellId = 0;
    if (itemInfo) {
        for (const auto& sp : itemInfo->spells) {
            if (sp.spellId != 0 && (sp.spellTrigger == 0 || sp.spellTrigger == 5)) {
                useSpellId = sp.spellId;
                break;
            }
        }
    }
    LOG_DEBUG("useItem: bag=", (int)wowBag, " slot=", (int)wowSlot, " entry=", item.itemId,
              " spellId=", useSpellId);

    // Pre-WotLK mounts are items, so the same rule applies to them: using the
    // one you are riding dismounts you rather than re-summoning it.
    if (useSpellId != 0 && owner_.isMounted() &&
        (useSpellId == owner_.getMountAuraSpellId() ||
         owner_.playerCarriesAura(useSpellId))) {
        owner_.dismount();
        LOG_INFO("Dismount via mount item: entry=", item.itemId, " spell=", useSpellId);
        return;
    }
    if (useSpellId != 0 && !owner_.isMounted() && owner_.getSpellHandler()) {
        owner_.getSpellHandler()->noteGroundCastSpell(useSpellId);
    }

    // A glyph: the use waits for a socket. Stock fires USE_GLYPH and the
    // glyph frame opens (uiparent.lua: USE_GLYPH -> OpenGlyphFrame); the
    // socket click then places it through PlaceGlyphInSocket. Sent here the
    // use carried glyph index 0 and put every glyph in the first socket.
    if (useSpellId != 0 && unitTarget == 0 && owner_.glyphPropertiesForItem(item.itemId) != 0) {
        pendingGlyph_ = PendingItemTarget{.bag = wowBag, .slot = wowSlot, .itemGuid = itemGuid,
                                          .spellId = useSpellId, .itemId = item.itemId,
                                          .itemName = item.name};
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("USE_GLYPH", {std::to_string(useSpellId)});
        return;
    }

    // A bomb or a grenade: its spell waits on a point, so the use waits with
    // it for the ground to be clicked, as a spell cast at a point does
    // (docs/re/spell-cast-pipeline.md section 15).
    if (useSpellId != 0 && unitTarget == 0 && owner_.getSpellHandler() &&
        owner_.getSpellHandler()->beginItemGroundTargeting(useSpellId, wowBag, wowSlot, itemGuid)) {
        return;
    }

    if (useSpellId != 0 &&
        (owner_.getSpellTargetFlags(useSpellId) & kSpellTargetFlagItem) != 0) {
        // The targeting cursor is the whole prompt; the stock client says
        // nothing in chat.
        pendingItemTarget_ = PendingItemTarget{.bag = wowBag, .slot = wowSlot, .itemGuid = itemGuid, .spellId = useSpellId,
                                               .itemId = item.itemId, .itemName = item.name};
        return;
    }

    // A spell that must land on a unit, with nothing selected: WoW gives the
    // player a targeting cursor rather than refusing. The use is parked until
    // they click someone, which is what "right-click the treat, then click the
    // dog" means - and without it the item did nothing at all.
    {
        // An unknown spell counts as wanting one: with nothing selected there is
        // no way to tell the client what the item is for, so the cursor asks.
        const bool wantsUnit =
            (useSpellId != 0 && spellNeedsAUnit(owner_.getSpellImplicitTargetA(useSpellId))) ||
            (useSpellId != 0 && !owner_.isSpellKnownToClient(useSpellId));
        const uint64_t chosen = unitTarget != 0
            ? unitTarget : targetGuidForUseItem(owner_, itemInfo, useSpellId);
        // Bandages and scrolls. Using either with nothing selected puts it on
        // you: a Scroll of Protection buffs whoever read it, and asking them to
        // click somebody first is a cursor over a decision that has already
        // been made. Food, drink and potions never reach here at all - their
        // spells aim at the caster, so spellNeedsAUnit is already false for
        // them - so naming the whole consumable class here would have caught
        // nothing but treats and the other items that do need somebody picked.
        const bool selfIsImplicit =
            isBandageItem(itemInfo) ||
            (itemInfo && itemInfo->valid &&
             itemInfo->itemClass == ITEM_CLASS_CONSUMABLE &&
             itemInfo->subClass == ITEM_SUBCLASS_SCROLL);
        if (wantsUnit && !selfIsImplicit && chosen == owner_.getPlayerGuid() &&
            owner_.getTargetGuid() == 0) {
            pendingUnitTarget_ = PendingItemTarget{.bag = wowBag, .slot = wowSlot, .itemGuid = itemGuid, .spellId = useSpellId,
                                                   .itemId = item.itemId, .itemName = item.name, .fromSpell = false};
            return;
        }
    }

    if (isBandageItem(itemInfo)) synchronizeStationaryBandageCast(owner_);
    // A unit the caller named wins over the default the item's class implies.
    // Only the interface's /use handling passes one, and it is the whole of
    // what `/use [target=Bob] <bandage>` means - targetGuidForUseItem answers
    // the player for every consumable, so without this the bandage went on
    // whoever asked for it rather than on whoever was named.
    sendUseItem(wowBag, wowSlot, itemGuid, useSpellId,
                unitTarget != 0 ? unitTarget
                                : targetGuidForUseItem(owner_, itemInfo, useSpellId), 0);
}

void InventoryHandler::sendUseItem(uint8_t wowBag, uint8_t wowSlot, uint64_t itemGuid,
                                   uint32_t spellId, uint64_t targetGuid, uint64_t itemTargetGuid) {
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildUseItem(wowBag, wowSlot, itemGuid, spellId,
                                                  targetGuid, itemTargetGuid)
        : UseItemPacket::build(wowBag, wowSlot, itemGuid, spellId, targetGuid, itemTargetGuid);
    // The unit target and how it was decided, because "the item did nothing"
    // is a question about exactly this and the packet is where it is answered.
    // WOWEE_LOG_LEVEL=info opens it.
    LOG_INFO("Sending CMSG_USE_ITEM: bag=", (int)wowBag, " slot=", (int)wowSlot,
             " spell=", spellId,
             " known=", (spellId != 0 && owner_.isSpellKnownToClient(spellId)) ? 1 : 0,
             " aim=", spellId != 0 ? owner_.getSpellImplicitTargetA(spellId) : 0,
             " unitTarget=0x", std::hex, targetGuid,
             " itemTarget=0x", itemTargetGuid, std::dec,
             " selected=", owner_.getTargetGuid() != 0 ? 1 : 0,
             " packetSize=", packet.getSize());
    owner_.getSocket()->send(packet);
}

bool InventoryHandler::isAwaitingUnitTarget() const {
    if (!pendingUnitTarget_) return false;
    // Same rule as the item version: leaving the world abandons it, because the
    // slot and GUID would be stale for the next character.
    if (owner_.getState() != WorldState::IN_WORLD) {
        pendingUnitTarget_.reset();
        return false;
    }
    return true;
}

uint32_t InventoryHandler::getPendingUnitTargetSourceItemId() const {
    return isAwaitingUnitTarget() ? pendingUnitTarget_->itemId : 0;
}

void InventoryHandler::cancelUnitTargeting() {
    pendingUnitTarget_.reset();
}

void InventoryHandler::completeItemUseOnUnit(uint64_t targetUnitGuid) {
    if (!isAwaitingUnitTarget()) return;
    const PendingItemTarget pending = *pendingUnitTarget_;
    pendingUnitTarget_.reset();
    if (targetUnitGuid == 0) return;

    const auto* itemInfo = owner_.getItemInfo(pending.itemId);
    if (isBandageItem(itemInfo)) synchronizeStationaryBandageCast(owner_);
    sendUseItem(pending.bag, pending.slot, pending.itemGuid, pending.spellId,
                targetUnitGuid, 0);
}

bool InventoryHandler::isAwaitingItemTarget() const {
    if (!pendingItemTarget_) return false;
    // Leaving the world abandons the pending use - the slot and GUID would be
    // stale for the next character.
    if (owner_.getState() != WorldState::IN_WORLD) {
        pendingItemTarget_.reset();
        return false;
    }
    return true;
}

uint32_t InventoryHandler::getPendingItemTargetSourceItemId() const {
    return pendingItemTarget_ ? pendingItemTarget_->itemId : 0;
}

void InventoryHandler::cancelItemTargeting() {
    pendingItemTarget_.reset();
}

void InventoryHandler::beginSpellItemTargeting(uint32_t spellId, const std::string& spellName) {
    PendingItemTarget pending;
    pending.spellId = spellId;
    pending.itemName = spellName;
    pending.fromSpell = true;
    // The targeting cursor alone, as for an item's spell.
    pendingItemTarget_ = pending;
}

void InventoryHandler::replaceEnchant() {
    const PendingEnchant pending = pendingEnchant_;
    pendingEnchant_ = PendingEnchant{};
    if (!pending.active) return;
    // Put the request back only to answer it, so a refusal leaves nothing
    // waiting for a target. At the stage the answer reached: a bind answer
    // still meets the replace question.
    pendingItemTarget_ = pending.request;
    confirmStage_ = pending.nextStage;
    completeItemUseOnItem(pending.targetItemGuid, pending.nextStage >= 2);
}

void InventoryHandler::completeItemUseOnItem(uint64_t targetItemGuid, bool confirmed) {
    const int stageIn = confirmStage_;
    confirmStage_ = 0;
    if (!isAwaitingItemTarget()) return;
    const PendingItemTarget pending = *pendingItemTarget_;

    if (targetItemGuid == 0 || !owner_.getSocket()) {
        pendingItemTarget_.reset();
        owner_.raiseUiError("That is not a valid target.");
        return;
    }

    // A spell that takes the item apart rather than changing it - disenchant,
    // prospecting, milling. Neither enchant warning applies to one: nothing is
    // enchanted, and an enchant already on the item goes with it. Both were
    // asked anyway, so disenchanting a bind-on-equip item put "Enchanting this
    // item will bind it to you" in front of a cast that turns it into dust.
    bool destroysTarget = false;
    bool isDisenchant = false;
    if (pending.fromSpell) {
        const auto& cache = owner_.spellNameCacheRef();
        if (auto it = cache.find(pending.spellId); it != cache.end()) {
            destroysTarget = spellclass::destroysTargetItem(it->second.effectIds, 3);
            isDisenchant = spellclass::hasDisenchantEffect(it->second.effectIds, 3);
        }
    }

    // What it does ask instead, for a disenchant: the item is gone and there is
    // no undoing it. The real client asks nothing here, and prospecting and
    // milling keep that - they are spent five at a time from a stack, and a
    // dialog for each would be in the way of the profession rather than a
    // guard on it.
    if (destroysTarget && isDisenchant && !confirmed) {
        const uint32_t targetEntry = owner_.getItemEntryByGuid(targetItemGuid);
        const auto* targetInfo = targetEntry ? owner_.getItemInfo(targetEntry) : nullptr;

        // Only what can actually be taken apart. Disenchanting is uncommon and
        // better weapons and armour and nothing else, and the server answers
        // anything else with SPELL_FAILED_CANT_BE_DISENCHANTED - so asking
        // "shall I destroy this?" about a bag or a potion is a question with no
        // outcome behind it, and an alarming one to be asked while moving
        // things around a bank.
        //
        // The pending target stays armed, which is what the real client does:
        // the cursor is still waiting for something it can be used on, and the
        // way out of it is escape or the right button.
        constexpr uint32_t kItemClassWeapon = 2;
        constexpr uint32_t kItemClassArmor  = 4;
        constexpr uint32_t kQualityUncommon = 2;
        const bool canDisenchant =
            targetInfo && targetInfo->valid &&
            targetInfo->quality >= kQualityUncommon &&
            (targetInfo->itemClass == kItemClassWeapon ||
             targetInfo->itemClass == kItemClassArmor);
        if (!canDisenchant) {
            owner_.raiseUiError("Item cannot be disenchanted");
            return;
        }

        pendingItemTarget_.reset();
        pendingEnchant_ = PendingEnchant{.active = true, .targetItemGuid = targetItemGuid, .request = pending};
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()(
                "WOWEE_DISENCHANT_CONFIRM",
                {targetInfo && !targetInfo->name.empty() ? targetInfo->name
                                                         : std::string("that item")});
        }
        return;
    }

    // Something permanent is already on it, and applying this destroys it.
    //
    // Only the permanent slot. A weapon carrying a sharpening stone is not
    // warned about, because the temporary slot is minutes of a stone rather
    // than an enchanter, and naming a permanent enchant that is not being
    // replaced would be a warning about the wrong thing.
    // The enchanting questions, in Wow.exe's order (0x5210D0, SpellTargetItem's
    // body), for each ENCHANT_ITEM (53), ENCHANT_ITEM_TEMPORARY (54) or
    // ENCHANT_ITEM_PRISMATIC (156) effect of the spell:
    //   - BIND_ENCHANT when the enchantment binds (SpellItemEnchantment Flags
    //     bit 0) and the item is equipment not yet bound. Not every BoE item:
    //     a sharpening stone on an unbound sword binds nothing;
    //   - END_REFUND for a refundable item, END_BOUND_TRADEABLE("itemenchant")
    //     for a bound one still tradeable - a permanent enchant ends both;
    //   - REPLACE_ENCHANT when the slot the effect writes already holds one,
    //     naming the old enchantment and the new. The temporary slot too: a
    //     poison over a poison asks, as a rogue knows.
    // Each answer (BindEnchant, EndRefund, EndBoundTradeable, ReplaceEnchant)
    // comes back through replaceEnchant at the next stage, so a bind answer
    // can still meet the replace question - the stock client asks both.
    const int stage = confirmed ? 2 : stageIn;
    if (stage < 2 && !destroysTarget) {
        const uint32_t targetEntry = owner_.getItemEntryByGuid(targetItemGuid);
        const auto* targetInfo = targetEntry ? owner_.getItemInfo(targetEntry) : nullptr;
        const auto& cache = owner_.spellNameCacheRef();
        auto sit = cache.find(pending.spellId);
        auto park = [&](int nextStage, const char* event, std::vector<std::string> args) {
            pendingItemTarget_.reset();
            pendingEnchant_ = PendingEnchant{.active = true, .targetItemGuid = targetItemGuid,
                                             .request = pending, .nextStage = nextStage};
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()(event, args);
        };
        constexpr uint32_t kEffectEnchantItem = 53, kEffectEnchantTemp = 54,
                           kEffectEnchantPrismatic = 156;
        constexpr uint32_t kFieldFlagBopTradeable = 0x100, kFieldFlagRefundable = 0x1000;
        const uint32_t fieldFlags = owner_.getItemFieldFlags(targetItemGuid);
        const bool bound = owner_.isItemSoulbound(targetItemGuid);
        for (int e = 0; sit != cache.end() && e < 3; ++e) {
            const uint32_t effect = sit->second.effectIds[e];
            if (effect != kEffectEnchantItem && effect != kEffectEnchantTemp &&
                effect != kEffectEnchantPrismatic) continue;
            const uint32_t enchantId = static_cast<uint32_t>(sit->second.effectMiscValues[e]);
            const bool binds = (owner_.getEnchantFlags(enchantId) & 1u) != 0;
            if (stage < 1) {
                if (binds && !bound && targetInfo && targetInfo->inventoryType != 0) {
                    park(1, "BIND_ENCHANT", {});
                    return;
                }
                if (effect != kEffectEnchantTemp && (fieldFlags & kFieldFlagRefundable)) {
                    park(1, "END_REFUND", {"1"});
                    return;
                }
                if (bound && (fieldFlags & kFieldFlagBopTradeable) &&
                    !(effect == kEffectEnchantTemp && !binds)) {
                    park(1, "END_BOUND_TRADEABLE", {"itemenchant"});
                    return;
                }
            }
            const auto enchants = owner_.getItemEnchantIds(targetItemGuid);
            const uint32_t existing = effect == kEffectEnchantItem ? enchants.first : enchants.second;
            if (existing != 0 && enchantId != 0) {
                const std::string oldName = owner_.getEnchantName(existing);
                const std::string newName = owner_.getEnchantName(enchantId);
                park(2, "REPLACE_ENCHANT",
                     {oldName.empty() ? std::string("an enchantment") : oldName,
                      newName.empty() ? pending.itemName : newName});
                return;
            }
            break;
        }
    }

    pendingItemTarget_.reset();

    if (pending.fromSpell) {
        auto packet = owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildCastSpellOnItem(pending.spellId, targetItemGuid)
            : CastSpellPacket::buildItemTarget(pending.spellId, targetItemGuid, 0);
        owner_.getSocket()->send(packet);
        LOG_INFO("Casting ", pending.itemName, " (spell ", pending.spellId, ") on item 0x",
                 std::hex, targetItemGuid, std::dec);
        return;
    }
    // Applying to itself is never valid and the server would silently drop it.
    // Dropped without a line of this client's own invention.
    if (targetItemGuid == pending.itemGuid) return;

    sendUseItem(pending.bag, pending.slot, pending.itemGuid, pending.spellId, 0, targetItemGuid);
}

void InventoryHandler::useItemBySlot(int backpackIndex, bool confirmed,
                                     uint64_t unitTarget) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    const auto& slot = owner_.inventoryRef().getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    uint64_t itemGuid = owner_.backpackSlotGuidsRef()[backpackIndex];
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    dispatchUseItem(0xFF, static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + backpackIndex),
                    itemGuid, slot.item, confirmed, unitTarget);
}

void InventoryHandler::useEquippedItem(int equipSlot, bool confirmed,
                                      uint64_t unitTarget) {
    if (equipSlot < 0 || equipSlot >= Inventory::NUM_EQUIP_SLOTS) return;
    const auto& slot =
        owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(equipSlot));
    if (slot.empty()) return;
    uint64_t itemGuid = owner_.equipSlotGuidsRef()[static_cast<size_t>(equipSlot)];
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    // The worn slots are the first wire slots there are, so the index is the
    // slot: no offset the way the backpack needs one.
    dispatchUseItem(0xFF, static_cast<uint8_t>(equipSlot), itemGuid, slot.item,
                    confirmed, unitTarget);
}

void InventoryHandler::useKeyringItem(int index, bool confirmed, uint64_t unitTarget) {
    if (index < 0 || index >= Inventory::KEYRING_SLOTS) return;
    const auto& slot = owner_.inventoryRef().getKeyringSlot(index);
    if (slot.empty()) return;
    // The keyring keeps its guid on the slot; keyringSlotGuids_ is cleared on
    // login and never filled, so reading it here would answer zero every time.
    uint64_t itemGuid = slot.item.guid;
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    dispatchUseItem(0xFF, static_cast<uint8_t>(slots::keyringWireSlot(index)),
                    itemGuid, slot.item, confirmed, unitTarget);
}

void InventoryHandler::useItemInBag(int bagIndex, int slotIndex, bool confirmed,
                                    uint64_t unitTarget) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return;
    const auto& slot = owner_.inventoryRef().getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return;

    uint64_t itemGuid = 0;
    uint64_t bagGuid = owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid != 0) {
        auto it = owner_.containerContentsRef().find(bagGuid);
        if (it != owner_.containerContentsRef().end() && slotIndex < static_cast<int>(it->second.numSlots)) {
            itemGuid = it->second.slotGuids[slotIndex];
        }
    }
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    LOG_INFO("useItemInBag: bag=", bagIndex, " slot=", slotIndex, " itemId=", slot.item.itemId,
             " itemGuid=0x", std::hex, itemGuid, std::dec);

    dispatchUseItem(static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex),
                    static_cast<uint8_t>(slotIndex), itemGuid, slot.item, confirmed,
                    unitTarget);
}

void InventoryHandler::placeGlyphFromBag(uint8_t wireBag, uint8_t wireSlot,
                                         uint32_t socketIndex) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The server refuses the whole use when the index is past the last socket,
    // so a bad one loses the item use as well as the placement.
    if (socketIndex >= 6) return;
    // The item's guid and its use spell, which AzerothCore's
    // HandleUseItemOpcode checks before anything else: spell 0 is "unknown
    // spell" and a guid that is not the item at that position is
    // EQUIP_ERR_ITEM_NOT_FOUND. Both went out as zero, so no drop placed.
    const ItemDef* item = nullptr;
    uint64_t itemGuid = 0;
    auto& inv = owner_.inventoryRef();
    if (wireBag == 0xFF && wireSlot >= Inventory::NUM_EQUIP_SLOTS) {
        const int index = wireSlot - Inventory::NUM_EQUIP_SLOTS;
        if (index < inv.getBackpackSize() && !inv.getBackpackSlot(index).empty()) {
            item = &inv.getBackpackSlot(index).item;
            itemGuid = owner_.backpackSlotGuidsRef()[index];
        }
    } else if (wireBag >= Inventory::FIRST_BAG_EQUIP_SLOT &&
               wireBag < Inventory::FIRST_BAG_EQUIP_SLOT + inv.NUM_BAG_SLOTS) {
        const int bagIndex = wireBag - Inventory::FIRST_BAG_EQUIP_SLOT;
        if (wireSlot < inv.getBagSize(bagIndex) && !inv.getBagSlot(bagIndex, wireSlot).empty()) {
            item = &inv.getBagSlot(bagIndex, wireSlot).item;
            const uint64_t bagGuid = owner_.equipSlotGuidsRef()[wireBag];
            auto it = owner_.containerContentsRef().find(bagGuid);
            if (bagGuid != 0 && it != owner_.containerContentsRef().end() &&
                wireSlot < it->second.numSlots)
                itemGuid = it->second.slotGuids[wireSlot];
        }
    }
    if (!item) return;
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(item->itemId);
    uint32_t spellId = 0;
    if (const auto* info = owner_.getItemInfo(item->itemId)) {
        for (const auto& sp : info->spells) {
            if (sp.spellId != 0 && (sp.spellTrigger == 0 || sp.spellTrigger == 5)) {
                spellId = sp.spellId;
                break;
            }
        }
    }
    auto packet = UseItemPacket::build(wireBag, wireSlot, itemGuid, spellId,
                                       /*targetGuid=*/0, /*itemTargetGuid=*/0,
                                       /*gameObjectGuid=*/0, socketIndex);
    LOG_INFO("placeGlyphFromBag: bag=", (int)wireBag, " slot=", (int)wireSlot,
             " socket=", socketIndex, " spell=", spellId);
    owner_.getSocket()->send(packet);
}

bool InventoryHandler::placePendingGlyph(uint32_t socketIndex) {
    if (!pendingGlyph_) return false;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    if (socketIndex >= 6) return false;
    const PendingItemTarget p = *pendingGlyph_;
    pendingGlyph_.reset();
    auto packet = UseItemPacket::build(p.bag, p.slot, p.itemGuid, p.spellId,
                                       /*targetGuid=*/0, /*itemTargetGuid=*/0,
                                       /*gameObjectGuid=*/0, socketIndex);
    LOG_INFO("placePendingGlyph: ", p.itemName, " socket=", socketIndex);
    owner_.getSocket()->send(packet);
    return true;
}

void InventoryHandler::openItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    if (owner_.inventoryRef().getBackpackSlot(backpackIndex).empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = OpenItemPacket::build(0xFF, static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + backpackIndex));
    LOG_INFO("openItemBySlot: CMSG_OPEN_ITEM bag=0xFF slot=", (Inventory::NUM_EQUIP_SLOTS + backpackIndex));
    owner_.getSocket()->send(packet);
}

void InventoryHandler::openItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return;
    if (owner_.inventoryRef().getBagSlot(bagIndex, slotIndex).empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    uint8_t wowBag = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex);
    auto packet = OpenItemPacket::build(wowBag, static_cast<uint8_t>(slotIndex));
    LOG_INFO("openItemInBag: CMSG_OPEN_ITEM bag=", (int)wowBag, " slot=", slotIndex);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::readItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    const auto& slot = owner_.inventoryRef().getBackpackSlot(backpackIndex);
    if (slot.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    const auto* info = owner_.getItemInfo(slot.item.itemId);
    uint32_t pageTextId = info ? info->pageTextId : slot.item.pageTextId;
    {
        uint64_t guid = owner_.backpackSlotGuidsRef()[backpackIndex];
        if (pageTextId == 0 && readItemText(guid, info)) return;
    }
    if (pageTextId == 0) {
        // Said rather than shrugged off. This is the first of four places a
        // read can end with nothing on screen, and the reader is left looking
        // at an empty page either way - so which one it was has to come from
        // somewhere. An item with no page text is not readable at all, and
        // arriving here means either the item query has not answered yet or it
        // answered without the field.
        LOG_WARNING("readItemBySlot: no page text for item ", slot.item.itemId,
                    " (info ", info ? "cached" : "not yet queried",
                    ") - nothing to read");
        return;
    }

    uint64_t itemGuid = owner_.backpackSlotGuidsRef()[backpackIndex];
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);

    owner_.bookPagesRef().clear();
    owner_.setBookTitle(info ? info->name : std::string());
    owner_.setBookMaterial(info ? info->pageMaterial : 0u);
    auto readPacket = ReadItemPacket::build(0xFF, static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + backpackIndex));
    owner_.getSocket()->send(readPacket);
    auto pagePacket = PageTextQueryPacket::build(pageTextId, itemGuid != 0 ? itemGuid : owner_.getPlayerGuid());
    owner_.getSocket()->send(pagePacket);
    LOG_INFO("readItemBySlot: CMSG_READ_ITEM pageTextId=", pageTextId,
             " bag=0xFF slot=", (Inventory::NUM_EQUIP_SLOTS + backpackIndex));
}

void InventoryHandler::readItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return;
    const auto& slot = owner_.inventoryRef().getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    const auto* info = owner_.getItemInfo(slot.item.itemId);
    uint32_t pageTextId = info ? info->pageTextId : slot.item.pageTextId;
    if (pageTextId == 0 && readItemText(owner_.getBagItemGuid(bagIndex, slotIndex), info)) return;
    if (pageTextId == 0) {
        LOG_WARNING("readItemInBag: no page text for item ", slot.item.itemId,
                    " (info ", info ? "cached" : "not yet queried",
                    ") - nothing to read");
        return;
    }

    uint64_t itemGuid = 0;
    uint64_t bagGuid = owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid != 0) {
        auto it = owner_.containerContentsRef().find(bagGuid);
        if (it != owner_.containerContentsRef().end() && slotIndex < static_cast<int>(it->second.numSlots)) {
            itemGuid = it->second.slotGuids[slotIndex];
        }
    }
    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);

    uint8_t wowBag = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex);
    owner_.bookPagesRef().clear();
    owner_.setBookTitle(info ? info->name : std::string());
    owner_.setBookMaterial(info ? info->pageMaterial : 0u);
    auto readPacket = ReadItemPacket::build(wowBag, static_cast<uint8_t>(slotIndex));
    owner_.getSocket()->send(readPacket);
    auto pagePacket = PageTextQueryPacket::build(pageTextId, itemGuid != 0 ? itemGuid : owner_.getPlayerGuid());
    owner_.getSocket()->send(pagePacket);
    LOG_INFO("readItemInBag: CMSG_READ_ITEM pageTextId=", pageTextId,
             " bag=", static_cast<int>(wowBag), " slot=", slotIndex);
}

void InventoryHandler::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Zero means the whole stack, and this used to turn it into one.
    // HandleDestroyItemOpcode branches on exactly this: a count destroys that
    // many, no count destroys the slot. Coercing it away left no way to say
    // "all of them" - which is what the confirmation prompt is asking about,
    // and the only thing a stack larger than 255 could be told to do.
    constexpr uint16_t kCmsgDestroyItem = 0x111;
    network::Packet packet(kCmsgDestroyItem);
    packet.writeUInt8(bag);
    packet.writeUInt8(slot);
    packet.writeUInt32(static_cast<uint32_t>(count));
    LOG_DEBUG("Destroy item request: bag=", (int)bag, " slot=", (int)slot,
              " count=", (int)count, " wire=0x", std::hex, kCmsgDestroyItem, std::dec);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::splitItemTo(uint8_t srcBag, uint8_t srcSlot,
                                   uint8_t dstBag, uint8_t dstSlot, uint32_t count) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (count == 0) return;
    LOG_INFO("splitItem: src(bag=", (int)srcBag, " slot=", (int)srcSlot,
             ") count=", count, " -> dst(bag=", (int)dstBag,
             " slot=", (int)dstSlot, ")");
    owner_.getSocket()->send(SplitItemPacket::build(srcBag, srcSlot, dstBag, dstSlot, count));
}

void InventoryHandler::autostoreBagItem(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // No local move: the server chooses the slot (stacks first) and answers
    // with the item update or an equip error, so guessing one here would only
    // be undone.
    owner_.getSocket()->send(AutoStoreBagItemPacket::build(srcBag, srcSlot, dstBag));
}

void InventoryHandler::splitItem(uint8_t srcBag, uint8_t srcSlot, uint32_t count) {
    // No destination given: the first free slot, which is what this client's
    // own bag window means by a split. The interface means something else and
    // says where it wants it - see splitItemTo.
    const int freeBp = owner_.inventoryRef().findFreeBackpackSlot();
    if (freeBp >= 0) {
        splitItemTo(srcBag, srcSlot, 0xFF,
                    static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + freeBp), count);
        return;
    }
    for (int b = 0; b < owner_.inventoryRef().NUM_BAG_SLOTS; b++) {
        const int bagSize = owner_.inventoryRef().getBagSize(b);
        for (int s = 0; s < bagSize; s++) {
            if (!owner_.inventoryRef().getBagSlot(b, s).empty()) continue;
            splitItemTo(srcBag, srcSlot,
                        static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + b),
                        static_cast<uint8_t>(s), count);
            return;
        }
    }
    owner_.raiseUiError("Cannot split: no free inventory slots.");
}

void InventoryHandler::fireBagUpdates() {
    auto& fire = owner_.addonEventCallbackRef();
    if (!fire) return;
    // Every bag, because the callers that reach here know the inventory changed
    // without always knowing which bag it was, and an interface that redraws a
    // bag it did not need to is cheaper than one that never redraws at all.
    //
    // Every bag means the seven bank bags as well. The interface numbers them
    // straight after the four worn ones - NUM_BAG_SLOTS + 1 upward - and each
    // bank bag's frame redraws from BAG_UPDATE carrying its own number, exactly
    // as a worn bag's does. Stopping at four left them out, so an item moved
    // into or out of a purchased bank bag sat on screen where it had been until
    // the bank was closed and reopened. The bank's own twenty-eight slots are
    // not here: they are player fields rather than a container, and they are
    // told through PLAYERBANKSLOTS_CHANGED.
    constexpr int kLastWornBag = game::Inventory::NUM_BAG_SLOTS;          // 1..4
    const int kLastBankBag = kLastWornBag + game::slots::bankBagCount();  // 5..11, or 5..10
    for (int bag = 0; bag <= kLastBankBag; ++bag) fire("BAG_UPDATE", {std::to_string(bag)});
    // The character sheet redraws from this one rather than from BAG_UPDATE, so
    // both go out together - equipping something changes a bag and a slot.
    fire("UNIT_INVENTORY_CHANGED", {"player"});
    // Currency counts are items too - emblems and marks in the token slots
    // (118-149) or a bag - and the currency tab and the backpack's token row
    // redraw only on CURRENCY_DISPLAY_UPDATE (TokenFrame, BackpackTokenFrame).
    // Only when one of them changed: Wow.exe fires it from a currency item's
    // count changing (0x707720, item class 10) or a token slot changing
    // (0x5D9790, slots 118-149), not on every bag update. The counts of
    // every CurrencyTypes.dbc item stand in for "which item changed".
    {
        uint64_t signature = 1469598103934665603ull;
        for (const auto& c : owner_.getCurrencyTypes()) {
            const uint64_t count = owner_.getInventory().countItem(c.itemId) +
                                   owner_.countCurrencyTokens(c.itemId);
            signature = (signature ^ (count + (static_cast<uint64_t>(c.itemId) << 32))) * 1099511628211ull;
        }
        if (!currencySignatureKnown_ || signature != currencySignature_) {
            currencySignatureKnown_ = true;
            currencySignature_ = signature;
            fire("CURRENCY_DISPLAY_UPDATE", {});
        }
    }
    // Action buttons redraw their item counts only on ACTIONBAR_SLOT_CHANGED.
    // A stack shrinking arrives as a CREATE block for the existing item and
    // comes through here, not through the values path that used to send it.
    for (const auto& a : owner_.getActionBar()) {
        if (a.type == ActionBarSlot::ITEM && a.id != 0) {
            fire("ACTIONBAR_SLOT_CHANGED", {"0"});
            break;
        }
    }
    // An open trade skill window is looking at the bags too, and it does not
    // watch them: TradeSkillFrame_OnLoad registers TRADE_SKILL_UPDATE,
    // TRADE_SKILL_FILTER_UPDATE, UNIT_PORTRAIT_UPDATE and
    // UPDATE_TRADESKILL_RECAST, and nothing else. So the only thing that redraws
    // a reagent's "12 /1" or a recipe's "[14]" is this event, and without it
    // both stood still while the reagents were spent - craft after craft against
    // numbers that never moved, until the window was closed and reopened.
    //
    // It also releases the row list, whose canMake comes from the same bag
    // counts and is otherwise held between redraws.
    if (owner_.isCraftingWindowOpen()) fire("TRADE_SKILL_UPDATE", {});
    // The first few only: enough to tell "the event never goes out" from "it
    // goes out and the interface ignores it", without a line every time the
    // inventory is rebuilt.
    // Rate-limited rather than counted: the first few are the inventory being
    // loaded at startup, and capping the count spent them all there - leaving
    // nothing to say for the drag that prompted the question.
    static double lastSaid = 0.0;
    // ...and an end to it. The rate limit keeps the startup burst from
    // spending the count, and the budget keeps a session of looting from
    // writing this every two seconds until the player logs out.
    static core::LogBudget bagUpdateBudget(8, "BAG_UPDATE + UNIT_INVENTORY_CHANGED");
    const double now = core::appTimeSeconds();
    if (now - lastSaid > 2.0 && bagUpdateBudget.take()) {
        lastSaid = now;
        LOG_WARNING("BAG_UPDATE + UNIT_INVENTORY_CHANGED fired");
    }
}

/// Read and write a model slot by the wire's numbering.
///
/// The wire keeps everything in one flat space - the backpack is slots 23
/// upward inside container 0xFF, a worn bag is container 19 upward with its own
/// slots from zero - and the model keeps the two apart. False for a pair that
/// names neither, which is every bank and keyring slot: those have their own
/// paths and a swap should leave them alone rather than guess.
bool InventoryHandler::readWireSlot(uint8_t container, uint8_t slot,
                                    ItemDef& out) const {
    const auto& inv = owner_.inventoryRef();
    if (container == slots::kNoContainer) {
        const int index = static_cast<int>(slot) - slots::kBackpackFirst;
        if (index < 0 || index >= inv.getBackpackSize()) return false;
        out = inv.getBackpackSlot(index).item;
        return true;
    }
    const int bagIndex = static_cast<int>(container) - slots::kWornBagFirst;
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (static_cast<int>(slot) >= inv.getBagSize(bagIndex)) return false;
    out = inv.getBagSlot(bagIndex, static_cast<int>(slot)).item;
    return true;
}

bool InventoryHandler::writeWireSlot(uint8_t container, uint8_t slot,
                                     const ItemDef& item) {
    auto& inv = owner_.inventoryRef();
    if (container == slots::kNoContainer) {
        const int index = static_cast<int>(slot) - slots::kBackpackFirst;
        if (index < 0 || index >= inv.getBackpackSize()) return false;
        return inv.setBackpackSlot(index, item);
    }
    const int bagIndex = static_cast<int>(container) - slots::kWornBagFirst;
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (static_cast<int>(slot) >= inv.getBagSize(bagIndex)) return false;
    return inv.setBagSlot(bagIndex, static_cast<int>(slot), item);
}

void InventoryHandler::swapContainerItems(uint8_t srcBag, uint8_t srcSlot,
                                          uint8_t dstBag, uint8_t dstSlot,
                                          bool applyLocally) {
    if (!owner_.getSocket() || !owner_.getSocket()->isConnected()) return;
    LOG_INFO("swapContainerItems: src(bag=", (int)srcBag, " slot=", (int)srcSlot,
             ") -> dst(bag=", (int)dstBag, " slot=", (int)dstSlot, ")");
    auto packet = SwapItemPacket::build(dstBag, dstSlot, srcBag, srcSlot);
    owner_.getSocket()->send(packet);

    // Moved here as well as sent, which swapBagSlots directly below has always
    // done and this never did. Nothing else moved the item: the bags redrew
    // only if the server's answer happened to change a field the update path
    // watches, and a swap between two slots of one bag changes none of the
    // item's own fields - it changes which slot holds which guid.
    //
    // So an item dragged across a bag stayed where it was drawn until
    // something unrelated forced a rebuild.
    //
    // Both sides are read before either is written, or a move into an
    // unreachable slot would empty the one it came from.
    //
    // Unless the caller has already moved it. A sort computes the whole
    // finished layout, writes it into the model in one go and then sends the
    // moves that get the server there - so applying each of those a second time
    // permutes a layout that was already final, and the bags came out shuffled
    // rather than sorted. That is the two halves of this file's history meeting:
    // the local move above was added for drag-and-drop, which had no other way
    // to move an item, and the sort has never needed it.
    if (!applyLocally) return;

    ItemDef from, to;
    if (readWireSlot(srcBag, srcSlot, from) && readWireSlot(dstBag, dstSlot, to)) {
        writeWireSlot(srcBag, srcSlot, to);
        writeWireSlot(dstBag, dstSlot, from);
        fireBagUpdates();
    }
}

void InventoryHandler::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (srcBagIndex < 0 || srcBagIndex > 3 || dstBagIndex < 0 || dstBagIndex > 3) return;
    if (srcBagIndex == dstBagIndex) return;

    auto srcEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + srcBagIndex);
    auto dstEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + dstBagIndex);
    auto srcItem = owner_.inventoryRef().getEquipSlot(srcEquip).item;
    auto dstItem = owner_.inventoryRef().getEquipSlot(dstEquip).item;
    owner_.inventoryRef().setEquipSlot(srcEquip, dstItem);
    owner_.inventoryRef().setEquipSlot(dstEquip, srcItem);
    owner_.inventoryRef().swapBagContents(srcBagIndex, dstBagIndex);

    // A bag that moved takes its window with it, which is what the real client
    // does and what ContainerFrame_OnEvent is written for: BAG_CLOSED carrying
    // the bag's number hides that frame. Nothing was said here at all, so a bag
    // rearranged on the bar while its window was open left the window standing
    // over contents that had moved to the other bag - and clicking a slot in it
    // acted on the container the frame still named. Closing both and reopening
    // was the only way to move anything again.
    //
    // The interface numbers a worn bag from one; these indices count from zero.
    auto& fire = owner_.addonEventCallbackRef();
    if (fire) {
        fire("BAG_CLOSED", {std::to_string(srcBagIndex + 1)});
        fire("BAG_CLOSED", {std::to_string(dstBagIndex + 1)});
    }
    // And the bar itself, plus every frame that draws from a bag: the two have
    // exchanged contents whether or not either was open.
    fireBagUpdates();

    if (owner_.getSocket() && owner_.getSocket()->isConnected()) {
        uint8_t srcSlot = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + srcBagIndex);
        uint8_t dstSlot = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + dstBagIndex);
        LOG_INFO("swapBagSlots: bag ", srcBagIndex, " (slot ", (int)srcSlot,
                 ") <-> bag ", dstBagIndex, " (slot ", (int)dstSlot, ")");
        auto packet = SwapItemPacket::build(255, dstSlot, 255, srcSlot);
        owner_.getSocket()->send(packet);
    }
}

void InventoryHandler::unequipToBackpack(EquipSlot equipSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    int freeSlot = owner_.inventoryRef().findFreeBackpackSlot();
    if (freeSlot < 0) {
        owner_.raiseUiError("Cannot unequip: no free backpack slots.");
        return;
    }

    uint8_t srcBag = 0xFF;
    uint8_t srcSlot = static_cast<uint8_t>(equipSlot);
    uint8_t dstBag = 0xFF;
    uint8_t dstSlot = static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + freeSlot);

    LOG_INFO("UnequipToBackpack: equipSlot=", (int)srcSlot,
             " -> backpackIndex=", freeSlot, " (dstSlot=", (int)dstSlot, ")");

    auto packet = SwapItemPacket::build(dstBag, dstSlot, srcBag, srcSlot);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::useItemById(uint32_t itemId, uint64_t unitTarget) {
    if (itemId == 0) return;
    LOG_DEBUG("useItemById: searching for itemId=", itemId);
    for (int i = 0; i < owner_.inventoryRef().getBackpackSize(); i++) {
        const auto& slot = owner_.inventoryRef().getBackpackSlot(i);
        if (!slot.empty() && slot.item.itemId == itemId) {
            LOG_DEBUG("useItemById: found itemId=", itemId, " at backpack slot ", i);
            useItemBySlot(i, false, unitTarget);
            return;
        }
    }
    for (int bag = 0; bag < owner_.inventoryRef().NUM_BAG_SLOTS; bag++) {
        int bagSize = owner_.inventoryRef().getBagSize(bag);
        for (int slot = 0; slot < bagSize; slot++) {
            const auto& bagSlot = owner_.inventoryRef().getBagSlot(bag, slot);
            if (!bagSlot.empty() && bagSlot.item.itemId == itemId) {
                LOG_DEBUG("useItemById: found itemId=", itemId, " in bag ", bag, " slot ", slot);
                useItemInBag(bag, slot, false, unitTarget);
                return;
            }
        }
    }
    // The keyring and the worn slots, which this searched neither of.
    //
    // A key lives nowhere else, so a macro or an action-bar slot naming one
    // could never find it. The worn slots are worse than an omission: /use
    // with an equipment slot number reads that slot's item id and hands it
    // straight to this, so the one call site written for equipped items was
    // the one that could not work at all.
    //
    // Last, so nothing that already resolved changes where it resolves to. A
    // trinket carried in a bag and a second one worn keep answering the bag.
    for (int i = 0; i < owner_.inventoryRef().getKeyringSize(); i++) {
        const auto& slot = owner_.inventoryRef().getKeyringSlot(i);
        if (!slot.empty() && slot.item.itemId == itemId) {
            LOG_DEBUG("useItemById: found itemId=", itemId, " in keyring slot ", i);
            useKeyringItem(i, false, unitTarget);
            return;
        }
    }
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; i++) {
        const auto& slot =
            owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(i));
        if (!slot.empty() && slot.item.itemId == itemId) {
            LOG_DEBUG("useItemById: found itemId=", itemId, " equipped in slot ", i);
            useEquippedItem(i, false, unitTarget);
            return;
        }
    }
    LOG_WARNING("useItemById: itemId=", itemId,
                " not found in the bags, the keyring or the worn slots");
}

void InventoryHandler::handleListInventory(network::Packet& packet) {
    if (!ListInventoryParser::parse(packet, currentVendorItems_)) return;

    // Detect repair capability from NPC flags (covers direct vendors without gossip).
    // UNIT_NPC_FLAG_REPAIR = 0x1000.
    if (!currentVendorItems_.canRepair && currentVendorItems_.vendorGuid != 0) {
        if (auto* unit = owner_.getUnitByGuid(currentVendorItems_.vendorGuid)) {
            if (unit->getNpcFlags() & NPC_FLAG_REPAIR) {
                currentVendorItems_.canRepair = true;
            }
        }
    }
    vendorWindowOpen_ = true;
    owner_.closeGossip();
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MERCHANT_SHOW", {});

    // Auto-sell grey items
    if (autoSellGrey_ && currentVendorItems_.vendorGuid != 0 && owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        int itemsSold = 0;
        uint32_t totalSellPrice = 0;
        for (int i = 0; i < owner_.inventoryRef().getBackpackSize(); ++i) {
            const auto& slot = owner_.inventoryRef().getBackpackSlot(i);
            if (slot.empty()) continue;
            uint32_t quality = 0;
            uint32_t sellPrice = slot.item.sellPrice;
            if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
                quality = info->quality;
                if (sellPrice == 0) sellPrice = info->sellPrice;
            }
            if (quality == 0 && sellPrice > 0) {
                uint64_t itemGuid = owner_.backpackSlotGuidsRef()[i];
                if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
                if (itemGuid != 0) {
                    const uint32_t count = sellCountForStack(slot.item.stackCount);
                    sellItem(currentVendorItems_.vendorGuid, itemGuid, count);
                    // Per unit, so the money reported matches the money paid.
                    totalSellPrice += sellPrice * count;
                    ++itemsSold;
                }
            }
        }
        for (int b = 0; b < owner_.inventoryRef().NUM_BAG_SLOTS; ++b) {
            int bagSize = owner_.inventoryRef().getBagSize(b);
            for (int s = 0; s < bagSize; ++s) {
                const auto& slot = owner_.inventoryRef().getBagSlot(b, s);
                if (slot.empty()) continue;
                uint32_t quality = 0;
                uint32_t sellPrice = slot.item.sellPrice;
                if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
                    quality = info->quality;
                    if (sellPrice == 0) sellPrice = info->sellPrice;
                }
                if (quality == 0 && sellPrice > 0) {
                    uint64_t itemGuid = 0;
                    uint64_t bagGuid = owner_.equipSlotGuidsRef()[19 + b];
                    if (bagGuid != 0) {
                        auto cit = owner_.containerContentsRef().find(bagGuid);
                        if (cit != owner_.containerContentsRef().end() && s < static_cast<int>(cit->second.numSlots))
                            itemGuid = cit->second.slotGuids[s];
                    }
                    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
                    if (itemGuid != 0) {
                        const uint32_t count = sellCountForStack(slot.item.stackCount);
                        sellItem(currentVendorItems_.vendorGuid, itemGuid, count);
                        totalSellPrice += sellPrice * count;
                        ++itemsSold;
                    }
                }
            }
        }
        if (itemsSold > 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "|cffaaaaaaAuto-sold %d grey item%s for %s.|r",
                itemsSold, itemsSold == 1 ? "" : "s",
                game::formatCopperPrice(totalSellPrice).c_str());
            owner_.addSystemChatMessage(buf);
        }
    }

    // Auto-repair
    if (autoRepair_ && currentVendorItems_.canRepair && currentVendorItems_.vendorGuid != 0) {
        bool anyDamaged = false;
        for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i) {
            const auto& slot = owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(i));
            if (!slot.empty() && slot.item.maxDurability > 0
                    && slot.item.curDurability < slot.item.maxDurability) {
                anyDamaged = true;
                break;
            }
        }
        if (anyDamaged) {
            repairAll(currentVendorItems_.vendorGuid, false);
            owner_.addSystemChatMessage("|cffaaaaaaAuto-repair triggered.|r");
        }
    }

    // No voice line here: the stock client has one greeting per NPC, said when
    // its window opens (GameHandler::updateInteractTarget, 0x720860).

    for (const auto& item : currentVendorItems_.items) {
        owner_.queryItemInfo(item.itemId, 0);
    }
}

// ============================================================
// Trainer
// ============================================================

void InventoryHandler::handleTrainerList(network::Packet& packet) {
    const bool isClassic = isClassicLikeExpansion();
    if (!TrainerListParser::parse(packet, currentTrainerList_, isClassic)) return;
    trainerWindowOpen_ = true;
    owner_.closeGossip();
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRAINER_SHOW", {});

    // At WARNING, and broken down by the three states the panel filters on,
    // because a trainer showing no rows looks the same whether it sent none,
    // sent only things this character already knows, or the filter hid what it
    // did send - and which of those it was decides whether anything is wrong.
    // The states are the ones on the wire; the panel promotes a spell to known
    // the moment the spell handler has it, so its own count can be higher.
    size_t available = 0, unavailable = 0, known = 0;
    for (const auto& sp : currentTrainerList_.spells) {
        if (sp.state == 0)      ++available;
        else if (sp.state == 2) ++known;
        else                    ++unavailable;
    }
    LOG_WARNING("Trainer list: ", currentTrainerList_.spells.size(), " spells - ",
                available, " available, ", unavailable, " unavailable, ",
                known, " already known");

    owner_.loadSpellNameCache();
    owner_.loadSkillLineDbc();
    owner_.loadSkillLineAbilityDbc();
    categorizeTrainerSpells();
}

void InventoryHandler::trainSpell(uint32_t spellId) {
    LOG_INFO("Trainer purchase requested: spellId=", spellId,
             " state=", (int)owner_.getState(),
             " socket=", (owner_.getSocket() ? "yes" : "no"));
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        LOG_WARNING("trainSpell: Not in world or no socket connection");
        return;
    }

    uint32_t spellCost = 0;
    for (const auto& spell : currentTrainerList_.spells) {
        if (spell.spellId == spellId) {
            spellCost = spell.spellCost;
            break;
        }
    }
    LOG_INFO("Player money: ", owner_.playerMoneyCopperRef(), " copper, spell cost: ", spellCost, " copper");

    LOG_INFO("Sending CMSG_TRAINER_BUY_SPELL: guid=", currentTrainerList_.trainerGuid,
             " spellId=", spellId);
    auto packet = TrainerBuySpellPacket::build(
        currentTrainerList_.trainerGuid,
        spellId);
    owner_.getSocket()->send(packet);
    LOG_INFO("CMSG_TRAINER_BUY_SPELL sent");
}

void InventoryHandler::closeTrainer() {
    trainerWindowOpen_ = false;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRAINER_CLOSED", {});
    currentTrainerList_ = TrainerListData{};
    trainerTabs_.clear();
}

void InventoryHandler::categorizeTrainerSpells() {
    trainerTabs_.clear();

    static constexpr uint32_t SKILLLINE_CATEGORY_CLASS = 7;

    std::map<uint32_t, std::vector<const TrainerSpell*>> specialtySpells;
    std::vector<const TrainerSpell*> generalSpells;

    for (const auto& spell : currentTrainerList_.spells) {
        auto slIt = owner_.spellToSkillLineRef().find(spell.spellId);
        if (slIt != owner_.spellToSkillLineRef().end()) {
            uint32_t skillLineId = slIt->second;
            auto catIt = owner_.skillLineCategoriesRef().find(skillLineId);
            if (catIt != owner_.skillLineCategoriesRef().end() && catIt->second == SKILLLINE_CATEGORY_CLASS) {
                specialtySpells[skillLineId].push_back(&spell);
                continue;
            }
        }
        generalSpells.push_back(&spell);
    }

    auto byName = [this](const TrainerSpell* a, const TrainerSpell* b) {
        return owner_.getSpellName(a->spellId) < owner_.getSpellName(b->spellId);
    };

    std::vector<std::pair<std::string, std::vector<const TrainerSpell*>>> named;
    for (auto& [skillLineId, spells] : specialtySpells) {
        auto nameIt = owner_.skillLineNamesRef().find(skillLineId);
        std::string tabName = (nameIt != owner_.skillLineNamesRef().end()) ? nameIt->second : "Specialty";
        std::sort(spells.begin(), spells.end(), byName);
        named.emplace_back(std::move(tabName), std::move(spells));
    }
    std::sort(named.begin(), named.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& [name, spells] : named) {
        trainerTabs_.push_back({.name = std::move(name), .spells = std::move(spells)});
    }

    if (!generalSpells.empty()) {
        std::sort(generalSpells.begin(), generalSpells.end(), byName);
        trainerTabs_.push_back({.name = "General", .spells = std::move(generalSpells)});
    }

    LOG_INFO("Trainer: Categorized into ", trainerTabs_.size(), " tabs");
}

// ============================================================
// Item socketing
// ============================================================

void InventoryHandler::openSocketing(uint64_t itemGuid) {
    if (itemGuid == 0) return;
    socketSession_ = SocketSession{};
    socketSession_.open = true;
    socketSession_.itemGuid = itemGuid;

    // The template is what carries the socket colours and the bonus. Ask for it
    // if it has not been seen - the panel redraws on the next SOCKET_INFO_UPDATE
    // and an item nobody has queried yet would otherwise show no sockets at all.
    const auto& online = owner_.onlineItemsRef();
    auto it = online.find(itemGuid);
    if (it != online.end()) {
        socketSession_.itemId = it->second.entry;
        owner_.ensureItemInfo(socketSession_.itemId);
    }

    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("SOCKET_INFO_UPDATE", {});
}

void InventoryHandler::closeSocketing() {
    if (!socketSession_.open) return;
    socketSession_ = SocketSession{};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("SOCKET_INFO_CLOSE", {});
}

bool InventoryHandler::setSocketGem(int index, uint64_t gemGuid, uint32_t gemItemId) {
    if (!socketSession_.open || index < 0 || index > 2) return false;

    // The server drops the whole request when two sockets name one guid, so a
    // gem already placed cannot be placed again - it moves instead.
    if (gemGuid != 0) {
        for (int i = 0; i < 3; ++i) {
            if (i != index && socketSession_.newGemGuid[i] == gemGuid) {
                socketSession_.newGemGuid[i] = 0;
                socketSession_.newGemItemId[i] = 0;
            }
        }
    }
    socketSession_.newGemGuid[index] = gemGuid;
    socketSession_.newGemItemId[index] = gemItemId;

    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("SOCKET_INFO_UPDATE", {});
    return true;
}

void InventoryHandler::acceptSockets() {
    if (!socketSession_.open || socketSession_.itemGuid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    const bool anyPending = socketSession_.newGemGuid[0] || socketSession_.newGemGuid[1] ||
                            socketSession_.newGemGuid[2];
    if (!anyPending) return;

    auto packet = SocketGemsPacket::build(socketSession_.itemGuid, socketSession_.newGemGuid);
    owner_.getSocket()->send(packet);

    // The gems are gone from this client's hands the moment the request leaves.
    // What comes back is the item's new enchantment fields, which is where the
    // panel reads a socketed gem from - so clear the pending set and let the
    // update redraw it as an existing gem rather than a waiting one.
    socketSession_.newGemGuid = {};
    socketSession_.newGemItemId = {};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("SOCKET_INFO_UPDATE", {});
}

// ============================================================
// Mail
// ============================================================

void InventoryHandler::openMailbox(uint64_t guid) {
    mailboxGuid_ = guid;
    mailboxOpen_ = true;
    // Through the setter, which is the only thing that says so. Assigning the
    // member cleared the flag and told nobody: this client's own minimap asks
    // hasNewMail() again on every frame it draws, so it went out by itself,
    // and an interface that is told once kept the envelope up for the rest of
    // the session.
    setHasNewMail(false);
    selectedMailIndex_ = -1;
    showMailCompose_ = false;
    clearMailAttachments();
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MAIL_SHOW", {});
    selectDefaultStationery();
    refreshMailList();
}

void InventoryHandler::selectDefaultStationery() {
    // The paper a letter is written on, chosen the way it arrives already in
    // the frame.
    //
    // SendMailFrame_CanSend counts three things before it enables the Send
    // button, and one is that a stationery has been chosen. Nothing chooses one
    // when the frame opens: SendMailFrame_Reset is the only thing in the whole
    // interface that does, and it runs after a letter has been sent
    // successfully. So the first letter of a session could never be sent - the
    // button was disabled before it was ever pressed, which is exactly what the
    // input log shows and what "mail not being sent" turned out to be.
    owner_.runInterfaceCommand(
        "if StationeryPopupFrame and not StationeryPopupFrame.selectedIndex then "
        "StationeryPopupButton_OnClick(nil, 1) end");
}

void InventoryHandler::closeMailbox() {
    mailboxOpen_ = false;
    mailboxGuid_ = 0;
    showMailCompose_ = false;
    clearMailAttachments();
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MAIL_CLOSED", {});
    // What was read at the box is no longer news: ask again, so the envelope
    // and its sender list match the server rather than the last reply.
    queryNextMailTime();
}

void InventoryHandler::refreshMailList() {
    if (!mailboxOpen_ || mailboxGuid_ == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GetMailListPacket::build(mailboxGuid_);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::refuseSend(const std::string& reason, const char* logLine) {
    // The compose frame disables its Send button the moment it is pressed and
    // re-enables it only when it hears how the send went. A refusal on this
    // side used to return without a word, so the button stayed disabled - one
    // silent refusal and no letter could be sent for the rest of the session,
    // whatever was wrong the first time.
    // Not said: SendMail (Wow.exe 0x570910) refuses these in silence. The
    // frame still has to hear that nothing went.
    LOG_WARNING("sendMail: ", logLine, " (", reason, ")");
    if (owner_.addonEventCallbackRef()) {
        // Zero is "internal error" in the server's own table, which is the
        // honest answer for a refusal that never reached it.
        owner_.addonEventCallbackRef()("MAIL_FAILED", {"0"});
    }
}

void InventoryHandler::sendMail(const std::string& recipient, const std::string& subject,
                                const std::string& body, uint64_t money, uint64_t cod) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        refuseSend("Cannot send mail right now.", "not in world");
        return;
    }
    if (!owner_.getSocket()) {
        refuseSend("Cannot send mail: not connected.", "no socket");
        return;
    }
    if (mailboxGuid_ == 0) {
        refuseSend("You are not at a mailbox.", "mailboxGuid_ is 0 (mailbox closed?)");
        return;
    }
    if (recipient.empty()) {
        refuseSend("Enter a recipient.", "no recipient");
        return;
    }
    // Collect attached item GUIDs
    std::vector<uint64_t> itemGuids;
    for (const auto& att : mailAttachments_) {
        if (att.occupied()) {
            itemGuids.push_back(att.itemGuid);
        }
    }
    const int sendable = maxSendableMailAttachments();
    if (static_cast<int>(itemGuids.size()) > sendable) {
        // Should be unreachable now that attaching is capped, but dropping
        // attachments without saying so is how this went unnoticed.
        LOG_ERROR("sendMail: ", itemGuids.size(), " attachments but this expansion's "
                  "packet carries ", sendable, " - refusing to send and lose the rest");
        refuseSend("This realm's mail carries one item per letter.", "too many attachments");
        return;
    }
    auto packet = owner_.getPacketParsers()->buildSendMail(mailboxGuid_, recipient, subject, body, money, cod, itemGuids);
    LOG_INFO("sendMail: to='", recipient, "' subject='", subject, "' money=", money,
             " attachments=", itemGuids.size(), " mailboxGuid=", mailboxGuid_);
    owner_.getSocket()->send(packet);
    // The attachments stay until the server answers: handleSendMailResult
    // clears them on MAIL_SEND / MAIL_OK. A refused letter (no such player,
    // not enough money) keeps its items on the compose frame, as the stock
    // client does, instead of the frame emptying under a MAIL_FAILED.
}

int InventoryHandler::maxSendableMailAttachments() {
    // Vanilla's packet has one uint64 item GUID where TBC and later have a
    // count followed by an array.
    return isClassicLikeExpansion() ? 1 : MAIL_MAX_ATTACHMENTS;
}

/// Warn when an attached item still has a refund window, which posting it ends.
///
/// Only tellable since the refund info is asked for and kept: this used to be
/// recorded as correctly-unfired on the grounds that the window was a timer
/// this client did not have. It has one now.
///
/// The interface answers with RespondMailLockSendItem(slot, keep).
void InventoryHandler::noteMailAttachRefundable(int attachIndex) {
    if (attachIndex < 0 || attachIndex >= maxSendableMailAttachments()) return;
    const auto& att = mailAttachments_[static_cast<size_t>(attachIndex)];
    if (!att.occupied()) return;
    const auto* refund = owner_.getItemRefundInfo(att.itemGuid);
    if (!refund) {
        // Not asked yet. Ask, so the next attachment of the same item knows -
        // and say nothing now rather than guess, which is what an item with no
        // window looks like too.
        owner_.requestItemRefundInfo(att.itemGuid);
        return;
    }
    constexpr uint32_t kRefundWindow = 2 * 60 * 60;
    if (refund->playedSincePurchase >= kRefundWindow) return;   // window is over
    if (!owner_.addonEventCallbackRef()) return;
    const auto* info = owner_.getItemInfo(att.item.itemId);
    owner_.addonEventCallbackRef()(
        "MAIL_LOCK_SEND_ITEMS",
        {std::to_string(attachIndex + 1),
         info ? game::buildItemLink(att.item.itemId, info->quality, info->name)
              : att.item.name});
}

bool InventoryHandler::attachItemFromBackpack(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return false;
    const auto& slot = owner_.inventoryRef().getBackpackSlot(backpackIndex);
    if (slot.empty()) return false;
    uint64_t itemGuid = owner_.backpackSlotGuidsRef()[backpackIndex];
    if (itemGuid == 0) return false;
    for (int i = 0; i < maxSendableMailAttachments(); ++i) {
        if (!mailAttachments_[i].occupied()) {
            mailAttachments_[i].itemGuid = itemGuid;
            mailAttachments_[i].item = slot.item;
            mailAttachments_[i].srcBag = 0xFF;
            mailAttachments_[i].srcSlot = static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + backpackIndex);
            noteMailAttachRefundable(i);
            notifyMailComposeChanged();
            return true;
        }
    }
    return false;
}

bool InventoryHandler::attachItemFromBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventoryRef().NUM_BAG_SLOTS) return false;
    if (slotIndex < 0 || slotIndex >= owner_.inventoryRef().getBagSize(bagIndex)) return false;
    const auto& slot = owner_.inventoryRef().getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return false;
    uint64_t bagGuid = owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid == 0) return false;
    auto it = owner_.containerContentsRef().find(bagGuid);
    if (it == owner_.containerContentsRef().end()) return false;
    if (slotIndex >= static_cast<int>(it->second.numSlots)) return false;
    uint64_t itemGuid = it->second.slotGuids[slotIndex];
    if (itemGuid == 0) return false;
    for (int i = 0; i < maxSendableMailAttachments(); ++i) {
        if (!mailAttachments_[i].occupied()) {
            mailAttachments_[i].itemGuid = itemGuid;
            mailAttachments_[i].item = slot.item;
            mailAttachments_[i].srcBag = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex);
            mailAttachments_[i].srcSlot = static_cast<uint8_t>(slotIndex);
            noteMailAttachRefundable(i);
            notifyMailComposeChanged();
            return true;
        }
    }
    return false;
}

bool InventoryHandler::detachMailAttachment(int attachIndex) {
    if (attachIndex < 0 || attachIndex >= MAIL_MAX_ATTACHMENTS) return false;
    mailAttachments_[attachIndex] = MailAttachSlot{};
    notifyMailComposeChanged();
    return true;
}

void InventoryHandler::clearMailAttachments() {
    for (auto& a : mailAttachments_) a = MailAttachSlot{};
    notifyMailComposeChanged();
}

void InventoryHandler::notifyMailComposeChanged() {
    // The send frame recomputes its slots, its postage and its Send button from
    // this and from nothing else - there is no poll behind it.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("MAIL_SEND_INFO_UPDATE", {});
    }
}

int InventoryHandler::getMailAttachmentCount() const {
    int count = 0;
    for (const auto& a : mailAttachments_)
        if (a.occupied()) ++count;
    return count;
}

void InventoryHandler::mailTakeMoney(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mailboxGuid_ == 0) return;
    auto packet = MailTakeMoneyPacket::build(mailboxGuid_, mailId);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mailboxGuid_ == 0) return;
    auto packet = MailTakeItemPacket::build(mailboxGuid_, mailId, itemGuidLow);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::mailReturnToSender(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mailboxGuid_ == 0) return;
    uint64_t senderGuid = 0;
    for (const auto& mail : mailInbox_) {
        if (mail.messageId == mailId) { senderGuid = mail.senderGuid; break; }
    }
    auto packet = MailReturnToSenderPacket::build(mailboxGuid_, mailId, senderGuid);
    owner_.getSocket()->send(packet);
    // Same as deleting: the letter being read is this one, and the view over it
    // hides when told which mail closed.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("CLOSE_INBOX_ITEM", {std::to_string(mailId)});
    }
}

void InventoryHandler::mailDelete(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mailboxGuid_ == 0) return;
    auto packet = MailDeletePacket::build(mailboxGuid_, mailId, 0);
    owner_.getSocket()->send(packet);
    // The letter being read is this one, so the view over it has to go: the
    // interface hides it when told which mail closed, and was never told.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("CLOSE_INBOX_ITEM", {std::to_string(mailId)});
    }
}

void InventoryHandler::mailMarkAsRead(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mailboxGuid_ == 0) return;
    auto packet = MailMarkAsReadPacket::build(mailboxGuid_, mailId);
    owner_.getSocket()->send(packet);

    // And locally, because the server does not send the list again for this.
    // Without it the letter stays bold and HasNewMail keeps answering true
    // until something else refreshes the inbox - which, for a player who reads
    // their mail and walks away, is never.
    //
    // The flag is set *before* the event, and that order is load-bearing:
    // MailFrame answers MAIL_INBOX_UPDATE with OpenMail_Update, which asks
    // GetInboxText, which is what marks a letter read. Setting the flag first
    // makes the second pass find it already read and stop. Firing first would
    // recur until the stack ran out.
    bool marked = false;
    for (auto& mail : mailInbox_) {
        if (mail.messageId != mailId || mail.read) continue;
        mail.read = true;
        marked = true;
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
        break;
    }

    // And the envelope on the minimap, which is a different event and was
    // never fired. MAIL_INBOX_UPDATE redraws the list of letters;
    // MiniMapMailFrame does not listen to it and answers UPDATE_PENDING_MAIL
    // alone. So the last unread letter could be read with the list in front of
    // the player and the notification stayed up - nothing had said the thing
    // it was reporting had stopped being true.
    if (marked) {
        bool anyUnread = false;
        for (const auto& mail : mailInbox_) {
            if (!mail.read) { anyUnread = true; break; }
        }
        if (anyUnread != hasNewMail_) setHasNewMail(anyUnread);
    }
}

void InventoryHandler::handleShowMailbox(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    mailboxGuid_ = packet.readUInt64();
    mailboxOpen_ = true;
    selectedMailIndex_ = -1;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MAIL_SHOW", {});
    selectDefaultStationery();
    refreshMailList();
}

void InventoryHandler::handleMailListResult(network::Packet& packet) {
    if (!owner_.getPacketParsers()->parseMailList(packet, mailInbox_)) return;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
    for (const auto& mail : mailInbox_) {
        // Player mail carries a GUID, not a name. Ask for it once here; the UI
        // reads the name through GameHandler::getMailSenderName, so it appears
        // as soon as the query comes back.
        if (mail.messageType == 0 && mail.senderGuid != 0 &&
            owner_.lookupName(mail.senderGuid).empty()) {
            owner_.queryPlayerName(mail.senderGuid);
        }
        if (mail.messageType == 2) {
            AuctionMailSubject auction;
            if (parseAuctionMailSubject(mail.subject, auction)) {
                owner_.ensureItemInfo(auction.itemEntry);
            }
        }
        for (const auto& att : mail.attachments) {
            if (att.itemId != 0) owner_.ensureItemInfo(att.itemId);
        }
    }
}

void InventoryHandler::handleSendMailResult(network::Packet& packet) {
    if (!packet.hasRemaining(12)) return;
    uint32_t mailId = packet.readUInt32();
    uint32_t action = packet.readUInt32();
    uint32_t error  = packet.readUInt32();
    // A refusal is worded as Wow.exe's handler (0x5717B0) words it - the
    // only line a success gets is ERR_MAIL_SENT - and every result ends with
    // MAIL_SUCCESS or MAIL_FAILED.
    if (error != 0) {
        struct Refusal { uint32_t code; const char* key; const char* text; };
        static constexpr Refusal kRefusals[] = {
            {2,  "ERR_MAIL_TO_SELF", "You can't send mail to yourself."},
            {3,  "ERR_NOT_ENOUGH_MONEY", "You don't have enough money."},
            {4,  "ERR_MAIL_TARGET_NOT_FOUND", "Cannot find mail recipient."},
            {5,  "ERR_PLAYER_WRONG_FACTION", "Target is unfriendly."},
            {14, "ERR_RESTRICTED_ACCOUNT", "Trial accounts cannot perform that action"},
            {15, "ERR_MAIL_REACHED_CAP", "Recipient's mailbox is full."},
            {16, "ERR_MAIL_WRAPPED_COD", "You can't send a wrapped item COD."},
            {17, "ERR_USER_SQUELCHED", "Your chat privileges have been suspended."},
            {18, "ERR_MAIL_TOO_MANY_ATTACHMENTS", "Too many attachments."},
            {19, "ERR_MAIL_INVALID_ATTACHMENT", "Invalid attachment."},
            {21, "ERR_MAIL_ATTACHMENT_EXPIRED", "The attachment has expired."},
        };
        if (error == 1) {
            const char* key = packet.hasRemaining(4) ? inventoryResultKey(packet.readUInt32()) : "ERR_BAG_FULL";
            if (key) owner_.displayGameMessage(key, stockGlobal(owner_, key));
        } else if (error != 20) {
            const Refusal* hit = nullptr;
            for (const auto& r : kRefusals) if (r.code == error) hit = &r;
            if (hit) owner_.displayGameMessage(hit->key, hit->text);
            else owner_.displayGameMessage("ERR_MAIL_DATABASE_ERROR", "Internal mail database error.");
        }
    }
    // WotLK MailResponseType values from SharedDefines.h.
    constexpr uint32_t MAIL_SEND = 0;
    constexpr uint32_t MAIL_MONEY_TAKEN = 1;
    constexpr uint32_t MAIL_ITEM_TAKEN = 2;
    constexpr uint32_t MAIL_RETURNED_TO_SENDER = 3;
    constexpr uint32_t MAIL_DELETED = 4;
    constexpr uint32_t MAIL_MADE_PERMANENT = 5;

    auto mailIt = std::find_if(mailInbox_.begin(), mailInbox_.end(),
        [mailId](const MailMessage& mail) { return mail.messageId == mailId; });

    if (action == MAIL_SEND) {
        if (error == 0) {
            owner_.displayGameMessage("ERR_MAIL_SENT", "Mail sent.");   // info text
            clearMailAttachments();
            // The frame stays where it is - WoW resets its fields and leaves it
            // open. This used to close this client's own compose window here,
            // and that flag now means "the interface's send tab is showing",
            // which is what decides whether clicking an item in a bag attaches
            // it. Clearing it made the first letter of a session the last one
            // that could have anything attached to it.
            // The send frame waits on these before it will clear its fields and
            // re-enable the Send button. Without them a mail went out and the
            // frame sat there as though it had not.
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("MAIL_SEND_SUCCESS", {});
            }
        }
    } else if (action == MAIL_ITEM_TAKEN) {
        if (error == 0 && owner_.addonEventCallbackRef()) fireBagUpdates();
    } else if (action == MAIL_MONEY_TAKEN) {
        if (error == 0) {
            if (mailIt != mailInbox_.end()) mailIt->money = 0;
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("PLAYER_MONEY", {});
                owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
            }
        }
    } else if (action == MAIL_DELETED || action == MAIL_RETURNED_TO_SENDER) {
        if (error == 0) {
            if (mailIt != mailInbox_.end()) {
                const int erasedIndex = static_cast<int>(std::distance(mailInbox_.begin(), mailIt));
                mailInbox_.erase(mailIt);
                if (selectedMailIndex_ == erasedIndex) selectedMailIndex_ = -1;
                else if (selectedMailIndex_ > erasedIndex) --selectedMailIndex_;
            }
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
        }
    }
    (void)MAIL_MADE_PERMANENT;
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()(error == 0 ? "MAIL_SUCCESS" : "MAIL_FAILED", {});
    refreshMailList();
}

void InventoryHandler::handleReceivedMail(network::Packet& packet) {
    (void)packet;
    setHasNewMail(true);
    // Ask who it is from, so the envelope's tooltip can name the sender.
    queryNextMailTime();
}

void InventoryHandler::queryNextMailTime() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = QueryNextMailTimePacket::build();
    owner_.getSocket()->send(packet);
}

void InventoryHandler::handleQueryNextMailTime(network::Packet& packet) {
    // The senders are kept for GetLatestThreeSenders, which is how the
    // minimap envelope's tooltip says who the mail is from away from a
    // mailbox (MinimapMailFrameUpdate). The list layout is the TBC/WotLK one;
    // classic-like realms only get the two leading fields read.
    NextMailTimeData data;
    const bool ok = QueryNextMailTimePacket::parse(packet, data, !isClassicLikeExpansion());
    packet.skipAll();
    if (!ok) return;
    nextMailSenders_ = std::move(data.senders);
    for (const auto& sender : nextMailSenders_) {
        if (sender.messageType == 0 && sender.senderGuid != 0 &&
            owner_.lookupName(sender.senderGuid).empty()) {
            owner_.queryPlayerName(sender.senderGuid);
        }
    }
    setHasNewMail(data.nextTime >= 0.0f && data.count > 0);
}

void InventoryHandler::setHasNewMail(bool value) {
    // Announce a chat line only on the rising edge (no unread -> unread), so the
    // periodic next-mail-time poll doesn't repeat the notification while mail sits unread.
    // No sound: the stock client plays none for new mail (nothing in Wow.exe's
    // own sound calls, nothing on UPDATE_PENDING_MAIL in FrameXML), and the
    // one here was iTellMessage.wav - a whisper's sound.
    // No chat line either: the stock client has no "new mail" string and
    // shows new mail only as the minimap envelope (MiniMapMailFrame on
    // UPDATE_PENDING_MAIL).
    hasNewMail_ = value;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("UPDATE_PENDING_MAIL", {});
}

// ============================================================
// Bank
// ============================================================

void InventoryHandler::openBank(uint64_t guid) {
    bankerGuid_ = guid;
    bankOpen_ = true;
    if (isClassicLikeExpansion()) {
        effectiveBankSlots_ = 24;
        effectiveBankBagSlots_ = 6;
    } else {
        effectiveBankSlots_ = 28;
        effectiveBankBagSlots_ = 7;
    }
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("BANKFRAME_OPENED", {});
}

void InventoryHandler::closeBank() {
    bankOpen_ = false;
    bankerGuid_ = 0;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("BANKFRAME_CLOSED", {});
}

void InventoryHandler::buyBankSlot() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || bankerGuid_ == 0) return;
    auto packet = BuyBankSlotPacket::build(bankerGuid_);
    owner_.getSocket()->send(packet);
}

uint32_t InventoryHandler::getBankBagSlotPrice(int slotIndex) const {
    // Read from BankBagSlotPrices.dbc rather than written out: AzerothCore's
    // BuyBankSlot charges sBankBagSlotPricesStore.LookupEntry(slot) for slot
    // = owned + 1, and the two tables this client carried (here and in
    // GetBankSlotCost) disagreed with each other and with the file - 3.3.5's
    // is 10s, 1g, 10g, 25g, 25g, 25g, 25g.
    if (slotIndex < 0 || slotIndex >= Inventory::BANK_BAG_SLOTS) return 0;
    if (!bankBagSlotPricesLoaded_) {
        auto* am = owner_.services().assetManager;
        // Not an attempt until the assets are there; see loadRepairDbc.
        if (!am || !am->isInitialized()) return 0;
        bankBagSlotPricesLoaded_ = true;
        auto dbc = am->loadDBC("BankBagSlotPrices.dbc");
        if (dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                bankBagSlotPrices_[dbc->getUInt32(i, 0)] = dbc->getUInt32(i, 1);
            }
        }
    }
    auto it = bankBagSlotPrices_.find(static_cast<uint32_t>(slotIndex + 1));
    return it == bankBagSlotPrices_.end() ? 0 : it->second;
}

void InventoryHandler::depositItem(uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // CMSG_AUTOBANK_ITEM lets the server place the item into the first free slot
    // across the whole bank - the main slots AND the purchased bank bags. The
    // old code scanned only the main bank slots and reported "Bank is full" the
    // moment those filled, ignoring free space in the bank bags. The server
    // replies with a bank-full error if there is genuinely no room.
    auto packet = AutoBankItemPacket::build(srcBag, srcSlot);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::withdrawItem(uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // CMSG_AUTOSTORE_BANK_ITEM lets the server place the item into the first
    // free slot across ALL bags (retail right-click-to-withdraw), not just the
    // backpack. The server replies with an inventory-full error if there's no
    // room, so no client-side capacity check is needed here.
    auto packet = AutoStoreBankItemPacket::build(srcBag, srcSlot);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::handleShowBank(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    uint64_t guid = packet.readUInt64();
    openBank(guid);
}

void InventoryHandler::handleBuyBankSlotResult(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    // AzerothCore's BankBagSlotResult: 0 too many, 1 not enough money,
    // 2 not at a banker, 3 bought. Wow.exe (0x6E2E90, table 0xA32618) says
    // ERR_BANKSLOT_* for the three refusals and nothing for a purchase. This
    // read 0 as the purchase.
    uint32_t result = packet.readUInt32();
    static const char* const kKeys[] = {"ERR_BANKSLOT_FAILED_TOO_MANY", "ERR_BANKSLOT_INSUFFICIENT_FUNDS",
                                        "ERR_BANKSLOT_NOTBANKER"};
    if (result < 3) {
        owner_.displayGameMessage(kKeys[result], stockGlobal(owner_, kKeys[result]));
    } else if (result == 3) {
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYERBANKBAGSLOTS_CHANGED", {});
    }
}

// ============================================================
// Guild Bank
// ============================================================

void InventoryHandler::openGuildBank(uint64_t guid) {
    // A vault belongs to a guild, and a player in none has nothing to open. The
    // server answers CMSG_GUILD_BANKER_ACTIVATE from a guildless player with an
    // error and sends no bank list, so the window this raised at the moment the
    // object was clicked had nothing behind it and never would: tabs to click,
    // no contents, and no way to tell that from a vault that had not loaded yet.
    if (!owner_.isInGuild()) {
        owner_.raiseUiError("You are not in a guild.");
        return;
    }
    guildBankerGuid_ = guid;
    guildBankActiveTab_ = 0;
    // The window opens when the vault answers, not when it is asked.
    //
    // Raising it here meant any refusal - a guild the client thought the player
    // was in and the server did not, a banker out of range, a permission the
    // rank does not carry - left an empty vault on screen with nothing coming.
    // handleGuildBankList shows it the moment a tab list arrives, which is the
    // server agreeing that this player may look.
    // CMSG_GUILD_BANKER_ACTIVATE registers this session with the guild banker so
    // the server sends the tab list + contents (SMSG_GUILD_BANK_LIST). Without it
    // the follow-up query-tab is rejected and no bank list ever arrives, so the
    // window opened empty (or not at all).
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket() && guid != 0) {
        auto activate = GuildBankerActivatePacket::build(guid);
        owner_.getSocket()->send(activate);
    }
    queryGuildBankTab(0);
    // The gold allowance is not in the bank list: AzerothCore sends it only
    // in answer to MSG_GUILD_BANK_MONEY_WITHDRAWN (Guild::SendMoneyInfo).
    queryGuildBankMoneyWithdrawn();
}

void InventoryHandler::queryGuildBankMoneyWithdrawn() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet p(wireOpcode(Opcode::MSG_GUILD_BANK_MONEY_WITHDRAWN));
    owner_.getSocket()->send(p);
}

void InventoryHandler::setGuildBankLockedSlot(uint8_t tab, uint8_t slot) {
    if (tab == guildBankLockedTab_ && (tab == 0xFF || slot == guildBankLockedSlot_)) return;
    guildBankLockedTab_ = tab;
    guildBankLockedSlot_ = slot;
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILDBANK_ITEM_LOCK_CHANGED", {});
}

void InventoryHandler::guildBankMoveItem(uint8_t srcTab, uint8_t srcSlot, uint8_t dstTab,
                                         uint8_t dstSlot, uint32_t splitCount) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    if (srcTab == dstTab && srcSlot == dstSlot) return;   // Guild::SwapItems ignores it too
    auto entryAt = [this](uint8_t tab, uint8_t slot) -> uint32_t {
        if (tab >= guildBankData_.tabs.size()) return 0;
        for (const auto& it : guildBankData_.tabs[tab].items)
            if (it.slotId == slot) return it.itemEntry;
        return 0;
    };
    auto packet = GuildBankSwapItemsPacket::buildBankToBank(
        guildBankerGuid_, dstTab, dstSlot, entryAt(dstTab, dstSlot),
        srcTab, srcSlot, entryAt(srcTab, srcSlot), splitCount);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::closeGuildBank() {
    guildBankOpen_ = false;
    guildBankerGuid_ = 0;
    setGuildBankLockedSlot(0xFF, 0);
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GUILDBANKFRAME_CLOSED", {});
}

void InventoryHandler::queryGuildBankTab(uint8_t tabId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    // Asking for the full tab, not a slot refresh. The byte reaches
    // SendBankList as "with content" on the cores that read it, so a false here
    // asked the server for a tab and told it not to send what is in it. This
    // client keeps no cached tab to refresh, so there is nothing a partial
    // update could be for.
    auto packet = GuildBankQueryTabPacket::build(guildBankerGuid_, tabId, true);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::buyGuildBankTab() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    uint8_t nextTab = static_cast<uint8_t>(guildBankData_.tabs.size());
    auto packet = GuildBankBuyTabPacket::build(guildBankerGuid_, nextTab);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::depositGuildBankMoney(uint32_t amount) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    auto packet = GuildBankDepositMoneyPacket::build(guildBankerGuid_, amount);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::withdrawGuildBankMoney(uint32_t amount) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    auto packet = GuildBankWithdrawMoneyPacket::build(guildBankerGuid_, amount);
    owner_.getSocket()->send(packet);
    // What is left of today's allowance changed, and nothing reports it
    // unasked.
    queryGuildBankMoneyWithdrawn();
}

void InventoryHandler::guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag,
                                             uint8_t destSlot, uint32_t splitCount) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    auto packet = GuildBankSwapItemsPacket::buildBankToInventory(guildBankerGuid_, tabId, bankSlot,
                                                                 destBag, destSlot, splitCount);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::queryGuildBankText(uint8_t tabId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet p(wireOpcode(Opcode::MSG_QUERY_GUILD_BANK_TEXT));
    p.writeUInt8(tabId);
    owner_.getSocket()->send(p);
}

const std::string& InventoryHandler::getGuildBankTabText(uint8_t tabId) const {
    static const std::string kNone;
    return tabId < guildBankTabText_.size() ? guildBankTabText_[tabId] : kNone;
}

void InventoryHandler::setGuildBankTabInfo(uint8_t tabId, const std::string& name,
                                           const std::string& icon) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guildBankerGuid_ == 0) return;
    // Both or neither: the server tests them before it does anything, so a
    // half-filled request is a packet that changes nothing and says nothing.
    if (name.empty() || icon.empty()) return;
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_UPDATE_TAB));
    p.writeUInt64(guildBankerGuid_);
    p.writeUInt8(tabId);
    p.writeString(name);
    p.writeString(icon);
    owner_.getSocket()->send(p);
}

void InventoryHandler::guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || guildBankerGuid_ == 0) return;
    auto packet = GuildBankSwapItemsPacket::buildInventoryToBank(guildBankerGuid_, tabId, bankSlot, srcBag, srcSlot);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::guildBankDepositFromInventory(uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() ||
        guildBankerGuid_ == 0 || !guildBankOpen_) return;
    // CMSG_GUILD_BANK_SWAP_ITEMS has no server-side auto-store for the deposit
    // direction (that path forces bank→character), so the client picks the
    // target slot - the first empty one in the tab the player is viewing, which
    // is what the retail client does on a right-click deposit.
    constexpr int kTabSlots = 98; // GUILD_BANK_MAX_SLOTS
    std::array<bool, kTabSlots> occupied{};
    if (guildBankActiveTab_ < guildBankData_.tabs.size()) {
        for (const auto& slot : guildBankData_.tabs[guildBankActiveTab_].items) {
            if (slot.itemEntry != 0 && slot.slotId < kTabSlots)
                occupied[slot.slotId] = true;
        }
    }
    int freeSlot = -1;
    for (int s = 0; s < kTabSlots; ++s) {
        if (!occupied[s]) { freeSlot = s; break; }
    }
    if (freeSlot < 0) {
        owner_.displayGameMessage("ERR_GUILD_BANK_FULL", "This guild bank tab is full");
        return;
    }
    guildBankDepositItem(guildBankActiveTab_, static_cast<uint8_t>(freeSlot), srcBag, srcSlot);
}

void InventoryHandler::setGuildBankMoney(uint64_t money) {
    if (guildBankData_.money == money) return;
    guildBankData_.money = money;
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_MONEY", {});
}

void InventoryHandler::handleGuildBankList(network::Packet& packet) {
    if (!GuildBankListParser::parse(packet, guildBankData_)) return;
    // Receiving the bank list means the banker accepted us - make sure the
    // window is shown even if the open path didn't (e.g. a server-initiated
    // refresh, or the banker guid arrived only with the list).
    if (guildBankerGuid_ != 0 && !guildBankOpen_) {
        guildBankOpen_ = true;
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GUILDBANKFRAME_OPENED", {});
    }
    // The list says which tab it describes, not which one the player is
    // looking at. SetCurrentGuildBankTab sets that (GuildBankTab_OnClick calls
    // it before QueryGuildBankTab), and a move by another member in some
    // other tab is broadcast to everyone at the vault - following it switched
    // the tab under the player.
    //
    // A list naming the slot a pickup came from is the answer to the move
    // that pickup started (_SendBankContentUpdate sends both ends of a move),
    // so the slot is no longer held.
    if (guildBankLockedTab_ == guildBankData_.tabId) {
        for (const auto& slot : guildBankData_.tabItems) {
            if (slot.slotId != guildBankLockedSlot_) continue;
            setGuildBankLockedSlot(0xFF, 0);
            break;
        }
    }
    if (owner_.addonEventCallbackRef()) {
        // The three the panel listens for and this fired none of.
        //
        // GUILDBANK_UPDATE_TABS is the one that runs
        // GuildBankFrame_SelectAvailableTab, which is what picks a tab the
        // player may look in and draws it - without it the frame kept whatever
        // tab it had and never chose one. Only when a tab list actually
        // arrived: the event means the set of tabs changed, and a slot refresh
        // does not change it.
        if (guildBankData_.tabsIncluded && !guildBankData_.tabs.empty()) {
            owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_TABS", {});
        }
        owner_.addonEventCallbackRef()("GUILDBANKBAGSLOTS_CHANGED", {});
        // The money line and the withdraw allowance, which are read from
        // GetGuildBankMoney and GetGuildBankWithdrawMoney and were never asked
        // for again after the frame first drew.
        owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_MONEY", {});
        owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_WITHDRAWMONEY", {});
    }
    for (const auto& tab : guildBankData_.tabs) {
        for (const auto& item : tab.items) {
            if (item.itemEntry != 0) owner_.ensureItemInfo(item.itemEntry);
        }
    }
}

// ============================================================
// Auction House
// ============================================================

void InventoryHandler::openAuctionHouse(uint64_t guid) {
    auctioneerGuid_ = guid;
    auctionOpen_ = true;
    auctionActiveTab_ = 0;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("AUCTION_HOUSE_SHOW", {});
}

void InventoryHandler::closeAuctionHouse() {
    auctionOpen_ = false;
    auctioneerGuid_ = 0;
    auctionMultisell_ = AuctionMultisell{};   // the rest cannot be posted away from the auctioneer
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("AUCTION_HOUSE_CLOSED", {});
}

void InventoryHandler::auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                                      uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                                      uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset,
                                      const std::vector<AuctionSortKey>& sort) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    lastAuctionSearch_ = {.name = name, .levelMin = levelMin, .levelMax = levelMax, .quality = quality, .itemClass = itemClass, .itemSubClass = itemSubClass,
                          .invTypeMask = invTypeMask, .usableOnly = usableOnly, .offset = offset, .sort = sort};
    hasAuctionSearch_ = true;
    auto packet = AuctionListItemsPacket::build(auctioneerGuid_, offset, name,
                                                  levelMin, levelMax, invTypeMask,
                                                  itemClass, itemSubClass, quality, usableOnly, 0,
                                                  sort);
    owner_.getSocket()->send(packet);
    // Blocks the Search button until the answer comes back. The result carries
    // the server's own wait and replaces this with it, so five seconds is only
    // what applies while a reply is outstanding - and on Classic and TBC, whose
    // list result ends before that field.
    auctionSearchDelayTimer_ = 5.0f;
}

void InventoryHandler::auctionSellItem(int backpackIndex, uint32_t bid,
                                        uint32_t buyout, uint32_t duration) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    if (backpackIndex < 0 || backpackIndex >= owner_.inventoryRef().getBackpackSize()) return;
    const auto& slot = owner_.inventoryRef().getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    uint64_t itemGuid = owner_.backpackSlotGuidsRef()[backpackIndex];
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }
    if (itemGuid == 0) {
        LOG_ERROR("auctionSellItem: could not resolve GUID for backpack slot ", backpackIndex);
        return;
    }

    uint32_t stackCount = slot.item.stackCount;
    auto packet = AuctionSellItemPacket::build(auctioneerGuid_, itemGuid, stackCount, bid, buyout, duration,
                                                isPreWotlk());
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionSellItemByGuid(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                                             uint32_t buyout, uint32_t duration) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    if (itemGuid == 0) {
        LOG_ERROR("auctionSellItemByGuid: refusing to post with a null item GUID");
        return;
    }
    if (stackCount == 0) stackCount = 1;
    auto packet = AuctionSellItemPacket::build(auctioneerGuid_, itemGuid, stackCount, bid, buyout, duration,
                                                isPreWotlk());
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionPlaceBid(uint32_t auctionId, uint32_t amount) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    auto packet = AuctionPlaceBidPacket::build(auctioneerGuid_, auctionId, amount);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    auto packet = AuctionPlaceBidPacket::build(auctioneerGuid_, auctionId, buyoutPrice);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionCancelItem(uint32_t auctionId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    auto packet = AuctionRemoveItemPacket::build(auctioneerGuid_, auctionId);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionListOwnerItems(uint32_t offset) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    auto packet = AuctionListOwnerItemsPacket::build(auctioneerGuid_, offset);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::auctionListBidderItems(uint32_t offset) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    auto packet = AuctionListBidderItemsPacket::build(auctioneerGuid_, offset);
    owner_.getSocket()->send(packet);
}

void InventoryHandler::handleAuctionHello(network::Packet& packet) {
    // Through the parser and the opener rather than reading and setting inline.
    // Both existed and neither had a caller: this read the same two fields a
    // second time and set the same state a second time, so the packet's layout
    // was written down twice and openAuctionHouse was dead code that looked
    // live. The parser also reads the trailing enabled byte, which is in the
    // WotLK packet and not the vanilla one - this did not, and would have
    // needed the same expansion difference written a second time to.
    AuctionHelloData data;
    if (!AuctionHelloParser::parse(packet, data)) return;
    openAuctionHouse(data.auctioneerGuid);
    owner_.closeGossip();
}

// Each list answer has its own opcode and goes to its own list. They used to
// share one "which list was asked for last" flag, so a browse search and an
// owner refresh in flight together - which is what posting or cancelling
// while a search is outstanding does - put one list's rows in the other.
void InventoryHandler::handleAuctionListResult(network::Packet& packet) {
    storeAuctionListResult(packet, AuctionResultTarget::BROWSE);
}

void InventoryHandler::onPlayerNameKnown(uint64_t guid) {
    if (guid == 0 || !owner_.addonEventCallbackRef()) return;
    // An invoice in the open inbox naming them (Wow.exe 0x56D4B0).
    if (mailboxOpen_) {
        for (const auto& m : mailInbox_) {
            AuctionMailInvoice invoice;
            if (m.messageType == 2 && parseAuctionMailBody(m.body, invoice) &&
                invoice.ownerGuidLow == static_cast<uint32_t>(guid)) {
                owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
                break;
            }
        }
    }
    if (!auctionOpen_) return;
    auto names = [guid](const AuctionListResult& r, bool bidders) {
        for (const auto& e : r.auctions) {
            if (e.ownerGuid == guid || (bidders && e.bidderGuid == guid)) return true;
        }
        return false;
    };
    if (names(auctionBrowseResults_, false))
        owner_.addonEventCallbackRef()("AUCTION_ITEM_LIST_UPDATE", {});
    if (names(auctionOwnerResults_, true))
        owner_.addonEventCallbackRef()("AUCTION_OWNED_LIST_UPDATE", {});
    if (names(auctionBidderResults_, false))
        owner_.addonEventCallbackRef()("AUCTION_BIDDER_LIST_UPDATE", {});
}

void InventoryHandler::storeAuctionListResult(network::Packet& packet, AuctionResultTarget target) {
    AuctionListResult result;
    // Vanilla sends a single enchantId; TBC inspects 6 enchant slots per item,
    // WotLK 7 (PRISMATIC_ENCHANTMENT_SLOT joined the inspected range in 3.x).
    const int enchantSlots = isClassicLikeExpansion() ? 1 : (isPreWotlk() ? 6 : 7);
    if (!AuctionListResultParser::parse(packet, result, enchantSlots)) return;

    // How long before another search may be sent - the server's own figure,
    // which every list result carries and which was parsed and then dropped.
    // AzerothCore's AUCTION_SEARCH_DELAY is 300, in milliseconds; the guess it
    // replaces was five whole seconds, so the Search button sat dead for about
    // seventeen times longer than the server ever asked for, and paging
    // through results crawled.
    //
    // A zero means the field was not sent at all rather than "no wait" -
    // Classic and TBC end the packet before it - so the old guess stays as the
    // floor for those, where an unthrottled client is what the server would
    // have to defend itself against.
    if (result.searchDelay > 0) {
        auctionSearchDelayTimer_ = static_cast<float>(result.searchDelay) / 1000.0f;
    }

    if (target == AuctionResultTarget::OWNER) {
        auctionOwnerResults_ = std::move(result);
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("AUCTION_OWNED_LIST_UPDATE", {});
    } else if (target == AuctionResultTarget::BIDDER) {
        auctionBidderResults_ = std::move(result);
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("AUCTION_BIDDER_LIST_UPDATE", {});
    } else {
        auctionBrowseResults_ = std::move(result);
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("AUCTION_ITEM_LIST_UPDATE", {});
    }

    // Ensure item info for all entries
    auto ensureEntries = [this, target](const AuctionListResult& r) {
        for (const auto& e : r.auctions) {
            owner_.ensureItemInfo(e.itemEntry);
            if (e.ownerGuid != 0) owner_.queryPlayerName(e.ownerGuid);
            // The Auctions tab names who holds the bid on each of yours.
            if (target == AuctionResultTarget::OWNER && e.bidderGuid != 0)
                owner_.queryPlayerName(e.bidderGuid);
        }
    };
    if (target == AuctionResultTarget::OWNER) ensureEntries(auctionOwnerResults_);
    else if (target == AuctionResultTarget::BIDDER) ensureEntries(auctionBidderResults_);
    else ensureEntries(auctionBrowseResults_);
}

void InventoryHandler::handleAuctionOwnerListResult(network::Packet& packet) {
    storeAuctionListResult(packet, AuctionResultTarget::OWNER);
}

void InventoryHandler::handleAuctionBidderListResult(network::Packet& packet) {
    storeAuctionListResult(packet, AuctionResultTarget::BIDDER);
}

void InventoryHandler::handleAuctionCommandResult(network::Packet& packet) {
    if (!packet.hasRemaining(12)) return;
    uint32_t auctionId = packet.readUInt32();
    uint32_t action = packet.readUInt32();
    uint32_t error  = packet.readUInt32();
    (void)auctionId;
    constexpr uint32_t kActionSell = 0, kActionCancel = 1, kActionBid = 2;
    constexpr uint32_t kErrInventory = 1;

    // The words are the stock GlobalStrings for AzerothCore's AuctionError
    // codes (auctionCommandResultKey). The table here had them shifted -
    // 1 read "not enough money" where 3 is, 2 "item not found" where 4 is,
    // 7 "own auction" where 10 is - so most refusals said the wrong thing.
    const char* key = auctionCommandResultKey(action, error);
    std::string text = key ? stockGlobal(owner_, key) : std::string();
    if (text.empty() && key) {
        static const std::unordered_map<std::string, const char*> kEnglish = {
            {"ERR_AUCTION_STARTED", "Auction created."},
            {"ERR_AUCTION_REMOVED", "Auction cancelled."},
            {"ERR_AUCTION_BID_PLACED", "Bid accepted."},
            {"ERR_NOT_ENOUGH_MONEY", "You don't have enough money."},
            {"ERR_ITEM_NOT_FOUND", "The item was not found."},
            {"ERR_AUCTION_HIGHER_BID", "There is already a higher bid on that item."},
            {"ERR_AUCTION_BID_INCREMENT", "Your bid increment is too small."},
            {"ERR_AUCTION_BID_OWN", "You cannot bid on your own auction."},
            {"ERR_RESTRICTED_ACCOUNT", "Trial accounts cannot perform that action"},
            {"ERR_AUCTION_DATABASE_ERROR", "Internal auction error."},
        };
        if (auto it = kEnglish.find(key); it != kEnglish.end()) text = it->second;
    }

    // Said as Wow.exe's handler (0x59FFB0) says them: the successes are chat
    // lines, the refusals red errors, each with its GameError's sound.
    if (error == 0) {
        if (key) owner_.displayGameMessage(key, text);
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_MONEY", {});
            fireBagUpdates();
        }
        // The lists the change shows up in, asked for again: the server
        // pushes none of them. A posted or cancelled auction is on the
        // Auctions tab; a bid is on the Bids tab and changes the row in the
        // browse list. Without this the tabs showed what they held before.
        if (action == kActionSell || action == kActionCancel) {
            auctionListOwnerItems(0);
        } else if (action == kActionBid) {
            auctionListBidderItems(0);
            // Previously gated on name.length()>0 which skipped browse-all (empty name).
            if (hasAuctionSearch_) {
                auctionSearch(lastAuctionSearch_.name, lastAuctionSearch_.levelMin, lastAuctionSearch_.levelMax,
                             lastAuctionSearch_.quality, lastAuctionSearch_.itemClass, lastAuctionSearch_.itemSubClass,
                             lastAuctionSearch_.invTypeMask, lastAuctionSearch_.usableOnly,
                             lastAuctionSearch_.offset, lastAuctionSearch_.sort);
            }
        }
        if (action == kActionSell && auctionMultisell_.active()) {
            ++auctionMultisell_.done;
            if (auctionMultisell_.announce && owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("AUCTION_MULTISELL_UPDATE",
                    {std::to_string(auctionMultisell_.done), std::to_string(auctionMultisell_.total)});
            }
            if (auctionMultisell_.pending.empty()) auctionMultisell_ = AuctionMultisell{};
            else sendNextAuctionPost();
        }
        return;
    }

    // An inventory refusal carries its InventoryResult when the server sends
    // one; that is the reason to show.
    if (error == kErrInventory) {
        key = packet.hasRemaining(4) ? inventoryResultKey(packet.readUInt32()) : "ERR_BAG_FULL";
        text = key ? stockGlobal(owner_, key) : std::string();
    }
    if (key) owner_.displayGameMessage(key, text);
    if (action == kActionSell && auctionMultisell_.active()) {
        // The rest are not sent: the sell tab hides its progress frame on
        // this and the player is left with what did post.
        if (auctionMultisell_.announce && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("AUCTION_MULTISELL_FAILURE", {});
        }
        auctionMultisell_ = AuctionMultisell{};
    }
}

void InventoryHandler::auctionPostStacks(uint64_t firstGuid, uint32_t itemId, uint32_t stackSize,
                                         uint32_t numStacks, uint32_t bid, uint32_t buyout,
                                         uint32_t duration) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) return;
    if (firstGuid == 0 || itemId == 0) return;
    if (stackSize == 0) stackSize = 1;
    if (numStacks == 0) numStacks = 1;
    if (isPreWotlk()) {
        // Classic and TBC post one whole item per packet, no counts.
        auctionSellItemByGuid(firstGuid, stackSize, bid, buyout, duration);
        return;
    }
    // Every stack of the item the player carries, the one dropped on the sell
    // slot first. One stack used to be all an auction could draw on, so a
    // stack size larger than the clicked stack, or a second auction, could
    // not be posted at all.
    std::vector<AuctionSellSource> sources;
    auto& inv = owner_.inventoryRef();
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& s = inv.getBackpackSlot(i);
        if (s.empty() || s.item.itemId != itemId) continue;
        const uint64_t g = owner_.backpackSlotGuidsRef()[i];
        if (g) sources.push_back({g, s.item.stackCount ? s.item.stackCount : 1});
    }
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b) {
        for (int s = 0; s < inv.getBagSize(b); ++s) {
            const auto& slot = inv.getBagSlot(b, s);
            if (slot.empty() || slot.item.itemId != itemId) continue;
            const uint64_t g = owner_.getBagItemGuid(b, s);
            if (g) sources.push_back({g, slot.item.stackCount ? slot.item.stackCount : 1});
        }
    }
    std::stable_partition(sources.begin(), sources.end(),
                          [firstGuid](const AuctionSellSource& s) { return s.guid == firstGuid; });
    auto plan = planAuctionStacks(sources, stackSize, numStacks);
    if (plan.empty()) {
        owner_.addUIError(stockGlobal(owner_, "ERR_AUCTION_ENOUGH_ITEMS"));
        return;
    }
    auctionMultisell_ = AuctionMultisell{};
    auctionMultisell_.total = static_cast<uint32_t>(plan.size());
    auctionMultisell_.pending = std::move(plan);
    auctionMultisell_.bid = bid;
    auctionMultisell_.buyout = buyout;
    auctionMultisell_.duration = duration;
    auctionMultisell_.announce = numStacks > 1;
    if (auctionMultisell_.announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("AUCTION_MULTISELL_START",
                                       {std::to_string(auctionMultisell_.total)});
    }
    sendNextAuctionPost();
}

void InventoryHandler::sendNextAuctionPost() {
    if (auctionMultisell_.pending.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || auctioneerGuid_ == 0) {
        auctionMultisell_ = AuctionMultisell{};
        return;
    }
    auto pieces = std::move(auctionMultisell_.pending.front());
    auctionMultisell_.pending.erase(auctionMultisell_.pending.begin());
    auto packet = AuctionSellItemPacket::build(auctioneerGuid_, pieces, auctionMultisell_.bid,
                                               auctionMultisell_.buyout, auctionMultisell_.duration);
    owner_.getSocket()->send(packet);
}

// ============================================================
// Item Text
// ============================================================

// A letter copied out of the mail: no page on its template, its words on the
// item (ITEM_FIELD_FLAGS 0x200, set by AzerothCore's HandleMailCreateTextItem).
// Wow.exe 0x58A1A0 reads those through the item text cache - CMSG_ITEM_TEXT_
// QUERY with the item's guid - and sends no CMSG_READ_ITEM for it.
bool InventoryHandler::readItemText(uint64_t itemGuid, const ItemQueryResponseData* info) {
    constexpr uint32_t kItemFieldFlagReadable = 0x200u;
    if (itemGuid == 0 || (owner_.getItemFieldFlags(itemGuid) & kItemFieldFlagReadable) == 0)
        return false;
    owner_.bookPagesRef().clear();
    owner_.setBookTitle(info ? info->name : std::string());
    owner_.setBookMaterial(info ? info->pageMaterial : 0u);
    itemTextGuid_ = itemGuid;
    queryItemText(itemGuid);
    return true;
}

void InventoryHandler::queryItemText(uint64_t itemGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_ITEM_TEXT_QUERY));
    pkt.writeUInt64(itemGuid);
    owner_.getSocket()->send(pkt);
    // The read has started. The frame answers this by clearing the page and
    // picking the material's text colour, before any words have arrived -
    // which is why it is a separate event from ITEM_TEXT_READY and not a
    // duplicate of it.
    owner_.fireAddonEvent("ITEM_TEXT_BEGIN", {});
}

void InventoryHandler::closeItemText() {
    itemTextGuid_ = 0;
    // A book is open too while its pages are held - those come by page text
    // and never set itemTextOpen_, so CloseItemText on a book announced
    // nothing and the frame stayed up (stock fires ITEM_TEXT_CLOSED for both).
    if (!itemTextOpen_ && owner_.bookPagesRef().empty()) return;
    itemTextOpen_ = false;
    // Before the event, so the OnHide that answers it finds nothing open.
    owner_.bookPagesRef().clear();
    // Announced so the interface's window closes with this client's own. Only
    // when it was open: the frame calls this from OnHide as well as from its
    // close button, and firing on an already-closed book would bounce.
    owner_.fireAddonEvent("ITEM_TEXT_CLOSED", {});
}

void InventoryHandler::handleItemTextQueryResponse(network::Packet& packet) {
    // uint8 0 then the item's guid and the text, or uint8 1 alone when the
    // server has no such item (AzerothCore WorldSession::HandleItemTextQuery).
    // This read the whole body as one string, so the leading zero ended it at
    // once and every letter opened blank.
    if (!packet.hasRemaining(1)) return;
    const uint8_t missing = packet.readUInt8();
    if (missing != 0 || !packet.hasRemaining(8)) return;
    /*itemGuid*/ packet.readUInt64();
    std::string text = packet.readString();
    if (!text.empty()) {
        itemText_ = std::move(text);
        itemTextOpen_ = true;
        // ITEM_TEXT_READY is what the reading frame answers by asking for the
        // page it has just been told about. Without it the text arrived, was
        // stored, and only this client's own window ever showed it.
        owner_.fireAddonEvent("ITEM_TEXT_READY", {});
    }
}

// ============================================================
// Trade
// ============================================================

void InventoryHandler::acceptTradeRequest() {
    if (tradeStatus_ != TradeStatus::PendingIncoming) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = BeginTradePacket::build();
    owner_.getSocket()->send(packet);
}

void InventoryHandler::declineTradeRequest() {
    if (tradeStatus_ != TradeStatus::PendingIncoming) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = CancelTradePacket::build();
    owner_.getSocket()->send(packet);
    resetTradeState();
}

/// TRADE_ACCEPT_UPDATE(playerState, targetState) - 1 for accepted, 0 for not.
///
/// TradeFrame_SetAcceptState reads both: the first decides whether the player's
/// half is highlighted and whether the trade button is enabled, the second does
/// the same for the target's. Fired with neither, both read nil, the highlights
/// stayed off and the trade button stayed enabled after accepting.
void InventoryHandler::fireTradeAcceptUpdate() {
    if (!owner_.addonEventCallbackRef()) return;
    owner_.addonEventCallbackRef()("TRADE_ACCEPT_UPDATE",
                                   {tradeSelfAccepted_ ? "1" : "0",
                                    tradePartnerAccepted_ ? "1" : "0"});
}

void InventoryHandler::acceptTrade() {
    // Open *or* Accepted. The status becomes Accepted the moment the partner
    // presses accept, and requiring Open meant that whoever pressed second
    // could not press at all - the trade sat with one acceptance in it and no
    // way to add the other.
    if (!isTradeOpen()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = AcceptTradePacket::build();
    owner_.getSocket()->send(packet);
    tradeSelfAccepted_ = true;
    fireTradeAcceptUpdate();
}

void InventoryHandler::unacceptTrade() {
    // Only where there is one of *ours* to take back. The status says the
    // trade has an acceptance in it, and TradeStatus::Accepted is set when the
    // partner accepts - gating on that let the player withdraw an acceptance
    // they had never made, and stopped them withdrawing one they had.
    if (!tradeSelfAccepted_) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = UnacceptTradePacket::build();
    owner_.getSocket()->send(packet);
    tradeSelfAccepted_ = false;
    fireTradeAcceptUpdate();
}

void InventoryHandler::cancelTrade() {
    if (tradeStatus_ == TradeStatus::None) return;
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = CancelTradePacket::build();
        owner_.getSocket()->send(packet);
    }
    resetTradeState();
}

// The offer can change while the window is open, accepted or not. These
// required Open, and the status turns Accepted when the partner presses
// accept - so after they did, nothing more could be put down or taken back.
// The server allows it: TradeData::SetItem and SetMoney clear both
// acceptances and the trade goes back to editing (TradeHandler.cpp).
// The player's own side of the trade is kept here, not learned from the
// server: AzerothCore's TradeData::SetItem and SetMoney call Update(), which
// sends SMSG_TRADE_STATUS_EXTENDED to the *trader* only (SendUpdateTrade(true)),
// so the stock client draws its own offer from what it put down. Waiting for
// the server left this player's trade slots and gold empty for the whole trade.
bool InventoryHandler::setTradeItem(uint8_t tradeSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (!isTradeOpen()) return false;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    auto packet = SetTradeItemPacket::build(tradeSlot, srcBag, srcSlot);
    owner_.getSocket()->send(packet);
    LOG_INFO("CMSG_SET_TRADE_ITEM slot=", (int)tradeSlot, " from ", (int)srcBag, "/", (int)srcSlot);
    if (tradeSlot < TRADE_SLOT_COUNT) {
        const ItemSlot* src = nullptr;
        const auto& inv = owner_.inventoryRef();
        if (srcBag == 0xFF && srcSlot >= 23 && srcSlot < 23 + inv.getBackpackSize())
            src = &inv.getBackpackSlot(srcSlot - 23);
        else if (srcBag >= 19 && srcBag <= 22 && srcSlot < inv.getBagSize(srcBag - 19))
            src = &inv.getBagSlot(srcBag - 19, srcSlot);
        auto& mine = myTradeSlots_[tradeSlot];
        mine = TradeSlot{};
        if (src && !src->empty()) {
            mine.itemId = src->item.itemId;
            mine.displayId = src->item.displayInfoId;
            mine.stackCount = src->item.stackCount;
        }
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("TRADE_PLAYER_ITEM_CHANGED", {std::to_string(tradeSlot + 1)});
            owner_.addonEventCallbackRef()("TRADE_UPDATE", {});
        }
    }
    return true;
}

bool InventoryHandler::clearTradeItem(uint8_t tradeSlot) {
    if (!isTradeOpen()) return false;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    auto packet = ClearTradeItemPacket::build(tradeSlot);
    owner_.getSocket()->send(packet);
    if (tradeSlot < TRADE_SLOT_COUNT) {
        myTradeSlots_[tradeSlot] = TradeSlot{};
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("TRADE_PLAYER_ITEM_CHANGED", {std::to_string(tradeSlot + 1)});
            owner_.addonEventCallbackRef()("TRADE_UPDATE", {});
        }
    }
    return true;
}

bool InventoryHandler::setTradeGold(uint64_t amount) {
    if (!isTradeOpen()) return false;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    auto packet = SetTradeGoldPacket::build(static_cast<uint32_t>(amount));
    owner_.getSocket()->send(packet);
    if (myTradeGold_ != amount) {
        myTradeGold_ = amount;
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_TRADE_MONEY", {});
    }
    return true;
}

void InventoryHandler::resetTradeState() {
    tradeStatus_ = TradeStatus::None;
    tradePeerGuid_ = 0;
    tradePeerName_.clear();
    myTradeSlots_ = {};
    peerTradeSlots_ = {};
    myTradeGold_ = 0;
    peerTradeGold_ = 0;
    myTradeSpell_ = 0;
    peerTradeSpell_ = 0;
    tradeSelfAccepted_ = false;
    tradePartnerAccepted_ = false;
}

void InventoryHandler::handleTradeStatus(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t status = packet.readUInt32();
    LOG_WARNING("SMSG_TRADE_STATUS: status=", status, " size=", packet.getSize());
    // A request that ends before it was answered takes its popup with it:
    // UIParent_OnEvent hides StaticPopup "TRADE" on TRADE_REQUEST_CANCEL
    // (uiparent.lua). Nothing fired it, so a request the other player
    // withdrew, or that died with distance or a logout, left a Yes/No up
    // that answered nothing.
    const bool wasPendingRequest = tradeStatus_ == TradeStatus::PendingIncoming;
    handleTradeStatusCode(status, packet);
    if (wasPendingRequest && tradeStatus_ == TradeStatus::None && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TRADE_REQUEST_CANCEL", {});
    }
}

void InventoryHandler::handleTradeStatusCode(uint32_t status, network::Packet& packet) {
    switch (status) {
        // The codes are AzerothCore's TradeStatus enum, checked against
        // SharedDefines.h rather than against the names that used to be here.
        // Two of those names were on the wrong cases and they were the two that
        // matter: 7 is BACK_TO_TRADE, the trade returning to editing because
        // somebody took their acceptance back, and 8 is TRADE_COMPLETE. Read
        // the other way round, every finished trade left the window open with
        // stale bags and no money update, and a partner un-accepting closed the
        // window announcing "Trade complete".
        // Each refusal is said as Wow.exe's trade frame says it (0x5873E0).
        case 0:   // BUSY
        case 5:   // BUSY_2
            if (!tradePeerName_.empty())
                owner_.displayGameMessage("ERR_PLAYER_BUSY_S", tradePeerName_ + " is busy right now.", {tradePeerName_});
            resetTradeState();
            break;
        case 1: { // BEGIN_TRADE - someone is asking
            if (packet.hasRemaining(8))
                tradePeerGuid_ = packet.readUInt64();
            tradeStatus_ = TradeStatus::PendingIncoming;
            // Resolve name
            auto nit = owner_.getPlayerNameCache().find(tradePeerGuid_);
            if (nit != owner_.getPlayerNameCache().end()) tradePeerName_ = nit->second;
            else tradePeerName_ = "Unknown";
            // Block Trades, which the panel offers and nothing read: every
            // request opened a window whatever it said. Refused before the
            // window rather than after, and said in chat, because a request
            // silently swallowed reads as the other player being ignored.
            // ERR_TRADE_BLOCKED_S, as the stock client does with BlockTrades
            // set; and someone ignored is turned away in silence.
            if (owner_.ignoreListGuidsRef().count(tradePeerGuid_)) {
                declineTradeRequest();
                break;
            }
            if (addons::storedCVarValue("blockTrades", "0") != "0") {
                declineTradeRequest();
                owner_.displayGameMessage("ERR_TRADE_BLOCKED_S", tradePeerName_ + " has requested to trade.  You have Block Trades turned on.",
                                          {tradePeerName_});
                break;
            }
            // ERR_TRADE_REQUEST_S: a system line ("%s has requested to trade
            // with you.") and its sound, LEVELUP. The TRADE popup FrameXML
            // shows for this has no sound of its own.
            owner_.displayGameMessage("ERR_TRADE_REQUEST_S",
                                      tradePeerName_ + " has requested to trade with you.",
                                      {tradePeerName_});
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRADE_REQUEST", {tradePeerName_});
            break;
        }
        case 2:   // OPEN_WINDOW
            tradeStatus_ = TradeStatus::Open;
            tradeSelfAccepted_ = false;
            tradePartnerAccepted_ = false;
            // A new trade starts empty. TRADE_COMPLETE keeps the last offer
            // for the closing update to read, and nothing cleared it after,
            // so the previous trade's items and gold showed in the next
            // window until each slot happened to be rewritten.
            myTradeSlots_ = {};
            peerTradeSlots_ = {};
            myTradeGold_ = 0;
            peerTradeGold_ = 0;
            myTradeSpell_ = 0;
            peerTradeSpell_ = 0;
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRADE_SHOW", {});
            break;
        case 3:   // TRADE_CANCELED
            resetTradeState();
            owner_.displayGameMessage("ERR_TRADE_CANCELLED", "Trade cancelled.");   // info text
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRADE_CLOSED", {});
            break;
        case 4:   // TRADE_ACCEPT - the other side pressed accept
            tradeStatus_ = TradeStatus::Accepted;
            tradePartnerAccepted_ = true;
            fireTradeAcceptUpdate();
            break;
        case 6:   // NO_TARGET
            resetTradeState();
            owner_.displayGameMessage("ERR_GENERIC_NO_TARGET", "You have no target.");
            break;
        case 7:   // BACK_TO_TRADE - an acceptance was taken back
            tradeStatus_ = TradeStatus::Open;
            tradeSelfAccepted_ = false;
            tradePartnerAccepted_ = false;
            fireTradeAcceptUpdate();
            break;
        case 8:   // TRADE_COMPLETE
            // Not reset immediately - SMSG_TRADE_STATUS_EXTENDED may arrive in
            // the same batch and needs the trade state to read the final items
            // and gold from.
            tradeStatus_ = TradeStatus::None;
            tradeSelfAccepted_ = false;
            tradePartnerAccepted_ = false;
            owner_.displayGameMessage("ERR_TRADE_COMPLETE", "Trade complete.");   // info text
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("TRADE_CLOSED", {});
                fireBagUpdates();
                owner_.addonEventCallbackRef()("PLAYER_MONEY", {});
            }
            break;
        case 9:   // TRADE_REJECTED
            // No line (0x586390): the trade window simply closes.
            resetTradeState();
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TRADE_CLOSED", {});
            break;
        case 10:  // TARGET_TO_FAR
            // Two places send it. On a request, the trade never began. On an
            // accept (SendUpdateTrade's distance check), the server only
            // withdraws our acceptance - the trade stays open and either side
            // may still close the gap and accept again.
            if (isTradeOpen()) {
                tradeSelfAccepted_ = false;
                fireTradeAcceptUpdate();
            } else {
                resetTradeState();
            }
            owner_.displayGameMessage("ERR_TRADE_TOO_FAR", stockGlobal(owner_, "ERR_TRADE_TOO_FAR"));
            break;
        case 11:  // WRONG_FACTION
            resetTradeState();
            owner_.displayGameMessage("ERR_PLAYER_WRONG_FACTION", "Target is unfriendly.");
            break;
        case 12: { // CLOSE_WINDOW - the trade could not complete
            // Not the end of the trade, despite the name. SendTradeStatus
            // writes uint32 InventoryResult, uint8 isTargetResult, uint32
            // the limit-category item, and SendUpdateTrade sends it when a
            // side cannot afford its gold or cannot carry what it would
            // receive - then clears both acceptances and leaves the window
            // open (TradeHandler.cpp). Closing here dropped the trade on the
            // client while the server still held it open.
            uint32_t result = 0;
            uint8_t isTarget = 0;
            if (packet.hasRemaining(4)) result = packet.readUInt32();
            if (packet.hasRemaining(1)) isTarget = packet.readUInt8();
            if (packet.hasRemaining(4)) packet.readUInt32();   // limit-category item
            constexpr uint32_t kBagFull = 4, kCantCarryMore = 17, kInventoryFull = 50;
            std::string text;
            if (result == kBagFull || result == kInventoryFull) {
                text = stockGlobal(owner_, isTarget ? "ERR_TRADE_TARGET_BAG_FULL" : "ERR_TRADE_BAG_FULL");
            } else if (result == kCantCarryMore) {
                text = stockGlobal(owner_, isTarget ? "ERR_TRADE_TARGET_MAX_COUNT_EXCEEDED"
                                                    : "ERR_TRADE_MAX_COUNT_EXCEEDED");
            }
            if (text.empty()) {
                if (const char* inv = inventoryResultKey(result)) text = stockGlobal(owner_, inv);
            }
            if (!text.empty()) owner_.addUIError(text);
            tradeSelfAccepted_ = false;
            tradePartnerAccepted_ = false;
            if (isTradeOpen()) tradeStatus_ = TradeStatus::Open;
            fireTradeAcceptUpdate();
            break;
        }
        case 14:  // IGNORE_YOU
            if (!tradePeerName_.empty())
                owner_.displayGameMessage("ERR_IGNORING_YOU_S", tradePeerName_ + " is ignoring you.", {tradePeerName_});
            resetTradeState();
            break;
        case 15:  // YOU_STUNNED
            owner_.displayGameMessage("ERR_GENERIC_STUNNED", "You are stunned");
            break;
        case 16:  // TARGET_STUNNED
            owner_.displayGameMessage("ERR_TARGET_STUNNED", "Target is stunned");
            break;
        case 17:  // YOU_DEAD
            owner_.displayGameMessage("ERR_PLAYER_DEAD", "You can't do that when you're dead.");
            break;
        case 18:  // TARGET_DEAD
            owner_.displayGameMessage("ERR_TRADE_TARGET_DEAD", "You cannot trade with dead players.");
            break;
        case 19:  // YOU_LOGOUT
            resetTradeState();
            owner_.displayGameMessage("ERR_LOGGING_OUT", "You are logging out");
            break;
        case 20:  // TARGET_LOGOUT
            resetTradeState();
            owner_.displayGameMessage("ERR_TARGET_LOGGING_OUT", "Target is logging out");
            break;
        case 23:  // NOT_ON_TAPLIST
            owner_.displayGameMessage("ERR_TRADE_NOT_ON_TAPLIST", "You may only trade bound items to players that were originally eligible to loot them.");
            break;
        default:
            LOG_DEBUG("Unhandled SMSG_TRADE_STATUS: ", status);
            break;
    }
}

void InventoryHandler::handleTradeStatusExtended(network::Packet& packet) {
    // SendUpdateTrade writes:
    //
    //   uint8  traderData     1 = the other player's offer, 0 = your own
    //   uint32 tradeId
    //   uint32 slotCount      seven, twice
    //   uint32 slotCount
    //   uint32 gold           the *header* carries it, not the tail
    //   uint32 spell          cast on the lowest slot's item
    //   per slot (seven of them):
    //     uint8  slotIndex
    //     uint32 entry, displayId, stackCount, wrapped
    //     uint64 giftCreator
    //     uint32 permEnchant, gem1, gem2, gem3
    //     uint64 creator
    //     uint32 charges, suffixFactor
    //     int32  randomPropertyId
    //     uint32 lockId, maxDurability, durability
    //
    // An empty slot writes eighteen zeroed uint32s, which is the same
    // seventy-two bytes, so the stride never changes.
    //
    // What was here read a four-byte "whichPlayer" and a four-byte count and
    // then walked slots of sixty-five bytes from offset eight. Every one of
    // those is wrong: the discriminator is one byte, the item block starts at
    // twenty-one, and a slot is seventy-three.
    //
    // It went unnoticed because the arithmetic came out right. The note above
    // it read "8 + 8x65 + 4 = 532 (matches observed packet size)", and 532 is
    // the true size - 21 + 7x73. A total that agrees says nothing about where
    // the fields are, and every item in the trade window was read from the
    // wrong offset with the wrong stride.
    constexpr size_t kHeaderBytes = 1 + 4 * 5;
    constexpr size_t kSlotBytes = 1 + 4 * 4 + 8 + 4 * 4 + 8 + 4 * 6;
    if (!packet.hasRemaining(kHeaderBytes)) { packet.skipAll(); return; }

    const uint8_t traderData = packet.readUInt8();
    /*uint32_t tradeId    =*/ packet.readUInt32();
    const uint32_t slotCount = packet.readUInt32();
    /*uint32_t slotCount2 =*/ packet.readUInt32();
    const uint32_t gold = packet.readUInt32();
    const uint32_t spell = packet.readUInt32();

    // Zero is this player's own offer, which is the side myTradeSlots_ holds.
    const bool ownSide = (traderData == 0);
    auto& slots = ownSide ? myTradeSlots_ : peerTradeSlots_;
    // What the slots held before, to say which ones changed: the trade frame
    // redraws a single slot on TRADE_PLAYER_ITEM_CHANGED(slot) /
    // TRADE_TARGET_ITEM_CHANGED(slot) (tradeframe.lua TradeFrame_OnEvent).
    const auto before = slots;
    // The sender's enchanting spell. It is cast on the *other* side's
    // "will not be traded" item, so ours changes their slot 7 and theirs ours.
    uint32_t& sideSpell = ownSide ? myTradeSpell_ : peerTradeSpell_;
    const bool spellChanged = sideSpell != spell;
    sideSpell = spell;

    uint32_t count = slotCount;
    if (count > TRADE_SLOT_COUNT) count = TRADE_SLOT_COUNT;
    LOG_DEBUG("SMSG_TRADE_STATUS_EXTENDED: ", ownSide ? "own" : "trader",
              " slots=", count, " gold=", gold);

    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(kSlotBytes)) { packet.skipAll(); break; }
        const uint8_t slotNum = packet.readUInt8();
        const uint32_t itemId    = packet.readUInt32();
        const uint32_t displayId = packet.readUInt32();
        const uint32_t stackCnt  = packet.readUInt32();
        /*uint32_t wrapped     =*/ packet.readUInt32();
        /*uint64_t giftCreator =*/ packet.readUInt64();
        /*uint32_t permEnchant =*/ packet.readUInt32();
        for (int g = 0; g < 3; ++g) packet.readUInt32();   // socket enchants
        /*uint64_t creator     =*/ packet.readUInt64();
        /*uint32_t charges     =*/ packet.readUInt32();
        /*uint32_t suffixFactor=*/ packet.readUInt32();
        /*int32_t randomProp   =*/ packet.readUInt32();
        /*uint32_t lockId      =*/ packet.readUInt32();
        /*uint32_t maxDur      =*/ packet.readUInt32();
        /*uint32_t durability  =*/ packet.readUInt32();

        if (slotNum < TRADE_SLOT_COUNT) {
            slots[slotNum].itemId = itemId;
            slots[slotNum].displayId = displayId;
            slots[slotNum].stackCount = stackCnt;
        }
        if (itemId != 0) owner_.ensureItemInfo(itemId);
    }
    packet.skipAll();

    uint64_t& side = ownSide ? myTradeGold_ : peerTradeGold_;
    const bool goldChanged = (side != gold);
    side = gold;

    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TRADE_UPDATE", {});
        for (int i = 0; i < TRADE_SLOT_COUNT; ++i) {
            const auto& a = before[static_cast<size_t>(i)];
            const auto& b = slots[static_cast<size_t>(i)];
            if (a.itemId != b.itemId || a.displayId != b.displayId || a.stackCount != b.stackCount) {
                owner_.addonEventCallbackRef()(
                    ownSide ? "TRADE_PLAYER_ITEM_CHANGED" : "TRADE_TARGET_ITEM_CHANGED",
                    {std::to_string(i + 1)});
            }
        }
        if (spellChanged) {
            owner_.addonEventCallbackRef()(
                ownSide ? "TRADE_TARGET_ITEM_CHANGED" : "TRADE_PLAYER_ITEM_CHANGED",
                {std::to_string(TRADE_SLOT_NONTRADED + 1)});
        }
        // The trade window's own refresh redraws the item slots and nothing
        // else; the two money frames listen for this and only this. Without it
        // the gold on the table stays at whatever it read when the window
        // opened, however much either side puts down.
        //
        // One event per side, because they are two different frames. A money
        // frame carries a moneyType and answers only its own name:
        // TARGET_TRADE listens for TRADE_MONEY_CHANGED, PLAYER_TRADE for
        // PLAYER_TRADE_MONEY. Announcing the peer's event for both refreshed
        // their side when our own gold moved and never refreshed ours - so the
        // amount *this* player had put down never appeared.
        if (goldChanged) {
            owner_.addonEventCallbackRef()(
                ownSide ? "PLAYER_TRADE_MONEY" : "TRADE_MONEY_CHANGED", {});
        }
    }
}

// ============================================================
// Equipment Sets
// ============================================================

bool InventoryHandler::supportsEquipmentSets() const {
    return isActiveExpansion("wotlk");
}

void InventoryHandler::useEquipmentSet(uint32_t setId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_EQUIPMENT_SET_USE);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    const EquipmentSet* es = nullptr;
    for (const auto& s : equipmentSets_) {
        if (s.setId == setId) { es = &s; break; }
    }
    if (!es) {
        LOG_WARNING("useEquipmentSet: no set ", setId);
        return;
    }
    network::Packet pkt(wire);
    for (int slot = 0; slot < 19; ++slot) {
        // An ignored slot goes as the raw guid 1, which HandleEquipmentSetUse
        // skips (CharacterHandler.cpp:1867). Sent as 0 - which is how the list
        // parser stores it - it reads as "nothing here" and the server took
        // off whatever was worn in the slot the set meant to leave alone.
        const bool ignored = (es->ignoreSlotMask & (1u << slot)) != 0;
        uint64_t itemGuid = ignored ? 1 : es->itemGuids[slot];
        pkt.writePackedGuid(itemGuid);
        uint8_t srcBag = 0xFF;
        uint8_t srcSlot = 0;
        if (itemGuid != 0 && !ignored) {
            bool found = false;
            for (int eq = 0; eq < 19 && !found; ++eq) {
                if (owner_.getEquipSlotGuid(eq) == itemGuid) {
                    srcBag = 0xFF;
                    srcSlot = static_cast<uint8_t>(eq);
                    found = true;
                }
            }
            for (int bp = 0; bp < 16 && !found; ++bp) {
                if (owner_.getBackpackItemGuid(bp) == itemGuid) {
                    srcBag = 0xFF;
                    srcSlot = static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + bp);
                    found = true;
                }
            }
            for (int bag = 0; bag < 4 && !found; ++bag) {
                int bagSize = owner_.inventoryRef().getBagSize(bag);
                for (int s = 0; s < bagSize && !found; ++s) {
                    if (owner_.getBagItemGuid(bag, s) == itemGuid) {
                        srcBag = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bag);
                        srcSlot = static_cast<uint8_t>(s);
                        found = true;
                    }
                }
            }
        }
        pkt.writeUInt8(srcBag);
        pkt.writeUInt8(srcSlot);
    }
    owner_.getSocket()->send(pkt);
    // The swap is under way; SMSG_EQUIPMENT_SET_USE_RESULT finishes it, and
    // the gear manager re-reads the set's ignored slots on
    // EQUIPMENT_SWAP_FINISHED(completed, setName) (GearManagerDialog_OnEvent).
    pendingUseSetName_ = es->name;
    owner_.fireAddonEvent("EQUIPMENT_SWAP_PENDING", {});
}

void InventoryHandler::saveEquipmentSet(const std::string& name, const std::string& iconName,
                                         uint64_t existingGuid, uint32_t setIndex,
                                         uint32_t ignoreMask) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_EQUIPMENT_SET_SAVE);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    pendingSaveSetName_ = name;
    pendingSaveSetIcon_ = iconName;
    pendingSaveIgnoreMask_ = ignoreMask;
    if (setIndex == 0xFFFFFFFF) {
        setIndex = 0;
        for (const auto& es : equipmentSets_) {
            if (es.setId >= setIndex) setIndex = es.setId + 1;
        }
    }
    network::Packet pkt(wire);
    // Packed, like everything guid-shaped in this family.
    // HandleEquipmentSetSave opens with readPackGUID, so a fixed eight bytes
    // here left seven behind: the server read the index out of the padding,
    // the name out of what was left of it, and the nineteen item guids from
    // wherever it had got to. Saving a set has never worked.
    pkt.writePackedGuid(existingGuid);
    pkt.writeUInt32(setIndex);
    pkt.writeString(name);
    pkt.writeString(iconName);
    // A slot the player marked ignored goes as the raw guid 1 - the value
    // HandleEquipmentSetSave turns into the set's IgnoreMask
    // (CharacterHandler.cpp:1823). The worn item was sent instead, so an
    // ignored slot was saved as part of the set.
    for (int i = 0; i < 19; ++i) {
        pkt.writePackedGuid((ignoreMask & (1u << i)) ? 1 : owner_.getEquipSlotGuid(i));
    }
    owner_.getSocket()->send(pkt);
    // Overwriting a set gets no answer - Player::SetEquipmentSet sends
    // SMSG_EQUIPMENT_SET_SAVED only for a new one - so the local copy is
    // brought up to date here, or the manager would go on showing the items
    // the set had before.
    if (existingGuid != 0) {
        for (auto& es : equipmentSets_) {
            if (es.setGuid != existingGuid) continue;
            es.name = name;
            es.iconName = iconName;
            es.ignoreSlotMask = ignoreMask;
            for (int s = 0; s < 19; ++s) {
                es.itemGuids[s] = (ignoreMask & (1u << s)) ? 0 : owner_.getEquipSlotGuid(s);
            }
        }
        for (auto& info : equipmentSetInfo_) {
            if (info.setGuid != existingGuid) continue;
            info.name = name;
            info.iconName = iconName;
        }
        owner_.fireAddonEvent("EQUIPMENT_SETS_CHANGED", {});
    }
}

void InventoryHandler::deleteEquipmentSet(uint64_t setGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_DELETEEQUIPMENT_SET);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    network::Packet pkt(wire);
    // HandleEquipmentSetDelete reads this packed too.
    pkt.writePackedGuid(setGuid);
    owner_.getSocket()->send(pkt);
    equipmentSets_.erase(
        std::remove_if(equipmentSets_.begin(), equipmentSets_.end(),
                        [setGuid](const EquipmentSet& es) { return es.setGuid == setGuid; }),
        equipmentSets_.end());
    equipmentSetInfo_.erase(
        std::remove_if(equipmentSetInfo_.begin(), equipmentSetInfo_.end(),
                        [setGuid](const EquipmentSetInfo& info) { return info.setGuid == setGuid; }),
        equipmentSetInfo_.end());
    // The server sends nothing back for a delete (Player::DeleteEquipmentSet),
    // and the gear manager redraws its list only on this - the deleted set
    // stayed in it until the next login.
    owner_.fireAddonEvent("EQUIPMENT_SETS_CHANGED", {});
}

void InventoryHandler::handleEquipmentSetList(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    if (count > 10) {
        LOG_WARNING("SMSG_EQUIPMENT_SET_LIST: unexpected count ", count, ", ignoring");
        packet.skipAll();
        return;
    }
    equipmentSets_.clear();
    equipmentSets_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        // Every guid in this message is packed - a mask byte and then only the
        // non-zero bytes - and all twenty were read as fixed eight-byte values.
        // A set guid of one is two bytes on the wire, so the first read ate the
        // set id and the front of the name, and with more than one set the loop
        // lost its place entirely. SendEquipmentSetList writes each of these
        // with appendPackGUID.
        if (!packet.hasRemaining(2)) break;
        EquipmentSet es;
        es.setGuid  = packet.readPackedGuid();
        if (!packet.hasRemaining(4)) break;
        es.setId    = packet.readUInt32();
        es.name     = packet.readString();
        es.iconName = packet.readString();
        // No ignore mask on the wire. A uint32 was being read for one here and
        // the server never sends it: an ignored slot is written as an item guid
        // of literal one, which is the mask. Reading a word that is not there
        // took the first four bytes of the slot list every time.
        es.ignoreSlotMask = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if (!packet.hasRemaining(1)) break;
            const uint64_t itemGuid = packet.readPackedGuid();
            if (itemGuid == 1) {
                // "Ignore this slot" - the set leaves whatever is worn there.
                es.ignoreSlotMask |= (1u << slot);
                es.itemGuids[slot] = 0;
            } else {
                es.itemGuids[slot] = itemGuid;
            }
        }
        equipmentSets_.push_back(std::move(es));
    }
    equipmentSetInfo_.clear();
    equipmentSetInfo_.reserve(equipmentSets_.size());
    for (const auto& es : equipmentSets_) {
        EquipmentSetInfo info;
        info.setGuid  = es.setGuid;
        info.setId    = es.setId;
        info.name     = es.name;
        info.iconName = es.iconName;
        equipmentSetInfo_.push_back(std::move(info));
    }
    LOG_INFO("SMSG_EQUIPMENT_SET_LIST: ", equipmentSets_.size(), " equipment sets received");
    // The manager redraws on this, and the list arriving is the only time it
    // has anything new to draw - including straight after a set is saved or
    // deleted, which is when the server sends it again.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("EQUIPMENT_SETS_CHANGED", {});
    }
}

// ============================================================
// Inventory field / rebuild methods (moved from GameHandler)
// ============================================================

void InventoryHandler::queryItemInfo(uint32_t entry, uint64_t guid) {
    if (owner_.itemInfoCacheRef().count(entry) || owner_.pendingItemQueriesRef().count(entry)) return;
    if (!owner_.isInWorld()) return;

    owner_.pendingItemQueriesRef().insert(entry);
    // Some cores reject CMSG_ITEM_QUERY_SINGLE when the GUID is 0.
    // If we don't have the item object's GUID (e.g. visible equipment decoding),
    // fall back to the player's GUID to keep the request non-zero.
    uint64_t queryGuid = (guid != 0) ? guid : owner_.getPlayerGuid();
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildItemQuery(entry, queryGuid)
        : ItemQueryPacket::build(entry, queryGuid);
    owner_.getSocket()->send(packet);
    LOG_DEBUG("queryItemInfo: SENT entry=", entry, " guid=0x", std::hex, queryGuid, std::dec,
              " pending=", owner_.pendingItemQueriesRef().size());
}

// An auction GameError naming the item - its plain name with any random
// suffix, as Wow.exe writes it (0x706D70) - said once the item's name is
// known; until then it waits on the item query, as the stock client's
// callback does (0x59A400).
void InventoryHandler::announceAuctionItem(uint32_t itemId, int32_t randomProp, const char* key) {
    const ItemQueryResponseData* info = owner_.getItemInfo(itemId);
    if (!info || info->name.empty()) {
        if (pendingAuctionNotices_.size() < 32) pendingAuctionNotices_.push_back({itemId, randomProp, key});
        owner_.ensureItemInfo(itemId);
        return;
    }
    std::string name = info->name;
    if (randomProp != 0) {
        const std::string suffix = owner_.getRandomPropertyName(randomProp);
        if (!suffix.empty()) name += " " + suffix;
    }
    owner_.displayGameMessage(key, formatGlobalString(stockGlobal(owner_, key), {name}), {name});
}

void InventoryHandler::handleItemQueryResponse(network::Packet& packet) {
    ItemQueryResponseData data;
    bool parsed = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->parseItemQueryResponse(packet, data)
        : ItemQueryResponseParser::parse(packet, data);
    if (!parsed) {
        // Extract entry from raw packet so we can clear the pending query even on parse failure.
        // Without this, the entry stays in pendingItemQueries_ forever, blocking retries.
        if (packet.getSize() >= 4) {
            packet.setReadPos(0);
            // High bit indicates a negative (invalid/missing) item entry response;
            // mask it off so we can still clear the pending query by entry ID.
            uint32_t rawEntry = packet.readUInt32() & ~0x80000000u;
            owner_.pendingItemQueriesRef().erase(rawEntry);
        }
        LOG_WARNING("handleItemQueryResponse: parse failed, size=", packet.getSize());
        return;
    }

    owner_.pendingItemQueriesRef().erase(data.entry);
    LOG_DEBUG("handleItemQueryResponse: RECV entry=", data.entry, " name='", data.name,
              "' displayInfoId=", data.displayInfoId,
              " class=", data.itemClass, " subClass=", data.subClass,
              " invType=", data.inventoryType, " valid=", data.valid);

    if (data.valid) {
        owner_.itemInfoCacheRef()[data.entry] = data;
        rebuildOnlineInventory();
        maybeDetectVisibleItemLayout();

        // The answer to a question a tooltip asked and could not wait for.
        //
        // _GetItemTooltipData returns nothing for an item that is not cached
        // yet and asks for it, so a tooltip built before the reply has no
        // stats at all - and nothing rebuilt it when the reply came. It shows
        // most on a comparison, where the item being compared against is one
        // the player has worn rather than hovered and so is often unknown, but
        // any tooltip opened early reads the same way.
        //
        // Named so a listener can tell whether the item it is showing is the
        // one that just arrived, rather than rebuilding on every reply.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("GET_ITEM_INFO_RECEIVED",
                                           {std::to_string(data.entry)});
        }

        // The loot window's row for this item, which was drawn before the
        // item was known.
        //
        // SMSG_LOOT_RESPONSE carries an item id and a count and nothing a
        // player can read, so the queries go out as the window opens and land
        // after it has drawn - "Item #4606" against an empty square, kept for
        // as long as the corpse is open. LOOT_SLOT_CHANGED is the event the
        // loot frame redraws one button on.
        if (lootWindowOpen_ && owner_.addonEventCallbackRef()) {
            const int coinSlots = currentLoot_.hasCoinSlot() ? 1 : 0;
            for (size_t i = 0; i < currentLoot_.items.size(); ++i) {
                if (currentLoot_.items[i].itemId != data.entry) continue;
                owner_.addonEventCallbackRef()(
                    "LOOT_SLOT_CHANGED",
                    {std::to_string(coinSlots + static_cast<int>(i) + 1)});
            }
        }

        // An objective that had no name for what it asks for now has one.
        //
        // The quest log and the tracker draw "0/1 Item #1349" until the item
        // query answers, and the answer lands well after the row was drawn -
        // for a quest accepted while the tracker is on screen, always. Nothing
        // asked them to look again, so the number stayed for the life of the
        // quest. The same shape the creature and game object queries have.
        if (owner_.addonEventCallbackRef()) {
            bool wanted = false;
            for (const auto& quest : owner_.getQuestLog()) {
                for (const auto& obj : quest.itemObjectives) {
                    if (obj.itemId == data.entry && obj.required > 0) { wanted = true; break; }
                }
                if (wanted) break;
            }
            if (wanted) owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
        }

        // A quest's item icons are drawn before their items are known - the
        // query goes out when the panel opens and lands after it has drawn - so
        // without this they stayed blank until something else redrew them.
        // Fired only while a quest window is up: the query runs hundreds of
        // times over a login, and the handler is only interesting when there is
        // a panel to refresh.
        //
        // All four panels, not two. The progress page is the one that shows
        // what a quest is asking for, and it was the one missing - worse, it
        // clears the details and gossip flags as it opens, so neither of the
        // two that were listed could stand in for it. Its required item was a
        // blank square for as long as the window stayed up.
        // Not the gossip list: it draws no items, and every item answer that
        // landed while one was open (hundreds after a login) fired this.
        if (owner_.isQuestDetailsOpen() ||
            owner_.isQuestRequestItemsOpen() || owner_.isQuestOfferRewardOpen()) {
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("QUEST_ITEM_UPDATE", {});
            }
        }

        // Auction mail subjects contain an item entry rather than display text.
        // Refresh FrameXML mail rows once that item's name becomes available.
        bool resolvedAuctionSubject = false;
        for (const auto& mail : mailInbox_) {
            AuctionMailSubject auction;
            if (mail.messageType == 2 && parseAuctionMailSubject(mail.subject, auction) &&
                auction.itemEntry == data.entry) {
                resolvedAuctionSubject = true;
                break;
            }
        }
        if (resolvedAuctionSubject && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("MAIL_INBOX_UPDATE", {});
        }

        // The auction list, for the same reason as the quest rewards above.
        //
        // An auction row carries an item entry and nothing else - no name, no
        // icon - so the list arrives, the queries go out, and the answers land
        // after the rows are drawn. This client's own auction window read the
        // item cache on every frame it drew and so filled itself in; an
        // interface told once draws "Item #41394" against a question mark and
        // keeps it. Only while the house is open, and only for a row that is
        // actually waiting on this entry: the query runs hundreds of times
        // over a login and the rest of them are nothing to do with auctions.
        if (auctionOpen_ && owner_.addonEventCallbackRef()) {
            auto listWants = [&data](const AuctionListResult& r) {
                for (const auto& e : r.auctions) {
                    if (e.itemEntry == data.entry) return true;
                }
                return false;
            };
            if (listWants(auctionBrowseResults_)) {
                owner_.addonEventCallbackRef()("AUCTION_ITEM_LIST_UPDATE", {});
            }
            if (listWants(auctionOwnerResults_)) {
                owner_.addonEventCallbackRef()("AUCTION_OWNED_LIST_UPDATE", {});
            }
            if (listWants(auctionBidderResults_)) {
                owner_.addonEventCallbackRef()("AUCTION_BIDDER_LIST_UPDATE", {});
            }
        }

        // The vendor list, which arrives the same way: SMSG_LIST_INVENTORY
        // gives an item id and a price per row and nothing a player can read.
        // Same shape, same cause, same gate - the window has to be open and a
        // row has to be waiting on this entry.
        if (vendorWindowOpen_ && owner_.addonEventCallbackRef()) {
            for (const auto& v : currentVendorItems_.items) {
                if (v.itemId != data.entry) continue;
                owner_.addonEventCallbackRef()("MERCHANT_UPDATE", {});
                break;
            }
        }

        // Auction notices held for this item's name.
        for (auto it = pendingAuctionNotices_.begin(); it != pendingAuctionNotices_.end(); ) {
            if (it->itemId == data.entry) {
                const auto notice = *it;
                it = pendingAuctionNotices_.erase(it);
                announceAuctionItem(notice.itemId, notice.randomProp, notice.key);
            } else {
                ++it;
            }
        }

        // Flush any deferred loot notifications waiting on this item's name/quality.
        for (auto it = owner_.pendingItemPushNotifsRef().begin(); it != owner_.pendingItemPushNotifsRef().end(); ) {
            if (it->itemId == data.entry) {
                std::string itemName = data.name.empty() ? ("item #" + std::to_string(data.entry)) : data.name;
                std::string link = buildItemLink(data.entry, data.quality, itemName);
                addItemPushLine(owner_, link, it->count, it->received, it->created, it->displayInChat);
                // The stock client also defers the whole push, sound included,
                // until the item cache answers (0x523900 requeues itself).
                playItemPushSound(owner_, data.displayInfoId);
                // ...and the event, which this path had never fired. An item
                // whose name was not cached when it arrived is exactly the
                // first one of its kind the player picks up, so the interface
                // heard nothing on precisely the pickups it most wanted to
                // hear about - watchframe refreshes its objectives on this.
                if (owner_.addonEventCallbackRef()) {
                    owner_.addonEventCallbackRef()("ITEM_PUSH",
                            {std::to_string(it->bagButtonId),
                             owner_.getItemIconPath(data.displayInfoId)});
                }
                it = owner_.pendingItemPushNotifsRef().erase(it);
            } else {
                ++it;
            }
        }

        // Selectively re-emit only players whose equipment references this item entry
        const uint32_t resolvedEntry = data.entry;
        int reemitCount = 0;
        for (const auto& [guid, entries] : owner_.otherPlayerVisibleItemEntriesRef()) {
            for (uint32_t e : entries) {
                if (e == resolvedEntry) {
                    emitOtherPlayerEquipment(guid);
                    reemitCount++;
                    break;
                }
            }
        }
        if (reemitCount > 0) {
            LOG_DEBUG("Re-emitted equipment for ", reemitCount, " players after resolving entry=", resolvedEntry);
        }
        // Same for inspect-based entries
        if (owner_.playerEquipmentCallbackRef()) {
            for (const auto& [guid, entries] : owner_.inspectedPlayerItemEntriesRef()) {
                bool relevant = false;
                for (uint32_t e : entries) {
                    if (e == resolvedEntry) { relevant = true; break; }
                }
                if (!relevant) continue;
                std::array<uint32_t, 19> displayIds{};
                std::array<uint8_t, 19> invTypes{};
                for (int s = 0; s < 19; s++) {
                    uint32_t entry = entries[s];
                    if (entry == 0) continue;
                    auto infoIt = owner_.itemInfoCacheRef().find(entry);
                    if (infoIt == owner_.itemInfoCacheRef().end()) continue;
                    displayIds[s] = infoIt->second.displayInfoId;
                    invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
                }
                owner_.playerEquipmentCallbackRef()(guid, displayIds, invTypes);
            }
        }

        // And the interface, for a unit it can be showing: an inspect square
        // drawn before this template came back drew a question mark, and it
        // redraws on UNIT_INVENTORY_CHANGED for its unit and nothing else.
        // Only the guids a unit token can name are checked - every other
        // player has no token for a window to be showing them under.
        if (owner_.addonEventCallbackRef()) {
            auto wears = [&](uint64_t guid) {
                for (const auto* table : {&owner_.otherPlayerVisibleItemEntriesRef(),
                                          &owner_.inspectedPlayerItemEntriesRef()}) {
                    auto it = table->find(guid);
                    if (it == table->end()) continue;
                    for (uint32_t e : it->second) {
                        if (e == resolvedEntry) return true;
                    }
                }
                return false;
            };
            std::vector<uint64_t> shown = {owner_.getTargetGuid(), owner_.getFocusGuid(),
                                           owner_.getMouseoverGuid()};
            for (const auto& m : owner_.getPartyData().members) shown.push_back(m.guid);
            std::sort(shown.begin(), shown.end());
            shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
            for (uint64_t guid : shown) {
                if (guid != 0 && wears(guid)) fireUnitInventoryChangedFor(owner_, guid);
            }
        }
    }
}

uint64_t InventoryHandler::resolveOnlineItemGuid(uint32_t itemId) const {
    if (itemId == 0) return 0;
    uint64_t candidate = 0;
    for (const auto& [guid, info] : owner_.onlineItemsRef()) {
        if (info.entry != itemId) continue;
        // Item IDs are templates, not identities. Only use this compatibility
        // fallback when the matching online object is unambiguous.
        if (candidate != 0) return 0;
        candidate = guid;
    }
    return candidate;
}

void InventoryHandler::detectInventorySlotBases(const FlatFieldMap& fields) {
    if (owner_.invSlotBaseRef() >= 0 && owner_.packSlotBaseRef() >= 0) return;
    if (fields.empty()) return;

    std::vector<uint16_t> matchingPairs;
    matchingPairs.reserve(32);

    for (const auto& [idx, low] : fields) {
        if ((idx % 2) != 0) continue;
        auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
        if (itHigh == fields.end()) continue;
        uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
        if (guid == 0) continue;
        // Primary signal: GUID pairs that match spawned ITEM objects.
        if (!owner_.onlineItemsRef().empty() && owner_.onlineItemsRef().count(guid)) {
            matchingPairs.push_back(idx);
        }
    }

    // Fallback signal (when ITEM objects haven't been seen yet):
    // collect any plausible non-zero GUID pairs and derive a base by density.
    if (matchingPairs.empty()) {
        for (const auto& [idx, low] : fields) {
            if ((idx % 2) != 0) continue;
            auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
            if (itHigh == fields.end()) continue;
            uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
            if (guid == 0) continue;
            // Heuristic: item GUIDs tend to be non-trivial and change often; ignore tiny values.
            if (guid < 0x10000ull) continue;
            matchingPairs.push_back(idx);
        }
    }

    if (matchingPairs.empty()) return;
    std::sort(matchingPairs.begin(), matchingPairs.end());

    if (owner_.invSlotBaseRef() < 0) {
        // The lowest matching field is the first EQUIPPED slot (not necessarily HEAD).
        // With 2+ matches we can derive the true base: all matches must be at
        // even offsets from the base, spaced 2 fields per slot.
        const int knownBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD));
        constexpr int slotStride = 2;
        bool allAlign = true;
        for (uint16_t p : matchingPairs) {
            if (p < knownBase || (p - knownBase) % slotStride != 0) {
                allAlign = false;
                break;
            }
        }
        if (allAlign) {
            owner_.invSlotBaseRef() = knownBase;
        } else {
            // Fallback: if we have 2+ matches, derive base from their spacing
            if (matchingPairs.size() >= 2) {
                uint16_t lo = matchingPairs[0];
                // lo must be base + 2*slotN, and slotN is 0..22
                // Try each possible slot for 'lo' and see if all others also land on valid slots
                for (int s = 0; s <= 22; s++) {
                    int candidate = lo - s * slotStride;
                    if (candidate < 0) break;
                    bool ok = true;
                    for (uint16_t p : matchingPairs) {
                        int off = p - candidate;
                        if (off < 0 || off % slotStride != 0 || off / slotStride > 22) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        owner_.invSlotBaseRef() = candidate;
                        break;
                    }
                }
                if (owner_.invSlotBaseRef() < 0) owner_.invSlotBaseRef() = knownBase;
            } else {
                owner_.invSlotBaseRef() = knownBase;
            }
        }
        owner_.packSlotBaseRef() = owner_.invSlotBaseRef() + (game::Inventory::NUM_EQUIP_SLOTS * 2);
        LOG_INFO("Detected inventory field base: equip=", owner_.invSlotBaseRef(),
                 " pack=", owner_.packSlotBaseRef());
    }
}

bool InventoryHandler::applyInventoryFields(const FlatFieldMap& fields) {
    bool slotsChanged = false;
    bool buybackSlotsChanged = false;
    int equipBase = (owner_.invSlotBaseRef() >= 0) ? owner_.invSlotBaseRef() : static_cast<int>(fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD));
    int packBase = (owner_.packSlotBaseRef() >= 0) ? owner_.packSlotBaseRef() : static_cast<int>(fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1));
    int bankBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_BANK_SLOT_1));
    int bankBagBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_BANKBAG_SLOT_1));

    // Derive slot counts from field gap (Classic=24/6, TBC/WotLK=28/7).
    if (bankBase != 0xFFFF && bankBagBase != 0xFFFF) {
        effectiveBankSlots_ = std::min((bankBagBase - bankBase) / 2, 28);
        effectiveBankBagSlots_ = (effectiveBankSlots_ <= 24) ? 6 : 7;
    }

    int keyringBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_KEYRING_SLOT_1));
    const uint16_t playerBytesIdx = fieldIndex(UF::PLAYER_FIELD_BYTES);
    const uint32_t buybackPriceBase = (isActiveExpansion("wotlk") && playerBytesIdx != 0xFFFF)
        ? static_cast<uint32_t>(playerBytesIdx) + 4u : 0xFFFFu;
    const int buybackBase = bankBagBase != 0xFFFF
        ? bankBagBase + (effectiveBankBagSlots_ * 2) : 0xFFFF;
    if (keyringBase == 0xFFFF && bankBagBase != 0xFFFF) {
        // Layout fallback for profiles that don't define PLAYER_FIELD_KEYRING_SLOT_1.
        // Bank bag slots are followed by 12 vendor buyback slots (24 fields), then keyring.
        keyringBase = bankBagBase + (effectiveBankBagSlots_ * 2) + 24;
    }

    // Which bank slots actually moved, so the bank window can be told.
    //
    // BankFrame registers PLAYERBANKSLOTS_CHANGED and refreshes exactly one
    // button from it. It does *not* register BAG_UPDATE, so that event was the
    // only thing that could redraw a bank slot - and it was never fired, which
    // left an item moved into or out of the bank sitting on screen in its old
    // place until the window was closed and reopened.
    // The interface's own NUM_BANKGENERIC_SLOTS, which is 28 on WotLK and 24
    // on Classic - the same figure this function already derives from the
    // field gap, so it is taken from there rather than written out again.
    const int kBankGeneralSlotCount = effectiveBankSlots_;
    std::set<int> changedBankSlots;
    // Honor and arena points are player fields rather than items; the
    // currency tab lists them (extraCurrencyType 2 and 1) and redraws only on
    // CURRENCY_DISPLAY_UPDATE. Their values are stored before this runs.
    const uint16_t honorField = fieldIndex(UF::PLAYER_FIELD_HONOR_CURRENCY);
    const uint16_t arenaField = fieldIndex(UF::PLAYER_FIELD_ARENA_CURRENCY);
    bool pointsChanged = false;
    for (const auto& [key, val] : fields) {
        if ((honorField != 0xFFFF && key == honorField) ||
            (arenaField != 0xFFFF && key == arenaField)) {
            pointsChanged = true;
        }
        if (key >= equipBase && key <= equipBase + (game::Inventory::NUM_EQUIP_SLOTS * 2 - 1)) {
            int slotIndex = (key - equipBase) / 2;
            bool isLow = ((key - equipBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.equipSlotGuidsRef().size())) {
                uint64_t& guid = owner_.equipSlotGuidsRef()[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        } else if (key >= packBase && key <= packBase + (game::Inventory::BACKPACK_SLOTS * 2 - 1)) {
            int slotIndex = (key - packBase) / 2;
            bool isLow = ((key - packBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.backpackSlotGuidsRef().size())) {
                uint64_t& guid = owner_.backpackSlotGuidsRef()[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        } else if (keyringBase != 0xFFFF &&
                   key >= keyringBase &&
                   key <= keyringBase + (game::Inventory::KEYRING_SLOTS * 2 - 1)) {
            int slotIndex = (key - keyringBase) / 2;
            bool isLow = ((key - keyringBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.keyringSlotGuidsRef().size())) {
                uint64_t& guid = owner_.keyringSlotGuidsRef()[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        }
        if (bankBase != 0xFFFF && key >= static_cast<uint16_t>(bankBase) &&
            key <= static_cast<uint16_t>(bankBase) + (effectiveBankSlots_ * 2 - 1)) {
            int slotIndex = (key - bankBase) / 2;
            bool isLow = ((key - bankBase) % 2 == 0);
            if (slotIndex < static_cast<int>(bankSlotGuids_.size())) {
                uint64_t& guid = bankSlotGuids_[slotIndex];
                const uint64_t before = guid;
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
                // Counted as the bank window counts: its general slots first,
                // from one. A guid arrives as two fields, so this is a set.
                if (guid != before) changedBankSlots.insert(slotIndex + 1);
            }
        }

        // Bank bag slots starting at PLAYER_FIELD_BANKBAG_SLOT_1
        if (bankBagBase != 0xFFFF && key >= static_cast<uint16_t>(bankBagBase) &&
            key <= static_cast<uint16_t>(bankBagBase) + (effectiveBankBagSlots_ * 2 - 1)) {
            int slotIndex = (key - bankBagBase) / 2;
            bool isLow = ((key - bankBagBase) % 2 == 0);
            if (slotIndex < static_cast<int>(bankBagSlotGuids_.size())) {
                uint64_t& guid = bankBagSlotGuids_[slotIndex];
                const uint64_t before = guid;
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
                // The bag slots continue the same numbering, after the 28
                // general ones - which is how bankframe.lua splits them again.
                if (guid != before) changedBankSlots.insert(kBankGeneralSlotCount + slotIndex + 1);
            }
        }

        // The ring's prices and sale times, twelve of each, which 3.3.5 keeps
        // four fields after PLAYER_FIELD_BYTES (PLAYER_FIELD_BUYBACK_PRICE_1,
        // then PLAYER_FIELD_BUYBACK_TIMESTAMP_1). GetBuybackItemInfo answers
        // the price from these, as the stock client does.
        if (buybackPriceBase != 0xFFFF && key >= buybackPriceBase &&
            key < buybackPriceBase + 2 * kBuybackWireSlotCount) {
            const uint32_t i = key - buybackPriceBase;
            if (i < kBuybackWireSlotCount) buybackPrices_[i] = val;
            else buybackTimestamps_[i - kBuybackWireSlotCount] = val;
            buybackSlotsChanged = true;
        }

        // The server owns the buyback ring. Bind local item metadata to these
        // GUID fields rather than assuming displayed row N means wire slot 74+N.
        if (buybackBase != 0xFFFF && key >= static_cast<uint16_t>(buybackBase) &&
            key <= static_cast<uint16_t>(buybackBase) +
                   (kBuybackWireSlotCount * 2 - 1)) {
            const int slotIndex = (key - buybackBase) / 2;
            const bool isLow = ((key - buybackBase) % 2 == 0);
            uint64_t& guid = buybackSlotGuids_[slotIndex];
            if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
            else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
            buybackSlotsChanged = true;
        }
    }

    if (buybackSlotsChanged) reconcileBuybackSlots();
    // Held rather than announced. The bank frame redraws a slot by asking what
    // that slot holds, and what it holds is written by rebuildOnlineInventory,
    // which runs after this returns - so announcing here redrew the slot from
    // the item that had just left it, and nothing said so again. The item
    // stayed on screen until the bank was closed and opened.
    pendingBankSlotEvents_.insert(pendingBankSlotEvents_.end(),
                                  changedBankSlots.begin(), changedBankSlots.end());
    if (pointsChanged) owner_.fireAddonEvent("CURRENCY_DISPLAY_UPDATE", {});


    return slotsChanged;
}

void InventoryHandler::extractContainerFields(uint64_t containerGuid, const FlatFieldMap& fields) {
    const uint16_t numSlotsIdx = fieldIndex(UF::CONTAINER_FIELD_NUM_SLOTS);
    const uint16_t slot1Idx = fieldIndex(UF::CONTAINER_FIELD_SLOT_1);
    if (numSlotsIdx == 0xFFFF || slot1Idx == 0xFFFF) return;

    auto& info = owner_.containerContentsRef()[containerGuid];

    // Read number of slots
    auto numIt = fields.find(numSlotsIdx);
    if (numIt != fields.end()) {
        info.numSlots = std::min(numIt->second, 36u);
    }

    // Read slot GUIDs (each is 2 uint32 fields: lo + hi)
    for (const auto& [key, val] : fields) {
        if (key < slot1Idx) continue;
        int offset = key - slot1Idx;
        int slotIndex = offset / 2;
        if (slotIndex >= 36) continue;
        bool isLow = (offset % 2 == 0);
        uint64_t& guid = info.slotGuids[slotIndex];
        if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
        else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
    }
}

// Builds an ItemDef from an OnlineItemInfo by merging server field data (stack
// count, durability) with cached item info (name, stats, quality). Centralised
// here so adding new ItemDef fields doesn't require editing 4 separate copy-
// paste sites in rebuildOnlineInventory.
ItemDef InventoryHandler::buildItemDef(uint32_t entry, uint32_t stackCount,
                                       uint32_t curDur, uint32_t maxDur, uint64_t guid,
                                       uint32_t flags, int32_t randomPropertyId,
                                       uint32_t suffixFactor) {
    ItemDef def;
    def.itemId = entry;
    def.guid = guid;
    def.stackCount = stackCount;
    def.curDurability = curDur;
    def.maxDurability = maxDur;
    def.maxStack = 1;
    // ITEM_FLAG_SOULBOUND (0x1): a BoE item that has already bound must not re-prompt.
    def.soulbound = (flags & 0x1u) != 0;
    def.randomPropertyId = randomPropertyId;
    def.suffixFactor = suffixFactor;

    auto infoIt = owner_.itemInfoCacheRef().find(entry);
    if (infoIt != owner_.itemInfoCacheRef().end()) {
        const auto& info = infoIt->second;
        def.name = info.name;
        def.quality = static_cast<ItemQuality>(info.quality);
        def.inventoryType = info.inventoryType;
        def.maxStack = std::max(1, info.maxStack);
        def.displayInfoId = info.displayInfoId;
        def.subclassName = info.subclassName;
        def.damageMin = info.damageMin;
        def.damageMax = info.damageMax;
        def.delayMs = info.delayMs;
        def.armor = info.armor;
        def.stamina = info.stamina;
        def.strength = info.strength;
        def.agility = info.agility;
        def.intellect = info.intellect;
        def.spirit = info.spirit;
        def.sellPrice = info.sellPrice;
        def.itemLevel = info.itemLevel;
        def.requiredLevel = info.requiredLevel;
        def.bindType = info.bindType;
        def.description = info.description;
        def.pageTextId = info.pageTextId;
        def.startQuestId = info.startQuestId;
        def.extraStats.clear();
        for (const auto& es : info.extraStats)
            def.extraStats.push_back({.statType = es.statType, .statValue = es.statValue});
    } else {
        def.name = "Item " + std::to_string(def.itemId);
        queryItemInfo(def.itemId, guid);
    }

    // Merge the rolled random-suffix/property stats on top of the base template. Primary
    // stats fold into their dedicated fields; everything else (ratings, AP, spell power)
    // joins extraStats so the existing tooltip rendering shows them as green stat lines.
    if (randomPropertyId != 0) {
        for (const auto& b : owner_.getRandomStatBonuses(randomPropertyId, suffixFactor)) {
            if (b.value == 0) continue;
            switch (b.statType) {
                case 3: def.agility   += b.value; break;
                case 4: def.strength  += b.value; break;
                case 5: def.intellect += b.value; break;
                case 6: def.spirit    += b.value; break;
                case 7: def.stamina   += b.value; break;
                default: def.extraStats.push_back({.statType = b.statType, .statValue = b.value}); break;
            }
        }
    }
    return def;
}

void InventoryHandler::rebuildOnlineInventory() {
    // Announced from here rather than from the callers, because this is the one
    // place the inventory picture is rebuilt and only one of its six callers
    // was saying so. Moving an item between two slots changes which item sits
    // in which of the player's slot fields - not any item's own fields - so it
    // never reached the path that announced a change, and the bags and the
    // character sheet went on drawing what they were last told.
    struct Announce {
        InventoryHandler& self;
        ~Announce() { self.fireBagUpdates(); }
    } announce{*this};


    uint8_t savedBankBagSlots = owner_.inventoryRef().getPurchasedBankBagSlots();
    owner_.inventoryRef() = Inventory();
    owner_.inventoryRef().setPurchasedBankBagSlots(savedBankBagSlots);

    // Equipment slots
    for (int i = 0; i < 23; i++) {
        uint64_t guid = owner_.equipSlotGuidsRef()[i];
        if (guid == 0) continue;
        auto itemIt = owner_.onlineItemsRef().find(guid);
        if (itemIt == owner_.onlineItemsRef().end()) continue;
        owner_.inventoryRef().setEquipSlot(static_cast<EquipSlot>(i), buildItemDef(itemIt->second.entry, itemIt->second.stackCount, itemIt->second.curDurability, itemIt->second.maxDurability, guid, itemIt->second.flags, itemIt->second.randomPropertyId, itemIt->second.suffixFactor));
    }

    // Backpack slots
    for (int i = 0; i < 16; i++) {
        uint64_t guid = owner_.backpackSlotGuidsRef()[i];
        if (guid == 0) continue;
        auto itemIt = owner_.onlineItemsRef().find(guid);
        if (itemIt == owner_.onlineItemsRef().end()) continue;
        owner_.inventoryRef().setBackpackSlot(i, buildItemDef(itemIt->second.entry, itemIt->second.stackCount, itemIt->second.curDurability, itemIt->second.maxDurability, guid, itemIt->second.flags, itemIt->second.randomPropertyId, itemIt->second.suffixFactor));
    }

    // Keyring slots
    for (int i = 0; i < game::Inventory::KEYRING_SLOTS; i++) {
        uint64_t guid = owner_.keyringSlotGuidsRef()[i];
        if (guid == 0) continue;
        auto itemIt = owner_.onlineItemsRef().find(guid);
        if (itemIt == owner_.onlineItemsRef().end()) continue;
        owner_.inventoryRef().setKeyringSlot(i, buildItemDef(itemIt->second.entry, itemIt->second.stackCount, itemIt->second.curDurability, itemIt->second.maxDurability, guid, itemIt->second.flags, itemIt->second.randomPropertyId, itemIt->second.suffixFactor));
    }

    // Bag contents (BAG1-BAG4 are equip slots 19-22)
    for (int bagIdx = 0; bagIdx < 4; bagIdx++) {
        uint64_t bagGuid = owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + bagIdx];
        if (bagGuid == 0) continue;

        // Determine bag size from container fields or item template
        int numSlots = 0;
        auto contIt = owner_.containerContentsRef().find(bagGuid);
        if (contIt != owner_.containerContentsRef().end()) {
            numSlots = static_cast<int>(contIt->second.numSlots);
        }
        const ItemQueryResponseData* bagTemplate = nullptr;
        {
            auto bagItemIt = owner_.onlineItemsRef().find(bagGuid);
            if (bagItemIt != owner_.onlineItemsRef().end()) {
                auto bagInfoIt = owner_.itemInfoCacheRef().find(bagItemIt->second.entry);
                if (bagInfoIt != owner_.itemInfoCacheRef().end())
                    bagTemplate = &bagInfoIt->second;
            }
        }
        if (numSlots <= 0 && bagTemplate) {
            numSlots = bagTemplate->containerSlots;
        }
        if (numSlots <= 0) continue;

        // Set the bag size in the inventory bag data
        owner_.inventoryRef().setBagSize(bagIdx, numSlots);
        // Quivers (class 11) and profession bags (class 1, subclass != 0) only
        // accept their own item type - sorting and the combined grid must know.
        owner_.inventoryRef().setBagSpecial(bagIdx, bagTemplate &&
            (bagTemplate->itemClass == 11 ||
             (bagTemplate->itemClass == 1 && bagTemplate->subClass != 0)));

        // Also set bagSlots on the equipped bag item (for UI display)
        auto& bagEquipSlot = owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIdx));
        if (!bagEquipSlot.empty()) {
            ItemDef bagDef = bagEquipSlot.item;
            bagDef.bagSlots = numSlots;
            owner_.inventoryRef().setEquipSlot(static_cast<EquipSlot>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIdx), bagDef);
        }

        // Populate bag slot items
        if (contIt == owner_.containerContentsRef().end()) continue;
        const auto& container = contIt->second;
        for (int s = 0; s < numSlots && s < 36; s++) {
            uint64_t itemGuid = container.slotGuids[s];
            if (itemGuid == 0) continue;

            auto itemIt = owner_.onlineItemsRef().find(itemGuid);
            if (itemIt == owner_.onlineItemsRef().end()) continue;
            ItemDef def = buildItemDef(itemIt->second.entry, itemIt->second.stackCount, itemIt->second.curDurability, itemIt->second.maxDurability, itemGuid, itemIt->second.flags, itemIt->second.randomPropertyId, itemIt->second.suffixFactor);
            // Bags inside bags need containerSlots for the UI slot-count display.
            auto bagInfoIt = owner_.itemInfoCacheRef().find(itemIt->second.entry);
            if (bagInfoIt != owner_.itemInfoCacheRef().end())
                def.bagSlots = bagInfoIt->second.containerSlots;
            owner_.inventoryRef().setBagSlot(bagIdx, s, def);
        }
    }

    // Bank slots (24 for Classic, 28 for TBC/WotLK)
    for (int i = 0; i < effectiveBankSlots_; i++) {
        uint64_t guid = bankSlotGuids_[i];
        if (guid == 0) { owner_.inventoryRef().clearBankSlot(i); continue; }

        auto itemIt = owner_.onlineItemsRef().find(guid);
        if (itemIt == owner_.onlineItemsRef().end()) { owner_.inventoryRef().clearBankSlot(i); continue; }

        ItemDef def;
        def.itemId = itemIt->second.entry;
        def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
        def.maxStack = 1;

        auto infoIt = owner_.itemInfoCacheRef().find(itemIt->second.entry);
        if (infoIt != owner_.itemInfoCacheRef().end()) {
            def.name = infoIt->second.name;
            def.quality = static_cast<ItemQuality>(infoIt->second.quality);
            def.inventoryType = infoIt->second.inventoryType;
            def.maxStack = std::max(1, infoIt->second.maxStack);
            def.displayInfoId = infoIt->second.displayInfoId;
            def.subclassName = infoIt->second.subclassName;
            def.damageMin = infoIt->second.damageMin;
            def.damageMax = infoIt->second.damageMax;
            def.delayMs = infoIt->second.delayMs;
            def.armor = infoIt->second.armor;
            def.stamina = infoIt->second.stamina;
            def.strength = infoIt->second.strength;
            def.agility = infoIt->second.agility;
            def.intellect = infoIt->second.intellect;
            def.spirit = infoIt->second.spirit;
            def.itemLevel = infoIt->second.itemLevel;
            def.requiredLevel = infoIt->second.requiredLevel;
            def.bindType = infoIt->second.bindType;
            def.description = infoIt->second.description;
            def.startQuestId = infoIt->second.startQuestId;
            def.extraStats.clear();
            for (const auto& es : infoIt->second.extraStats)
                def.extraStats.push_back({.statType = es.statType, .statValue = es.statValue});
            def.sellPrice = infoIt->second.sellPrice;
            def.bagSlots = infoIt->second.containerSlots;
        } else {
            def.name = "Item " + std::to_string(def.itemId);
            queryItemInfo(def.itemId, guid);
        }

        owner_.inventoryRef().setBankSlot(i, def);
    }

    // Bank bag contents (6 for Classic, 7 for TBC/WotLK)
    for (int bagIdx = 0; bagIdx < effectiveBankBagSlots_; bagIdx++) {
        uint64_t bagGuid = bankBagSlotGuids_[bagIdx];
        if (bagGuid == 0) { owner_.inventoryRef().setBankBagSize(bagIdx, 0); continue; }

        int numSlots = 0;
        auto contIt = owner_.containerContentsRef().find(bagGuid);
        if (contIt != owner_.containerContentsRef().end()) {
            numSlots = static_cast<int>(contIt->second.numSlots);
        }

        // Populate the bag item itself (for icon/name in the bank bag equip slot)
        auto bagItemIt = owner_.onlineItemsRef().find(bagGuid);
        if (bagItemIt != owner_.onlineItemsRef().end()) {
            if (numSlots <= 0) {
                auto bagInfoIt = owner_.itemInfoCacheRef().find(bagItemIt->second.entry);
                if (bagInfoIt != owner_.itemInfoCacheRef().end()) {
                    numSlots = bagInfoIt->second.containerSlots;
                }
            }
            ItemDef bagDef;
            bagDef.itemId = bagItemIt->second.entry;
            bagDef.stackCount = 1;
            bagDef.inventoryType = 18; // bag
            auto bagInfoIt = owner_.itemInfoCacheRef().find(bagItemIt->second.entry);
            if (bagInfoIt != owner_.itemInfoCacheRef().end()) {
                bagDef.name = bagInfoIt->second.name;
                bagDef.quality = static_cast<ItemQuality>(bagInfoIt->second.quality);
                bagDef.displayInfoId = bagInfoIt->second.displayInfoId;
                bagDef.bagSlots = bagInfoIt->second.containerSlots;
            } else {
                bagDef.name = "Bag";
                queryItemInfo(bagDef.itemId, bagGuid);
            }
            owner_.inventoryRef().setBankBagItem(bagIdx, bagDef);
        }
        if (numSlots <= 0) continue;

        owner_.inventoryRef().setBankBagSize(bagIdx, numSlots);

        if (contIt == owner_.containerContentsRef().end()) continue;
        const auto& container = contIt->second;
        for (int s = 0; s < numSlots && s < 36; s++) {
            uint64_t itemGuid = container.slotGuids[s];
            if (itemGuid == 0) continue;

            auto itemIt = owner_.onlineItemsRef().find(itemGuid);
            if (itemIt == owner_.onlineItemsRef().end()) continue;

            ItemDef def;
            def.itemId = itemIt->second.entry;
            def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
            def.maxStack = 1;

            auto infoIt = owner_.itemInfoCacheRef().find(itemIt->second.entry);
            if (infoIt != owner_.itemInfoCacheRef().end()) {
                def.name = infoIt->second.name;
                def.quality = static_cast<ItemQuality>(infoIt->second.quality);
                def.inventoryType = infoIt->second.inventoryType;
                def.maxStack = std::max(1, infoIt->second.maxStack);
                def.displayInfoId = infoIt->second.displayInfoId;
                def.subclassName = infoIt->second.subclassName;
                def.damageMin = infoIt->second.damageMin;
                def.damageMax = infoIt->second.damageMax;
                def.delayMs = infoIt->second.delayMs;
                def.armor = infoIt->second.armor;
                def.stamina = infoIt->second.stamina;
                def.strength = infoIt->second.strength;
                def.agility = infoIt->second.agility;
                def.intellect = infoIt->second.intellect;
                def.spirit = infoIt->second.spirit;
                def.itemLevel = infoIt->second.itemLevel;
                def.requiredLevel = infoIt->second.requiredLevel;
                def.sellPrice = infoIt->second.sellPrice;
                def.bindType = infoIt->second.bindType;
                def.description = infoIt->second.description;
                def.startQuestId = infoIt->second.startQuestId;
                def.extraStats.clear();
                for (const auto& es : infoIt->second.extraStats)
                    def.extraStats.push_back({.statType = es.statType, .statValue = es.statValue});
                def.bagSlots = infoIt->second.containerSlots;
            } else {
                def.name = "Item " + std::to_string(def.itemId);
                queryItemInfo(def.itemId, itemGuid);
            }

            owner_.inventoryRef().setBankBagSlot(bagIdx, s, def);
        }
    }

    // Only mark equipment dirty if equipped item displayInfoIds actually changed.
    // Enchants count too: applying a sharpening stone leaves the displayInfoId alone
    // but changes the visual effect hanging off the weapon.
    std::array<uint32_t, 19> currentEquipDisplayIds{};
    std::array<uint64_t, 19> currentEquipEnchantIds{};
    for (int i = 0; i < 19; i++) {
        const auto& slot = owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(i));
        if (!slot.empty()) currentEquipDisplayIds[i] = slot.item.displayInfoId;

        uint64_t guid = owner_.equipSlotGuidsRef()[i];
        if (guid != 0) {
            auto [permEnchantId, tempEnchantId] = owner_.getItemEnchantIds(guid);
            currentEquipEnchantIds[i] =
                (static_cast<uint64_t>(permEnchantId) << 32) | tempEnchantId;
        }
    }
    if (currentEquipDisplayIds != owner_.lastEquipDisplayIdsRef() ||
        currentEquipEnchantIds != lastEquipEnchantIds_) {
        owner_.lastEquipDisplayIdsRef() = currentEquipDisplayIds;
        lastEquipEnchantIds_ = currentEquipEnchantIds;
        owner_.onlineEquipDirtyRef() = true;
        // And say so. The flag is polled by this client's own panels, which is
        // why they redrew and FrameXML's did not - equipping something changes
        // the player's own equipment fields rather than any item or container
        // field, so the BAG_UPDATE the object path sends never fired for it.
        //
        // Both events, because equipping moves an item out of a bag as well as
        // into a slot: the character sheet reads UNIT_INVENTORY_CHANGED and the
        // bags read BAG_UPDATE, and only one of the two is enough to leave the
        // other stale.
        fireBagUpdates();
    }

    LOG_DEBUG("Rebuilt online inventory: equip=", [&](){
        int c = 0; for (auto g : owner_.equipSlotGuidsRef()) if (g) c++; return c;
    }(), " backpack=", [&](){
        int c = 0; for (auto g : owner_.backpackSlotGuidsRef()) if (g) c++; return c;
    }(), " keyring=", [&](){
        int c = 0; for (auto g : owner_.keyringSlotGuidsRef()) if (g) c++; return c;
    }());

    // Reconcile collect-item quest objectives against what the player is
    // actually carrying. In 3.3.5a the server never pushes item objective
    // counts, so this bag-count pass is the only thing that advances "collect
    // N of item" progress when quest items are looted (or removed). Count
    // backpack + the four equipped bags - the same set the server checks at
    // turn-in - summing stacks per item id.
    std::unordered_map<uint32_t, uint32_t> carriedCounts;
    const auto& inv = owner_.inventoryRef();
    for (int i = 0; i < inv.getBackpackSize(); i++) {
        const auto& slot = inv.getBackpackSlot(i);
        if (!slot.empty())
            carriedCounts[slot.item.itemId] += std::max<uint32_t>(1, slot.item.stackCount);
    }
    for (int bagIdx = 0; bagIdx < 4; bagIdx++) {
        int numSlots = inv.getBagSize(bagIdx);
        for (int s = 0; s < numSlots; s++) {
            const auto& slot = inv.getBagSlot(bagIdx, s);
            if (!slot.empty())
                carriedCounts[slot.item.itemId] += std::max<uint32_t>(1, slot.item.stackCount);
        }
    }
    owner_.reconcileQuestItemObjectives(carriedCounts);
    // The ammo slot's picture, from PLAYER_AMMO_ID and what the bags hold of
    // it - after the bags, because the count is theirs.
    // None left in the bags draws the slot empty, as Wow.exe's
    // GetInventoryItemTexture does for a count of zero (0x5E9BC0).
    if (const uint32_t ammoId = getAmmoItemId()) {
        if (const uint32_t carried = inv.countItem(ammoId)) {
            owner_.inventoryRef().setAmmoSlot(buildItemDef(ammoId, carried, 0, 0, 0, 0, 0, 0));
        }
    }
    // Last, with the slots holding what they now hold.
    fireBankSlotEvents();
}

void InventoryHandler::fireBankSlotEvents() {
    if (pendingBankSlotEvents_.empty()) return;
    // Moved out first: the frame redraws from inside this, and a slot that
    // changes again while it does must queue rather than be dropped.
    std::vector<int> slots;
    slots.swap(pendingBankSlotEvents_);
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    if (!owner_.addonEventCallbackRef()) return;
    for (int slot : slots) {
        owner_.addonEventCallbackRef()("PLAYERBANKSLOTS_CHANGED", {std::to_string(slot)});
    }
}

void InventoryHandler::maybeDetectVisibleItemLayout() {
    if (owner_.visibleItemLayoutVerifiedRef()) return;
    if (usesVisibleItemDisplayIds()) {
        std::array<uint32_t, 19> equipDisplayIds = owner_.lastEquipDisplayIdsRef();
        int nonZero = 0;
        for (int i = 0; i < 19; ++i) {
            if (equipDisplayIds[i] == 0) {
                const auto& slot = owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(i));
                if (!slot.empty()) equipDisplayIds[i] = slot.item.displayInfoId;
            }
            if (equipDisplayIds[i] != 0) nonZero++;
        }

        int bestBase = -1;
        int bestStride = 0;
        int bestMatches = 0;
        int bestMismatches = 9999;
        int bestScore = -999999;

        if (nonZero >= 2 && !owner_.lastPlayerFieldsRef().empty()) {
            const uint16_t maxKey = owner_.lastPlayerFieldsRef().back().first;
            const int strides[] = {16, 12, 8, 4, 2, 3, 1};
            for (int stride : strides) {
                for (const auto& [baseIdxU16, _v] : owner_.lastPlayerFieldsRef()) {
                    const int base = static_cast<int>(baseIdxU16);
                    if (base + 18 * stride > static_cast<int>(maxKey)) continue;

                    int matches = 0;
                    int mismatches = 0;
                    for (int s = 0; s < 19; ++s) {
                        uint32_t want = equipDisplayIds[s];
                        if (want == 0) continue;
                        const uint16_t idx = static_cast<uint16_t>(base + s * stride);
                        auto it = owner_.lastPlayerFieldsRef().find(idx);
                        if (it == owner_.lastPlayerFieldsRef().end()) continue;
                        if (it->second == want) {
                            matches++;
                        } else if (it->second != 0) {
                            mismatches++;
                        }
                    }

                    int score = matches * 3 - mismatches * 2;
                    if (score > bestScore ||
                        (score == bestScore && matches > bestMatches) ||
                        (score == bestScore && matches == bestMatches && mismatches < bestMismatches) ||
                        (score == bestScore && matches == bestMatches && mismatches == bestMismatches &&
                         (bestBase < 0 || base < bestBase))) {
                        bestScore = score;
                        bestMatches = matches;
                        bestMismatches = mismatches;
                        bestBase = base;
                        bestStride = stride;
                    }
                }
            }
        }

        bool changed = false;
        if (bestMatches >= 2 && bestBase >= 0 && bestStride > 0 && bestMismatches <= 2) {
            changed = owner_.visibleItemEntryBaseRef() != bestBase ||
                      owner_.visibleItemStrideRef() != bestStride;
            owner_.visibleItemEntryBaseRef() = bestBase;
            owner_.visibleItemStrideRef() = bestStride;
            owner_.visibleItemLayoutVerifiedRef() = true;
            LOG_INFO("Detected TBC PLAYER_VISIBLE_ITEM display layout: base=", bestBase,
                     " stride=", bestStride, " (matches=", bestMatches,
                     " mismatches=", bestMismatches, " score=", bestScore, ")");
        } else {
            // TBC exposes display IDs rather than item entries, but private cores
            // are not perfectly consistent about the base offset. Keep the known
            // 2.4.x fallback active without marking it verified, so later local
            // equipment updates can still detect a better layout.
            const int kTbcFallbackBase = tbcVisibleItemBaseFallback();
            constexpr int kTbcFallbackStride = kTbcVisibleItemStride;
            changed = owner_.visibleItemEntryBaseRef() != kTbcFallbackBase ||
                      owner_.visibleItemStrideRef() != kTbcFallbackStride;
            owner_.visibleItemEntryBaseRef() = kTbcFallbackBase;
            owner_.visibleItemStrideRef() = kTbcFallbackStride;
            static bool loggedProvisionalLayout = false;
            if (!loggedProvisionalLayout) {
                loggedProvisionalLayout = true;
                LOG_INFO("Using provisional TBC PLAYER_VISIBLE_ITEM display layout: base=",
                         kTbcFallbackBase, " stride=", kTbcFallbackStride,
                         " (waiting for local display-ID confirmation)");
            }
            if (visibleEquipmentFieldDiagEnabled()) {
                LOG_INFO("TBC visible layout detection pending: localDisplayIds=", nonZero,
                         " bestBase=", bestBase, " bestStride=", bestStride,
                         " matches=", bestMatches, " mismatches=", bestMismatches,
                         " score=", bestScore);
            }
        }

        if (changed || owner_.visibleItemLayoutVerifiedRef()) {
            for (const auto& [guid, ent] : owner_.getEntityManager().getEntities()) {
                if (!ent || ent->getType() != ObjectType::PLAYER) continue;
                if (guid == owner_.getPlayerGuid()) continue;
                updateOtherPlayerVisibleItems(guid, ent->getFields());
            }
        }
        return;
    }
    if (owner_.lastPlayerFieldsRef().empty()) return;

    if (isActiveExpansion("tbc")) {
        owner_.visibleItemEntryBaseRef() = tbcVisibleItemBaseFallback();
        owner_.visibleItemStrideRef() = kTbcVisibleItemStride;
    }

    std::array<uint32_t, 19> equipEntries{};
    int nonZero = 0;
    // Prefer authoritative equipped item entry IDs derived from item objects (onlineItems_),
    // because Inventory::ItemDef may not be populated yet if templates haven't been queried.
    for (int i = 0; i < 19; i++) {
        uint64_t itemGuid = owner_.equipSlotGuidsRef()[i];
        if (itemGuid != 0) {
            auto it = owner_.onlineItemsRef().find(itemGuid);
            if (it != owner_.onlineItemsRef().end() && it->second.entry != 0) {
                equipEntries[i] = it->second.entry;
            }
        }
        if (equipEntries[i] == 0) {
            const auto& slot = owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(i));
            equipEntries[i] = slot.empty() ? 0u : slot.item.itemId;
        }
        if (equipEntries[i] != 0) nonZero++;
    }
    if (nonZero < 2) return;

    const uint16_t maxKey = owner_.lastPlayerFieldsRef().back().first;
    int bestBase = -1;
    int bestStride = 0;
    int bestMatches = 0;
    int bestMismatches = 9999;
    int bestScore = -999999;

    const std::array<int, 5> strides = isActiveExpansion("tbc")
        ? std::array<int, 5>{16, 2, 3, 4, 1}
        : std::array<int, 5>{2, 3, 4, 1, 16};
    for (int stride : strides) {
        for (const auto& [baseIdxU16, _v] : owner_.lastPlayerFieldsRef()) {
            const int base = static_cast<int>(baseIdxU16);
            if (base + 18 * stride > static_cast<int>(maxKey)) continue;

            int matches = 0;
            int mismatches = 0;
            for (int s = 0; s < 19; s++) {
                uint32_t want = equipEntries[s];
                if (want == 0) continue;
                const uint16_t idx = static_cast<uint16_t>(base + s * stride);
                auto it = owner_.lastPlayerFieldsRef().find(idx);
                if (it == owner_.lastPlayerFieldsRef().end()) continue;
                if (it->second == want) {
                    matches++;
                } else if (it->second != 0) {
                    mismatches++;
                }
            }

            int score = matches * 2 - mismatches * 3;
            if (score > bestScore ||
                (score == bestScore && matches > bestMatches) ||
                (score == bestScore && matches == bestMatches && mismatches < bestMismatches) ||
                (score == bestScore && matches == bestMatches && mismatches == bestMismatches && base < bestBase)) {
                bestScore = score;
                bestMatches = matches;
                bestMismatches = mismatches;
                bestBase = base;
                bestStride = stride;
            }
        }
    }

    if (bestMatches >= 2 && bestBase >= 0 && bestStride > 0 && bestMismatches <= 1) {
        owner_.visibleItemEntryBaseRef() = bestBase;
        owner_.visibleItemStrideRef() = bestStride;
        owner_.visibleItemLayoutVerifiedRef() = true;
        LOG_INFO("Detected PLAYER_VISIBLE_ITEM entry layout: base=", owner_.visibleItemEntryBaseRef(),
                 " stride=", owner_.visibleItemStrideRef(), " (matches=", bestMatches,
                 " mismatches=", bestMismatches, " score=", bestScore, ")");

        // Backfill existing player entities already in view.
        for (const auto& [guid, ent] : owner_.getEntityManager().getEntities()) {
            if (!ent || ent->getType() != ObjectType::PLAYER) continue;
            if (guid == owner_.getPlayerGuid()) continue;
            updateOtherPlayerVisibleItems(guid, ent->getFields());
        }
    }
    // If heuristic didn't find a match, keep using the default WotLK layout (base=284, stride=2).
}

void InventoryHandler::updateOtherPlayerVisibleItems(uint64_t guid, const FlatFieldMap& fields) {
    if (guid == 0 || guid == owner_.getPlayerGuid()) return;

    // Use the current base/stride (defaults are correct for WotLK 3.3.5a: base=284, stride=2).
    // The heuristic may refine these later, but we proceed immediately with whatever values
    // are set rather than waiting for verification.
    const bool valuesAreDisplayIds = usesVisibleItemDisplayIds();
    int base = owner_.visibleItemEntryBaseRef();
    int stride = owner_.visibleItemStrideRef();
    if (isActiveExpansion("tbc") && !owner_.visibleItemLayoutVerifiedRef()) {
        const int kTbcFallbackBase = tbcVisibleItemBaseFallback();
        constexpr int kTbcFallbackStride = kTbcVisibleItemStride;
        if (base != kTbcFallbackBase || stride != kTbcFallbackStride) {
            base = kTbcFallbackBase;
            stride = kTbcFallbackStride;
            owner_.visibleItemEntryBaseRef() = base;
            owner_.visibleItemStrideRef() = stride;
        }
    }
    if (base < 0 || stride <= 0) return; // Defensive: should never happen with defaults.

    std::array<uint32_t, 19> newEntries{};
    for (int s = 0; s < 19; s++) {
        uint16_t idx = static_cast<uint16_t>(base + s * stride);
        auto it = fields.find(idx);
        if (it != fields.end()) newEntries[s] = it->second;
    }

    int nonZero = 0;
    for (uint32_t e : newEntries) { if (e != 0) nonZero++; }
    const bool hasVisibleBodySlot =
        newEntries[2] != 0 ||  // shoulders
        newEntries[4] != 0 ||  // chest
        newEntries[6] != 0 ||  // legs
        newEntries[7] != 0 ||  // feet
        newEntries[9] != 0 ||  // hands
        newEntries[15] != 0 || // main hand
        newEntries[16] != 0;   // off hand
    const bool sparseTbcVisible =
        valuesAreDisplayIds &&
        (nonZero == 0 || nonZero < 4 || (!hasVisibleBodySlot && nonZero < 8));
    const bool hasInspectedEntries =
        owner_.inspectedPlayerItemEntriesRef().find(guid) != owner_.inspectedPlayerItemEntriesRef().end();

    // Dump raw fields around visible item range to find the correct offset
    static int dumpCount = 0;
    if (dumpCount < 3 && fields.size() > 20) {
        dumpCount++;
        std::string dump;
        for (const auto& [idx, val] : fields) {
            if (idx >= 270 && idx <= 340 && val != 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), " [%u]=%u", idx, val);
                dump += buf;
            }
        }
        LOG_DEBUG("RAW FIELDS 270-340:", dump);
    }
    static int diagDumpCount = 0;
    if (visibleEquipmentFieldDiagEnabled() && diagDumpCount < 8 && fields.size() > 20) {
        diagDumpCount++;
        std::ostringstream dump;
        int emitted = 0;
        uint16_t maxField = 0;
        for (const auto& [idx, val] : fields) {
            if (idx > maxField) maxField = idx;
            if (idx < 220 || idx > 760 || val == 0) continue;
            dump << " [" << idx << "]=" << val;
            if (++emitted >= 180) {
                dump << " ...";
                break;
            }
        }
        LOG_INFO("VISIBLE EQUIP FIELD DIAG guid=0x", std::hex, guid, std::dec,
                 " fieldCount=", fields.size(), " maxField=", maxField,
                 " base=", base, " stride=", stride,
                 " verified=", owner_.visibleItemLayoutVerifiedRef(),
                 " nonZero[220-760]=", dump.str());
    }

    if (nonZero > 0) {
        LOG_DEBUG("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                 " nonZero=", nonZero, " base=", base, " stride=", stride,
                 " values=", (valuesAreDisplayIds ? "displayIds" : "itemEntries"),
                 " head=", newEntries[0], " shoulders=", newEntries[2],
                 " chest=", newEntries[4], " legs=", newEntries[6],
                 " mainhand=", newEntries[15], " offhand=", newEntries[16]);
    }

    if (sparseTbcVisible) {
        if (!hasInspectedEntries && owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
            owner_.pendingAutoInspectRef().insert(guid);
            LOG_DEBUG("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                      " sparse TBC visible fields (nonZero=", nonZero,
                      ", base=", base, ", stride=", stride,
                      ") - queuing auto-inspect");
        } else if (hasInspectedEntries) {
            LOG_DEBUG("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                      " sparse TBC visible fields ignored; inspect gear already cached");
        }
    }

    bool changed = false;
    auto& old = owner_.otherPlayerVisibleItemEntriesRef()[guid];
    if (old != newEntries) {
        old = newEntries;
        changed = true;
    }
    // The weapon slots' enchant half (WotLK: entry, then enchantment, so the
    // field after each entry). Stock picks the glow from it (0x759050) and
    // refreshes the glows when only it changes (0x6E1E10 -> 0x6D6C10).
    if (stride == 2 && !valuesAreDisplayIds) {
        std::array<uint32_t, 3> enchants{};
        for (int s = 15; s <= 17; ++s) {
            auto it = fields.find(static_cast<uint16_t>(base + s * stride + 1));
            if (it != fields.end()) enchants[s - 15] = it->second;
        }
        auto& oldEnchants = owner_.otherPlayerWeaponEnchantsRef()[guid];
        if (oldEnchants != enchants) {
            oldEnchants = enchants;
            changed = true;
        }
    }

    if (valuesAreDisplayIds) {
        if (sparseTbcVisible && hasInspectedEntries) {
            return;
        }
        if (changed) {
            owner_.otherPlayerVisibleDirtyRef().insert(guid);
            emitOtherPlayerEquipment(guid);
        }
        return;
    }

    // Request item templates for any new visible entries.
    for (uint32_t entry : newEntries) {
        if (entry == 0) continue;
        if (!owner_.itemInfoCacheRef().count(entry) && !owner_.pendingItemQueriesRef().count(entry)) {
            queryItemInfo(entry, 0);
        }
    }

    // Only fall back to auto-inspect if ALL extracted entries are zero (server didn't
    // send visible item fields at all). If we got at least one non-zero entry, the
    // update-field approach is working and inspect is unnecessary.
    if (nonZero == 0) {
        LOG_DEBUG("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                  " all entries zero (base=", base, " stride=", stride,
                  " fieldCount=", fields.size(), ") - queuing auto-inspect");
        if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
            owner_.pendingAutoInspectRef().insert(guid);
        }
    }

    if (changed) {
        owner_.otherPlayerVisibleDirtyRef().insert(guid);
        emitOtherPlayerEquipment(guid);
        // The squares of an open inspect window are read from these fields;
        // a piece swapped while it is open should show as swapped.
        fireUnitInventoryChangedFor(owner_, guid);
    }
}

void InventoryHandler::cacheInspectedPlayerEquipment(uint64_t guid, const std::array<uint32_t, 19>& itemEntries) {
    if (guid == 0) return;

    owner_.inspectedPlayerItemEntriesRef()[guid] = itemEntries;

    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> invTypes{};
    int entries = 0;
    int resolved = 0;

    for (int s = 0; s < 19; ++s) {
        const uint32_t entry = itemEntries[s];
        if (entry == 0) continue;
        entries++;

        auto infoIt = owner_.itemInfoCacheRef().find(entry);
        if (infoIt != owner_.itemInfoCacheRef().end()) {
            displayIds[s] = infoIt->second.displayInfoId;
            invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
            resolved++;
            continue;
        }

        queryItemInfo(entry, 0);
    }

    LOG_DEBUG("cacheInspectedPlayerEquipment: guid=0x", std::hex, guid, std::dec,
              " entries=", entries, " resolved=", resolved,
              " head=", displayIds[0], " shoulders=", displayIds[2],
              " chest=", displayIds[4], " legs=", displayIds[6],
              " mainhand=", displayIds[15], " offhand=", displayIds[16]);

    if (resolved > 0 && owner_.playerEquipmentCallbackRef()) {
        owner_.playerEquipmentCallbackRef()(guid, displayIds, invTypes);
    }
}

/// What another player is visibly wearing, without announcing it.
///
/// emitOtherPlayerEquipment pushes this to whoever registered the callback,
/// which suits the world - a spawn happens and the model is dressed. A
/// portrait asks the other way round: it is drawn for whichever unit a frame
/// has claimed, at a moment nothing has just changed. Same resolution, so it
/// is the same code rather than a second reading of the same two layouts.
///
/// False when nothing is known yet, which is different from "wearing nothing"
/// - the difference between leaving a model as it is and stripping it.
bool InventoryHandler::resolvePlayerEquipment(
        std::array<uint32_t, 19>& displayIds,
        std::array<uint8_t, 19>& invTypes) const {
    displayIds = {};
    invTypes = {};
    int resolved = 0;
    // The nineteen equipment slots, in the order the visible-item fields use -
    // which is the order the slots themselves are in.
    for (int slot = 0; slot < 19; ++slot) {
        const auto& equipped =
            owner_.inventoryRef().getEquipSlot(static_cast<EquipSlot>(slot));
        if (equipped.empty()) continue;
        // The item as the inventory holds it, and the template behind it when
        // the inventory's copy has not been filled in yet: an item known only
        // by its entry until its query comes back is the ordinary case on the
        // first frames after login.
        uint32_t displayId = equipped.item.displayInfoId;
        uint8_t invType = equipped.item.inventoryType;
        if (displayId == 0) {
            auto infoIt = owner_.itemInfoCacheRef().find(equipped.item.itemId);
            if (infoIt == owner_.itemInfoCacheRef().end()) continue;
            displayId = infoIt->second.displayInfoId;
            invType = static_cast<uint8_t>(infoIt->second.inventoryType);
        }
        if (displayId == 0) continue;
        displayIds[static_cast<size_t>(slot)] = displayId;
        invTypes[static_cast<size_t>(slot)] = invType;
        ++resolved;
    }
    return resolved > 0;
}

bool InventoryHandler::resolveOtherPlayerEquipment(
        uint64_t guid, std::array<uint32_t, 19>& displayIds,
        std::array<uint8_t, 19>& invTypes) const {
    displayIds = {};
    invTypes = {};
    // This character, out of this character's own inventory.
    //
    // The table below is built from other players' visible-item fields and
    // updateOtherPlayerVisibleItems refuses the player's own guid outright, so
    // asking it about the player answered "nothing known" - and every portrait
    // drawn from that answer put the player in nothing but their skin. That is
    // what targeting yourself showed: the player frame draws from the character
    // list and was dressed, and the target frame drew the same character bare.
    if (guid != 0 && guid == owner_.getPlayerGuid()) {
        return resolvePlayerEquipment(displayIds, invTypes);
    }
    auto it = owner_.otherPlayerVisibleItemEntriesRef().find(guid);
    if (it == owner_.otherPlayerVisibleItemEntriesRef().end()) return false;

    if (usesVisibleItemDisplayIds()) {
        displayIds = it->second;
        invTypes = inferredVisibleInventoryTypes();
        for (uint32_t displayId : displayIds) {
            if (displayId != 0) return true;
        }
        return false;
    }

    bool anyEntry = false;
    int resolved = 0;
    for (int s = 0; s < 19; s++) {
        const uint32_t entry = it->second[s];
        if (entry == 0) continue;
        anyEntry = true;
        auto infoIt = owner_.itemInfoCacheRef().find(entry);
        if (infoIt == owner_.itemInfoCacheRef().end()) continue;
        displayIds[s] = infoIt->second.displayInfoId;
        invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
        resolved++;
    }
    // Entries with nothing resolved is "the item queries have not come back",
    // not "bare". Answering true there would dress the model in nothing.
    return !anyEntry || resolved > 0;
}

void InventoryHandler::emitOtherPlayerEquipment(uint64_t guid) {
    if (!owner_.playerEquipmentCallbackRef()) return;
    auto it = owner_.otherPlayerVisibleItemEntriesRef().find(guid);
    if (it == owner_.otherPlayerVisibleItemEntriesRef().end()) return;

    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> invTypes{};
    if (usesVisibleItemDisplayIds()) {
        displayIds = it->second;
        invTypes = inferredVisibleInventoryTypes();
        int nonZeroDisplay = 0;
        for (uint32_t displayId : displayIds) {
            if (displayId != 0) nonZeroDisplay++;
        }

        LOG_DEBUG("emitOtherPlayerEquipment: guid=0x", std::hex, guid, std::dec,
                 " TBC displayIds=", nonZeroDisplay,
                 " head=", displayIds[0], " shoulders=", displayIds[2],
                 " chest=", displayIds[4], " legs=", displayIds[6],
                 " mainhand=", displayIds[15], " offhand=", displayIds[16]);

        if (nonZeroDisplay == 0) return;
        owner_.playerEquipmentCallbackRef()(guid, displayIds, invTypes);
        owner_.otherPlayerVisibleDirtyRef().erase(guid);
        return;
    }

    bool anyEntry = false;
    int resolved = 0, unresolved = 0;

    for (int s = 0; s < 19; s++) {
        uint32_t entry = it->second[s];
        if (entry == 0) continue;
        anyEntry = true;
        auto infoIt = owner_.itemInfoCacheRef().find(entry);
        if (infoIt == owner_.itemInfoCacheRef().end()) { unresolved++; continue; }
        displayIds[s] = infoIt->second.displayInfoId;
        invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
        resolved++;
    }

    LOG_DEBUG("emitOtherPlayerEquipment: guid=0x", std::hex, guid, std::dec,
             " entries=", (anyEntry ? "yes" : "none"),
             " resolved=", resolved, " unresolved=", unresolved,
             " head=", displayIds[0], " shoulders=", displayIds[2],
             " chest=", displayIds[4], " legs=", displayIds[6],
             " mainhand=", displayIds[15], " offhand=", displayIds[16]);

    // Don't emit all-zero displayIds - that strips existing equipment for no reason.
    // Wait until at least one item resolves before applying.
    if (anyEntry && resolved == 0) {
        LOG_DEBUG("emitOtherPlayerEquipment: skipping all-zero emit (waiting for item queries)");
        return;
    }

    owner_.playerEquipmentCallbackRef()(guid, displayIds, invTypes);
    owner_.otherPlayerVisibleDirtyRef().erase(guid);

    // If we had entries but couldn't resolve any templates, also try inspect as a fallback.
    if (anyEntry && !resolved) {
        owner_.pendingAutoInspectRef().insert(guid);
    }
}

void InventoryHandler::emitAllOtherPlayerEquipment() {
    if (!owner_.playerEquipmentCallbackRef()) return;
    for (const auto& [guid, _] : owner_.otherPlayerVisibleItemEntriesRef()) {
        emitOtherPlayerEquipment(guid);
    }
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void InventoryHandler::handleTrainerBuySucceeded(network::Packet& packet) {
    /*uint64_t guid =*/ packet.readUInt64();
    uint32_t spellId = packet.readUInt32();
    if (owner_.getSpellHandler() && !owner_.getSpellHandler()->hasKnownSpell(spellId)) {
        owner_.getSpellHandler()->addKnownSpell(spellId);
    }
    // No chat line here. AzerothCore's Trainer::TeachSpell learns the spell
    // (SMSG_LEARNED_SPELL, whose handler prints the stock "You have learned a
    // new spell: X.") before it sends this, so a second "You have learned X."
    // was printed for every purchase. The stock client says nothing on this
    // message either.
    //
    // No sound here. The learning sound is the server's: AzerothCore's
    // Trainer::TeachSpell sends SMSG_PLAY_SPELL_IMPACT with SpellVisualKit 362
    // ("Learn Impact Base", SoundEntries 1455 SealofMight.wav) on the player
    // before this packet, and the spell visual system plays that kit with its
    // sound. Castable trainer spells add their own SMSG_SPELL_GO visual. The
    // trainer frame itself plays nothing on a purchase. This used to also play
    // iQuestActivate.wav, which is the quest-accept sound, on top of it.
    owner_.fireAddonEvent("TRAINER_UPDATE", {});
    owner_.fireAddonEvent("SPELLS_CHANGED", {});
}

void InventoryHandler::handleTrainerBuyFailed(network::Packet& packet) {
    /*uint64_t trainerGuid =*/ packet.readUInt64();
    uint32_t spellId = packet.readUInt32();
    uint32_t errorCode = 0;
    if (packet.hasRemaining(4))
        errorCode = packet.readUInt32();
    // The console alone, as Wow.exe (0x6D8410) has it - "Trainer service %d
    // unavailable", "Not enough money for ...", "Not enough skill points
    // for ..." - and nothing on screen or in chat.
    static const char* const kConsole[] = {"Trainer service %u unavailable",
                                           "Not enough money for trainer service %u",
                                           "Not enough skill points for trainer service %u"};
    if (errorCode < 3) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), kConsole[errorCode], spellId);
        LOG_INFO("[console] ", buf);
    }
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void InventoryHandler::initiateTrade(uint64_t targetGuid) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot initiate trade: not in world or not connected");
        return;
    }

    if (targetGuid == 0) {
        owner_.raiseUiError("You must target a player to trade with.");
        return;
    }

    auto packet = InitiateTradePacket::build(targetGuid);
    owner_.getSocket()->send(packet);
    // The asker's side learns the partner from nothing else: BEGIN_TRADE
    // (which carries the guid) goes only to the player asked, and the
    // OPEN_WINDOW we get back carries a trade id. Without it the trade window
    // on this side had no "npc" to name or draw.
    tradePeerGuid_ = targetGuid;
    tradePeerName_ = owner_.lookupName(targetGuid);
    owner_.displayGameMessage("ERR_INITIATE_TRADE_S",
                              "You have requested to trade with " + tradePeerName_ + ".",
                              {tradePeerName_});
    LOG_INFO("Initiated trade with target: 0x", std::hex, targetGuid, std::dec);
}

uint32_t InventoryHandler::getTempEnchantRemainingMs(uint32_t slot) const {
    uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (const auto& t : owner_.tempEnchantTimersRef()) {
        if (t.slot == slot) {
            return (t.expireMs > nowMs)
                ? static_cast<uint32_t>(t.expireMs - nowMs) : 0u;
        }
    }
    return 0u;
}

void InventoryHandler::addMoneyCopper(uint32_t amount) {
    if (amount == 0) return;
    owner_.playerMoneyCopperRef() += amount;
    const auto coins = game::splitCopper(amount);
    const uint32_t gold = coins.gold;
    const uint32_t silver = coins.silver;
    const uint32_t copper = coins.copper;
    std::string msg = "You receive ";
    msg += std::to_string(gold) + "g ";
    msg += std::to_string(silver) + "s ";
    msg += std::to_string(copper) + "c.";
    // As money, not as a system line. Both were added because the typed one
    // raised in the interface's handler and never appeared - see
    // ChatHandler::addLocalChatLine.
    owner_.addLocalChatLine(ChatType::MONEY, msg);
}

// ============================================================
// Repair cost estimation from DBC data
// ============================================================

void InventoryHandler::loadRepairDbc() const {
    if (repairDbcLoaded_) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    repairDbcLoaded_ = true;

    // DurabilityCosts.dbc: field 0 = itemLevel (key), fields 1-29 = cost multipliers
    // Columns 1-21 = weapon subclass (0-20), columns 22-29 = armor subclass (0-7)
    auto costsDbc = am->loadDBC("DurabilityCosts.dbc");
    if (costsDbc && costsDbc->isLoaded()) {
        uint32_t count = costsDbc->getRecordCount();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t itemLevel = costsDbc->getUInt32(i, 0);
            std::array<uint32_t, 29> mults{};
            for (uint32_t f = 0; f < 29; ++f)
                mults[f] = costsDbc->getUInt32(i, f + 1);
            durabilityCosts_[itemLevel] = mults;
        }
    }

    // DurabilityQuality.dbc: field 0 = id (key), field 1 = quality_mod (float)
    auto qualDbc = am->loadDBC("DurabilityQuality.dbc");
    if (qualDbc && qualDbc->isLoaded()) {
        uint32_t count = qualDbc->getRecordCount();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = qualDbc->getUInt32(i, 0);
            float mod = qualDbc->getFloat(i, 1);
            durabilityQuality_[id] = mod;
        }
    }
}

uint32_t InventoryHandler::estimateItemRepairCost(uint64_t itemGuid) const {
    auto itemIt = owner_.onlineItemsRef().find(itemGuid);
    if (itemIt == owner_.onlineItemsRef().end()) return 0;
    const auto& item = itemIt->second;

    if (item.maxDurability == 0 || item.curDurability >= item.maxDurability) return 0;
    uint32_t lostDur = item.maxDurability - item.curDurability;

    auto infoIt = owner_.itemInfoCacheRef().find(item.entry);
    if (infoIt == owner_.itemInfoCacheRef().end()) return 0;
    const auto& info = infoIt->second;

    loadRepairDbc();

    // Look up DurabilityCosts multiplier for this itemLevel
    auto costIt = durabilityCosts_.find(info.itemLevel);
    if (costIt == durabilityCosts_.end()) return 0;

    // Determine column index: weapon (class=2) uses subClass directly,
    // armor (class=4) uses subClass + 21
    uint32_t colIndex = 0;
    if (info.itemClass == 2) { // ITEM_CLASS_WEAPON
        colIndex = info.subClass;
    } else if (info.itemClass == 4) { // ITEM_CLASS_ARMOR
        colIndex = info.subClass + 21;
    } else {
        return 0; // only weapons and armor have durability
    }
    if (colIndex >= 29) return 0;

    uint32_t dmultiplier = costIt->second[colIndex];
    if (dmultiplier == 0) return 0;

    // Quality modifier lookup: index is (quality + 1) * 2
    uint32_t qualIndex = (info.quality + 1) * 2;
    auto qualIt = durabilityQuality_.find(qualIndex);
    if (qualIt == durabilityQuality_.end()) return 0;
    float qualMod = qualIt->second;

    uint32_t cost = static_cast<uint32_t>(lostDur * dmultiplier * qualMod);
    if (cost == 0 && lostDur > 0) cost = 1; // minimum 1 copper
    return cost;
}

std::vector<uint64_t> InventoryHandler::repairAllItemGuids() const {
    // What Player::DurabilityRepairAll walks (AzerothCore Player.cpp): the
    // equipment, backpack and worn-bag slots of bag 0, then the contents of
    // the four worn bags - "bank, buyback and keys not repaired". Every item
    // the client knew of was counted before, so a damaged sword in the bank
    // inflated the Repair All price and was "repaired" on screen too.
    std::vector<uint64_t> out;
    for (uint64_t g : owner_.equipSlotGuidsRef()) if (g) out.push_back(g);
    for (uint64_t g : owner_.backpackSlotGuidsRef()) if (g) out.push_back(g);
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b) {
        const uint64_t bagGuid =
            owner_.equipSlotGuidsRef()[Inventory::FIRST_BAG_EQUIP_SLOT + b];
        if (bagGuid == 0) continue;
        auto it = owner_.containerContentsRef().find(bagGuid);
        if (it == owner_.containerContentsRef().end()) continue;
        for (uint32_t s = 0; s < it->second.numSlots && s < it->second.slotGuids.size(); ++s) {
            if (it->second.slotGuids[s]) out.push_back(it->second.slotGuids[s]);
        }
    }
    return out;
}

uint32_t InventoryHandler::estimateRepairAllCost() const {
    uint32_t total = 0;
    for (uint64_t guid : repairAllItemGuids()) {
        total += estimateItemRepairCost(guid);
    }
    return total;
}

} // namespace game
} // namespace wowee
