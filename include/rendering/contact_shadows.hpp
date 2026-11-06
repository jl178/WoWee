#pragma once

/// Contact shadows (docs/enhancements-plan.md E14, the "Contact shadows"
/// row). Not stock.
///
/// The sun's shadow map has texels of inches near the player (the unit map)
/// and of a foot or more beyond it (the world map), so a crate on a table, a
/// boot on the road or a tuft of grass meets its surface with no shadow line
/// where it touches. With the row on and sun shadows on, a late full-screen
/// pass (contact_shadow.frag, recorded after the scene pass closes, before the
/// late particles) walks a short ray from each pixel within kMaxDistance of
/// the camera toward the sun through the scene's depth; where something
/// stands in the way within kRayYards it takes away the sun's share of that
/// pixel's light - the share the stock light equation gives the sun there,
/// times Light.dbc's shadow darkness - and nothing more. A pixel the shadow
/// map already shades is not darkened again. Off: no pass, no pipeline.
///
/// The constants and the arithmetic here are the shader's (contact_shadow.frag
/// keeps the same numbers); test_phase2_light holds them.

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {
namespace contact_shadow {

/// The ray's reach toward the sun, yards: a boot's height, a crate's edge.
inline constexpr float kRayYards = 0.6f;
/// Steps along it, shared by each 2x2 quad of pixels: pixel q of the four
/// takes steps q and q + 4 (kStepsPerPixel depth reads), and the quad pools
/// what they found, each pixel taking a neighbour's finding only when the
/// neighbour's depth is within quadDepthTolerance of its own. Jittered per
/// quad so the steps do not band. It was six reads in every pixel; the march
/// is now a quarter of that at 2560x1080 (enhancements-plan.md 3.15).
inline constexpr int kSteps = 8;
inline constexpr int kStepsPerPixel = 2;
static_assert(kSteps == 4 * kStepsPerPixel, "the quad's four pixels take every step once");
inline constexpr float kQuadDepthYards = 0.1f;
inline constexpr float kQuadDepthPerYard = 0.02f;
inline float quadDepthTolerance(float viewDistance) {
    return kQuadDepthYards + kQuadDepthPerYard * std::max(viewDistance, 0.0f);
}
/// The step (0-based) that pixel `lane` of the quad (x parity + 2 y parity)
/// takes as its k-th.
inline int quadStep(int lane, int k) { return std::clamp(lane, 0, 3) + 4 * std::clamp(k, 0, kStepsPerPixel - 1); }
/// Pixels further than this from the camera are left alone; the last
/// kFadeYards fade it out. It was 35 and 12: past about 20 yards a boot's
/// contact line is a pixel or two at 1080 lines, and the pixels between 24
/// and 35 yards were most of what a street view marched.
inline constexpr float kMaxDistance = 24.0f;
inline constexpr float kFadeYards = 8.0f;
/// A depth sample counts as in the way when the ray point is behind it by
/// more than the bias and less than the thickness (so a wall far behind a
/// post does not shade the ground beside the post).
inline constexpr float kThicknessYards = 0.35f;
inline constexpr float kThicknessPerYard = 0.01f;

/// A pixel whose depth changes by more than this to its neighbour is on a
/// silhouette: its facing (from the depth's slope) belongs to neither
/// surface, so it is left alone rather than given a dark rim.
inline constexpr float kEdgeYards = 0.15f;
inline constexpr float kEdgePerYard = 0.03f;
inline bool onSilhouette(float depthStepToNeighbour, float viewDistance) {
    return std::abs(depthStepToNeighbour) > kEdgeYards + kEdgePerYard * std::max(viewDistance, 0.0f);
}

/// How far step `i` (0-based, jitter 0..1) lies along the ray, yards. Never
/// past the reach, never at the start (the pixel itself).
inline float stepDistance(int i, float jitter) {
    jitter = std::clamp(jitter, 0.0f, 1.0f);
    const float t = (static_cast<float>(std::clamp(i, 0, kSteps - 1)) + 0.5f + 0.5f * jitter) /
                    static_cast<float>(kSteps);
    return kRayYards * std::min(t, 1.0f);
}

/// The ray's start lifted off the surface toward the camera's side, yards,
/// so a flat floor does not shade itself through depth precision.
inline float startBias(float viewDistance) {
    return 0.02f + 0.002f * std::max(viewDistance, 0.0f);
}

inline float thickness(float viewDistance) {
    return kThicknessYards + kThicknessPerYard * std::max(viewDistance, 0.0f);
}

/// 1 near the camera, falling smoothly to 0 at kMaxDistance.
inline float distanceFade(float viewDistance) {
    const float a = kMaxDistance - kFadeYards;
    if (viewDistance <= a) return 1.0f;
    if (viewDistance >= kMaxDistance) return 0.0f;
    const float t = (kMaxDistance - viewDistance) / kFadeYards;
    return t * t * (3.0f - 2.0f * t);
}

/// An occluder met at step `i` shades less the further along the ray it was
/// found: a soft end instead of a cut at the reach.
inline float stepWeight(int i) {
    const float t = static_cast<float>(std::clamp(i, 0, kSteps - 1)) / static_cast<float>(kSteps);
    return 1.0f - t * t;
}

/// The factor a pixel's colour is multiplied by. The stock light is
/// ambient + diffuse * N.L * s, s = 1 - darkness * (1 - lit) for a shadow
/// value `lit` (1 in the sun); `mapLit` is the shadow map's, `contact` the
/// ray's occlusion 0..1. The new lit is min(mapLit, 1 - contact), so the
/// pixel is never darker than the sun's full shadow would make it and a
/// pixel already shaded stays as it is. `ambient` and `diffuse` are the
/// luminances of the two light colours, `ndotl` the facing.
inline float colourFactor(float ambient, float diffuse, float ndotl, float darkness, float mapLit,
                          float contact) {
    ndotl = std::max(ndotl, 0.0f);
    darkness = std::clamp(darkness, 0.0f, 1.0f);
    mapLit = std::clamp(mapLit, 0.0f, 1.0f);
    contact = std::clamp(contact, 0.0f, 1.0f);
    const float litNew = std::min(mapLit, 1.0f - contact);
    const float s = 1.0f - darkness * (1.0f - mapLit);
    const float sNew = 1.0f - darkness * (1.0f - litNew);
    const float d = diffuse * ndotl;
    const float before = ambient + d * s;
    if (!(before > 1e-4f)) return 1.0f;
    return std::clamp((ambient + d * sNew) / before, 0.0f, 1.0f);
}

}  // namespace contact_shadow
}  // namespace rendering
}  // namespace wowee
