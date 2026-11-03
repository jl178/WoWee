#pragma once

// SpellChainEffects: the beams and bolts a spell draws between units - Chain
// Lightning's arcs, Drain Life's and Mind Flay's beams, Chain Heal, Death
// Grip. The stock client plays them from a SpellVisualKit CharProc of type 0
// or 12 (Wow.exe 0x7265C0 -> 0x7FC5F0) and draws each hop as a CLightning
// (Common\Lightning.cpp, 0x9A8C00..0x9AB3B0). This is that object: its
// joints, wave, arc, flicker and pulse, and the camera-facing strip it draws.
// See docs/re/spell-cast-pipeline.md, "Chain effects".

#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

/// One SpellChainEffects.dbc row (3.3.5a: 48 columns in a 177-byte record -
/// the colour and blend are bytes).
struct ChainEffectRow {
    uint32_t id = 0;
    float avgSegLen = 1.0f;
    float width = 0.5f;
    float noiseScale = 0.0f;
    float texCoordScale = 0.0f;   // texture scroll, turns a second
    int32_t segDuration = 1000;   // ms a hop lasts
    int32_t segDelay = 0;         // ms before the first hop
    std::string texture;
    uint32_t flags = 0;
    int32_t jointCount = 0;
    float jointOffsetRadius = 0.0f;
    int32_t jointsPerMinorJoint = 0;
    int32_t minorJointsPerMajorJoint = 0;
    float minorJointScale = 1.0f;
    float majorJointScale = 1.0f;
    float jointMoveSpeed = 0.0f;
    float jointSmoothness = 0.0f;
    float minDurationBetweenJointJumps = 0.0f;
    float maxDurationBetweenJointJumps = 0.0f;
    float waveHeight = 0.0f;
    float waveFreq = 0.0f;
    float waveSpeed = 0.0f;
    float minWaveAngle = 0.0f, maxWaveAngle = 0.0f;
    float minWaveSpin = 0.0f, maxWaveSpin = 0.0f;
    float arcHeight = 0.0f;
    float minArcAngle = 0.0f, maxArcAngle = 0.0f;
    float minArcSpin = 0.0f, maxArcSpin = 0.0f;
    float delayBetweenEffects = 0.0f;  // ms between one hop and the next
    float minFlickerOn = 0.0f, maxFlickerOn = 0.0f;
    float minFlickerOff = 0.0f, maxFlickerOff = 0.0f;
    float pulseSpeed = 0.0f;
    float pulseOnLength = 0.0f;
    float pulseFadeLength = 0.0f;
    uint8_t alpha = 255, red = 255, green = 255, blue = 255;
    uint8_t blendMode = 3;        // EGxBlend: 2 alpha, 3 add, 4 mod...
    std::vector<uint32_t> combo;  // more rows drawn along the same hops
    int32_t renderLayer = 0;
    float textureLength = 1.0f;
    float wavePhase = 0.0f;
};

/// Row flags the stock lightning reads.
namespace chainflags {
constexpr uint32_t kFixedJointCount = 0x1;   // JointCount, not length / AvgSegLen
constexpr uint32_t kMajorMinorJoints = 0x2;  // joints in three tiers
constexpr uint32_t kJointsJump = 0x4;        // joints re-roll on a timer, else drift
constexpr uint32_t kWaveSpin = 0x8;          // the wave turns round the line
constexpr uint32_t kFlicker = 0x10;
constexpr uint32_t kPulse = 0x20;            // only a travelling stretch shows
constexpr uint32_t kPulseLoops = 0x40;
constexpr uint32_t kFlat = 0x80;             // one width direction for the strip
constexpr uint32_t kTextureLength = 0x100;   // u tiles every TextureLength yards
constexpr uint32_t kTextureLengthScroll = 0x200;
constexpr uint32_t kFixedWavePhase = 0x400;
}  // namespace chainflags

/// The combo column: up to twelve four-byte groups, each a row id written
/// seven bits a byte so the string has no zero byte (Wow.exe 0x9A8CE0).
uint32_t decodeChainComboId(uint32_t packed);
std::vector<uint32_t> decodeChainCombo(const std::string& bytes);

/// Read a record of SpellChainEffects.dbc. A binary 3.3.5a record is 177
/// bytes (39 four-byte columns, five single bytes, then four more columns);
/// a record of 48 four-byte columns (a converted table) is read column by
/// column. strings resolves a string-block offset.
template <typename StringAt>
bool parseChainEffectRecord(const uint8_t* rec, uint32_t recordSize, uint32_t fieldCount,
                            StringAt&& strings, ChainEffectRow& out);

/// A vertex of the strip a bolt draws: a triangle strip, pairs of vertices
/// across the bolt from its start to its end.
struct ChainStripVertex {
    glm::vec3 pos;
    glm::vec4 color;  // rgb 0..1, a 0..1
    glm::vec2 uv;
};

/// One bolt between two points (CLightning).
class LightningBolt {
public:
    explicit LightningBolt(const ChainEffectRow* row, uint32_t seed);

    const ChainEffectRow* row() const { return row_; }

    /// Advance the joints, the wave, arc and pulse, and the flicker (0x9AB3B0).
    /// start and end are where the bolt runs now.
    void update(float dt, const glm::vec3& start, const glm::vec3& end);

    /// The strip for this frame (0x9AA210), seen from cameraPos. Empty while
    /// it flickers off or before its first update.
    void build(const glm::vec3& cameraPos, std::vector<ChainStripVertex>& out);

    /// Hidden by its flicker.
    bool flickeredOff() const { return (state_ & kHidden) != 0; }
    size_t pointCount() const { return points_.size(); }

private:
    struct Joint {
        glm::vec3 offset{0.0f};    // x across, z up - y unused
        glm::vec3 smoothed{0.0f};
        bool smoothedValid = false;
        glm::vec3 velocity{0.0f};
        float timer = 0.0f;
    };
    static constexpr uint32_t kJointsReady = 0x1, kHidden = 0x4, kPulseReverse = 0x8;

    float rand01();
    float randRange(float lo, float hi) { return lo + rand01() * (hi - lo); }
    float randSigned();  // -1..1
    void reroll();       // 0x9A8EC0
    void rollJoint(Joint& j, float radius);  // 0x9A90C0
    void ensureJoints(float length, float radius);  // 0x9AB2E0
    glm::vec3 jointPoint(const glm::vec3& base, const glm::vec3& dir, float t, Joint& j,
                         const glm::vec3& side, const glm::vec3& up, float scale);
    void layPoints();

    const ChainEffectRow* row_;
    std::mt19937 rng_;
    uint32_t state_ = 0;
    glm::vec3 start_{0.0f}, end_{0.0f};
    std::vector<Joint> joints_;
    std::vector<glm::vec3> points_;
    float waveAngle_ = 0.0f, waveSpin_ = 0.0f, wavePhase_ = 0.0f;
    float arcAngle_ = 0.0f, arcSpin_ = 0.0f;
    float flickerTimer_ = 0.0f;
    float pulsePos_ = 0.0f;
    float texOffset_ = 0.0f, texScroll_ = 0.0f;
    bool updated_ = false;
};

// ---------------------------------------------------------------------------

template <typename StringAt>
bool parseChainEffectRecord(const uint8_t* rec, uint32_t recordSize, uint32_t fieldCount,
                            StringAt&& strings, ChainEffectRow& out) {
    if (!rec) return false;
    auto u32 = [&](uint32_t off) -> uint32_t {
        uint32_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | rec[off + static_cast<uint32_t>(i)];
        return v;
    };
    auto i32 = [&](uint32_t off) { return static_cast<int32_t>(u32(off)); };
    auto f32 = [&](uint32_t off) {
        const uint32_t bits = u32(off);
        float f;
        static_assert(sizeof(f) == sizeof(bits));
        __builtin_memcpy(&f, &bits, sizeof(f));
        return f;
    };
    const bool packedBytes = recordSize == 177;
    const bool wide = !packedBytes && fieldCount >= 48 && recordSize >= 48 * 4;
    if (!packedBytes && !wide) return false;
    out.id = u32(0);
    out.avgSegLen = f32(4);
    out.width = f32(8);
    out.noiseScale = f32(12);
    out.texCoordScale = f32(16);
    out.segDuration = i32(20);
    out.segDelay = i32(24);
    out.texture = strings(u32(28));
    out.flags = u32(32);
    out.jointCount = i32(36);
    out.jointOffsetRadius = f32(40);
    out.jointsPerMinorJoint = i32(44);
    out.minorJointsPerMajorJoint = i32(48);
    out.minorJointScale = f32(52);
    out.majorJointScale = f32(56);
    out.jointMoveSpeed = f32(60);
    out.jointSmoothness = f32(64);
    out.minDurationBetweenJointJumps = f32(68);
    out.maxDurationBetweenJointJumps = f32(72);
    out.waveHeight = f32(76);
    out.waveFreq = f32(80);
    out.waveSpeed = f32(84);
    out.minWaveAngle = f32(88);
    out.maxWaveAngle = f32(92);
    out.minWaveSpin = f32(96);
    out.maxWaveSpin = f32(100);
    out.arcHeight = f32(104);
    out.minArcAngle = f32(108);
    out.maxArcAngle = f32(112);
    out.minArcSpin = f32(116);
    out.maxArcSpin = f32(120);
    out.delayBetweenEffects = f32(124);
    out.minFlickerOn = f32(128);
    out.maxFlickerOn = f32(132);
    out.minFlickerOff = f32(136);
    out.maxFlickerOff = f32(140);
    out.pulseSpeed = f32(144);
    out.pulseOnLength = f32(148);
    out.pulseFadeLength = f32(152);
    if (packedBytes) {
        out.alpha = rec[156];
        out.red = rec[157];
        out.green = rec[158];
        out.blue = rec[159];
        out.blendMode = rec[160];
        out.combo = decodeChainCombo(strings(u32(161)));
        out.renderLayer = i32(165);
        out.textureLength = f32(169);
        out.wavePhase = f32(173);
    } else {
        out.alpha = static_cast<uint8_t>(u32(156));
        out.red = static_cast<uint8_t>(u32(160));
        out.green = static_cast<uint8_t>(u32(164));
        out.blue = static_cast<uint8_t>(u32(168));
        out.blendMode = static_cast<uint8_t>(u32(172));
        out.combo = decodeChainCombo(strings(u32(176)));
        out.renderLayer = i32(180);
        out.textureLength = f32(184);
        out.wavePhase = f32(188);
    }
    return out.id != 0;
}

}  // namespace rendering
}  // namespace wowee
