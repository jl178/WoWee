// The animated tracks a ribbon emitter carries.
//
// A ribbon's alpha and visibility are stored the same "array of arrays" way as
// every other M2 track, but with different key types: alpha is fixed16 and
// visibility is a byte. The loader read both with its own inline copy of the
// walk rather than through parseAnimTrack, which is the shape that lets a fix
// to one reader miss the other.
//
// Neither failure is loud. Read the byte track as float and 0x01 becomes about
// 1.4e-45, which fails a `> 0.5` visibility test, so the ribbon is simply
// never drawn. Read the fixed16 track with the wrong divisor and a sword trail
// is either invisible or a solid block.
//
// This is real-asset coverage: it needs a model with ribbons extracted, and
// skips when there is none.
#include <catch_amalgamated.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "pipeline/m2_loader.hpp"

using namespace wowee::pipeline;

namespace {

std::vector<uint8_t> readFile(const std::string& relative) {
#ifdef WOWEE_SOURCE_DIR
    const std::string path = std::string(WOWEE_SOURCE_DIR) + "/" + relative;
#else
    const std::string path = relative;
#endif
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

/// Every value across every sequence of a track.
std::vector<float> allValues(const M2AnimationTrack& track) {
    std::vector<float> out;
    for (const auto& sequence : track.sequences) {
        out.insert(out.end(), sequence.floatValues.begin(), sequence.floatValues.end());
    }
    return out;
}

}  // namespace

TEST_CASE("a ribbon model's tracks parse into usable ranges", "[m2][ribbon]") {
    const auto data = readFile("Data/spell/lightningshield_impact_base.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }

    const M2Model model = M2Loader::load(data);
    if (model.ribbonEmitters.empty()) {
        SUCCEED("model carries no ribbon emitters; nothing to check here");
        return;
    }

    bool sawAlpha = false;
    bool sawVisibility = false;
    for (const auto& ribbon : model.ribbonEmitters) {
        for (float value : allValues(ribbon.alphaTrack)) {
            sawAlpha = true;
            // fixed16 over 32767. A value outside 0..1 means the divisor or
            // the key width is wrong, and both read as a trail that is either
            // invisible or a solid slab.
            INFO("alpha " << value);
            CHECK(value >= -0.001f);
            CHECK(value <= 1.001f);
        }
        for (float value : allValues(ribbon.visibilityTrack)) {
            sawVisibility = true;
            // Bytes, and only ever 0 or 1. Read as float, a 1 byte becomes a
            // denormal that fails every threshold test downstream.
            INFO("visibility " << value);
            CHECK((value == 0.0f || value == 1.0f));
        }
    }

    // Not a requirement of the format, but if neither track carried anything
    // this test proved nothing and should say so rather than pass quietly.
    if (!sawAlpha && !sawVisibility) {
        WARN("ribbon emitters present but neither track carried keys");
    }
}

TEST_CASE("ribbon track sequences line up with their timestamps",
          "[m2][ribbon]") {
    const auto data = readFile("Data/spell/lightningshield_impact_base.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }

    const M2Model model = M2Loader::load(data);
    if (model.ribbonEmitters.empty()) {
        SUCCEED("model carries no ribbon emitters; nothing to check here");
        return;
    }

    for (const auto& ribbon : model.ribbonEmitters) {
        for (const auto* track : {&ribbon.alphaTrack, &ribbon.visibilityTrack}) {
            for (const auto& sequence : track->sequences) {
                // A sequence with values but no timestamps cannot be sampled,
                // which is what reading the two sub-arrays out of step looks
                // like: the counts come from different headers.
                if (sequence.floatValues.empty()) continue;
                INFO("values " << sequence.floatValues.size()
                     << " timestamps " << sequence.timestamps.size());
                CHECK_FALSE(sequence.timestamps.empty());
            }
        }
    }
}

namespace {

template <typename T>
void put(std::vector<uint8_t>& buffer, size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

}  // namespace

// The emitter array's stride. WotLK ribbon emitters are 0xB0 bytes apiece -
// Wow.exe strides the array by 0xB0 - and the loader read them at 0xAC, so
// every emitter after the first came from four bytes further back than the
// one before: the second's bone was its ribbonId (-1) and it was dropped, the
// rest read garbage. Synthetic, so it always runs.
TEST_CASE("ribbon emitters are read at the 0xB0 stride", "[m2][ribbon]") {
    constexpr size_t kRibbonOffset = 0x200;
    constexpr size_t kStride = 0xB0;
    constexpr uint32_t kCount = 3;
    std::vector<uint8_t> data(kRibbonOffset + kCount * kStride + 0x40, 0);
    std::memcpy(data.data(), "MD20", 4);
    put<uint32_t>(data, 0x04, 264);
    put<uint32_t>(data, 0x120, kCount);
    put<uint32_t>(data, 0x124, static_cast<uint32_t>(kRibbonOffset));

    struct Expected { uint32_t bone; float edgesPerSecond; float lifetime; };
    // The last asks for a life under the stock floor of a quarter second.
    const Expected expected[kCount] = {{3, 30.0f, 0.5f}, {7, 44.0f, 1.65f}, {9, 20.0f, 0.1f}};
    for (uint32_t i = 0; i < kCount; ++i) {
        const size_t base = kRibbonOffset + i * kStride;
        put<int32_t>(data, base + 0x00, -1);
        put<uint32_t>(data, base + 0x04, expected[i].bone);
        put<float>(data, base + 0x74, expected[i].edgesPerSecond);
        put<float>(data, base + 0x78, expected[i].lifetime);
    }

    const M2Model model = M2Loader::load(data);
    REQUIRE(model.ribbonEmitters.size() == kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        INFO("emitter " << i);
        CHECK(model.ribbonEmitters[i].bone == expected[i].bone);
        CHECK(model.ribbonEmitters[i].edgesPerSecond == expected[i].edgesPerSecond);
    }
    CHECK(model.ribbonEmitters[0].edgeLifetime == 0.5f);
    CHECK(model.ribbonEmitters[1].edgeLifetime == 1.65f);
    CHECK(model.ribbonEmitters[2].edgeLifetime == 0.25f);
}

// A ribbon's material index is into the model's material table: the blend it
// draws with is that material's (stock 0x833900), not a skin batch's.
TEST_CASE("a ribbon takes its blend from the material table", "[m2][ribbon]") {
    constexpr size_t kRibbonOffset = 0x200, kMatOffset = 0x400, kIdxOffset = 0x420;
    std::vector<uint8_t> data(0x500, 0);
    std::memcpy(data.data(), "MD20", 4);
    put<uint32_t>(data, 0x04, 264);
    put<uint32_t>(data, 0x70, 2);  // materials
    put<uint32_t>(data, 0x74, static_cast<uint32_t>(kMatOffset));
    put<uint16_t>(data, kMatOffset + 0, 0);
    put<uint16_t>(data, kMatOffset + 2, 2);   // material 0: alpha
    put<uint16_t>(data, kMatOffset + 4, 23);
    put<uint16_t>(data, kMatOffset + 6, 4);   // material 1: add, flags 0x17
    put<uint32_t>(data, 0x120, 1);
    put<uint32_t>(data, 0x124, static_cast<uint32_t>(kRibbonOffset));
    put<uint32_t>(data, kRibbonOffset + 0x04, 7);
    put<uint32_t>(data, kRibbonOffset + 0x1C, 1);
    put<uint32_t>(data, kRibbonOffset + 0x20, static_cast<uint32_t>(kIdxOffset));
    put<uint16_t>(data, kIdxOffset, 1);
    put<float>(data, kRibbonOffset + 0x74, 10.0f);
    put<float>(data, kRibbonOffset + 0x78, 0.6f);

    const M2Model model = M2Loader::load(data);
    REQUIRE(model.ribbonEmitters.size() == 1);
    CHECK(model.ribbonEmitters[0].materialIndex == 1);
    CHECK(model.ribbonEmitters[0].blendMode == 4);
    CHECK(model.ribbonEmitters[0].materialFlags == 23);
}

// The WotLK particle emitter record (0x1DC bytes): every field the stock
// runtime reads, at its offset. Synthetic, so it always runs.
TEST_CASE("particle emitter fields are read at their WotLK offsets", "[m2][particles]") {
    constexpr size_t kEm = 0x200, kData = 0x600;
    std::vector<uint8_t> data(0x800, 0);
    std::memcpy(data.data(), "MD20", 4);
    put<uint32_t>(data, 0x04, 264);
    put<uint32_t>(data, 0x128, 1);
    put<uint32_t>(data, 0x12C, static_cast<uint32_t>(kEm));
    put<uint32_t>(data, kEm + 0x04, 0x60009u);
    put<uint16_t>(data, kEm + 0x14, 2);
    put<uint8_t>(data, kEm + 0x28, 4);
    put<uint8_t>(data, kEm + 0x29, 1);
    put<uint16_t>(data, kEm + 0x30, 2);
    put<uint16_t>(data, kEm + 0x32, 4);
    put<float>(data, kEm + 0xAC, 0.25f);   // lifespan vary
    put<float>(data, kEm + 0xC4, 3.0f);    // rate vary
    // Scale FBlock: three keys at 0, 11468, 32767 of (x, y).
    size_t o = kData;
    put<uint32_t>(data, kEm + 0x124, 3);
    put<uint32_t>(data, kEm + 0x128, static_cast<uint32_t>(o));
    put<int16_t>(data, o, 0); put<int16_t>(data, o + 2, 11468); put<int16_t>(data, o + 4, 32767);
    o += 8;
    put<uint32_t>(data, kEm + 0x12C, 3);
    put<uint32_t>(data, kEm + 0x130, static_cast<uint32_t>(o));
    const float sx[3] = {3.77f, 2.0f, 1.0f}, sy[3] = {1.94f, 1.0f, 0.5f};
    for (int k = 0; k < 3; ++k) {
        put<float>(data, o + k * 8, sx[k]);
        put<float>(data, o + k * 8 + 4, sy[k]);
    }
    o += 24;
    // Alpha FBlock, two keys, the last short of 32767.
    put<uint32_t>(data, kEm + 0x114, 2);
    put<uint32_t>(data, kEm + 0x118, static_cast<uint32_t>(o));
    put<int16_t>(data, o, 0); put<int16_t>(data, o + 2, 16384);
    o += 4;
    put<uint32_t>(data, kEm + 0x11C, 2);
    put<uint32_t>(data, kEm + 0x120, static_cast<uint32_t>(o));
    put<int16_t>(data, o, 32767); put<int16_t>(data, o + 2, 0);
    o += 4;
    // Head cell FBlock: cells 0 then 3.
    put<uint32_t>(data, kEm + 0x13C, 2);
    put<uint32_t>(data, kEm + 0x140, static_cast<uint32_t>(o));
    put<int16_t>(data, o, 0); put<int16_t>(data, o + 2, 32767);
    o += 4;
    put<uint32_t>(data, kEm + 0x144, 2);
    put<uint32_t>(data, kEm + 0x148, static_cast<uint32_t>(o));
    put<uint16_t>(data, o, 0); put<uint16_t>(data, o + 2, 3);
    put<float>(data, kEm + 0x134, 0.1f);   // scale vary x
    put<float>(data, kEm + 0x15C, 1.2f);   // tail length
    put<float>(data, kEm + 0x160, 10.0f);  // twinkle speed
    put<float>(data, kEm + 0x164, 0.5f);   // twinkle percent
    put<float>(data, kEm + 0x168, 0.7f);
    put<float>(data, kEm + 0x16C, 1.0f);
    put<float>(data, kEm + 0x170, 2.0f);   // burst
    put<float>(data, kEm + 0x174, 0.5f);   // drag
    put<float>(data, kEm + 0x17C, 3.14f);  // base spin vary
    put<float>(data, kEm + 0x184, 0.35f);  // spin vary
    put<float>(data, kEm + 0x1A0, 1.0f);   // wind x
    put<float>(data, kEm + 0x1AC, 0.75f);  // wind time
    put<float>(data, kEm + 0x1B0, 2.5f);   // follow speed 1

    const M2Model model = M2Loader::load(data);
    REQUIRE(model.particleEmitters.size() == 1);
    const auto& em = model.particleEmitters[0];
    CHECK(em.flags == 0x60009u);
    CHECK(em.blendingType == 4);
    CHECK(em.textureRows == 2);
    CHECK(em.textureCols == 4);
    CHECK(em.lifespanVary == 0.25f);
    CHECK(em.emissionRateVary == 3.0f);
    REQUIRE(em.particleScale.vec2Values.size() == 3);
    CHECK(em.particleScale.vec2Values[0] == glm::vec2(3.77f, 1.94f));
    CHECK(em.particleScale.floatValues[0] == 3.77f);
    // Times are fixed16 over 32767, not over the last key.
    CHECK(em.particleScale.timestamps[1] == Catch::Approx(11468.0f / 32767.0f));
    CHECK(em.particleAlpha.timestamps[1] == Catch::Approx(16384.0f / 32767.0f));
    CHECK(em.particleAlpha.floatValues[0] == Catch::Approx(1.0f));
    REQUIRE(em.headCell.floatValues.size() == 2);
    CHECK(em.headCell.floatValues[1] == 3.0f);
    CHECK(em.scaleVary.x == 0.1f);
    CHECK(em.tailLength == 1.2f);
    CHECK(em.twinkleSpeed == 10.0f);
    CHECK(em.twinklePercent == 0.5f);
    CHECK(em.twinkleScaleMin == 0.7f);
    CHECK(em.twinkleScaleMax == 1.0f);
    CHECK(em.burstMultiplier == 2.0f);
    CHECK(em.drag == 0.5f);
    CHECK(em.baseSpinVary == 3.14f);
    CHECK(em.spinVary == 0.35f);
    CHECK(em.windVector.x == 1.0f);
    CHECK(em.windTime == 0.75f);
    CHECK(em.followSpeed1 == 2.5f);
}
