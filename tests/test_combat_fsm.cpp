// CombatFSM unit tests
#include <catch_amalgamated.hpp>
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/inventory.hpp"

using namespace wowee::rendering;
namespace anim = wowee::rendering::anim;

static AnimCapabilitySet makeCombatCaps() {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedCombatIdle = anim::READY_UNARMED;
    caps.resolvedMelee1H = anim::ATTACK_1H;
    caps.resolvedMelee2H = anim::ATTACK_2H;
    caps.resolvedMeleeUnarmed = anim::ATTACK_UNARMED;
    caps.resolvedReadyUnarmed = anim::READY_UNARMED;
    caps.resolvedStun = anim::STUN;
    caps.resolvedUnsheathe = anim::SHEATHE;
    caps.resolvedSheathe = anim::SHEATHE;
    caps.hasMelee = true;
    return caps;
}

static CombatFSM::Input combatInput() {
    CombatFSM::Input in;
    in.inCombat = true;
    in.grounded = true;
    return in;
}

static WeaponLoadout unarmedLoadout() {
    return WeaponLoadout{}; // Default is unarmed (inventoryType=0)
}

TEST_CASE("CombatFSM: INACTIVE by default", "[combat]") {
    CombatFSM fsm;
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    CHECK_FALSE(fsm.isActive());
}

TEST_CASE("CombatFSM: unarmed combat uses fist-ready and fist-swing animations", "[combat][unarmed]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto loadout = unarmedLoadout();
    auto in = combatInput();
    in.hasUnsheathe = false;

    auto ready = fsm.resolve(in, caps, loadout);
    REQUIRE(ready.valid);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
    CHECK(ready.animId == anim::READY_UNARMED);

    in.meleeSwingTimer = 0.5f;
    auto swing = fsm.resolve(in, caps, loadout);
    REQUIRE(swing.valid);
    CHECK(fsm.getState() == CombatFSM::State::MELEE_SWING);
    CHECK(swing.animId == anim::ATTACK_UNARMED);
}

TEST_CASE("CombatFSM: equipped fist weapons retain fist-weapon animations", "[combat][fist-weapon]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReadyFist = anim::READY_1H;
    caps.resolvedMeleeFist = anim::ATTACK_1H_PIERCE;
    WeaponLoadout loadout;
    loadout.inventoryType = wowee::game::InvType::ONE_HAND;
    loadout.isFist = true;
    auto in = combatInput();

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_1H);

    in.meleeSwingTimer = 0.5f;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::ATTACK_1H_PIERCE);
}

TEST_CASE("CombatFSM: INACTIVE → COMBAT_IDLE on COMBAT_ENTER event", "[combat]") {
    // Entering combat moves no weapons in 3.3.5, so there is no sheath reach.
    CombatFSM fsm;
    fsm.onEvent(AnimEvent::COMBAT_ENTER);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
    CHECK(fsm.isActive());
}

TEST_CASE("CombatFSM: stun overrides active combat", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    // Enter combat
    fsm.onEvent(AnimEvent::COMBAT_ENTER);

    // Stun
    fsm.setStunned(true);
    auto in = combatInput();
    auto out = fsm.resolve(in, caps, wl);

    CHECK(fsm.isStunned());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::STUN);
}

TEST_CASE("CombatFSM: stun does not override swimming", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.setStunned(true);
    auto in = combatInput();
    in.swimming = true;
    auto out = fsm.resolve(in, caps, wl);

    // Swimming overrides combat entirely - FSM should go inactive
    // The exact behavior depends on implementation, but stun should not
    // force an animation while swimming
    CHECK(fsm.getState() != CombatFSM::State::STUNNED);
}

TEST_CASE("CombatFSM: spell cast sequence", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.startSpellCast(anim::SPELL_PRECAST, anim::SPELL, true, anim::SPELL_CAST);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_PRECAST);

    auto in = combatInput();
    auto out = fsm.resolve(in, caps, wl);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SPELL_PRECAST);
}

TEST_CASE("CombatFSM: stopSpellCast transitions to finalize", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.startSpellCast(0, anim::SPELL, true, anim::SPELL_CAST);
    // Skip precast (animId=0), go to casting
    fsm.setState(CombatFSM::State::SPELL_CASTING);

    fsm.stopSpellCast();
    CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
}

TEST_CASE("CombatFSM: a cast that did not go off puts its pose away with no release", "[combat][cast_end]") {
    // 0x7FECC0 -> 0x7FEC00: a failed, interrupted or cancelled cast, a
    // channel's end and a caster's death put the cast and its pose away
    // (0x72E430). Only SMSG_SPELL_GO plays the release. Seen on the lab:
    // Frostbolt interrupted by its target's death played SpellCastDirected
    // (53) after the interrupt, and an Arcane Missiles channel ended by its
    // target's death played ChannelCastDirected once more.
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    for (const bool channel : {false, true}) {
        CombatFSM fsm;
        fsm.startSpellCast(0, channel ? anim::CHANNEL_CAST_DIRECTED : anim::READY_SPELL_DIRECTED, true,
                           channel ? 0u : static_cast<uint32_t>(anim::SPELL_CAST_DIRECTED));
        REQUIRE(fsm.getState() == CombatFSM::State::SPELL_CASTING);
        fsm.cancelSpellCast();
        CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
        auto in = combatInput();
        in.inCombat = false;
        const auto out = fsm.resolve(in, caps, wl);
        CHECK((!out.valid || (out.animId != anim::SPELL_CAST_DIRECTED &&
                              out.animId != anim::CHANNEL_CAST_DIRECTED &&
                              out.animId != anim::READY_SPELL_DIRECTED)));
    }
    {
        // In the precast too.
        CombatFSM fsm;
        fsm.startSpellCast(anim::SPELL_PRECAST, anim::READY_SPELL_OMNI, true, anim::SPELL_CAST_OMNI);
        REQUIRE(fsm.getState() == CombatFSM::State::SPELL_PRECAST);
        fsm.cancelSpellCast();
        CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    }
    {
        // A release already playing for a cast that went off is left to finish.
        CombatFSM fsm;
        fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, anim::SPELL_CAST_DIRECTED);
        fsm.stopSpellCast();
        REQUIRE(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
        fsm.cancelSpellCast();
        CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
    }
}

namespace {
// One frame: the body plays what the last resolve asked for (from its first
// frame on a restart), `elapsed` ms into it, of `duration` ms.
struct Body { uint32_t anim = 0; float t = 0.0f; };
CombatFSM::Input bodyInput(const Body& b, float durationMs = 500.0f) {
    CombatFSM::Input in;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = b.anim;
    in.currentAnimTime = b.t;
    in.currentAnimDuration = durationMs;
    return in;
}
void play(Body& b, const AnimOutput& out) {
    if (!out.valid) return;
    if (out.restart || out.animId != b.anim) b.t = 0.0f;
    b.anim = out.animId;
}
}  // namespace

TEST_CASE("CombatFSM: a wound during a cast does not end the cast pose", "[combat][cast_end][wound]") {
    // 0x736640 plays the flinch; the re-pick after it (0x73AC30) answers the
    // spell pose while the unit casts (0x7224D0). Wowee cleared the cast on
    // the hit and stood the caster idle for the rest of it (lab report:
    // "when hit, I did a new cast and I just stand still").
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    CombatFSM fsm;
    Body b;
    fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, anim::SPELL_CAST_DIRECTED);
    play(b, fsm.resolve(bodyInput(b), caps, wl));
    REQUIRE(b.anim == anim::READY_SPELL_DIRECTED);
    fsm.triggerHitReaction(anim::STAND_WOUND);
    play(b, fsm.resolve(bodyInput(b), caps, wl));
    CHECK(b.anim == anim::STAND_WOUND);
    // The flinch plays out...
    for (int i = 0; i < 40; ++i) {
        b.t += 20.0f;
        play(b, fsm.resolve(bodyInput(b), caps, wl));
    }
    // ...and the cast pose is back, the cast still on.
    CHECK(b.anim == anim::READY_SPELL_DIRECTED);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_CASTING);
    // The cast went off: its release.
    fsm.stopSpellCast();
    play(b, fsm.resolve(bodyInput(b), caps, wl));
    CHECK(b.anim == anim::SPELL_CAST_DIRECTED);

    // A cast that ends during the flinch: the flinch finishes, no pose after.
    CombatFSM f2;
    Body c;
    f2.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, anim::SPELL_CAST_DIRECTED);
    play(c, f2.resolve(bodyInput(c), caps, wl));
    f2.triggerHitReaction(anim::STAND_WOUND);
    play(c, f2.resolve(bodyInput(c), caps, wl));
    f2.cancelSpellCast();
    for (int i = 0; i < 40; ++i) {
        c.t += 20.0f;
        play(c, f2.resolve(bodyInput(c), caps, wl));
    }
    CHECK(c.anim != anim::READY_SPELL_DIRECTED);
    CHECK(f2.getState() == CombatFSM::State::INACTIVE);
}

TEST_CASE("CombatFSM: back-to-back casts replace the release from its first frame", "[combat][chain]") {
    // Each kit plays its AnimID through 0x7385C0 when it plays (0x73B140):
    // a second instant's release starts over, and a new cast's pose cuts the
    // last release. The same id requested again used to be left playing, so
    // the second release ended with the first.
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    CombatFSM fsm;
    Body b;
    // Instant 1: start and release in one frame.
    fsm.startSpellCast(0, anim::READY_SPELL_OMNI, true, anim::SPELL_CAST_OMNI);
    fsm.stopSpellCast();
    auto out = fsm.resolve(bodyInput(b), caps, wl);
    CHECK(out.animId == anim::SPELL_CAST_OMNI);
    CHECK(out.restart);
    play(b, out);
    b.t = 400.0f;  // near the end of the first release
    // Instant 2 in the same way.
    fsm.startSpellCast(0, anim::READY_SPELL_OMNI, true, anim::SPELL_CAST_OMNI);
    fsm.stopSpellCast();
    out = fsm.resolve(bodyInput(b), caps, wl);
    CHECK(out.animId == anim::SPELL_CAST_OMNI);
    CHECK(out.restart);
    play(b, out);
    CHECK(b.t == 0.0f);
    // The first release's end is not taken for the second's.
    b.t = 100.0f;
    play(b, fsm.resolve(bodyInput(b), caps, wl));
    CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
    // A cast started during the release cuts it with its pose.
    fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, anim::SPELL_CAST_DIRECTED);
    out = fsm.resolve(bodyInput(b), caps, wl);
    CHECK(out.animId == anim::READY_SPELL_DIRECTED);
    CHECK(out.loop);
    // A plain frame afterwards asks for no restart.
    play(b, out);
    out = fsm.resolve(bodyInput(b), caps, wl);
    CHECK_FALSE(out.restart);
}

TEST_CASE("CombatFSM: a server spline under the player moves at its pace", "[combat][charge]") {
    // 0x717050, for every unit on a spline: above 11 yd/s Sprint, above twice
    // the walk speed Run, else Walk. A charge runs at the 28 yd/s cap.
    CHECK(CombatFSM::paceFor(28.0f, 2.5f) == CombatFSM::Pace::Sprint);
    CHECK(CombatFSM::paceFor(11.0f, 2.5f) == CombatFSM::Pace::Run);
    CHECK(CombatFSM::paceFor(7.0f, 2.5f) == CombatFSM::Pace::Run);
    CHECK(CombatFSM::paceFor(5.0f, 2.5f) == CombatFSM::Pace::Walk);
    auto caps = makeCombatCaps();
    caps.resolvedRun = anim::RUN;
    caps.resolvedSprint = anim::SPRINT;
    caps.resolvedWalk = anim::WALK;
    auto wl = unarmedLoadout();
    CombatFSM fsm;
    fsm.setCharging(true);
    fsm.setChargePace(28.0f, 2.5f);
    auto in = combatInput();
    auto out = fsm.resolve(in, caps, wl);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SPRINT);
    fsm.setChargePace(7.0f, 2.5f);
    out = fsm.resolve(in, caps, wl);
    CHECK(out.animId == anim::RUN);
    caps.resolvedSprint = 0;  // a model without Sprint runs
    fsm.setChargePace(28.0f, 2.5f);
    out = fsm.resolve(in, caps, wl);
    CHECK(out.animId == anim::RUN);
}

TEST_CASE("CombatFSM: hit reaction", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);
}

TEST_CASE("CombatFSM: reset clears all state", "[combat]") {
    CombatFSM fsm;
    fsm.onEvent(AnimEvent::COMBAT_ENTER);
    fsm.setStunned(true);

    fsm.reset();
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    CHECK_FALSE(fsm.isStunned());
}

TEST_CASE("CombatFSM: equipped thrown weapon does not replace melee ready stance", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedReadyThrown = anim::READY_THROWN;
    WeaponLoadout loadout;
    loadout.inventoryType = wowee::game::InvType::ONE_HAND;
    loadout.rangedType = RangedWeaponType::THROWN;

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    auto in = combatInput();
    in.rangedWeaponActive = false;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_1H);

    in.rangedWeaponActive = true;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_THROWN);
}

// A wand shares INVTYPE_RANGEDRIGHT with a gun, so reading the inventory type
// alone shouldered it like a rifle. It is aimed the way a directed spell is.
TEST_CASE("CombatFSM: a drawn wand takes the directed spell stance, not the rifle's",
          "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReadyRifle = anim::READY_RIFLE;
    caps.resolvedReadyWand  = anim::READY_SPELL_DIRECTED;
    WeaponLoadout loadout;
    loadout.rangedType = RangedWeaponType::WAND;

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    auto in = combatInput();
    in.rangedWeaponActive = true;
    const uint32_t chosen = fsm.resolve(in, caps, loadout).animId;
    CHECK(chosen == anim::READY_SPELL_DIRECTED);
    CHECK(chosen != anim::READY_RIFLE);

    // And a gun still takes the rifle's, which is the half that was right.
    loadout.rangedType = RangedWeaponType::GUN;
    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_RIFLE);
}

// Auto Shot sets no attack target, so the stock client's stance between shots
// is the ranged weapon's Load, then its Hold, replayed while the shot stays
// on (Wow.exe 0x714F90, 0x73B510) - never ReadyBow.
TEST_CASE("CombatFSM: Auto Shot loads, then holds, between shots", "[combat][ranged]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedLoadBow = anim::LOAD_BOW;
    caps.resolvedHoldBow = anim::HOLD_BOW;
    caps.resolvedLoadRifle = anim::LOAD_RIFLE;
    caps.resolvedHoldRifle = anim::HOLD_RIFLE;
    caps.resolvedHoldThrown = anim::HOLD_THROWN;
    WeaponLoadout loadout;
    loadout.rangedType = RangedWeaponType::BOW;

    CombatFSM::Input in;
    in.grounded = true;
    in.rangedWeaponActive = true;
    in.autoRepeatShot = true;

    // Standing, no attack target: the load, as a one-shot.
    auto load = fsm.resolve(in, caps, loadout);
    REQUIRE(load.valid);
    CHECK(fsm.getState() == CombatFSM::State::RANGED_LOAD);
    CHECK(load.animId == anim::LOAD_BOW);
    CHECK_FALSE(load.loop);

    // Still loading: it stays.
    in.haveAnimState = true;
    in.currentAnimId = anim::LOAD_BOW;
    in.currentAnimDuration = 1000.0f;
    in.currentAnimTime = 300.0f;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::LOAD_BOW);

    // The load ends: the hold, looping.
    in.currentAnimTime = 1000.0f;
    auto hold = fsm.resolve(in, caps, loadout);
    CHECK(fsm.getState() == CombatFSM::State::RANGED_HOLD);
    CHECK(hold.animId == anim::HOLD_BOW);
    CHECK(hold.loop);

    // Moving lets the run take over; standing again loads afresh.
    in.moving = true;
    CHECK_FALSE(fsm.resolve(in, caps, loadout).valid);
    in.moving = false;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::LOAD_BOW);

    // The shot stops: back to standing.
    in.autoRepeatShot = false;
    CHECK_FALSE(fsm.resolve(in, caps, loadout).valid);
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
}

TEST_CASE("CombatFSM: Auto Shot stance by ranged weapon", "[combat][ranged]") {
    auto caps = makeCombatCaps();
    caps.resolvedLoadRifle = anim::LOAD_RIFLE;
    caps.resolvedHoldRifle = anim::HOLD_RIFLE;
    caps.resolvedHoldThrown = anim::HOLD_THROWN;
    CombatFSM::Input in;
    in.grounded = true;
    in.rangedWeaponActive = true;
    in.autoRepeatShot = true;

    // A crossbow loads like a gun (LoadRifle), not like a bow.
    {
        CombatFSM fsm;
        WeaponLoadout loadout;
        loadout.rangedType = RangedWeaponType::CROSSBOW;
        CHECK(fsm.resolve(in, caps, loadout).animId == anim::LOAD_RIFLE);
    }
    // A wand has nothing to load and holds HoldThrown at once.
    {
        CombatFSM fsm;
        WeaponLoadout loadout;
        loadout.rangedType = RangedWeaponType::WAND;
        auto out = fsm.resolve(in, caps, loadout);
        CHECK(fsm.getState() == CombatFSM::State::RANGED_HOLD);
        CHECK(out.animId == anim::HOLD_THROWN);
    }
    // With a melee attack target the Ready stance comes first, as 0x71E0D0
    // is asked before 0x714F90.
    {
        CombatFSM fsm;
        caps.resolvedReady1H = anim::READY_1H;
        WeaponLoadout loadout;
        loadout.inventoryType = wowee::game::InvType::ONE_HAND;
        loadout.rangedType = RangedWeaponType::GUN;
        in.inCombat = true;
        in.rangedWeaponActive = false;
        in.autoRepeatShot = false;
        CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_1H);
    }
}
