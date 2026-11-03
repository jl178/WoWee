#include <catch_amalgamated.hpp>

#include <string>

#include "game/pet_action.hpp"

using namespace wowee::game::pet;

// The bug these cover: dismiss packed action 0 under the command type, which is
// COMMAND_STAY, so the pet planted itself instead of leaving. The ids overlap
// between the two built-in types, so a test that only checks the low bits would
// have passed on the broken encoding too - each case pins both halves.

TEST_CASE("pet action packs type into the high byte", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) == 0x07000003u);
    REQUIRE(packPetAction(ActionType::Command, kStay)    == 0x07000000u);
    REQUIRE(packPetAction(ActionType::Command, kFollow)  == 0x07000001u);
    REQUIRE(packPetAction(ActionType::Command, kAttack)  == 0x07000002u);

    REQUIRE(packPetAction(ActionType::Reaction, kPassive)    == 0x06000000u);
    REQUIRE(packPetAction(ActionType::Reaction, kDefensive)  == 0x06000001u);
    REQUIRE(packPetAction(ActionType::Reaction, kAggressive) == 0x06000002u);
}

TEST_CASE("dismiss is not stay", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) !=
            packPetAction(ActionType::Command, kStay));
}

TEST_CASE("built-in ids collide across types and are told apart by type", "[pet]") {
    const uint32_t follow    = packPetAction(ActionType::Command,  kFollow);
    const uint32_t defensive = packPetAction(ActionType::Reaction, kDefensive);

    // Same low bits - the id alone cannot distinguish them.
    REQUIRE(petActionId(follow) == petActionId(defensive));
    REQUIRE(follow != defensive);
    REQUIRE(petActionType(follow) == ActionType::Command);
    REQUIRE(petActionType(defensive) == ActionType::Reaction);
}

TEST_CASE("round trip through unpack", "[pet]") {
    for (uint32_t id : {kStay, kFollow, kAttack, kAbandon, kMoveTo}) {
        const uint32_t packed = packPetAction(ActionType::Command, id);
        REQUIRE(petActionId(packed) == id);
        REQUIRE(petActionType(packed) == ActionType::Command);
    }
}

TEST_CASE("only castable slots count as spells", "[pet]") {
    // Commands and stances are not spells, whatever their id - the old test for
    // this was "id > 6", which called stay/follow/attack spells the moment the
    // server sent them with their real ids of 0/1/2.
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kStay)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kAttack)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Reaction, kPassive)));

    REQUIRE(isPetSpellAction(packPetAction(ActionType::Disabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Enabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Passive, 3110u)));
}

TEST_CASE("a spell id larger than 24 bits cannot corrupt the type", "[pet]") {
    const uint32_t packed = packPetAction(ActionType::Enabled, 0xFFFFFFFFu);
    REQUIRE(petActionType(packed) == ActionType::Enabled);
    REQUIRE(petActionId(packed) == 0x00FFFFFFu);
}

// ---- The stock bookkeeping (docs/re/pets.md) ----

TEST_CASE("slot kinds fold the autocast bits away", "[pet]") {
    REQUIRE(slotKind(0x01000011u) == 1);   // ACT_PASSIVE
    REQUIRE(slotKind(0x81000011u) == 1);   // ACT_DISABLED
    REQUIRE(slotKind(0xC1000011u) == 1);   // ACT_ENABLED
    REQUIRE(slotKind(0x07000002u) == 7);
    REQUIRE(isSpellKind(1));
    REQUIRE(isSpellKind(8));
    REQUIRE(isSpellKind(0x11));
    REQUIRE_FALSE(isSpellKind(6));
    REQUIRE_FALSE(isSpellKind(7));
    REQUIRE(slotAutocastAllowed(0x81000011u));
    REQUIRE_FALSE(slotAutocastOn(0x81000011u));
    REQUIRE(slotAutocastOn(0xC1000011u));
    REQUIRE_FALSE(slotAutocastAllowed(0x01000011u));
}

TEST_CASE("the mode word is react low, command above (0xC234C8)", "[pet]") {
    // AzerothCore writes react then command as two bytes; the client reads
    // the four bytes with the flags as one word.
    const uint32_t mode = 0x00000102u;  // command follow (1), react aggressive (2)
    REQUIRE(modeReact(mode) == kAggressive);
    REQUIRE(modeCommand(mode) == kFollow);
}

TEST_CASE("built-in slots are checked by state", "[pet]") {
    const uint32_t follow = packPetAction(ActionType::Command, kFollow);
    const uint32_t stay = packPetAction(ActionType::Command, kStay);
    const uint32_t attack = packPetAction(ActionType::Command, kAttack);
    const uint32_t defensive = packPetAction(ActionType::Reaction, kDefensive);
    REQUIRE(builtinActive(follow, kDefensive, kFollow, false));
    REQUIRE_FALSE(builtinActive(stay, kDefensive, kFollow, false));
    REQUIRE(builtinActive(defensive, kDefensive, kFollow, false));
    // Attack is lit by the attack state, not by the command.
    REQUIRE_FALSE(builtinActive(attack, kDefensive, kAttack, false));
    REQUIRE(builtinActive(attack, kDefensive, kFollow, true));
    REQUIRE_FALSE(builtinActive(0xC1000011u, kDefensive, kFollow, true));
    REQUIRE(std::string(commandWord(kStay)) == "WAIT");
    REQUIRE(std::string(commandWord(kAbandon)) == "DISMISS");
    REQUIRE(std::string(reactionWord(kAggressive)) == "AGGRESSIVE");
}

TEST_CASE("happiness faces by PetPersonality row 1", "[pet]") {
    HappinessRow row;
    REQUIRE(happinessIndex(0, row) == 0);
    REQUIRE(happinessIndex(332999, row) == 0);
    REQUIRE(happinessIndex(333000, row) == 1);
    // Thirds of the maximum would have called this unhappy-to-content edge
    // content at 350000 only.
    REQUIRE(happinessIndex(349999, row) == 1);
    REQUIRE(happinessIndex(665999, row) == 1);
    REQUIRE(happinessIndex(666000, row) == 2);
    REQUIRE(happinessIndex(1050000, row) == 2);
}

TEST_CASE("dropping on the pet bar", "[pet]") {
    const uint32_t attack = packPetAction(ActionType::Command, kAttack);
    const uint32_t follow = packPetAction(ActionType::Command, kFollow);
    const uint32_t stay = packPetAction(ActionType::Command, kStay);
    const uint32_t aggressive = packPetAction(ActionType::Reaction, kAggressive);
    const uint32_t defensive = packPetAction(ActionType::Reaction, kDefensive);
    const uint32_t passive = packPetAction(ActionType::Reaction, kPassive);
    const uint32_t bite = 0xC1000000u | 17253u;
    const uint32_t growl = 0x81000000u | 2649u;
    const uint32_t empty = 0x01000000u;

    SECTION("a spell onto an empty slot") {
        uint32_t slots[10] = {attack, follow, stay, bite, empty, empty, empty, aggressive, defensive, passive};
        const auto r = dropOnSlot(slots, 4, growl);
        REQUIRE(r.accepted);
        REQUIRE(slots[4] == growl);
        REQUIRE(r.writeCount == 1);
        REQUIRE(r.writes[0].slot == 4);
        REQUIRE(r.toCursor == empty);  // the empty slot's value goes to the cursor - nothing to draw
    }
    SECTION("a spell already on the bar moves, and its old slot takes the target's") {
        uint32_t slots[10] = {attack, follow, stay, bite, growl, empty, empty, aggressive, defensive, passive};
        const auto r = dropOnSlot(slots, 5, bite);
        REQUIRE(r.accepted);
        REQUIRE(slots[5] == bite);
        REQUIRE(slots[3] == empty);
        REQUIRE(r.writeCount == 2);
        REQUIRE(r.writes[0].slot == 3);
        REQUIRE(r.writes[1].slot == 5);
        REQUIRE(r.toCursor == 0);
    }
    SECTION("a command in the target moves to the first empty slot") {
        uint32_t slots[10] = {attack, follow, stay, bite, growl, empty, empty, aggressive, defensive, passive};
        const uint32_t claw = 0xC1000000u | 16827u;
        const auto r = dropOnSlot(slots, 0, claw);
        REQUIRE(r.accepted);
        REQUIRE(slots[0] == claw);
        REQUIRE(slots[5] == attack);
        REQUIRE(r.toCursor == 0);
    }
    SECTION("with no empty slot a command cannot be displaced") {
        uint32_t slots[10] = {attack, follow, stay, bite, growl, bite + 1, bite + 2, aggressive, defensive, passive};
        const uint32_t claw = 0xC1000000u | 16827u;
        const auto r = dropOnSlot(slots, 1, claw);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(slots[1] == follow);
    }
    SECTION("the same action again is nothing") {
        uint32_t slots[10] = {attack, follow, stay, bite, growl, empty, empty, aggressive, defensive, passive};
        REQUIRE_FALSE(dropOnSlot(slots, 3, bite).accepted);
    }
    SECTION("autocast state does not make a different action") {
        uint32_t slots[10] = {attack, follow, stay, bite, growl, empty, empty, aggressive, defensive, passive};
        const uint32_t biteOff = 0x81000000u | 17253u;
        const auto r = dropOnSlot(slots, 6, biteOff);
        REQUIRE(r.accepted);
        REQUIRE(slots[3] == empty);   // the old copy moved, matched without the 0x40 bit
        REQUIRE(slots[6] == biteOff);
    }
}

TEST_CASE("stable drags", "[pet]") {
    // Nothing held: a select.
    REQUIRE(stableClick(-1, 2, true, 7, 0, false, 2, true).op == StableClick::Select);
    // The pet that is out onto an empty, bought slot: stable it.
    REQUIRE(stableClick(0, 1, false, 0, 0, true, 2, true).op == StableClick::Stable);
    // ... onto a slot not bought: nothing but the cursor clearing.
    REQUIRE(stableClick(0, 3, false, 0, 0, true, 2, true).op == StableClick::Drop);
    // ... onto an occupied slot: swap with that pet.
    auto r = stableClick(0, 1, true, 42, 0, true, 2, true);
    REQUIRE(r.op == StableClick::Swap);
    REQUIRE(r.petNumber == 42);
    // A stabled pet onto the current slot: unstable it with no pet out, swap with one.
    r = stableClick(2, 0, false, 0, 55, true, 2, false);
    REQUIRE(r.op == StableClick::Unstable);
    REQUIRE(r.petNumber == 55);
    r = stableClick(2, 0, true, 9, 55, true, 2, true);
    REQUIRE(r.op == StableClick::Swap);
    REQUIRE(r.petNumber == 55);
    // Stabled onto stabled, or onto itself: nothing.
    REQUIRE(stableClick(1, 2, true, 3, 4, true, 2, true).op == StableClick::Drop);
    REQUIRE(stableClick(1, 1, true, 3, 3, true, 2, true).op == StableClick::Drop);
}

TEST_CASE("pet talents by family category", "[pet]") {
    // Ferocity's Dash (talent 2109) and Dive (2203) share a square; the
    // CategoryMask says which families see which. Dash: 42476576 (bits 5, 10, 13,
    // 19, 23, 25) and 201326592 (bits 26, 27 -> 58, 59); Dive: 2064 (bits 4,
    // 11) and 268435456 (bit 28 -> 60).
    REQUIRE(petTalentForFamily(0, 0, 23));                    // every family's
    REQUIRE(petTalentForFamily(42476576u, 201326592u, 23));   // a wolf dashes
    REQUIRE_FALSE(petTalentForFamily(2064u, 268435456u, 23)); // and does not dive
    REQUIRE(petTalentForFamily(2064u, 268435456u, 4));        // a carrion bird dives
    REQUIRE_FALSE(petTalentForFamily(42476576u, 201326592u, 4));
    REQUIRE(petTalentForFamily(42476576u, 201326592u, 58));   // spirit beast, second word
    REQUIRE(petTalentForFamily(2064u, 268435456u, 60));
}
