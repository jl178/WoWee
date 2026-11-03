#pragma once

/// The stock 3.3.5a (12340) underwater particulates: the motes that hang in
/// the water around the camera while it is in a liquid. See docs/re/water.md
/// (Underwater) for the addresses; in short:
///
/// - One object of 4000 motes (0x79e100, made in World init 0x780f50 with
///   scale unit 1/36, a 30 yard box and Textures\WaterPoop02.blp), each a
///   camera-relative position in [-15, 15] yards on every axis and a size of
///   rand[0.5, 1.5] x LiquidType ParticleScale / 36 (0x79b8e0, 0x79b360).
/// - On by default (world flag 0x2000000, the waterParticulates command);
///   shown only while the camera is in a liquid whose LiquidType Flags have
///   0x8 (water, ocean and magma; not slime).
/// - Each frame (0x79bf40) the motes stay put in the world - the camera's
///   move is taken off them - drift by the liquid's ParticleMovement
///   (0x79be50) and wrap into the box; a jump of more than 30 yards makes a
///   new set.
/// - Drawn (0x79ca70) as camera-facing quads of WaterPoop02's 5x5 atlas, the
///   first 666 in front of the camera (a 90 degree cone), white, alpha
///   blended, no depth write, fogged only when LiquidType Flags has 0x10.

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wowee::rendering::stock_particulates {

inline constexpr int kMoteCount = 4000;
/// The vertex buffer holds 0xa68 vertices: 666 quads at most.
inline constexpr int kMaxDrawn = 666;
inline constexpr float kHalfBox = 15.0f;
inline constexpr float kBox = 30.0f;
/// A camera move longer than this (in one frame) regenerates the set.
inline constexpr float kRegenerateDistance = 30.0f;
/// ParticleScale x 1/36 (0xa3f854) is a mote's size unit in yards.
inline constexpr float kScaleUnit = 1.0f / 36.0f;
/// ParticleMovement 1 sinks and 2 rises at this many yards a second (0xadfbbc).
inline constexpr float kVerticalDrift = 0.02f;
/// One cell of the WaterPoop02 atlas: 51 of its 256 texels.
inline constexpr float kCellSize = 51.0f / 256.0f;

struct Mote {
    glm::vec3 offset{0.0f};  // from the camera, yards
    float size = 0.0f;       // yards
};

/// The slow "current" water motes move in (ParticleMovement 0, 0x79bcc0).
struct Current {
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    float rate = 0.0125f;  // half-cycles of the push per second / 2
    float amp = 0.005f;    // yards per frame at the push's peak
    float phase = 0.0f;    // seconds into this push
};

/// A mote's size for a uniform draw u in [0, 1): rand[0.5, 1.5] x scale / 36.
inline float moteSize(float u, float particleScale) {
    return (0.5f + u) * particleScale * kScaleUnit;
}

/// A new current from three uniform draws in [0, 1) (0x79bcc0): two angles
/// over [-pi, pi], a direction that never points down and only a quarter as
/// steep as it is wide, a rate of rand[1, 2) x 0.0125 and an amplitude of
/// rand[1, 2) x 0.005.
inline Current makeCurrent(float u0, float u1, float u2, float u3) {
    constexpr float kPi = 3.14159265358979f;
    const float a = (u0 * 2.0f - 1.0f) * kPi;
    const float b = (u1 * 2.0f - 1.0f) * kPi;
    glm::vec3 d(std::cos(b) * std::sin(a), std::sin(b) * std::sin(a), std::fabs(std::cos(a)) * 0.25f);
    const float len = glm::length(d);
    Current c;
    c.dir = len > 1e-6f ? d / len : glm::vec3(0.0f, 0.0f, 1.0f);
    c.rate = (1.0f + u2) * 0.0125f;
    c.amp = (1.0f + u3) * 0.005f;
    c.phase = 0.0f;
    return c;
}

/// Whether the current has run its course (phase x rate past half a cycle)
/// and a new one is due.
inline bool currentSpent(const Current& c) { return c.phase * c.rate > 0.5f; }

/// This frame's push on every mote (0x79be50). Movement 0 is the current:
/// dir x amp x sin(2 pi phase rate), a per-frame step - the stock client does
/// not scale it by the frame time. 1 sinks and 2 rises at 0.02 yd/s. The
/// current's phase must already have been advanced by dt.
inline glm::vec3 drift(int movement, const Current& c, float dt) {
    constexpr float kTwoPi = 6.28318530717959f;
    switch (movement) {
        case 0: return c.dir * (c.amp * std::sin(kTwoPi * c.phase * c.rate));
        case 1: return glm::vec3(0.0f, 0.0f, -kVerticalDrift * dt);
        case 2: return glm::vec3(0.0f, 0.0f, kVerticalDrift * dt);
        default: return glm::vec3(0.0f);
    }
}

/// Into [-15, 15] on one axis, by whole boxes.
inline float wrap(float v) {
    if (v > kHalfBox) v -= kBox * std::floor((v + kHalfBox) / kBox);
    else if (v < -kHalfBox) v += kBox * std::floor((kHalfBox - v) / kBox);
    return v;
}

/// One mote, one frame: the camera's move taken off (it stays in the world),
/// the drift added, wrapped back into the box.
inline glm::vec3 step(const glm::vec3& offset, const glm::vec3& cameraMove, const glm::vec3& push) {
    const glm::vec3 p = offset - cameraMove + push;
    return {wrap(p.x), wrap(p.y), wrap(p.z)};
}

/// Drawn only in front of the camera, inside a 90 degree cone round its view
/// axis: view-space depth d > 0 with |x| < d and |y| < d.
inline bool inViewCone(float viewX, float viewY, float depth) {
    return depth > 0.0f && std::fabs(viewX) < depth && std::fabs(viewY) < depth;
}

/// Table 0xa3f970: the atlas cell of mote `index` for LiquidType
/// ParticleTexSlots `slots` (0 water, 1 magma). Mote i + 1 takes the row's
/// entry i & 7; mote 0 always takes cell 8 (a stock quirk, kept).
inline int atlasCell(int slots, int index) {
    static constexpr int kTable[4][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {8, 9, 10, 11, 8, 9, 10, 11},
        {12, 12, 12, 12, 12, 12, 12, 12},
        {13, 14, 15, 16, 13, 14, 15, 16},
    };
    if (slots < 0 || slots > 3) slots = 0;
    if (index == 0) return 8;
    return kTable[slots][(index - 1) & 7];
}

/// The atlas cell's top-left uv (0xadf838: 8 floats a cell). Cells 0-11 are
/// rows 0-2 of four, 12-16 the fifth column, 17-20 row 3 and 21-24 row 4.
inline glm::vec2 cellOrigin(int cell) {
    int col = 0;
    int row = 0;
    if (cell < 12) { col = cell % 4; row = cell / 4; }
    else if (cell <= 16) { col = 4; row = cell - 12; }
    else if (cell <= 20) { col = cell - 17; row = 3; }
    else { col = (cell - 21) % 4; row = 4; }
    return {static_cast<float>(col) * kCellSize, static_cast<float>(row) * kCellSize};
}

}  // namespace wowee::rendering::stock_particulates
