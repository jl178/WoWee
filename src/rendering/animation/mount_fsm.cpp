#include "rendering/animation/mount_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/mount_rules.hpp"

namespace wowee {
namespace rendering {

void MountFSM::configure(const AnimCapabilitySet& caps) {
    caps_ = caps;
    loco_.reset();
    active_ = true;
    specialPending_ = false;
    specialPlaying_ = false;
    specialSeen_ = false;
    onSpline_ = false;
}

void MountFSM::clear() {
    active_ = false;
    caps_ = {};
    loco_.reset();
    specialPending_ = false;
    specialPlaying_ = false;
    specialSeen_ = false;
    onSpline_ = false;
}

MountFSM::Output MountFSM::evaluate(const Input& in) {
    Output out;
    if (!active_) return out;

    // A taxi: Fly (135) along the path (0x4F5290 in 0x717050), which a
    // model without it resolves through AnimationData's fallback (Swim).
    if (in.flyingSpline) {
        onSpline_ = true;
        specialPending_ = specialPlaying_ = false;
        out.anim = AnimOutput::ok(in.moving ? anim::FLY : anim::mountIdleOnSpline(), true);
        return out;
    }
    if (onSpline_) {
        // Off the spline: start the machine over from what the unit does now.
        onSpline_ = false;
        loco_.reset();
    }

    // MountSpecial: once, standing; moving cuts it (a re-pick replaces it).
    if (specialPending_ && !in.moving && in.grounded && !in.swimming && !in.flying) {
        specialPending_ = false;
        specialPlaying_ = true;
        specialSeen_ = false;
        out.anim = AnimOutput::ok(anim::MOUNT_SPECIAL, false);
        return out;
    }
    if (specialPlaying_) {
        if (in.moving || !in.grounded || in.swimming || in.flying) {
            specialPlaying_ = false;
        } else if (in.haveAnimState) {
            if (in.currentAnimId == anim::MOUNT_SPECIAL) specialSeen_ = true;
            const bool done = specialSeen_ &&
                (in.currentAnimId != anim::MOUNT_SPECIAL ||
                 (in.currentAnimDuration > 0.1f &&
                  in.currentAnimTime >= in.currentAnimDuration - 0.05f));
            if (!done) return out;  // stay
            specialPlaying_ = false;
            loco_.reset();
        } else {
            return out;
        }
    }

    LocomotionFSM::Input li;
    li.moving = in.moving;
    li.movingForward = in.moving && !in.movingBackward;
    li.movingBackward = in.moving && in.movingBackward;
    li.strafeLeft = in.moving && in.strafeLeft;
    li.strafeRight = in.moving && in.strafeRight;
    // In water or in the air, the swim strokes (0x717050, 0x71E340).
    li.swimming = in.swimming || in.flying;
    li.grounded = in.grounded || li.swimming;
    li.jumping = in.jumping && !li.swimming;
    const anim::MountPace pace = anim::mountGroundPace(in.speed, in.walkSpeed);
    li.sprinting = pace != anim::MountPace::Walk;          // Run or Sprint state
    li.sprintAura = pace == anim::MountPace::Sprint;       // Sprint (143)
    li.turnLeft = in.turnLeft;
    li.turnRight = !in.turnLeft && in.turnRight;
    li.deltaTime = in.deltaTime;
    li.currentAnimId = in.currentAnimId;
    li.currentAnimTime = in.currentAnimTime;
    li.currentAnimDuration = in.currentAnimDuration;
    li.haveAnimState = in.haveAnimState;

    out.anim = loco_.resolve(li, caps_);
    if (out.anim.valid && loco_.getState() == LocomotionFSM::State::JUMP_START &&
        caps_.resolvedJump != 0) {
        out.returnAnim = caps_.resolvedJump;  // JumpStart flows into the jump loop
    }
    return out;
}

} // namespace rendering
} // namespace wowee
