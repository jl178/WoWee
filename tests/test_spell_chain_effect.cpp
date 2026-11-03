// SpellChainEffects: the stock record layout, the combo id packing and the
// lightning bolt's strip (Wow.exe 0x7FC5F0, 0x9A8CE0, 0x9AA210, 0x9AB3B0).
#include <catch_amalgamated.hpp>

#include "rendering/spell_chain_effect.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace wowee::rendering;

namespace {
void put32(std::vector<uint8_t>& rec, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) rec[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
void putF(std::vector<uint8_t>& rec, size_t off, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(rec, off, v);
}

ChainEffectRow straightRow() {
    ChainEffectRow r;
    r.id = 1;
    r.avgSegLen = 1.0f;
    r.width = 0.5f;
    r.segDuration = 1000;
    return r;
}
}  // namespace

TEST_CASE("combo ids decode as the stock packs them", "[chain]") {
    // Drain Life (719) names six more rows in its combo column (the DBC's bytes).
    const std::string drainLife("Q\x03\x01\x03[\x03\x01\x13]\x03\x01\x03]\x03\x01\x13_\x03\x01\x03_\x03\x01\x13", 24);
    REQUIRE(decodeChainCombo(drainLife) == std::vector<uint32_t>{720, 731, 732, 733, 734, 735});
    // The real bytes of row 750 (Mind Flay): 'k\x03\x01\x13' is 747.
    const std::string mindFlay("k\x03\x01\x13m\x03\x01\x03", 8);
    REQUIRE(decodeChainCombo(mindFlay) == std::vector<uint32_t>{747, 748});
}

TEST_CASE("a 177-byte record reads its byte columns", "[chain]") {
    std::vector<uint8_t> rec(177, 0);
    put32(rec, 0, 743);
    putF(rec, 4, 0.334f);
    putF(rec, 8, 0.375f);
    put32(rec, 20, 1000);
    put32(rec, 28, 5);   // texture string offset
    put32(rec, 32, 70);  // flags
    putF(rec, 144, 150.0f);
    rec[156] = 0x33;  // alpha
    rec[157] = 0x3A;  // red
    rec[158] = 0xF7;  // green
    rec[159] = 0x5B;  // blue
    rec[160] = 3;     // blend
    put32(rec, 161, 0);
    put32(rec, 165, 2);
    putF(rec, 169, 2.0f);
    ChainEffectRow row;
    REQUIRE(parseChainEffectRecord(rec.data(), 177, 48,
                                   [](uint32_t off) { return off == 5 ? std::string("Tex.blp") : std::string(); },
                                   row));
    CHECK(row.id == 743);
    CHECK(row.texture == "Tex.blp");
    CHECK(row.flags == 70);
    CHECK(row.alpha == 0x33);
    CHECK(row.red == 0x3A);
    CHECK(row.green == 0xF7);
    CHECK(row.blue == 0x5B);
    CHECK(row.blendMode == 3);
    CHECK(row.renderLayer == 2);
    CHECK(row.textureLength == Catch::Approx(2.0f));
    CHECK(row.pulseSpeed == Catch::Approx(150.0f));
}

TEST_CASE("a bolt's strip runs from start to end, pointed at both", "[chain]") {
    const ChainEffectRow row = straightRow();
    LightningBolt bolt(&row, 1);
    std::vector<ChainStripVertex> verts;
    bolt.build(glm::vec3(0, -10, 0), verts);
    REQUIRE(verts.empty());  // nothing before its first update
    const glm::vec3 a(0, 0, 0), b(10, 0, 0);
    bolt.update(0.016f, a, b);
    bolt.build(glm::vec3(5, -10, 0), verts);
    // length / AvgSegLen + 2, plus one: 13 points, two vertices each.
    REQUIRE(bolt.pointCount() == 13);
    REQUIRE(verts.size() == 26);
    CHECK(verts[0].pos == a);
    CHECK(verts[1].pos == a);
    CHECK(verts[24].pos == b);
    CHECK(verts[25].pos == b);
    // No joint radius: the middle stays on the line, the width across it
    // facing the camera (up here, the camera being off to the side).
    const auto& m0 = verts[12].pos;
    const auto& m1 = verts[13].pos;
    CHECK((m0.y) == Catch::Approx(0.0f).margin(1e-4));
    CHECK(std::abs(m0.z - m1.z) == Catch::Approx(1.0f).margin(1e-3));
}

TEST_CASE("the pulse shows only its stretch of the bolt", "[chain]") {
    ChainEffectRow row = straightRow();
    row.flags = chainflags::kPulse;
    row.pulseSpeed = 10.0f;
    row.pulseOnLength = 2.0f;
    row.pulseFadeLength = 0.0f;
    LightningBolt bolt(&row, 2);
    bolt.update(0.5f, glm::vec3(0), glm::vec3(10, 0, 0));  // pulse at 5 yd
    std::vector<ChainStripVertex> verts;
    bolt.build(glm::vec3(5, -10, 0), verts);
    REQUIRE(verts.size() == 26);
    // Points every 10/12 yd: 3.33 and 4.17 are inside [3, 5), 0.83 and 6.67 not.
    CHECK(verts[2 * 4].color.a == Catch::Approx(1.0f));
    CHECK(verts[2 * 5].color.a == Catch::Approx(1.0f));
    CHECK(verts[2 * 1].color.a == Catch::Approx(0.0f));
    CHECK(verts[2 * 8].color.a == Catch::Approx(0.0f));
}

TEST_CASE("a flickering bolt goes dark and comes back", "[chain]") {
    ChainEffectRow row = straightRow();
    row.flags = chainflags::kFlicker;
    row.minFlickerOn = row.maxFlickerOn = 0.1f;
    row.minFlickerOff = row.maxFlickerOff = 0.1f;
    LightningBolt bolt(&row, 3);
    bolt.update(0.05f, glm::vec3(0), glm::vec3(5, 0, 0));
    CHECK_FALSE(bolt.flickeredOff());
    bolt.update(0.06f, glm::vec3(0), glm::vec3(5, 0, 0));
    CHECK(bolt.flickeredOff());
    std::vector<ChainStripVertex> verts;
    bolt.build(glm::vec3(0, -10, 0), verts);
    CHECK(verts.empty());
    bolt.update(0.11f, glm::vec3(0), glm::vec3(5, 0, 0));
    CHECK_FALSE(bolt.flickeredOff());
}
