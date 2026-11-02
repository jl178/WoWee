// The stock client's own interaction rules: reach, the packet a right-click
// sends, and the cursor. Each case is one the binary decides differently from
// what Wowee did before (docs/re/interactions.md).
#include <catch_amalgamated.hpp>

#include "game/interaction_rules.hpp"

using namespace wowee::game::interaction;

TEST_CASE("NPC reach is its combat reach plus four yards") {
    CHECK(npcInteractRange(1.5f) == Catch::Approx(5.5f));
    // A big NPC is spoken to from further out; the player's reach plays no part.
    CHECK(npcInteractRange(6.0f) == Catch::Approx(10.0f));
}

TEST_CASE("melee and corpse reach never drop under five yards") {
    CHECK(meleeReach(1.5f, 1.5f) == Catch::Approx(5.0f));
    CHECK(meleeReach(1.5f, 4.0f) == Catch::Approx(5.5f + 4.0f / 3.0f));
}

TEST_CASE("game object use range comes from the type handler") {
    CHECK(gameObjectUseRange(3) == Catch::Approx(5.0f));    // chest
    CHECK(gameObjectUseRange(2) == Catch::Approx(5.5556f)); // questgiver
    CHECK(gameObjectUseRange(4) == Catch::Approx(10.0f));   // binder
    CHECK(gameObjectUseRange(7) == Catch::Approx(3.0f));    // chair
    CHECK(gameObjectUseRange(17) == Catch::Approx(100.0f)); // bobber
}

TEST_CASE("a mailbox is used from the saddle, a barber's chair never") {
    CHECK(gameObjectAllowsMounted(19, nullptr));
    uint32_t data[24] = {};
    data[8] = 1;
    CHECK_FALSE(gameObjectAllowsMounted(32, data));
    CHECK(gameObjectAllowsMounted(2, data));        // questgiver data8
    CHECK_FALSE(gameObjectAllowsMounted(3, data));  // a chest has no such field
}

TEST_CASE("one right-click sends one packet, gossip first") {
    // A vendor that also gossips is greeted; the menu offers the wares.
    CHECK(npcHelloFor(kNpcGossip | kNpcVendor, 0) == NpcHello::Gossip);
    // A plain vendor gets CMSG_LIST_INVENTORY, not a gossip hello.
    CHECK(npcHelloFor(kNpcVendor | kNpcRepair, 0) == NpcHello::Vendor);
    // A quest giver with something to offer outranks its other services.
    CHECK(npcHelloFor(kNpcQuestGiver | kNpcTrainer, 8) == NpcHello::QuestGiver);
    CHECK(npcHelloFor(kNpcQuestGiver | kNpcTrainer, 0) == NpcHello::Trainer);
    CHECK(npcHelloFor(kNpcFlightMaster | kNpcVendor, 0) == NpcHello::Taxi);
    CHECK(npcHelloFor(0, 0) == NpcHello::None);
}

TEST_CASE("an NPC's cursor says what it is for, and 'Unable' out of reach") {
    UnitCursorInput in;
    in.npcInteractable = true;
    in.npcFlags = kNpcVendor;
    CHECK(unitCursorName(in) == "Pickup");
    in.npcFlags = kNpcVendor | kNpcRepair;
    CHECK(unitCursorName(in) == "RepairNPC");
    in.npcFlags = kNpcBanker;
    CHECK(unitCursorName(in) == "Buy");
    in.npcFlags = kNpcGossip | kNpcFlightMaster;
    CHECK(unitCursorName(in) == "Taxi");
    in.npcOutOfRange = true;
    CHECK(unitCursorName(in) == "UnableTaxi");
    // The quest status comes first, then the template's icon.
    in.questGiverStatus = 10;
    CHECK(unitCursorName(in) == "UnableQuestTurnIn");
    in.questGiverStatus = 0;
    in.iconName = "Directions";
    CHECK(unitCursorName(in) == "UnableDirections");
}

TEST_CASE("corpses, skinning and enemies") {
    UnitCursorInput in;
    in.lootable = true;
    in.canLootNow = true;
    CHECK(unitCursorName(in) == "Pickup");
    in.autoLoot = true;
    CHECK(unitCursorName(in) == "LootAll");
    in.canLootNow = false;
    CHECK(unitCursorName(in) == "UnableLootAll");

    UnitCursorInput skin;
    skin.skinnable = true;
    CHECK(unitCursorName(skin) == "Skin");
    skin.creatureTypeFlags = 0x200;
    skin.skinOutOfRange = true;
    CHECK(unitCursorName(skin) == "UnableMine");

    UnitCursorInput foe;
    foe.attackable = true;
    CHECK(unitCursorName(foe) == "Attack");
    foe.outOfMeleeRange = true;
    CHECK(unitCursorName(foe) == "UnableAttack");
    foe.pacified = true;
    CHECK(unitCursorName(foe).empty());
}

TEST_CASE("a game object's cursor: icon, lock, type, and unable out of reach") {
    GameObjectCursorInput in;
    in.interactable = true;
    in.type = 3;
    in.usableNow = true;
    CHECK(gameObjectCursorName(in) == "Interact");
    in.usableNow = false;
    CHECK(gameObjectCursorName(in) == "UnableInteract");

    in.lockCursor = "GatherHerbs";
    in.lockTypeId = 2;
    CHECK(gameObjectCursorName(in) == "UnableGatherHerbs");
    in.lockCursor = "PickLock";
    in.lockTypeId = 1;
    CHECK(gameObjectCursorName(in) == "PickLock");  // lockpicking never says unable

    GameObjectCursorInput mail;
    mail.interactable = true;
    mail.type = 19;
    mail.usableNow = true;
    CHECK(gameObjectCursorName(mail) == "Mail");

    GameObjectCursorInput icon;
    icon.interactable = true;
    icon.type = 10;
    icon.iconName = "Gunner";
    CHECK(gameObjectCursorName(icon) == "UnableGunner");

    GameObjectCursorInput scenery;
    scenery.type = 5;
    CHECK(gameObjectCursorName(scenery).empty());

    CHECK(cursorPath("UnableMail") == "Interface\\Cursor\\UnableMail.blp");
}
