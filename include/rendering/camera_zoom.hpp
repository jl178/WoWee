#pragma once

/// "Smooth camera zoom" (docs/enhancements-plan.md E20).
///
/// The stock client moves the camera's distance toward the wheel's request at
/// a constant cameraDistanceMoveSpeed (8.33 yd/s): it starts at full speed and
/// stops dead (CameraController::stepZoomTowardTarget). With the row on, the
/// distance follows the request as a critically damped spring instead: it
/// gathers speed, then settles onto the new distance without overshooting it,
/// and a notch turned while it is still moving carries its speed on. Only the
/// wheel's travel is eased; a wall still pulls the camera in at once (the
/// collision pull-in is applied after, unchanged).
///
/// Pure, for CameraController and test_phase2_motion.

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {
namespace camera_zoom {

/// The spring's time constant, seconds: a one-yard notch is nine tenths of the
/// way there in about 0.2 s, near the stock 0.12 s for a yard, and a long
/// spin of the wheel does not crawl as the stock constant speed does.
inline constexpr float kSmoothSeconds = 0.06f;
/// The fastest the eased zoom moves, yards a second; a spin from fully in to
/// fully out still takes a moment rather than a flash.
inline constexpr float kMaxSpeed = 60.0f;
/// Within this of the request and this slow, it is there.
inline constexpr float kSnapDistance = 0.002f;
inline constexpr float kSnapSpeed = 0.02f;

/// One step of the eased distance toward `target`, `velocity` carried
/// between steps (yd/s). Exact for any frame time: the critically damped
/// solution over dt, so the path is the same at 30 and 240 fps. Never passes
/// the target: a step that would is ended on it.
inline void step(float& distance, float& velocity, float target, float dt) {
    if (dt <= 0.0f) return;
    const float w = 1.0f / kSmoothSeconds;
    const float x0 = distance - target;
    const float v0 = velocity;
    const float e = std::exp(-w * dt);
    // x(t) = (x0 + (v0 + w x0) t) e^{-wt}; v(t) = (v0 - w (v0 + w x0) t) e^{-wt}
    const float c = v0 + w * x0;
    float x = (x0 + c * dt) * e;
    float v = (v0 - w * c * dt) * e;
    v = std::clamp(v, -kMaxSpeed, kMaxSpeed);
    // The speed cap can hold the spring back; it may not carry it past.
    const float maxMove = kMaxSpeed * dt;
    if (std::abs(x - x0) > maxMove) x = x0 + (x > x0 ? maxMove : -maxMove);
    if ((x0 > 0.0f && x < 0.0f) || (x0 < 0.0f && x > 0.0f)) {
        x = 0.0f;
        v = 0.0f;
    }
    if (std::abs(x) < kSnapDistance && std::abs(v) < kSnapSpeed) {
        x = 0.0f;
        v = 0.0f;
    }
    distance = target + x;
    velocity = v;
}

}  // namespace camera_zoom
}  // namespace rendering
}  // namespace wowee
