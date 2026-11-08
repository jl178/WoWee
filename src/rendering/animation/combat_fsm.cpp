#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/inventory.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

// ── One-shot completion helper ───────────────────────────────────────────────

void CombatFSM::enterRangedLoad() {
    state_ = State::RANGED_LOAD;
    rangedLoadSeen_ = false;
    rangedLoadFrames_ = 0;
}

bool CombatFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    // Renderer auto-returns one-shots to STAND; detect that OR normal completion
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
}

// ── Event handling ───────────────────────────────────────────────────────────

void CombatFSM::onEvent(AnimEvent event) {
    switch (event) {
        // Combat starting or ending moves no weapons in 3.3.5, so it plays no
        // sheath reach either (AppearanceComposer plays that when they move).
        case AnimEvent::COMBAT_ENTER:
            if (state_ == State::INACTIVE)
                state_ = State::COMBAT_IDLE;
            break;
        case AnimEvent::COMBAT_EXIT:
            if (state_ == State::COMBAT_IDLE)
                state_ = State::INACTIVE;
            break;
        case AnimEvent::STUN_ENTER:
            clearSpellState();
            hitReactionAnimId_ = 0;
            stunned_ = true;
            state_ = State::STUNNED;
            break;
        case AnimEvent::STUN_EXIT:
            stunned_ = false;
            if (state_ == State::STUNNED)
                state_ = State::INACTIVE;
            break;
        case AnimEvent::HIT_REACT:
            // Handled by triggerHitReaction() with animId
            break;
        case AnimEvent::CHARGE_START:
            charging_ = true;
            clearSpellState();
            state_ = State::CHARGE;
            break;
        case AnimEvent::CHARGE_END:
            charging_ = false;
            if (state_ == State::CHARGE)
                state_ = State::INACTIVE;
            break;
        case AnimEvent::SWIM_ENTER:
            clearSpellState();
            hitReactionAnimId_ = 0;
            stunned_ = false;
            state_ = State::INACTIVE;
            break;
        default:
            break;
    }
}

// ── Spell cast management ────────────────────────────────────────────────────

void CombatFSM::startSpellCast(uint32_t precast, uint32_t cast, bool castLoop, uint32_t finalize) {
    spellPrecastAnimId_ = precast;
    spellCastAnimId_ = cast;
    spellCastLoop_ = castLoop;
    spellFinalizeAnimId_ = finalize;
    spellPrecastAnimSeen_ = false;
    spellPrecastFrames_ = 0;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
    spellAnimFresh_ = true;
    resumeSpellAfterHit_ = false;
    state_ = (precast != 0) ? State::SPELL_PRECAST : State::SPELL_CASTING;
}

void CombatFSM::stopSpellCast() {
    // The cast went off during a flinch: the release still plays (the cast
    // kit plays its AnimID at SPELL_GO whatever the body is doing).
    const bool heldUnderHit = state_ == State::HIT_REACTION && resumeSpellAfterHit_;
    if (state_ != State::SPELL_PRECAST && state_ != State::SPELL_CASTING && !heldUnderHit) return;
    resumeSpellAfterHit_ = false;
    hitReactionAnimId_ = 0;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
    spellAnimFresh_ = true;
    state_ = State::SPELL_FINALIZE;
}

void CombatFSM::cancelSpellCast() {
    if (state_ == State::HIT_REACTION && resumeSpellAfterHit_) {
        // Ended during a flinch: the flinch finishes, nothing comes back.
        resumeSpellAfterHit_ = false;
        clearSpellState();
        return;
    }
    if (state_ != State::SPELL_PRECAST && state_ != State::SPELL_CASTING) return;
    clearSpellState();
    state_ = State::INACTIVE;
}

void CombatFSM::abandonSpell() {
    // Whatever of a cast is still to play - its build-up, its hold or a
    // release waiting to be seen - is dropped (a dismount: the summon's
    // release, kept while the rider sat, played the moment they stood).
    if (state_ == State::HIT_REACTION) resumeSpellAfterHit_ = false;
    clearSpellState();
    if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING ||
        state_ == State::SPELL_FINALIZE)
        state_ = State::INACTIVE;
}

void CombatFSM::clearSpellState() {
    spellPrecastAnimId_ = 0;
    spellCastAnimId_ = 0;
    spellCastLoop_ = false;
    spellFinalizeAnimId_ = 0;
    spellPrecastAnimSeen_ = false;
    spellPrecastFrames_ = 0;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
    spellAnimFresh_ = false;
    resumeSpellAfterHit_ = false;
}

// ── Hit/stun ─────────────────────────────────────────────────────────────────

void CombatFSM::triggerHitReaction(uint32_t animId) {
    // Don't interrupt swim/jump/stun states
    if (state_ == State::STUNNED) return;
    // A wound does not end a cast: the flinch plays (0x736640) and the cast
    // pose comes back after it - the re-pick (0x73AC30) answers the spell
    // pose while the unit casts (0x7224D0). Clearing the cast here left a
    // caster hit mid-cast standing idle for the rest of it. A release
    // already playing is a one-shot the flinch replaces.
    if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING) {
        if (animId != 0) resumeSpellAfterHit_ = true;
        else return;  // a clear, not a reaction: the cast goes on
    } else if (state_ == State::HIT_REACTION && resumeSpellAfterHit_) {
        // Hit again during the flinch: still casting underneath.
    } else if (state_ == State::SPELL_FINALIZE) {
        clearSpellState();
    }
    if (animId == 0) {  // a clear, not a reaction
        hitReactionAnimId_ = 0;
        if (state_ == State::HIT_REACTION) state_ = State::INACTIVE;
        return;
    }
    hitReactionAnimId_ = animId;
    hitReactionAnimSeen_ = false;
    hitReactionFrames_ = 0;
    state_ = State::HIT_REACTION;
}

void CombatFSM::setStunned(bool stunned) {
    stunned_ = stunned;
    if (stunned) {
        if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING || state_ == State::SPELL_FINALIZE) {
            clearSpellState();
        }
        hitReactionAnimId_ = 0;
        state_ = State::STUNNED;
    } else {
        if (state_ == State::STUNNED)
            state_ = State::INACTIVE;
    }
}

void CombatFSM::setCharging(bool charging) {
    charging_ = charging;
    if (charging) {
        clearSpellState();
        hitReactionAnimId_ = 0;
        state_ = State::CHARGE;
    } else if (state_ == State::CHARGE) {
        state_ = State::INACTIVE;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void CombatFSM::updateTransitions(const Input& in) {
    // Stun override: can't act while stunned
    if (stunned_ && state_ != State::STUNNED) {
        state_ = State::STUNNED;
        return;
    }

    // Force melee/ranged overrides
    if (in.meleeSwingTimer > 0.0f && !stunned_ && in.grounded && !in.swimming) {
        if (state_ != State::MELEE_SWING) {
            clearSpellState();
            hitReactionAnimId_ = 0;
            // The hand is chosen here, once, because this is where a swing
            // begins. Choosing it where the animation is resolved chose it
            // again on every frame the swing lasted.
            offHandThisSwing_ = offHandTurn_;
            offHandTurn_ = !offHandTurn_;
        }
        state_ = State::MELEE_SWING;
        return;
    }
    if (in.rangedShootTimer > 0.0f && !stunned_ && in.meleeSwingTimer <= 0.0f && in.grounded && !in.swimming) {
        if (state_ != State::RANGED_SHOOT) {
            clearSpellState();
            hitReactionAnimId_ = 0;
        }
        state_ = State::RANGED_SHOOT;
        return;
    }
    if (charging_ && !stunned_) {
        if (state_ != State::CHARGE) {
            clearSpellState();
            hitReactionAnimId_ = 0;
        }
        state_ = State::CHARGE;
        return;
    }

    switch (state_) {
        case State::INACTIVE:
            if (in.inCombat && in.grounded && !in.swimming && !in.moving) {
                state_ = in.hasUnsheathe ? State::UNSHEATHE : State::COMBAT_IDLE;
            } else if (in.autoRepeatShot && in.grounded && !in.swimming && !in.moving && !in.jumping) {
                // Standing again with Auto Shot on: the weapon is loaded
                // afresh (moving cleared the loaded state, 0x72AFE0).
                enterRangedLoad();
            }
            break;

        case State::COMBAT_IDLE:
            if (in.swimming || in.jumping || !in.grounded || in.moving) {
                state_ = State::INACTIVE;
            } else if (!in.inCombat) {
                state_ = in.hasSheathe ? State::SHEATHE : State::INACTIVE;
            }
            break;

        case State::MELEE_SWING:
            if (in.meleeSwingTimer <= 0.0f) {
                if (in.swimming) {
                    state_ = State::INACTIVE;
                } else if (in.inCombat && in.grounded) {
                    state_ = State::COMBAT_IDLE;
                } else {
                    state_ = State::INACTIVE;
                }
            }
            break;

        case State::RANGED_SHOOT:
            if (in.rangedShootTimer <= 0.0f) {
                if (in.swimming || !in.grounded) {
                    state_ = State::INACTIVE;
                } else if (in.autoRepeatShot) {
                    enterRangedLoad();
                } else if (in.inCombat) {
                    state_ = State::COMBAT_IDLE;
                } else {
                    state_ = State::INACTIVE;
                }
            }
            break;

        // While an auto-repeat shot runs, the base animation between shots
        // is the ranged weapon's Load (0x714F90), which on its end becomes
        // its Hold, replayed for as long as the shot stays on (0x73B510).
        // Moving takes the body for the run; standing again loads again.
        case State::RANGED_LOAD:
        case State::RANGED_HOLD:
            if (in.swimming || in.jumping || !in.grounded || in.moving) {
                state_ = State::INACTIVE;
            } else if (!in.autoRepeatShot) {
                state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
            } else if (state_ == State::RANGED_LOAD) {
                const uint32_t load = rangedLoadAnim_;
                if (load == 0) {
                    state_ = State::RANGED_HOLD;
                } else if (in.haveAnimState && in.currentAnimId == load) {
                    rangedLoadSeen_ = true;
                    if (oneShotComplete(in, load)) state_ = State::RANGED_HOLD;
                } else if (rangedLoadSeen_ || ++rangedLoadFrames_ > 10) {
                    state_ = State::RANGED_HOLD;
                }
            }
            break;

        case State::SPELL_PRECAST:
            if (in.swimming || (in.jumping && !in.grounded) || (!in.grounded && !in.jumping)) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.haveAnimState) {
                uint32_t expectedAnim = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
                if (in.currentAnimId == expectedAnim) spellPrecastAnimSeen_ = true;
                if (spellPrecastAnimSeen_ && oneShotComplete(in, expectedAnim)) {
                    state_ = State::SPELL_CASTING;
                }
                if (!spellPrecastAnimSeen_ && ++spellPrecastFrames_ > 10) {
                    state_ = State::SPELL_CASTING;
                }
            }
            break;

        case State::SPELL_CASTING:
            if (in.swimming || (in.jumping && !in.grounded) || (!in.grounded && !in.jumping)) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.moving) {
                clearSpellState();
                state_ = State::INACTIVE;
            }
            // Stays in SPELL_CASTING until stopSpellCast() is called externally
            break;

        case State::SPELL_FINALIZE: {
            // Starting to move mid-release hands the body to the legs, as
            // SPELL_CASTING does; a release begun on the move never gets
            // here (AnimationController puts it on the upper body).
            if (in.swimming || (in.jumping && !in.grounded) || in.moving) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.haveAnimState && !spellAnimFresh_) {
                // (Not on the frame it was asked for: the body still shows
                // the last release, which would read as this one ending.)
                uint32_t expectedAnim = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                    : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
                if (in.currentAnimId == expectedAnim) spellFinalizeAnimSeen_ = true;
                if (spellFinalizeAnimSeen_ && oneShotComplete(in, expectedAnim)) {
                    clearSpellState();
                    state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                }
                // Safety: if finalize anim never seen (model lacks it), finish after timeout
                if (!spellFinalizeAnimSeen_ && ++spellFinalizeFrames_ > 10) {
                    clearSpellState();
                    state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                }
            }
            break;
        }

        case State::HIT_REACTION:
            if (in.swimming || in.moving) {
                hitReactionAnimId_ = 0;
                if (resumeSpellAfterHit_) {
                    resumeSpellAfterHit_ = false;
                    clearSpellState();
                }
                state_ = State::INACTIVE;
            } else if (in.haveAnimState) {
                uint32_t expectedAnim = hitReactionAnimId_ ? hitReactionAnimId_ : anim::COMBAT_WOUND;
                // Wait for the wound anim to start before asking whether it ended:
                // on the first frame the old anim is still current, which read as done.
                if (in.currentAnimId == expectedAnim) hitReactionAnimSeen_ = true;
                if ((hitReactionAnimSeen_ && oneShotComplete(in, expectedAnim)) ||
                    (!hitReactionAnimSeen_ && ++hitReactionFrames_ > 10)) {
                    hitReactionAnimId_ = 0;
                    if (resumeSpellAfterHit_) {
                        resumeSpellAfterHit_ = false;
                        spellAnimFresh_ = true;
                        state_ = State::SPELL_CASTING;
                    } else {
                        state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                    }
                }
            }
            break;

        case State::STUNNED:
            if (!stunned_) {
                state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
            } else if (in.swimming) {
                stunned_ = false;
                state_ = State::INACTIVE;
            }
            break;

        case State::CHARGE:
            if (!charging_) {
                state_ = State::INACTIVE;
            }
            break;

        case State::UNSHEATHE:
            if (in.swimming || in.moving) {
                state_ = State::INACTIVE;
            } else if (in.haveAnimState && oneShotComplete(in, anim::SHEATHE)) {
                state_ = State::COMBAT_IDLE;
            }
            break;

        case State::SHEATHE:
            if (in.swimming || in.moving) {
                state_ = State::INACTIVE;
            } else if (in.inCombat) {
                state_ = State::COMBAT_IDLE;
            } else if (in.haveAnimState && oneShotComplete(in, anim::SHEATHE)) {
                state_ = State::INACTIVE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput CombatFSM::resolve(const Input& in, const AnimCapabilitySet& caps,
                               const WeaponLoadout& loadout) {
    updateTransitions(in);

    if (state_ == State::INACTIVE) return AnimOutput::stay();

    uint32_t animId = 0;
    bool loop = true;

    switch (state_) {
        case State::INACTIVE:
            return AnimOutput::stay();

        case State::COMBAT_IDLE:
            if (in.lowHealth && caps.resolvedStandWound) {
                animId = caps.resolvedStandWound;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::BOW) {
                animId = caps.resolvedReadyBow;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::GUN) {
                animId = caps.resolvedReadyRifle;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::CROSSBOW) {
                animId = caps.resolvedReadyCrossbow;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::THROWN) {
                animId = caps.resolvedReadyThrown;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::WAND) {
                animId = caps.resolvedReadyWand;
            } else if (loadout.is2HLoose) {
                animId = caps.resolvedReady2HLoose;
            } else if (loadout.inventoryType == game::InvType::TWO_HAND) {
                // Gripped with something else in the off hand: Ready1H
                // (Wow.exe 0x714DD0), as readyAnim() has it.
                animId = (loadout.hasOffHand || loadout.hasShield) ? caps.resolvedReady1H
                                                                   : caps.resolvedReady2H;
            } else if (loadout.isFist) {
                animId = caps.resolvedReadyFist;
            } else if (loadout.inventoryType == game::InvType::NON_EQUIP && !loadout.hasOffHand) {
                animId = caps.resolvedReadyUnarmed;
            } else {
                animId = caps.resolvedReady1H;
            }
            loop = true;
            break;

        case State::MELEE_SWING:
            if (in.specialAttackAnimId != 0) {
                animId = in.specialAttackAnimId;
            } else {
                // Resolve melee animation using probed capabilities + weapon
                // loadout. Which hand is already settled: this runs every frame
                // of the swing and has to answer the same way each time.
                const bool useOffHand = loadout.hasOffHand && offHandThisSwing_;

                if (useOffHand) {
                    // The off-hand weapon's kind, not the main hand's.
                    if (loadout.offHandIsFist) animId = caps.resolvedMeleeOffHandFist;
                    else if (loadout.offHandIsDagger) animId = caps.resolvedMeleeOffHandPierce;
                    else if (loadout.inventoryType == game::InvType::NON_EQUIP) animId = caps.resolvedMeleeOffHandUnarmed;
                    else animId = caps.resolvedMeleeOffHand;
                } else if (loadout.isFist) {
                    animId = caps.resolvedMeleeFist;
                } else if (loadout.isDagger) {
                    animId = caps.resolvedMeleePierce;
                } else if (loadout.is2HLoose) {
                    animId = caps.resolvedMelee2HLoose;
                } else if (loadout.inventoryType == game::InvType::TWO_HAND) {
                    animId = caps.resolvedMelee2H;
                } else if (loadout.inventoryType == game::InvType::NON_EQUIP) {
                    animId = caps.resolvedMeleeUnarmed;
                } else {
                    animId = caps.resolvedMelee1H;
                }
            }
            if (animId == 0) {
                LOG_DEBUG("CombatFSM: MELEE_SWING resolved animId=0, falling back to STAND");
                animId = anim::STAND;
            }
            loop = false;
            break;

        case State::RANGED_SHOOT:
            animId = in.rangedAnimId ? in.rangedAnimId : anim::ATTACK_BOW;
            loop = false;
            break;

        case State::RANGED_LOAD:
            // 0x714F90: a bow LoadBow, a gun or a crossbow LoadRifle, thrown
            // LoadThrown; a wand has nothing to load and holds HoldThrown.
            switch (loadout.rangedType) {
                case RangedWeaponType::BOW:      animId = caps.resolvedLoadBow; break;
                case RangedWeaponType::GUN:
                case RangedWeaponType::CROSSBOW: animId = caps.resolvedLoadRifle; break;
                case RangedWeaponType::THROWN:   animId = caps.resolvedLoadThrown; break;
                default: break;
            }
            rangedLoadAnim_ = animId;
            if (animId != 0) {
                loop = false;
                break;
            }
            // Nothing to load: straight to the hold.
            state_ = State::RANGED_HOLD;
            [[fallthrough]];

        case State::RANGED_HOLD:
            // 0x73B510: LoadBow ends in HoldBow, LoadRifle in HoldRifle, and
            // LoadThrown (and a wand's hold) in HoldThrown.
            switch (loadout.rangedType) {
                case RangedWeaponType::BOW:      animId = caps.resolvedHoldBow; break;
                case RangedWeaponType::GUN:
                case RangedWeaponType::CROSSBOW: animId = caps.resolvedHoldRifle; break;
                case RangedWeaponType::THROWN:
                case RangedWeaponType::WAND:     animId = caps.resolvedHoldThrown; break;
                default:                         animId = 0; break;
            }
            if (animId == 0) return AnimOutput::stay();
            loop = true;
            break;

        case State::SPELL_PRECAST:
            animId = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
            loop = false;
            break;

        case State::SPELL_CASTING:
            animId = spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL;
            loop = spellCastLoop_;
            break;

        case State::SPELL_FINALIZE:
            animId = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                   : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
            loop = false;
            break;

        case State::HIT_REACTION:
            animId = hitReactionAnimId_ ? hitReactionAnimId_
                   : (caps.resolvedCombatWound ? caps.resolvedCombatWound : anim::COMBAT_WOUND);
            loop = false;
            break;

        case State::STUNNED:
            animId = caps.resolvedStun ? caps.resolvedStun : anim::STUN;
            loop = true;
            break;

        case State::CHARGE:
            // The spline's pace (0x717050), as for any unit on one: a charge
            // at its 28 yd/s cap sprints. A leap in the air jumps.
            if (chargeAirborne_) {
                animId = caps.resolvedJump ? caps.resolvedJump : anim::JUMP;
                loop = true;
                break;
            }
            switch (paceFor(chargeSpeed_, chargeWalkSpeed_)) {
                case Pace::Sprint:
                    animId = caps.resolvedSprint ? caps.resolvedSprint
                           : caps.resolvedRun   ? caps.resolvedRun : anim::RUN;
                    break;
                case Pace::Run:
                    animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
                    break;
                case Pace::Walk:
                    animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
                    break;
            }
            loop = true;
            break;

        case State::UNSHEATHE:
            animId = caps.resolvedUnsheathe ? caps.resolvedUnsheathe : anim::SHEATHE;
            loop = false;
            break;

        case State::SHEATHE:
            animId = caps.resolvedSheathe ? caps.resolvedSheathe : anim::SHEATHE;
            loop = false;
            break;
    }

    if (animId == 0) return AnimOutput::stay();
    AnimOutput out = AnimOutput::ok(animId, loop);
    if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING ||
        state_ == State::SPELL_FINALIZE) {
        out.restart = spellAnimFresh_;
        spellAnimFresh_ = false;
    }
    return out;
}

// ── Reset ────────────────────────────────────────────────────────────────────

void CombatFSM::reset() {
    state_ = State::INACTIVE;
    clearSpellState();
    hitReactionAnimId_ = 0;
    stunned_ = false;
    charging_ = false;
    offHandTurn_ = false;
    offHandThisSwing_ = false;
}

} // namespace rendering
} // namespace wowee
