#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// MountFSM
//
// What the mount model of a mounted unit plays, by the stock rules (Wow.exe
// 12340, docs/re/mounts-taxi-vehicles.md section 2). A mounted unit's
// movement animation is the one it would play on foot (0x724500 ->
// 0x717050 / 0x71E340 / 0x724200), played on the mount model instead of the
// rider (0x7385C0 -> 0x7176F0 with the mount): Stand, Walk, Run above twice
// the walk speed, Sprint above 11 yd/s, WalkBackwards, the jump and fall,
// ShuffleLeft/Right turning in place, and in water *or in the air* the swim
// set - a flying mount flies on Swim (42), SwimIdle (41), SwimLeft/Right and
// SwimBackwards; it has no flight tier of its own. On a Flying spline (a
// taxi, 0x4F5290) it moves in Fly (135). SMSG_MOUNTSPECIAL_ANIM plays
// MountSpecial (94). Nothing else: no procedural bob or lean, no fidget or
// idle-sound timers (the Stand variations are the M2's own), no rear-up on
// a standing jump.
//
// The rider is not this machine's: it sits in Mount (91) or the mount
// aura's state-kit animation (mount_rules.hpp).
// ============================================================================
class MountFSM {
public:
    struct Input {
        bool moving = false;          // translating (any direction flag)
        bool movingBackward = false;
        bool strafeLeft = false;
        bool strafeRight = false;
        bool turnLeft = false;        // turning in place (the shuffle gate is the caller's)
        bool turnRight = false;
        bool grounded = true;
        bool jumping = false;         // in a jump that has left the ground
        bool swimming = false;        // SWIMMING
        bool flying = false;          // FLYING and in the air
        bool flyingSpline = false;    // on a Flying spline: a taxi
        float speed = -1.0f;          // yd/s over the ground; < 0 unknown (runs)
        float walkSpeed = 2.5f;
        float deltaTime = 0.0f;
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
    };

    struct Output {
        AnimOutput anim;              // valid=false: keep what plays
        uint32_t returnAnim = 0;      // a one-shot's follow-on (JumpStart -> Jump)
    };

    void configure(const AnimCapabilitySet& caps);
    void clear();
    /// SMSG_MOUNTSPECIAL_ANIM: MountSpecial (94) once.
    void requestSpecial() { if (active_) specialPending_ = true; }
    /// DISMOUNT ends it; the rest reach the mount through its input.
    void onEvent(AnimEvent event) { if (event == AnimEvent::DISMOUNT) clear(); }

    Output evaluate(const Input& in);

    [[nodiscard]] bool isActive() const { return active_; }
    [[nodiscard]] const AnimCapabilitySet& caps() const { return caps_; }

private:
    bool active_ = false;
    AnimCapabilitySet caps_;
    LocomotionFSM loco_;
    bool specialPending_ = false;
    bool specialPlaying_ = false;
    bool specialSeen_ = false;
    bool onSpline_ = false;
};

} // namespace rendering
} // namespace wowee
