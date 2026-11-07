#pragma once

// The stock 3.3.5a (build 12340) camera input rules, as numbers and pure
// functions. Each names the Wow.exe function it was read from; the prose is
// docs/re/movement-camera.md sections 4 and 10.
//
// Angles here are degrees unless a name says otherwise.

#include <algorithm>
#include <cmath>

namespace wowee::rendering::stock_camera {

// ---- Mouse look (0x6020B0, 0x47C020, 0x47BF90, 0x86A070) ----

/// cameraYawMoveSpeed / cameraPitchMoveSpeed defaults (0x5FD910), degrees.
inline constexpr float kDefaultYawMoveSpeed = 180.0f;
inline constexpr float kDefaultPitchMoveSpeed = 90.0f;
/// The Mouse Look Speed slider (InterfaceOptionsPanels: cameraYawMoveSpeed
/// 90..270 step 10; its OnValueChanged also sets cameraPitchMoveSpeed to half).
inline constexpr float kMinYawMoveSpeed = 90.0f;
inline constexpr float kMaxYawMoveSpeed = 270.0f;
/// What the Mouse Look Speed slider writes to cameraPitchMoveSpeed.
inline float pitchMoveSpeedForSlider(float yawMoveSpeed) { return yawMoveSpeed * 0.5f; }

/// The per-unit look scales, 1/800 (0xA1E904) and 1/600 (0xA1E900): one
/// 800x600 reference screen width of mouse travel turns cameraYawMoveSpeed
/// divided by the DDC width.
inline constexpr float kYawPerUnit = 0.00125f;
inline constexpr float kPitchPerUnit = 0.00166666670f;

/// The DDC rectangle (0x47BF90): the screen with a unit diagonal. 0x47C020
/// divides the look delta by these (0xAC0CB4 width, 0xAC0CB8 height).
inline float ddcWidth(float aspect) {
    aspect = std::max(aspect, 1e-3f);
    return aspect / std::sqrt(aspect * aspect + 1.0f);
}
inline float ddcHeight(float aspect) {
    aspect = std::max(aspect, 1e-3f);
    return 1.0f / std::sqrt(aspect * aspect + 1.0f);
}

/// mouseSpeed (registered 0x401B60 from the OS pointer speed x 0.1; set by
/// 0x4015E0 -> 0x86A070): clamped to [0.1, 2.0] (0x9E3004, 0xA4040C), times
/// 10 (0x9E30CC), rounded, and handed to SystemParametersInfo
/// SPI_SETMOUSESPEED. It is the Windows pointer speed and nothing else; the
/// client's look code never reads it.
inline constexpr float kDefaultMouseSpeed = 1.0f;  // pointer speed 10
inline constexpr float kMinMouseSpeed = 0.1f;
inline constexpr float kMaxMouseSpeed = 2.0f;
/// The Mouse Sensitivity slider (InterfaceOptionsPanels: 0.5..1.5 step 0.05).
inline constexpr float kSliderMinMouseSpeed = 0.5f;
inline constexpr float kSliderMaxMouseSpeed = 1.5f;

inline int osPointerSpeed(float mouseSpeed) {
    const float v = std::clamp(mouseSpeed, kMinMouseSpeed, kMaxMouseSpeed) * 10.0f;
    return std::clamp(static_cast<int>(std::nearbyint(v)), 1, 20);
}

/// What Windows does with a pointer speed (enhance pointer precision off):
/// 1/32, 1/16, 1/8, then eighths up to 1 at 10, then quarters to 3.5 at 20.
/// The stock look delta is the cursor's (0x86A210 WM_MOUSEMOVE -> 0x868C10
/// GetPhysicalCursorPos less the recentred point), so it carries this.
inline float osPointerMultiplier(int speed) {
    speed = std::clamp(speed, 1, 20);
    if (speed == 1) return 1.0f / 32.0f;
    if (speed == 2) return 1.0f / 16.0f;
    if (speed <= 10) return static_cast<float>(speed - 2) / 8.0f;
    return 1.0f + static_cast<float>(speed - 10) * 0.25f;
}

struct LookDelta {
    float yawDeg = 0.0f;    ///< subtract from the camera yaw (0x5FE5F0(-yaw))
    float pitchDeg = 0.0f;  ///< the stock pitch change for a downward delta
};

/// A relative mouse move of (dx, dy) cursor pixels (event 0xE, 0x47FA60: the
/// pixel deltas as floats, event +0x24 / +0x28) turned into the camera's turn
/// (0x6020B0): `dx / ddcWidth * 1/800 * cameraYawMoveSpeed` and
/// `dy / ddcHeight * 1/600 * cameraPitchMoveSpeed`, the pointer speed applied
/// first as Windows would. Inversion is the caller's (mouseInvertYaw /
/// mouseInvertPitch multiply by -1, 0x9E2EF4).
inline LookDelta mouseLook(float dxPixels, float dyPixels, float aspect,
                           float yawMoveSpeed, float pitchMoveSpeed, float mouseSpeed) {
    const float pointer = osPointerMultiplier(osPointerSpeed(mouseSpeed));
    LookDelta d;
    d.yawDeg = dxPixels * pointer / ddcWidth(aspect) * kYawPerUnit * yawMoveSpeed;
    d.pitchDeg = dyPixels * pointer / ddcHeight(aspect) * kPitchPerUnit * pitchMoveSpeed;
    return d;
}

// ---- Held view motion (MoveView*Start/Stop, 0x5FF000 -> 0x6000E0) ----

/// One frame of the held MoveView motions (0x6000E0, ms since the last frame
/// x 0.001): In/Out at cameraDistanceMoveSpeed x speed yd/s, Right/Left at
/// cameraYawMoveSpeed x speed deg/s and Up/Down at cameraPitchMoveSpeed x
/// speed. Right RAISES the stock yaw (+0x260), the way a mouse moved left
/// turns it - the camera goes round to the character's right; Up RAISES the
/// stock pitch (+0x230), which looks further down - the camera goes up.
/// Returned in this client's sense: yaw as it adds to the camera yaw, pitch
/// positive looking up.
struct ViewMoveStep {
    float zoomOut = 0.0f;   ///< yards, out positive
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
};
inline ViewMoveStep viewMoveStep(float in, float out, float right, float left, float up,
                                 float down, float distanceMoveSpeed, float yawMoveSpeed,
                                 float pitchMoveSpeed, float dt) {
    ViewMoveStep s;
    s.zoomOut = (out - in) * distanceMoveSpeed * dt;
    s.yawDeg = (right - left) * yawMoveSpeed * dt;
    s.pitchDeg = (down - up) * pitchMoveSpeed * dt;
    return s;
}

}  // namespace wowee::rendering::stock_camera
