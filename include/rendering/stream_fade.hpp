#pragma once

/// "Fade in new scenery" (docs/enhancements-plan.md E2): terrain chunks,
/// buildings and doodads that arrive with a streamed tile dissolve in over a
/// moment instead of appearing whole in one frame.
///
/// Each streamed thing is stamped with the renderer's stream-fade clock when
/// it comes in (only while the row is on - with it off nothing is stamped and
/// every draw is the stock one). The draw turns the time since into how much
/// is still hidden, and the fragment shaders drop that fraction of the pixels
/// on a 4x4 ordered pattern (stream_fade.glsli), the way the M2 distance fade
/// thins a cutout: an opaque wall cannot be alpha blended without sorting, but
/// it can be thinned. The C++ here is the shaders' twin, for the draw code and
/// the test (test_stream_fade).

#include <cstdint>

namespace wowee {
namespace rendering {
namespace stream_fade {

/// How long the dissolve takes. Long enough to read as a fade on a flying
/// mount, short enough that nothing seems to linger half there.
inline constexpr float kSeconds = 0.4f;

/// How much of a thing stamped at `start` is still hidden at `now`: 1 as it
/// arrives, 0 once it is fully in. A negative stamp - nothing streamed it, or
/// the row was off - is never hidden.
constexpr float hidden(float start, float now) {
    if (start < 0.0f) return 0.0f;
    const float t = (now - start) / kSeconds;
    if (t <= 0.0f) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    return 1.0f - t;
}

/// The hidden fraction as the byte the M2 draw carries in its instance flags
/// (bits 24-31, m2.vert): 0 shown, 255 hidden. Rounded up, so a thing is not
/// shown whole a step before its time is up.
constexpr uint32_t hideByte(float hide) {
    if (hide <= 0.0f) return 0u;
    if (hide >= 1.0f) return 255u;
    const float v = hide * 255.0f;
    const uint32_t floor = static_cast<uint32_t>(v);
    return static_cast<float>(floor) < v ? floor + 1u : floor;
}

/// The M2 instance flags bits for a stamp: 0 - the stock flags untouched -
/// whenever nothing is hidden.
constexpr int32_t m2Flags(float start, float now) {
    return static_cast<int32_t>(hideByte(hidden(start, now)) << 24);
}

/// The 4x4 Bayer threshold at a pixel, as m2.frag's bayerDither4x4 and
/// stream_fade.glsli have it.
constexpr float bayer4x4(int x, int y) {
    constexpr float m[16] = {
         0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
        12.0f / 16.0f,  4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
         3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
        15.0f / 16.0f,  7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f,
    };
    return m[(x & 3) + (y & 3) * 4];
}

/// Whether the shaders drop the pixel at (x, y) for a hidden fraction.
constexpr bool pixelHidden(float hide, int x, int y) {
    if (hide <= 0.0f) return false;
    return (1.0f - hide) <= bayer4x4(x, y);
}

}  // namespace stream_fade
}  // namespace rendering
}  // namespace wowee
