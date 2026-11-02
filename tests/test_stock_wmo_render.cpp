// The stock client's per-batch WMO state and the MLIQ liquid type, held to
// Wow.exe 3.3.5a at the addresses the headers name:
//   rendering/stock_wmo_render.hpp, pipeline/stock_wmo_liquid.hpp.
// docs/re/wmo-rendering.md has the walk-through. Header-only.
#include <catch_amalgamated.hpp>

#include "pipeline/stock_wmo_liquid.hpp"
#include "rendering/stock_wmo_render.hpp"

#include <array>
#include <cstdint>

using Catch::Approx;
namespace wmo = wowee::rendering::stock_wmo;
namespace liq = wowee::pipeline::stock_wmo_liquid;

TEST_CASE("EGxBlend factors are the GL device's tables 0xad8d90/0xad8dc0", "[stockwmo]") {
    using F = wmo::BlendFactor;
    // Opaque and AlphaKey draw with blending off.
    CHECK_FALSE(wmo::gxBlendFactors(0).enabled);
    CHECK_FALSE(wmo::gxBlendFactors(1).enabled);
    const auto alpha = wmo::gxBlendFactors(2);
    CHECK((alpha.src == F::SrcAlpha && alpha.dst == F::OneMinusSrcAlpha));
    const auto add = wmo::gxBlendFactors(3);
    CHECK((add.src == F::SrcAlpha && add.dst == F::One));
    const auto mod2x = wmo::gxBlendFactors(5);
    CHECK((mod2x.src == F::DstColor && mod2x.dst == F::SrcColor));
    // The trans-batch pair: SrcAlphaOpaque then InvSrcAlphaAdd.
    const auto p1 = wmo::gxBlendFactors(9);
    const auto p2 = wmo::gxBlendFactors(7);
    CHECK((p1.src == F::SrcAlpha && p1.dst == F::Zero));
    CHECK((p2.src == F::OneMinusSrcAlpha && p2.dst == F::One));
    const auto noAlphaAdd = wmo::gxBlendFactors(10);
    CHECK((noAlphaAdd.src == F::One && noAlphaAdd.dst == F::One));
}

TEST_CASE("the alpha reference goes with the blend (table 0xad8b7c)", "[stockwmo]") {
    // 0, 224, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 - in 1/255.
    constexpr std::array<int, 12> kTable{0, 224, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
    for (uint32_t b = 0; b < kTable.size(); ++b) {
        INFO("blend " << b);
        CHECK(wmo::gxAlphaRef(b) == Approx(kTable[b] / 255.0f));
    }
    // So neither pass of a trans batch tests alpha.
    CHECK(wmo::gxAlphaRef(9) == 0.0f);
    CHECK(wmo::gxAlphaRef(7) == 0.0f);
}

TEST_CASE("culling and wrap come from the MOMT flags", "[stockwmo]") {
    CHECK(wmo::cullsBackFaces(0x0));
    CHECK_FALSE(wmo::cullsBackFaces(0x4));      // F_UNCULLED
    CHECK(wmo::cullsBackFaces(0x1 | 0x10));     // unlit, SIDN: still culled
    CHECK(wmo::textureWrap(0x0).s);
    CHECK(wmo::textureWrap(0x0).t);
    CHECK_FALSE(wmo::textureWrap(0x40).s);      // F_CLAMP_S
    CHECK(wmo::textureWrap(0x40).t);
    CHECK_FALSE(wmo::textureWrap(0x80).t);      // F_CLAMP_T
}

TEST_CASE("the effect tables 0xd1c3f0 / 0xd1c3d4", "[stockwmo]") {
    using E = wmo::Effect;
    CHECK(wmo::effectFor(0, 0, true, false) == E::Diffuse);
    CHECK(wmo::effectFor(0, 0, false, false) == E::Opaque);  // alpha-less texture
    CHECK(wmo::effectFor(0, 1, false, false) == E::Diffuse); // a key keeps Diffuse
    CHECK(wmo::effectFor(3, 0, true, false) == E::Env);
    CHECK(wmo::effectFor(5, 0, true, true) == E::EnvMetal);
    CHECK(wmo::effectFor(6, 0, true, true) == E::Composite);
    CHECK(wmo::effectFor(6, 0, true, false) == E::None);     // classic slot 6 is empty
    CHECK(wmo::alphaFromTexture(E::Diffuse));
    CHECK(wmo::alphaFromTexture(E::Composite));
    CHECK_FALSE(wmo::alphaFromTexture(E::Specular));
    CHECK_FALSE(wmo::alphaFromTexture(E::EnvMetal));
}

TEST_CASE("env coordinates are the view-space reflection, unscaled", "[stockwmo]") {
    // Looking straight at a wall facing the camera: the ray comes straight
    // back, so the coordinate is the centre... of the unscaled map, 0.
    const glm::vec2 c = wmo::envCoord({0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f});
    CHECK(c.x == Approx(0.0f).margin(1e-6));
    CHECK(c.y == Approx(0.0f).margin(1e-6));
    // A floor seen at 45 degrees reflects up: y = sin 45.
    const glm::vec2 f = wmo::envCoord({0.0f, -1.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    CHECK(f.x == Approx(0.0f).margin(1e-6));
    CHECK(f.y == Approx(0.70710678f));
}

TEST_CASE("MapObjEnv, EnvMetal and Composite as the arbfp1 programs", "[stockwmo]") {
    const glm::vec3 lit(0.5f);  // 2 * C
    const glm::vec4 t0(0.4f, 0.2f, 0.8f, 0.5f);
    const glm::vec3 env(1.0f, 0.5f, 0.0f);
    const glm::vec3 e = wmo::envColor(wmo::Effect::Env, lit, t0, env);
    CHECK(e.r == Approx(0.4f * 0.5f + 0.5f * 1.0f));
    CHECK(e.g == Approx(0.2f * 0.5f + 0.5f * 0.5f));
    const glm::vec3 m = wmo::envColor(wmo::Effect::EnvMetal, lit, t0, env);
    CHECK(m.r == Approx(0.4f * 0.5f + 0.4f * 0.5f * 1.0f));
    CHECK(m.b == Approx(0.8f * 0.5f));
    const glm::vec4 c = wmo::compositeTexel(t0, glm::vec4(1.0f), 0.25f);
    CHECK(c.r == Approx(1.0f + (0.4f - 1.0f) * 0.25f));
    CHECK(c.a == Approx(1.0f + (0.5f - 1.0f) * 0.25f));
}

TEST_CASE("MLIQ liquid type as the group resolves it (0x7d82e0, 0x7d7310)", "[stockwmo]") {
    const std::array<uint8_t, 3> none{0x0f, 0x0f, 0x0f};
    // Old style (no MOHD 0x4): groupLiquid 0 is basic water -> WMO Water.
    CHECK(liq::groupLiquidType(0, 0, 0, none) == 13u);
    CHECK(liq::groupLiquidType(1, 0, 0, none) == 14u);   // ocean
    CHECK(liq::groupLiquidType(2, 0, 0, none) == 19u);   // magma
    CHECK(liq::groupLiquidType(3, 0, 0, none) == 20u);   // slime
    CHECK(liq::groupLiquidType(0, 0, 0x80000, none) == 14u);  // "is ocean"
    // 15 is none: then the tiles say.
    const std::array<uint8_t, 3> magmaTiles{0x0f, 0x42, 0x02};
    CHECK(liq::groupLiquidType(15, 0, 0, magmaTiles) == 19u);
    CHECK(liq::groupLiquidType(15, 0, 0, none) == 0u);
    // MOHD 0x4: a LiquidType id; basic ones are still mapped.
    CHECK(liq::groupLiquidType(21, 0x4, 0, none) == 21u);  // Naxxramas slime
    CHECK(liq::groupLiquidType(41, 0x4, 0, none) == 41u);
    CHECK(liq::groupLiquidType(1, 0x4, 0, none) == 13u);
}

TEST_CASE("water in an interior group is drawn as 17 (0x793d20)", "[stockwmo]") {
    CHECK(liq::drawnLiquidType(13, 0x2000, 0) == 17u);   // interior
    CHECK(liq::drawnLiquidType(13, 0x8, 0) == 13u);      // exterior
    CHECK(liq::drawnLiquidType(13, 0x40, 0) == 13u);     // exterior lit
    CHECK(liq::drawnLiquidType(14, 0x2000, 0) == 14u);   // ocean stays
    CHECK(liq::drawnLiquidType(19, 0x2000, 0) == 19u);   // magma stays
    CHECK(liq::drawnLiquidType(13, 0x2000, 0x200) == 13u);
    CHECK(liq::drawnLiquidType(41, 0x2000, 0) == 41u);   // not basic
}
