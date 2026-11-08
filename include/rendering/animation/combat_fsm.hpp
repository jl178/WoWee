#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include "rendering/animation/weapon_type.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// CombatFSM
//
// Pure logic state machine for combat animation. No renderer dependency.
// States: INACTIVE · COMBAT_IDLE · MELEE_SWING · RANGED_SHOOT · RANGED_LOAD ·
//         SPELL_PRECAST · SPELL_CASTING · SPELL_FINALIZE · HIT_REACTION ·
//         STUNNED · CHARGE · UNSHEATHE · SHEATHE · RANGED_HOLD
//
// Stun overrides all combat states. Spell state cleared on interrupts.
// offHandTurn_ alternation managed internally.
// ============================================================================
class CombatFSM {
public:
    enum class State : uint8_t {
        INACTIVE,
        COMBAT_IDLE,
        MELEE_SWING,
        RANGED_SHOOT,
        RANGED_LOAD,
        SPELL_PRECAST,
        SPELL_CASTING,
        SPELL_FINALIZE,
        HIT_REACTION,
        STUNNED,
        CHARGE,
        UNSHEATHE,
        SHEATHE,
        RANGED_HOLD,  // Auto Shot running: HoldBow/HoldRifle/HoldThrown after the load
    };

    struct Input {
        bool inCombat = false;
        bool grounded = true;
        bool jumping = false;
        bool swimming = false;
        bool moving = false;
        bool sprinting = false;
        bool lowHealth = false;
        bool rangedWeaponActive = false;
        /// An auto-repeat shot (Auto Shot, Shoot, Throw) is running with the
        /// ranged weapon out: between shots the stance is the weapon's
        /// Load, then its Hold (Wow.exe 0x714F90, 0x73B510).
        bool autoRepeatShot = false;
        float meleeSwingTimer = 0.0f;   // >0 = melee active
        float rangedShootTimer = 0.0f;  // >0 = ranged active
        uint32_t specialAttackAnimId = 0;
        uint32_t rangedAnimId = 0;
        // Animation state query for one-shot completion detection
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
        // Whether model has specific one-shot animations
        bool hasUnsheathe = false;
        bool hasSheathe = false;
    };

    void onEvent(AnimEvent event);

    /// Evaluate current state against input and capabilities.
    AnimOutput resolve(const Input& in, const AnimCapabilitySet& caps,
                       const WeaponLoadout& loadout);

    [[nodiscard]] State getState() const { return state_; }
    /// The cast pose a flinch taken during the cast returns to, 0 for none.
    [[nodiscard]] uint32_t resumeAnimAfterHit() const {
        return (state_ == State::HIT_REACTION && resumeSpellAfterHit_) ? spellCastAnimId_ : 0u;
    }
    void setState(State s) { state_ = s; }

    [[nodiscard]] bool isStunned() const { return state_ == State::STUNNED; }
    [[nodiscard]] bool isActive() const { return state_ != State::INACTIVE; }
    void reset();

    // ── Spell cast management ───────────────────────────────────────────
    void startSpellCast(uint32_t precast, uint32_t cast, bool castLoop, uint32_t finalize);
    /// The cast went off: its release (finalize) one-shot, then idle.
    void stopSpellCast();
    /// The cast ended without going off - failed, interrupted, cancelled, a
    /// channel's end, the caster's death: its pose is put away at once, no
    /// release (0x7FEC00 -> 0x72E430). A release already playing for a cast
    /// that did go off is left to finish.
    void cancelSpellCast();
    /// Drop every part of a cast still to play, the release included.
    void abandonSpell();
    void clearSpellState();

    // ── Hit/stun management ─────────────────────────────────────────────
    void triggerHitReaction(uint32_t animId);
    void setStunned(bool stunned);
    void setCharging(bool charging);
    /// While charging (on a server spline): the spline's speed over the
    /// ground and the unit's walk speed, for the pace 0x717050 picks.
    /// `airborne`: a parabolic spline's leap (or a falling one) is in the
    /// air - the jump loop, as its MSG_MOVE_JUMP event raises (0x6E9C30).
    void setChargePace(float speed, float walkSpeed, bool airborne = false) {
        chargeSpeed_ = speed;
        chargeWalkSpeed_ = walkSpeed;
        chargeAirborne_ = airborne;
    }
    /// The locomotion a unit moving at `speed` plays (0x717050): above
    /// 11 yd/s Sprint, above twice its walk speed Run, else Walk.
    enum class Pace : uint8_t { Walk, Run, Sprint };
    [[nodiscard]] static Pace paceFor(float speed, float walkSpeed) {
        if (speed > 11.0f) return Pace::Sprint;
        if (speed > walkSpeed + walkSpeed) return Pace::Run;
        return Pace::Walk;
    }

    static constexpr uint8_t PRIORITY = 50;

private:
    State state_ = State::INACTIVE;

    // Spell cast sequence
    uint32_t spellPrecastAnimId_ = 0;
    uint32_t spellCastAnimId_ = 0;
    uint32_t spellFinalizeAnimId_ = 0;
    bool spellCastLoop_ = false;
    bool spellPrecastAnimSeen_ = false;
    uint8_t spellPrecastFrames_ = 0;
    bool spellFinalizeAnimSeen_ = false;
    uint8_t spellFinalizeFrames_ = 0;

    // The spell one-shot or pose was (re)started since the last resolve: it
    // plays from its first frame (AnimOutput::restart).
    bool spellAnimFresh_ = false;
    // A wound taken during a cast: the cast goes on, and its pose comes back
    // when the flinch is over (0x73AC30's re-pick answers the spell pose,
    // 0x7224D0, while the unit casts).
    bool resumeSpellAfterHit_ = false;

    // Hit reaction
    uint32_t hitReactionAnimId_ = 0;
    bool hitReactionAnimSeen_ = false;  ///< the wound anim has started playing
    uint8_t hitReactionFrames_ = 0;     ///< frames waited for it to start

    // RANGED_LOAD: the load animation resolved for it, whether it has been
    // seen playing, and the frames waited for it to start.
    uint32_t rangedLoadAnim_ = 0;
    bool rangedLoadSeen_ = false;
    uint8_t rangedLoadFrames_ = 0;
    void enterRangedLoad();

    // Stun
    bool stunned_ = false;

    // Charge (a server spline moving the player)
    bool charging_ = false;
    float chargeSpeed_ = 7.0f;
    float chargeWalkSpeed_ = 2.5f;
    bool chargeAirborne_ = false;

    // Off-hand alternation for dual wielding
    /// Which hand the next swing uses, and which this one chose.
    ///
    /// The turn is taken when a swing begins. Taken where the animation is
    /// resolved instead, it was taken again on every frame the swing lasted -
    /// so a dual-wielding character alternated main-hand and off-hand
    /// animations sixty times a second and stood there shuddering. With one
    /// weapon there was no second animation to alternate with, which is why it
    /// only ever showed with two.
    bool offHandTurn_ = false;
    bool offHandThisSwing_ = false;

    /// Internal: update state transitions based on input.
    void updateTransitions(const Input& in);

    /// Detect if a one-shot animation has completed.
    [[nodiscard]] bool oneShotComplete(const Input& in, uint32_t expectedAnimId) const;
};

} // namespace rendering
} // namespace wowee
