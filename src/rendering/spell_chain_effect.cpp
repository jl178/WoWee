#include "rendering/spell_chain_effect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee {
namespace rendering {

namespace {
constexpr float kTwoPi = 6.28318530718f;
// Wow.exe 0x9EA27C: below this a basis component counts as zero.
constexpr float kBasisEpsilon = 2.384185791015625e-07f;
// 0x9EA4C8: the longest bolt the joints are laid for.
constexpr float kMaxBoltLength = 10000.0f;
// 0x9AB2E0: joints (points) a bolt may have.
constexpr int kMinJoints = 2, kMaxJoints = 1000;

// The bolt's own frame (0x9A9260): side is level and square to the line,
// up square to both. A vertical line takes +-y as its side.
void boltBasis(const glm::vec3& dir, glm::vec3& side, glm::vec3& up) {
    side = glm::vec3(dir.y, 0.0f, 0.0f);
    if (std::abs(side.x) >= kBasisEpsilon) {
        side.y = -dir.x;
        const float inv = 1.0f / std::sqrt(side.x * side.x + side.y * side.y);
        side.x *= inv;
        side.y *= inv;
    } else {
        side.y = dir.x <= 0.0f ? 1.0f : -1.0f;
    }
    const float len = glm::length(dir);
    up = glm::cross(side, dir) * (1.0f / (len + 1e-8f));
}

// The wave and arc both rise and fall with this over the bolt: 0 at its
// ends, 1 in the middle (1 - 4 (t - 0.5)^2).
float midBulge(float t) {
    const float c = t - 0.5f;
    return 1.0f - c * c * 4.0f;
}
}  // namespace

uint32_t decodeChainComboId(uint32_t p) {
    const uint32_t u = p & 0xFEFEFEFEu;
    const uint32_t hi =
        ((((((u >> 7) & 0x200000u) | (p & 0x20000000u)) >> 3 | (p & 0x2000000u)) >> 4 |
          (p & 0x40000000u)) >> 3 | (p & 0x4000000u)) >> 0xB;
    return hi | ((((u >> 4) & 0xFF800000u) | u) & 0xFFFFFFu);
}

std::vector<uint32_t> decodeChainCombo(const std::string& bytes) {
    std::vector<uint32_t> ids;
    // Wow.exe 0x7FC5F0 reads at most 0x30 bytes, four at a time, and stops
    // at the first zero id.
    for (size_t k = 0; k + 4 <= bytes.size() && k < 0x30; k += 4) {
        uint32_t packed = 0;
        for (int i = 3; i >= 0; --i)
            packed = (packed << 8) | static_cast<uint8_t>(bytes[k + static_cast<size_t>(i)]);
        const uint32_t id = decodeChainComboId(packed);
        if (id == 0) break;
        ids.push_back(id);
    }
    return ids;
}

LightningBolt::LightningBolt(const ChainEffectRow* row, uint32_t seed) : row_(row), rng_(seed) {
    reroll();  // 0x9A9900 sets the row and rolls the bolt's look
}

float LightningBolt::rand01() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_);
}

float LightningBolt::randSigned() {
    return std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng_);
}

void LightningBolt::reroll() {
    const ChainEffectRow& r = *row_;
    state_ &= ~(kJointsReady | kHidden);
    waveAngle_ = randRange(r.minWaveAngle, r.maxWaveAngle);
    waveSpin_ = randRange(r.minWaveSpin, r.maxWaveSpin);
    wavePhase_ = (r.flags & chainflags::kFixedWavePhase) ? r.wavePhase : rand01() * kTwoPi;
    arcAngle_ = randRange(r.minArcAngle, r.maxArcAngle);
    arcSpin_ = randRange(r.minArcSpin, r.maxArcSpin);
    flickerTimer_ = randRange(r.minFlickerOn, r.maxFlickerOn);
    pulsePos_ = 0.0f;
    if (r.pulseSpeed < 0.0f) state_ |= kPulseReverse;
    texOffset_ = rand01();
}

void LightningBolt::rollJoint(Joint& j, float radius) {
    const ChainEffectRow& r = *row_;
    const float a = randSigned(), b = randSigned();
    j.offset = glm::vec3(b * radius, 0.0f, a * radius);
    const float c = randSigned(), d = randSigned();
    j.velocity = glm::vec3(d * r.jointMoveSpeed, 0.0f, c * r.jointMoveSpeed);
    // The first roll spreads the jumps between the two durations; a joint
    // rolled again waits up to the longer one (0x9A90C0 reads the bolt's
    // joints-ready bit, which is set only after the first roll).
    if (state_ & kJointsReady)
        j.timer = rand01() * r.maxDurationBetweenJointJumps;
    else
        j.timer = randRange(r.minDurationBetweenJointJumps, r.maxDurationBetweenJointJumps);
}

void LightningBolt::ensureJoints(float length, float radius) {
    const ChainEffectRow& r = *row_;
    int n = (r.flags & chainflags::kFixedJointCount)
        ? r.jointCount
        : static_cast<int>(std::lround(length / std::max(r.avgSegLen, 1e-3f))) + 2;
    n = std::clamp(n + 1, kMinJoints, kMaxJoints);
    const size_t from = (state_ & kJointsReady) ? std::min(joints_.size(), static_cast<size_t>(n)) : 0;
    joints_.resize(static_cast<size_t>(n));
    points_.resize(static_cast<size_t>(n));
    for (size_t i = from; i < joints_.size(); ++i) {
        rollJoint(joints_[i], radius);
        joints_[i].smoothedValid = false;
    }
    state_ |= kJointsReady;
}

void LightningBolt::update(float dt, const glm::vec3& start, const glm::vec3& end) {
    const ChainEffectRow& r = *row_;
    start_ = start;
    end_ = end;
    updated_ = true;
    flickerTimer_ -= dt;
    if ((state_ & kHidden) == 0) {
        if ((r.flags & chainflags::kFlicker) && flickerTimer_ <= 0.0f) {
            flickerTimer_ = randRange(r.minFlickerOff, r.maxFlickerOff);
            state_ |= kHidden;
        }
    } else {
        if (flickerTimer_ > 0.0f) return;
        reroll();
    }
    texScroll_ = r.texCoordScale != 0.0f ? std::fmod(texScroll_ + r.texCoordScale * dt, 1.0f) : 0.0f;
    const float length = std::min(glm::length(end - start), kMaxBoltLength);
    const float radius = r.noiseScale * length + r.jointOffsetRadius;
    ensureJoints(length, radius);
    for (size_t i = 1; i < joints_.size(); ++i) {
        Joint& j = joints_[i];
        j.timer -= dt;
        if ((r.flags & chainflags::kJointsJump) && j.timer < 0.0f)
            rollJoint(j, radius);
        else
            j.offset += j.velocity * dt;
    }
    waveAngle_ += waveSpin_ * dt;
    wavePhase_ -= r.waveSpeed * dt;
    arcAngle_ += arcSpin_ * dt;
    if (r.flags & chainflags::kPulse) {
        pulsePos_ += dt * r.pulseSpeed;
        if ((state_ & kPulseReverse) || (r.flags & chainflags::kPulseLoops)) {
            const float span = r.pulseFadeLength * 2.0f + r.pulseOnLength;
            if (length <= pulsePos_ - span) {
                reroll();
                state_ &= ~kPulseReverse;
            } else if (pulsePos_ < 0.0f) {
                reroll();
                state_ &= ~kPulseReverse;
                pulsePos_ = span + length;
            }
        }
    }
}

// A point along base -> base + dir at t, put off the line by its joint in
// the side/up frame (0x9A9350). The joint is eased toward where it rolled by
// JointSmoothness each time it is laid.
glm::vec3 LightningBolt::jointPoint(const glm::vec3& base, const glm::vec3& dir, float t, Joint& j,
                                    const glm::vec3& side, const glm::vec3& up, float scale) {
    glm::vec3 sm = j.offset;
    if (j.smoothedValid) {
        const float s = row_->jointSmoothness;
        sm = j.offset * (1.0f - s) + j.smoothed * s;
    }
    j.smoothed = sm;
    j.smoothedValid = true;
    return base + dir * t + (side * sm.x + up * sm.z) * scale;
}

void LightningBolt::layPoints() {
    const ChainEffectRow& r = *row_;
    const int n = static_cast<int>(points_.size());
    if (n < 2) return;
    const int last = n - 1;
    points_[0] = start_;
    points_[static_cast<size_t>(last)] = end_;
    glm::vec3 dir = end_ - start_;
    glm::vec3 side, up;
    boltBasis(dir, side, up);
    if ((r.flags & chainflags::kMajorMinorJoints) == 0) {
        // 0x9A9CA0: every joint off the straight line.
        for (int i = 1; i < last; ++i)
            points_[static_cast<size_t>(i)] =
                jointPoint(start_, dir, static_cast<float>(i) / static_cast<float>(last),
                           joints_[static_cast<size_t>(i)], side, up, 1.0f);
        return;
    }
    // 0x9A9DC0: major joints off the straight line, minor joints off the
    // line between two majors, the rest off the line between two minors.
    const int minorStep = std::max(r.jointsPerMinorJoint, 1);
    const int majorStep = std::max(r.minorJointsPerMajorJoint * minorStep, 1);
    for (int i = majorStep; i < last; i += majorStep)
        points_[static_cast<size_t>(i)] =
            jointPoint(start_, dir, static_cast<float>(i) / static_cast<float>(last),
                       joints_[static_cast<size_t>(i)], side, up, r.majorJointScale);

    auto tier = [&](int step, int segStep, float scale) {
        int next = std::min(segStep, last);
        int prev = 0;
        glm::vec3 base = points_[0];
        glm::vec3 segDir = points_[static_cast<size_t>(next)] - base;
        boltBasis(segDir, side, up);
        float t = static_cast<float>(step) / static_cast<float>(next);
        float dt = t;
        for (int i = step; i < last; i += step) {
            if (i == next) {
                prev = i;
                next = std::min(i + segStep, last);
                base = points_[static_cast<size_t>(prev)];
                segDir = points_[static_cast<size_t>(next)] - base;
                boltBasis(segDir, side, up);
                t = dt = static_cast<float>(step) / static_cast<float>(next - prev);
            } else {
                points_[static_cast<size_t>(i)] =
                    jointPoint(base, segDir, t, joints_[static_cast<size_t>(i)], side, up, scale);
                t += dt;
            }
        }
    };
    tier(minorStep, majorStep, r.minorJointScale);
    tier(1, minorStep, 1.0f);
}

void LightningBolt::build(const glm::vec3& cameraPos, std::vector<ChainStripVertex>& out) {
    const ChainEffectRow& r = *row_;
    if (!updated_ || (state_ & kHidden) || points_.size() < 2) return;
    layPoints();
    const int n = static_cast<int>(points_.size());
    const int last = n - 1;
    const glm::vec3 dir = end_ - start_;
    const float length = glm::length(dir);
    glm::vec3 side, up;
    boltBasis(dir, side, up);
    const bool wave = std::abs(r.waveHeight) >= kBasisEpsilon;
    const bool arc = std::abs(r.arcHeight) >= kBasisEpsilon;
    const glm::vec3 waveVec = side * (std::cos(waveAngle_) * r.waveHeight) +
                              up * (std::sin(waveAngle_) * r.waveHeight);
    const glm::vec3 arcVec = side * (std::cos(arcAngle_) * r.arcHeight) +
                             up * (std::sin(arcAngle_) * r.arcHeight);
    // The wave along the bolt (0x9A9B30): a sine travelling down it, turned
    // round the line with the spin flag, and held to zero at both ends.
    auto displaced = [&](glm::vec3 p, float t) {
        const float env = midBulge(t);
        if (wave) {
            const float phase = r.waveFreq * t * length + wavePhase_;
            if (r.flags & chainflags::kWaveSpin) {
                const float amp = r.waveHeight * env;
                p += side * (std::sin(phase) * amp) + up * (std::cos(phase) * amp);
            } else {
                p += waveVec * (std::sin(phase) * env);
            }
        }
        if (arc) p += arcVec * env;
        return p;
    };

    std::vector<glm::vec3> verts(static_cast<size_t>(n) * 2, glm::vec3(0.0f));
    if ((r.flags & chainflags::kFlat) == 0) {
        // Each segment's width faces the camera; a joint takes the mean of
        // its two segments' (0x9AA210).
        glm::vec3 prev = points_[0];
        for (int k = 1; k <= last; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(last);
            const glm::vec3 p = displaced(points_[static_cast<size_t>(k)], t);
            const glm::vec3 seg = p - prev;
            const glm::vec3 toMid = (p + prev) * 0.5f - cameraPos;
            glm::vec3 w = glm::cross(seg, toMid);
            const float wl = glm::length(w);
            if (wl > 0.001f) w /= wl;
            w *= r.width;
            const size_t a = static_cast<size_t>(k - 1) * 2, b = static_cast<size_t>(k) * 2;
            verts[a] += (prev + w) * 0.5f;
            verts[a + 1] += (prev - w) * 0.5f;
            verts[b] = (p + w) * 0.5f;
            verts[b + 1] = (p - w) * 0.5f;
            prev = p;
        }
    } else {
        glm::vec3 w = glm::cross(dir, (start_ + end_) * 0.5f - cameraPos);
        const float wl = glm::length(w);
        if (wl > 0.001f) w /= wl;
        w *= r.width;
        for (int k = 0; k <= last; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(last);
            const glm::vec3 p = displaced(points_[static_cast<size_t>(k)], t);
            verts[static_cast<size_t>(k) * 2] = p + w;
            verts[static_cast<size_t>(k) * 2 + 1] = p - w;
        }
    }
    // Both ends come to a point.
    verts[0] = verts[1] = points_[0];
    verts[static_cast<size_t>(last) * 2] = verts[static_cast<size_t>(last) * 2 + 1] =
        points_[static_cast<size_t>(last)];

    // u along the bolt through the texture matrix (0x9AA210's local_130):
    // tiled every TextureLength yards with flag 0x100, scrolled by the bolt's
    // offset and TexCoordScale.
    float uScale = 1.0f;
    float uShift = texOffset_ + texScroll_;
    if (r.flags & chainflags::kTextureLength) {
        const float tl = std::abs(r.textureLength) > 1e-4f ? r.textureLength : 1.0f;
        uScale = length / tl;
        if (r.flags & chainflags::kTextureLengthScroll) uShift += length / tl;
    }
    const glm::vec3 rgb(r.red / 255.0f, r.green / 255.0f, r.blue / 255.0f);
    const float baseAlpha = r.alpha / 255.0f;
    // The pulse (0x9A9490): only a stretch PulseOnLength long, faded in and
    // out over PulseFadeLength, shows - at PulsePos yards down the bolt.
    const bool pulse = (r.flags & chainflags::kPulse) != 0;
    const float pEnd = pulsePos_;
    const float pOnEnd = pEnd - r.pulseFadeLength;
    const float pOnStart = pOnEnd - r.pulseOnLength;
    const float pStart = pOnStart - r.pulseFadeLength;
    const float stepLen = length / static_cast<float>(last);
    out.reserve(out.size() + verts.size());
    for (int k = 0; k <= last; ++k) {
        float a = baseAlpha;
        if (pulse) {
            const float d = static_cast<float>(k) * stepLen;
            if (d < pStart || d >= pEnd) a = 0.0f;
            else if (d < pOnStart) a = baseAlpha * (d - pStart) / std::max(pOnStart - pStart, 1e-4f);
            else if (d >= pOnEnd) a = baseAlpha * (pEnd - d) / std::max(pEnd - pOnEnd, 1e-4f);
        }
        float u = static_cast<float>(k) / static_cast<float>(last);
        u = u * uScale - uShift;
        const bool tip = k == 0 || k == last;
        const glm::vec4 c(rgb, a);
        out.push_back({verts[static_cast<size_t>(k) * 2], c, glm::vec2(u, tip ? 0.5f : 0.0f)});
        out.push_back({verts[static_cast<size_t>(k) * 2 + 1], c, glm::vec2(u, tip ? 0.5f : 1.0f)});
    }
}

}  // namespace rendering
}  // namespace wowee
