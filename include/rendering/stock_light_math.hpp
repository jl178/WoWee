#pragma once

/// The stock client's light arithmetic, read out of Wow.exe 3.3.5a (12340).
///
/// Pure functions only, so the unit tests can hold them to the binary without
/// a DBC or a GPU. docs/re/world-lighting.md has the full walk-through; the
/// addresses below are where each rule lives.
///
/// - Bands (LightIntBand / LightFloatBand): 0x7eae70 finds the pair of keys
///   around the time, 0x7eb070 interpolates a colour byte by byte (rounded),
///   0x7eaef0 a float. Time is half-minutes, round(dayFraction*2880 - 0.5),
///   wrapping at 2880. A band with no keys is black (0xff000000) or 0.
/// - Light volumes: 0x7f1360 starts from the map's default light (the
///   Light.dbc row at 0,0,0; Light 1 if the map has none) at full weight and
///   paints every other light whose outer radius is at least 3 yards and that
///   is within that radius over it, farthest first (heap 0x7f0dc0/0x7f1280,
///   order 0x7ed0a0), each by 0x7ee5d0's linear weight. No normalising, no
///   limit on how many.
/// - Fog: 0x7f16f0, 0x7ecd80, 0x7ecd00, 0x7ed1b0. Linear between start and
///   end on Azeroth and Kalimdor; on maps from 530 on (Outland, Northrend and
///   later instances) the end moves out to the far clip and the curve gets an
///   exponent of 1.5 to 7 instead. The stock vertex shaders turn that into
///   fogcoord = clamp(pow(max(viewDepth*c12.x + c12.y, 0), c12.z), 0, 1) with
///   OPTION ARB_fog_linear, so what shows of a surface is 1 - fogcoord.
/// - Terrain (shaders\vertex\arbvp1\terrain.bls, pixel\arbfp1\terrain1-3.bls):
///   light = min(ambient + diffuse * clamp(N.L, 0, 1), 1) per channel, times
///   the MCCV colour (or 0.5 without one), times 2 in the pixel shader; the
///   chunk shadow (MCSH, and the shadow map where it is on) takes it to 70%.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::stock_light {

/// Half-minutes in a day: the unit of every band key.
inline constexpr int kHalfMinutesPerDay = 2880;

/// 0x7f3230 / 0x7ee510: the key the bands are sampled at.
inline int bandTime(float dayFraction) {
    const float t = dayFraction * static_cast<float>(kHalfMinutesPerDay);
    // The client's ROUND is the FPU's default: to nearest, ties to even.
    int k = static_cast<int>(std::nearbyint(t - 0.5f));
    k %= kHalfMinutesPerDay;
    if (k < 0) k += kHalfMinutesPerDay;
    return k;
}

/// Where 0x7eae70 lands: the two keys and how far between them.
struct BandSegment {
    int lo = 0;
    int hi = 0;
    float t = 0.0f;
};

/// 0x7eae70. Walks the keys in file order for the first pair (i, i+1 mod n)
/// that holds the time; a pair that runs backwards wraps through midnight.
/// Keys need not start at zero. Returns {0,0,0} for no keys.
inline BandSegment bandSegment(const uint16_t* times, int count, int time) {
    BandSegment s;
    if (count <= 0) return s;
    for (int i = 0; i < count; ++i) {
        const int a = times[i];
        const int j = (i + 1) % count;
        int b = times[j];
        int now = time;
        if (a < b) {
            if (now < a || b < now) continue;
        } else {
            // b <= a: the pair spans midnight (or there is only one key), and
            // holds everything from a round to b, both ends included.
            if (now > b && now < a) continue;
            b += kHalfMinutesPerDay;
            if (now < a) now += kHalfMinutesPerDay;
        }
        s.lo = i;
        s.hi = j;
        s.t = b != a ? static_cast<float>(now - a) / static_cast<float>(b - a) : 0.0f;
        return s;
    }
    return s;
}

/// 0x7eb070: one byte of a colour between two keys, as the client rounds it.
inline uint8_t lerpByte(uint8_t a, uint8_t b, float t) {
    const float v = static_cast<float>(static_cast<int>(b) - static_cast<int>(a)) * t +
                    static_cast<float>(a);
    // Rounded as the FPU rounds by default: to nearest, ties to even.
    return static_cast<uint8_t>(static_cast<int>(std::nearbyint(v)) & 0xff);
}

/// 0x7eb070: a packed 0x00RRGGBB colour band sampled at `time`. No keys is
/// black.
inline uint32_t sampleColorBand(const uint16_t* times, const uint32_t* colors, int count,
                                int time) {
    if (count <= 0) return 0;
    const BandSegment s = bandSegment(times, count, time);
    const uint32_t a = colors[s.lo], b = colors[s.hi];
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        out |= static_cast<uint32_t>(lerpByte(static_cast<uint8_t>(a >> shift),
                                              static_cast<uint8_t>(b >> shift), s.t))
               << shift;
    }
    return out;
}

/// 0x7eaef0: a float band at `time`. No keys is 0.
inline float sampleFloatBand(const uint16_t* times, const float* values, int count, int time) {
    if (count <= 0) return 0.0f;
    const BandSegment s = bandSegment(times, count, time);
    return (values[s.hi] - values[s.lo]) * s.t + values[s.lo];
}

/// 0x7ee5d0: how much of a light volume is painted over what lies under it.
/// Full inside the inner radius, falling off linearly to nothing at the outer.
inline float volumeWeight(float distance, float inner, float outer) {
    if (distance <= inner) return 1.0f;
    if (outer <= inner) return 0.0f;
    const float w = 1.0f - (distance - inner) / (outer - inner);
    return std::clamp(w, 0.0f, 1.0f);
}

/// 0x7f1360: lights whose outer radius is under this are never considered.
inline constexpr float kMinVolumeOuterRadius = 3.0f;
/// 0x7ed0a0: two lights closer together than this are ordered by size.
inline constexpr float kSameCentreDistance = 1.0f / 3.0f;

/// One candidate for the paint order.
struct VolumeKey {
    float distanceSq = 0.0f;   ///< from the light-pick position to its centre
    float innerRadius = 0.0f;
    glm::vec3 centre{0.0f};
};

/// 0x7ed0a0 as the heap uses it: true if `a` is painted before `b`. Farther
/// lights first, so the nearest ends on top; lights sharing a centre (within
/// a third of a yard) bigger inner radius first, so the smaller, more
/// specific one ends on top.
inline bool paintedBefore(const VolumeKey& a, const VolumeKey& b) {
    const glm::vec3 d = a.centre - b.centre;
    if (std::sqrt(glm::dot(d, d)) <= kSameCentreDistance) {
        return a.innerRadius > b.innerRadius;
    }
    return a.distanceSq > b.distanceSq;
}

/// What the distance fog is for one frame.
struct Fog {
    float start = 0.0f;
    float end = 0.0f;
    /// The fog curve's power: 1 is linear (Azeroth), above 1 holds the fog
    /// back near the start and gathers it toward the far clip.
    float exponent = 1.0f;
};

/// 0x7ecd80: the band value can bring the fog no closer than this (yards).
inline constexpr float kMinFogEnd = 10.0f;
/// 0x7ecd80: the band fog end above which the far-clip curve takes over.
inline constexpr float kExponentFogEndThreshold = 1000.0f / 36.0f;
/// 0x7ecd00's constants.
inline constexpr float kExponentFarClipCap = 700.0f;
inline constexpr float kExponentFarClipMargin = 200.0f;
inline constexpr float kExponentBase = 1.5f;
inline constexpr float kExponentRange = 5.5f;
/// 0x781700: the far-clip fog is used from this map id on.
inline constexpr uint32_t kFirstFarClipFogMap = 530;

/// 0x781700: whether a map uses the far-clip fog curve. The client also
/// needs vertex shaders for it (the Gx caps check), which every card it
/// supports in this renderer has.
inline bool usesFarClipFog(uint32_t mapId) { return mapId >= kFirstFarClipFogMap; }

/// 0x7ecd00: the exponent for a fog that the band put between `start` and
/// `end`, with the given far clip.
inline float fogExponent(float start, float end, float farClip) {
    const float span = std::min(kExponentFarClipCap, farClip) - kExponentFarClipMargin;
    const float width = end - start;
    if (span > 0.0f && width <= span) {
        return (1.0f - width / span) * kExponentRange + kExponentBase;
    }
    return kExponentBase;
}

/// The fog one light set asks for, after 0x7ecd80 (per light, before the
/// lights are blended): `bandEnd` is FloatBand 0 already in yards,
/// `bandScaler` FloatBand 1 clamped to [-1, 1].
struct LightFog {
    float end = 0.0f;
    float scaler = 0.0f;
    float exponent = 1.0f;
};

inline LightFog lightFog(float bandEnd, float bandScaler, float farClip, bool farClipFog) {
    LightFog f;
    f.end = std::max(bandEnd, kMinFogEnd);
    f.scaler = std::clamp(bandScaler, -1.0f, 1.0f);
    f.exponent = 1.0f;
    if (farClipFog) {
        if (f.end > kExponentFogEndThreshold) {
            f.exponent = fogExponent(f.scaler * f.end, f.end, farClip);
            f.end = farClip;
        }
        if (f.scaler < 0.0f) f.scaler = 0.0f;
    }
    return f;
}

/// 0x7f16f0: the frame's fog from the blended light. Under liquid on a
/// far-clip map the exponent doubles.
inline Fog frameFog(const LightFog& blended, float farClip, bool farClipFog, bool underLiquid) {
    Fog f;
    f.end = std::min(blended.end, farClip);
    f.start = f.end * blended.scaler;
    f.exponent = blended.exponent;
    if (farClipFog && underLiquid) f.exponent *= 2.0f;
    return f;
}

/// What the stock shaders leave of a surface at view depth `depth`: 1 is
/// none of the fog, 0 all of it.
inline float fogVisibility(float depth, const Fog& fog) {
    const float range = fog.end - fog.start;
    if (range <= 1e-4f) return depth >= fog.end ? 0.0f : 1.0f;
    const float x = std::max((depth - fog.start) / range, 0.0f);
    return 1.0f - std::clamp(std::pow(x, fog.exponent), 0.0f, 1.0f);
}

/// terrain.bls: the light a terrain vertex carries, per channel, before the
/// texture. Clamped at one, as the vertex colour it travels in is.
inline glm::vec3 terrainLight(const glm::vec3& ambient, const glm::vec3& diffuse, float nDotL) {
    return glm::min(ambient + diffuse * std::clamp(nDotL, 0.0f, 1.0f), glm::vec3(1.0f));
}

/// terrain1-3.bls: what the chunk shadow (1 lit, 0 shadowed) leaves of it.
inline constexpr float kTerrainShadowFloor = 0.7f;
inline float terrainShadowScale(float lit) {
    return kTerrainShadowFloor + (1.0f - kTerrainShadowFloor) * std::clamp(lit, 0.0f, 1.0f);
}

}  // namespace wowee::rendering::stock_light
