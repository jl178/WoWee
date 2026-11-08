#pragma once

#include "rendering/animation/i_character_animator.hpp"
#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/activity_fsm.hpp"
#include "rendering/animation/mount_fsm.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// CharacterAnimator
//
// Generic animator for any character (player, NPC, companion).
// Composes LocomotionFSM, CombatFSM, ActivityFSM, and MountFSM with
// a priority resolver. Implements ICharacterAnimator.
//
// Priority order: Stun > HitReaction > Spell > Charge > Combat > Activity > Locomotion
//
// No idle fallback: if all FSMs return {valid=false}, last animation continues.
//
// Overlay layer (stealth, sprint) substitutes the resolved anim without
// changing sub-FSM state.
// ============================================================================
class CharacterAnimator final : public ICharacterAnimator {
public:
    CharacterAnimator();

    // ── IAnimator ───────────────────────────────────────────────────────
    void onEvent(AnimEvent event) override;
    void update(float dt) override;

    // ── ICharacterAnimator ──────────────────────────────────────────────
    void startSpellCast(uint32_t precast, uint32_t cast, bool loop, uint32_t finalize) override;
    void stopSpellCast() override;
    void cancelSpellCast() override;
    void abandonSpell() { combat_.abandonSpell(); }
    void triggerMeleeSwing() override;
    void triggerRangedShot() override;
    void triggerHitReaction(uint32_t animId) override;
    void setEquippedWeaponType(const WeaponLoadout& loadout) override;
    void setEquippedRangedType(RangedWeaponType type) override;
    void setRangedWeaponActive(bool active) override;
    void playEmote(uint32_t animId, bool loop) override;
    void cancelEmote() override;
    void startLooting() override;
    void stopLooting() override;
    void setStunned(bool stunned) override;
    void setCharging(bool charging) override;
    void setStandState(uint8_t state) override;
    void setStealthed(bool stealth) override;
    /// The character has an attack target (it asked to attack, until the
    /// attack stops). This, not being in combat, is what holds the stock
    /// client's Ready stance (Wow.exe 0x71E0D0 reads CGUnit_C +0xA20): a
    /// character hit while not attacking stands at ease and flinches.
    void setAttacking(bool attacking) { attacking_ = attacking; }
    /// An auto-repeat shot (Auto Shot, Shoot, Throw) is on: with the ranged
    /// weapon out, the stance between shots is its Load and Hold.
    void setAutoRepeatShot(bool on) { autoRepeatShot_ = on; }
    void setLowHealth(bool low) override;
    void setSprintAuraActive(bool active) override;

    // ── Configuration ───────────────────────────────────────────────────
    void setCapabilities(const AnimCapabilitySet& caps) { caps_ = caps; }
    [[nodiscard]] const AnimCapabilitySet& getCapabilities() const { return caps_; }
    void setWeaponLoadout(const WeaponLoadout& loadout) { loadout_ = loadout; }
    [[nodiscard]] const WeaponLoadout& getWeaponLoadout() const { return loadout_; }

    // ── Mount ───────────────────────────────────────────────────────────
    void configureMountFSM(const AnimCapabilitySet& mountCaps);
    void clearMountFSM();
    [[nodiscard]] bool isMountActive() const { return mount_.isActive(); }
    MountFSM& getMountFSM() { return mount_; }

    // ── Sub-FSM access (for transition queries) ─────────────────────────
    LocomotionFSM& getLocomotion() { return locomotion_; }
    [[nodiscard]] const LocomotionFSM& getLocomotion() const { return locomotion_; }
    CombatFSM& getCombat() { return combat_; }
    [[nodiscard]] const CombatFSM& getCombat() const { return combat_; }
    ActivityFSM& getActivity() { return activity_; }
    [[nodiscard]] const ActivityFSM& getActivity() const { return activity_; }

    // ── Last resolved output ────────────────────────────────────────────
    [[nodiscard]] AnimOutput getLastOutput() const { return lastOutput_; }

    // ── Input injection (set per-frame from AnimationController) ────────
    struct FrameInput {
        // From camera controller
        bool moving = false;
        bool sprinting = false;
        bool movingForward = false;
        bool movingBackward = false;
        bool strafeLeft = false;
        bool strafeRight = false;
        bool grounded = true;
        bool jumping = false;
        // SWIMMING or FLYING (stock 0x2200000): an unmounted character in
        // the air with flight allowed plays the swim set, so the caller sets
        // this for flight as well (AnimationController).
        bool swimming = false;
        bool sitting = false;
        bool ascending = false;
        bool descending = false;
        bool jumpKeyPressed = false;
        // Turning in place: see LocomotionFSM::Input::turnLeft/turnRight.
        bool turnLeft = false;
        bool turnRight = false;
        // The speed the movement flags select (0x987570) and the unit's walk
        // speed: the pace of a moving unit comes from them (0x717050, above
        // 11 Sprint, above twice the walk speed Run, else Walk). A negative
        // speed means unknown, and `sprinting` decides as before.
        float speed = -1.0f;
        float walkSpeed = 2.5f;
        float characterYaw = 0.0f;
        // Melee/ranged timers
        float meleeSwingTimer = 0.0f;
        float rangedShootTimer = 0.0f;
        uint32_t specialAttackAnimId = 0;
        uint32_t rangedAnimId = 0;
        // Animation state query
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
        // Mount state query
        uint32_t curMountAnim = 0;
        float curMountTime = 0.0f;
        float curMountDuration = 0.0f;
        bool haveMountState = false;
    };

    void setFrameInput(const FrameInput& input) { frameInput_ = input; }

private:
    AnimCapabilitySet caps_;
    WeaponLoadout loadout_;

    LocomotionFSM locomotion_;
    CombatFSM     combat_;
    ActivityFSM   activity_;
    MountFSM      mount_;

    // Overlay flags
    bool stealthed_ = false;
    bool sprintAura_ = false;
    bool lowHealth_ = false;
    bool attacking_ = false;
    bool autoRepeatShot_ = false;
    bool rangedWeaponActive_ = false;

    float lastDt_ = 0.0f;
    FrameInput frameInput_;
    AnimOutput lastOutput_;

    /// Priority resolver: highest-priority active FSM wins.
    AnimOutput resolveAnimation();

    /// Apply stealth/sprint overlays to the resolved animation.
    [[nodiscard]] AnimOutput applyOverlays(AnimOutput base) const;
};

} // namespace rendering
} // namespace wowee
