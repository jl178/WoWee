#pragma once

#include <cstdint>
#include <glm/vec3.hpp>

namespace wowee {
namespace rendering {

class Renderer;
class FootstepDriver;

// ============================================================================
// SfxStateDriver - extracted from AnimationController
//
// Tracks state transitions for activity SFX (jump, landing, swim) and
// mount ambient sounds.
// ============================================================================
class SfxStateDriver {
public:
    SfxStateDriver() = default;

    /// Track state transitions and trigger appropriate SFX.
    void update(float deltaTime, Renderer* renderer,
                bool mounted, bool taxiFlight,
                FootstepDriver& footstepDriver);

private:
    bool initialized_ = false;
    bool prevGrounded_ = true;
    bool prevFalling_ = false;
    float fallSeconds_ = 0.0f;  ///< time in the current fall, for hard landings
    float airTopZ_ = 0.0f;      ///< highest point since leaving the ground
    glm::vec3 lastPos_{0.0f};   ///< last frame's character position, to tell a teleport from a fall
    bool prevSwimming_ = false;

    // Seconds since the character last left the water. Climbing out is not a
    // jump and not a landing, but the vertical state passes through both on the
    // way - briefly airborne, then grounded - so the exertion sounds are held
    // off for a moment afterwards rather than for a single frame.
    float sinceWaterExit_ = kWaterExitSfxSuppress;
    static constexpr float kWaterExitSfxSuppress = 0.45f;
};

} // namespace rendering
} // namespace wowee
