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
