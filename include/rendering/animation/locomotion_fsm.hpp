#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// LocomotionFSM
//
// Pure logic state machine for movement animation. No renderer dependency.
// States: IDLE · WALK · RUN · JUMP_START · JUMP_MID · JUMP_END · SWIM_IDLE · SWIM
//
// Grace timer is internal - no external locomotionStopGraceTimer_ needed.
// ============================================================================
class LocomotionFSM {
public:
    enum class State : uint8_t {
        IDLE, WALK, RUN,
        JUMP_START, JUMP_MID, JUMP_END, FALL,
        SWIM_IDLE, SWIM,
    };

    struct Input {
        bool moving = false;
        bool movingForward = false;
        bool sprinting = false;
        bool movingBackward = false;
        bool strafeLeft = false;
        bool strafeRight = false;
        bool grounded = true;
        bool jumping = false;
        bool swimming = false;
        bool sitting = false;
        bool sprintAura = false;      // Sprint/Dash aura - use SPRINT anim
        // Turning in place (stock CGUnit_C 0x0073E2C0): standing still, on
        // the ground, out of water and flight, with the turn flag 0x10 held or
        // the drawn body stepping counter-clockwise by more than 1e-5 rad this
        // frame -> ShuffleLeft; 0x20 or a clockwise step -> ShuffleRight. The
        // caller applies that gate; left wins when both are set.
        bool turnLeft = false;
        bool turnRight = false;
        float deltaTime = 0.0f;
        // Animation state for one-shot completion detection (jump start/end)
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
    };

    /// Process event and update internal state.
    void onEvent(AnimEvent event);

    /// Evaluate current state against input and capabilities.
    /// Returns AnimOutput with valid=false if no change needed (STAY policy).
    AnimOutput resolve(const Input& in, const AnimCapabilitySet& caps);

    [[nodiscard]] State getState() const { return state_; }
    void setState(State s) { state_ = s; }
    void reset();

    static constexpr uint8_t PRIORITY = 10;

private:
    State state_ = State::IDLE;

    // Grace timer: short delay before switching from WALK/RUN to IDLE
    // to avoid flickering on network jitter
    float graceTimer_ = 0.0f;
    bool wasSprinting_ = false;

    // One-shot tracking for jump start/end animations
    bool jumpStartSeen_ = false;
    bool jumpEndSeen_ = false;
    /// Last frame's in.jumping, so a new jump is seen on its rising edge even
    /// where the landing between two jumps was never observed.
    bool wasJumping_ = false;

    /// The shuffle playing while IDLE (0 none). Once started it plays its lap
    /// out even if the turn stops: the stock client re-picks the animation
    /// from Shuffle only while a turn or step flag is set (0x0071DE90), so
    /// the lap ends before Stand comes back.
    uint32_t shuffleAnim_ = 0;
    float shuffleLastTime_ = 0.0f;

    static constexpr float kGraceSec = 0.12f;
    static constexpr float kFallDelaySec = 0.25f;  ///< airborne this long without a jump is a fall
    float ungroundedSeconds_ = 0.0f;

    /// Internal: update state transitions based on input.
    void updateTransitions(const Input& in, const AnimCapabilitySet& caps);
    [[nodiscard]] bool oneShotComplete(const Input& in, uint32_t expectedAnimId) const;
    /// IDLE's animation: a shuffle while turning in place, else Stand.
    uint32_t resolveIdle(const Input& in, const AnimCapabilitySet& caps);
};

} // namespace rendering
} // namespace wowee
