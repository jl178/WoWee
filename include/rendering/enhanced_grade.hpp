#pragma once

/// The daytime colour grade of the reference client (1.60.1.70862), for the "Colour
/// grade" enhancement (docs/re/enhanced-lighting.md section 3).
///
/// The reference client's LightData carries a ColorGradingFileDataID per keyframe. On the
/// two continents the outdoor lights set it on the 12:00 keyframe only (Time
/// 1440 in half-minutes) and leave the 8:00 and 15:00 keyframes either side of
/// it at 0, no grade; Eastern Kingdoms' lights use file 8286666 (70 of its 83
/// lights) and Kalimdor's 8286665 (103 of 132). Each file is a 32x32x32 LUT
/// (BLP2, 1024x32 BGRA) applied to the finished, display-encoded picture by
/// ffxcolorgrading.bls: the scene clamped to 0..1, blue picks the slice, red
/// and green the texel. Both files turn out to be one curve applied to each
/// channel alone (the separable fit leaves no error), so the grade is the 32
/// values below with linear filtering between them - no hue shift; it
/// steepens the mid-tones by about a tenth and rolls the top fifth onto a
/// shoulder, which also raises saturation where channels differ.
///
/// No renderer includes, so a test can read it.

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {
namespace enhanced_grade {

inline constexpr int kCurveSize = 32;

/// 8286666, the Eastern Kingdoms grade: output for input i / 31.
inline constexpr float kEasternKingdoms[kCurveSize] = {
    0.000f, 0.031f, 0.063f, 0.094f, 0.129f, 0.165f, 0.200f, 0.235f,
    0.267f, 0.306f, 0.341f, 0.376f, 0.412f, 0.451f, 0.490f, 0.529f,
    0.565f, 0.604f, 0.647f, 0.686f, 0.725f, 0.765f, 0.804f, 0.851f,
    0.890f, 0.929f, 0.957f, 0.980f, 0.988f, 0.996f, 0.996f, 1.000f};

/// 8286665, the Kalimdor grade (a little gentler).
inline constexpr float kKalimdor[kCurveSize] = {
    0.000f, 0.031f, 0.063f, 0.094f, 0.129f, 0.165f, 0.196f, 0.231f,
    0.263f, 0.302f, 0.337f, 0.373f, 0.408f, 0.443f, 0.482f, 0.518f,
    0.553f, 0.588f, 0.631f, 0.667f, 0.706f, 0.741f, 0.780f, 0.824f,
    0.863f, 0.902f, 0.937f, 0.969f, 0.988f, 0.996f, 0.996f, 1.000f};

/// How much of the curve is laid on at noon. The reference client grades a picture lit in
/// linear light and tone-mapped; Wowee's 3.3.5 light is already bright in
/// the mid-tones, and the full lift washed them out in Jered's first play.
inline constexpr float kMaxWeight = 0.6f;

/// The curve for a map, or null where the reference client draws no grade (instances,
/// Outland and Northrend have no graded light in the reference client's data).
inline const float* curveForMap(int mapId) {
    if (mapId == 0) return kEasternKingdoms;
    if (mapId == 1) return kKalimdor;
    return nullptr;
}

/// How much of the grade the hour gives: the keyframe weight between 8:00
/// (none), 12:00 (all) and 15:00 (none), linear in between as LightData's
/// keyframes are. `dayFraction` is 0..1 from midnight.
inline float timeWeight(float dayFraction) {
    float h = dayFraction * 24.0f;
    h = h - 24.0f * std::floor(h / 24.0f);
    if (h <= 8.0f || h >= 15.0f) return 0.0f;
    if (h <= 12.0f) return (h - 8.0f) / 4.0f;
    return (15.0f - h) / 3.0f;
}

/// The curve at `x` (0..1), linear between its 32 points, as the LUT's
/// bilinear lookup reads it.
inline float apply(const float* curve, float x) {
    const float t = std::clamp(x, 0.0f, 1.0f) * float(kCurveSize - 1);
    const int i = std::min(static_cast<int>(t), kCurveSize - 2);
    const float f = t - float(i);
    return curve[i] + (curve[i + 1] - curve[i]) * f;
}

}  // namespace enhanced_grade
}  // namespace rendering
}  // namespace wowee
