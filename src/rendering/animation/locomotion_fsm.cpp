#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

// ── One-shot completion helper ─────────────────────────────────────────────────────────

bool LocomotionFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
}

// ── Event handling ───────────────────────────────────────────────────────────

void LocomotionFSM::onEvent(AnimEvent event) {
    switch (event) {
        case AnimEvent::SWIM_ENTER:
            state_ = State::SWIM_IDLE;
            break;
        case AnimEvent::SWIM_EXIT:
            state_ = State::IDLE;
            break;
        case AnimEvent::JUMP:
            if (state_ != State::SWIM_IDLE && state_ != State::SWIM) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            }
            break;
        case AnimEvent::LANDED:
            if (state_ == State::JUMP_MID || state_ == State::JUMP_START) {
                jumpEndSeen_ = false;
                state_ = State::JUMP_END;
            }
            break;
        case AnimEvent::MOVE_START:
            graceTimer_ = kGraceSec;
            break;
        case AnimEvent::MOVE_STOP:
            // Grace timer handles the delay in updateTransitions
            break;
        default:
            break;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void LocomotionFSM::updateTransitions(const Input& in, const AnimCapabilitySet& caps) {
    ungroundedSeconds_ = in.grounded ? 0.0f : ungroundedSeconds_ + in.deltaTime;
    // Update grace timer
    if (in.moving) {
        graceTimer_ = kGraceSec;
        wasSprinting_ = in.sprinting;
    } else {
        graceTimer_ = std::max(0.0f, graceTimer_ - in.deltaTime);
    }

    const bool effectiveMoving = in.moving || graceTimer_ > 0.0f;
    const bool effectiveSprinting = in.sprinting || (!in.moving && effectiveMoving && wasSprinting_);

    switch (state_) {
        case State::IDLE:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded && ungroundedSeconds_ > kFallDelaySec) {
                // A real fall. A frame or two off the ground running down a
                // slope is not one, and used to flicker Fall and a landing.
                state_ = State::FALL;
            } else if (effectiveMoving && effectiveSprinting) {
                state_ = State::RUN;
            } else if (effectiveMoving) {
                state_ = State::WALK;
            }
            break;

        case State::WALK:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded && ungroundedSeconds_ > kFallDelaySec) {
                // A real fall. A frame or two off the ground running down a
                // slope is not one, and used to flicker Fall and a landing.
                state_ = State::FALL;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (effectiveSprinting) {
                state_ = State::RUN;
            }
            break;

        case State::RUN:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded && ungroundedSeconds_ > kFallDelaySec) {
                // A real fall. A frame or two off the ground running down a
                // slope is not one, and used to flicker Fall and a landing.
                state_ = State::FALL;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (!effectiveSprinting) {
                state_ = State::WALK;
            }
            break;

        case State::JUMP_START:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                state_ = State::JUMP_END;
                jumpEndSeen_ = false;
            } else if (caps.resolvedJumpStart == 0) {
                // Model doesn't have JUMP_START animation - skip to mid-air
                state_ = State::JUMP_MID;
            } else if (in.haveAnimState) {
                // Use the same resolved ID that resolve() outputs
                uint32_t expected = caps.resolvedJumpStart;
                if (in.currentAnimId == expected) jumpStartSeen_ = true;
                // Also detect completion via renderer's auto-STAND reset:
                // once the animation was seen and currentAnimId changed, it completed.
                if (jumpStartSeen_ && oneShotComplete(in, expected)) {
                    state_ = State::JUMP_MID;
                }
            } else {
                // No animation state available - fall through after 1 frame
                state_ = State::JUMP_MID;
            }
            break;

        case State::JUMP_MID:
        case State::FALL:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                jumpEndSeen_ = false;
                state_ = State::JUMP_END;
            }
            break;

        case State::JUMP_END: {
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
                break;
            }
            // Landing while running plays JumpLandRun through; landing still
            // plays JumpEnd through. A model without the one-shot moves on.
            const uint32_t expected = effectiveMoving ? caps.resolvedJumpLandRun : caps.resolvedJumpEnd;
            const State after = effectiveMoving ? (effectiveSprinting ? State::RUN : State::WALK) : State::IDLE;
            if (expected == 0 || !in.haveAnimState) {
                state_ = after;
            } else {
                if (in.currentAnimId == expected) jumpEndSeen_ = true;
                if (jumpEndSeen_ && oneShotComplete(in, expected)) state_ = after;
            }
        }
            break;

        case State::SWIM_IDLE:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (effectiveMoving) {
                state_ = State::SWIM;
            }
            break;

        case State::SWIM:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (!effectiveMoving) {
                state_ = State::SWIM_IDLE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput LocomotionFSM::resolve(const Input& in, const AnimCapabilitySet& caps) {
    updateTransitions(in, caps);

    const bool anyStrafeLeft = in.strafeLeft && !in.strafeRight && !in.movingBackward;
    const bool anyStrafeRight = in.strafeRight && !in.strafeLeft && !in.movingBackward;

    uint32_t animId = anim::STAND;
    bool animSelected = true;
    bool loop = true;

    switch (state_) {
        case State::IDLE:
            animId = anim::STAND;
            break;

        case State::WALK:
            if (in.movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else {
                animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
            }
            break;

        case State::RUN:
            if (in.movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else if (in.sprintAura) {
                animId = caps.resolvedSprint ? caps.resolvedSprint : anim::RUN;
            } else {
                animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
            }
            break;

        case State::JUMP_START:
            animId = caps.resolvedJumpStart ? caps.resolvedJumpStart : anim::JUMP_START;
            loop = false;
            break;
        case State::JUMP_MID:
            animId = caps.resolvedJump ? caps.resolvedJump : anim::JUMP;
            loop = true;  // Must loop - long falls outlast a single play cycle
            break;
        case State::FALL:
            animId = caps.resolvedFall ? caps.resolvedFall
                   : caps.resolvedJump ? caps.resolvedJump : anim::JUMP;
            loop = true;
            break;

        case State::JUMP_END:
            if (in.moving && caps.resolvedJumpLandRun) animId = caps.resolvedJumpLandRun;
            else animId = caps.resolvedJumpEnd ? caps.resolvedJumpEnd : anim::JUMP_END;
            loop = false;
            break;

        case State::SWIM_IDLE:
            animId = caps.resolvedSwimIdle ? caps.resolvedSwimIdle : anim::SWIM_IDLE;
            break;

        case State::SWIM:
            if (in.movingBackward) {
                animId = caps.resolvedSwimBackwards ? caps.resolvedSwimBackwards
                       : caps.resolvedSwim           ? caps.resolvedSwim
                       :                               anim::SWIM;
            } else if (anyStrafeLeft) {
                animId = caps.resolvedSwimLeft ? caps.resolvedSwimLeft
                       : caps.resolvedSwim    ? caps.resolvedSwim
                       :                        anim::SWIM;
            } else if (anyStrafeRight) {
                animId = caps.resolvedSwimRight ? caps.resolvedSwimRight
                       : caps.resolvedSwim     ? caps.resolvedSwim
                       :                         anim::SWIM;
            } else {
                animId = caps.resolvedSwim ? caps.resolvedSwim : anim::SWIM;
            }
            break;
    }

    if (!animSelected) return AnimOutput::stay();
    return AnimOutput::ok(animId, loop);
}

// ── Reset ────────────────────────────────────────────────────────────────────

void LocomotionFSM::reset() {
    state_ = State::IDLE;
    graceTimer_ = 0.0f;
    wasSprinting_ = false;
    jumpStartSeen_ = false;
    jumpEndSeen_ = false;
}

} // namespace rendering
} // namespace wowee
