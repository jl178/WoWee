// M2 batch shader selection as the stock 3.3.5a client does it
// (docs/re/m2-runtime.md §3, "Shader selection"):
//
// - the runtime shader id from the blend mode and texture coordinate combos
//   (0x836980), with and without the model's texture combiner combos;
// - the folding of an env layer and an alpha layer into a two-texture base
//   batch (0x837680), and the copy for batches sharing a material;
// - the vertex and pixel shader for a runtime id (0x836600, 0x836C90), with
//   the 0x11 fallback.
#include <catch_amalgamated.hpp>

#include "rendering/m2_effect_select.hpp"

using namespace wowee::rendering::m2fx;
using wowee::pipeline::M2Batch;
using wowee::pipeline::M2Material;
using wowee::pipeline::M2Model;

namespace {

M2Batch batch(uint16_t material, uint16_t layer, uint16_t texCount, uint16_t texCombo,
              uint16_t coordCombo, uint16_t shader = 0, uint16_t weight = 0,
              uint16_t transform = 0) {
    M2Batch b{};
    b.shader = shader;
    b.materialIndex = material;
    b.materialLayer = layer;
    b.textureCount = texCount;
    b.textureIndex = texCombo;
    b.textureUnit = coordCombo;
    b.transparencyIndex = weight;
    b.textureAnimIndex = transform;
    return b;
}

M2Model model() {
    M2Model m{};
    m.globalFlags = 0;
    // Coordinate combos: 0 = UV0, 1 = UV1, 2 = env (-1).
    m.textureCoordCombos = {0, 1, -1, 0, -1};
    m.textureLookup = {0, 1, 2, 3, 4};
    m.textureWeightLookup = {0, 1};
    m.textureTransformLookup = {0xFFFF, 0, 1};
    return m;
}

}  // namespace

TEST_CASE("without combiner combos the op is opaque for blend 0, modulate otherwise",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.materials = {M2Material{0, 0}, M2Material{0, 2}, M2Material{0, 4}};
    m.batches = {batch(0, 0, 1, 0, 0), batch(1, 0, 1, 1, 0), batch(2, 0, 1, 2, 2),
                 batch(1, 0, 1, 3, 1)};
    const auto fx = resolveBatchEffects(m);
    REQUIRE(fx.size() == 4);

    CHECK(fx[0].runtimeShader == 0x00);
    CHECK(fx[0].combiner == Combiner::Opaque);
    CHECK(fx[0].uv[0] == UvSource::Uv0);
    CHECK(fx[0].stages == 1);

    CHECK(fx[1].runtimeShader == 0x10);
    CHECK(fx[1].combiner == Combiner::Mod);

    // Env coordinate combo: bit 7, Diffuse_Env, and no matrix.
    CHECK(fx[2].runtimeShader == 0x90);
    CHECK(fx[2].combiner == Combiner::Mod);
    CHECK(fx[2].uv[0] == UvSource::Env);
    CHECK(fx[2].transform[0] == -1);

    // UV set 1: 0x4000 and Diffuse_T2.
    CHECK(fx[3].runtimeShader == 0x4010);
    CHECK(fx[3].uv[0] == UvSource::Uv1);
    CHECK(fx[3].texture[0] == 3);
}

TEST_CASE("combiner combos give each stage its op, the base of an opaque material opaque",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.globalFlags = 0x08;
    // Combos indexed by the file's shader id: [0,1] = Mod, Mod2x; [2,3] = Mod, Add.
    m.textureCombinerCombos = {1, 4, 1, 3};
    m.materials = {M2Material{0, 0}, M2Material{0, 2}};
    m.batches = {
        // Opaque material, T1 + env on stage 2 at Mod2x: Opaque_Mod2x, T1_Env.
        batch(0, 0, 2, 0, 3, /*shader*/ 0, 0, 1),
        // Alpha material, T1 + env at Add: Mod_Add.
        batch(1, 0, 2, 2, 3, /*shader*/ 2),
    };
    const auto fx = resolveBatchEffects(m);
    REQUIRE(fx.size() == 2);
    CHECK(fx[0].runtimeShader == 0x0C);
    CHECK(fx[0].combiner == Combiner::Opaque_Mod2x);
    CHECK(fx[0].stages == 2);
    CHECK(fx[0].uv[0] == UvSource::Uv0);
    CHECK(fx[0].uv[1] == UvSource::Env);
    CHECK(fx[0].texture[0] == 0);
    CHECK(fx[0].texture[1] == 1);
    CHECK(fx[0].transform[0] == 0);  // lookup[1] -> transform 0
    CHECK(fx[0].transform[1] == -1); // env: no matrix

    CHECK(fx[1].runtimeShader == 0x1B);
    CHECK(fx[1].combiner == Combiner::Mod_Add);
}

TEST_CASE("a pair with no stock pixel shader falls back to 0x11", "[m2][effect][stock]") {
    M2Model m = model();
    m.globalFlags = 0x08;
    m.textureCombinerCombos = {3, 3};  // Add, Add: no Combiners_Add_Add in the table
    m.materials = {M2Material{0, 2}};
    m.batches = {batch(0, 0, 2, 0, 0)};
    const auto fx = resolveBatchEffects(m);
    REQUIRE(fx.size() == 1);
    CHECK(fx[0].fallback);
    CHECK(fx[0].combiner == Combiner::Mod_Mod);
    CHECK(fx[0].uv[0] == UvSource::Uv0);
    CHECK(fx[0].uv[1] == UvSource::Uv1);

    // Mod2x over Mod reuses Combiners_Mod_Mod2x; Add over Mod is Add_Mod.
    Combiner c{};
    REQUIRE(combinerFor(2, 0x41, c));
    CHECK(c == Combiner::Mod_Mod2x);
    REQUIRE(combinerFor(2, 0x31, c));
    CHECK(c == Combiner::Add_Mod);
    CHECK_FALSE(combinerFor(2, 0x33, c));
    CHECK_FALSE(combinerFor(2, 0x20, c));
}

TEST_CASE("an additive env layer over an opaque base becomes its second texture",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.materials = {M2Material{0, 0}, M2Material{0, 4}, M2Material{0, 2}, M2Material{0, 6}};
    SECTION("blend 4: Opaque_AddAlpha, then Opaque_AddAlpha_Alpha with an alpha layer") {
        m.batches = {batch(0, 0, 1, 0, 0, 0, 0, 1),  // base, T1, transform 0
                     batch(1, 1, 1, 4, 2, 0, 0, 2),  // add layer, env, texture 4
                     batch(2, 2, 1, 0, 0)};          // alpha layer, base texture
        const auto fx = resolveBatchEffects(m);
        REQUIRE(fx.size() == 3);
        CHECK(fx[0].runtimeShader == 0x8003);
        CHECK(fx[0].combiner == Combiner::Opaque_AddAlpha_Alpha);
        CHECK(fx[0].stages == 2);
        CHECK(fx[0].uv[0] == UvSource::Uv0);
        CHECK(fx[0].uv[1] == UvSource::Env);
        CHECK(fx[0].texture[0] == 0);
        CHECK(fx[0].texture[1] == 4);
        CHECK(fx[0].transform[0] == 0);
        CHECK(fx[1].hidden);
        CHECK(fx[2].hidden);
    }
    SECTION("blend 6: Opaque_Mod2xNA on T1_Env, Mod2xNA_Alpha with an alpha layer") {
        m.batches = {batch(0, 0, 1, 0, 0), batch(3, 1, 1, 4, 2)};
        auto fx = resolveBatchEffects(m);
        REQUIRE(fx.size() == 2);
        CHECK(fx[0].runtimeShader == 0x000E);
        CHECK(fx[0].combiner == Combiner::Opaque_Mod2xNA);
        CHECK(fx[0].uv[1] == UvSource::Env);
        CHECK(fx[1].hidden);

        m.batches.push_back(batch(2, 2, 1, 0, 0));
        fx = resolveBatchEffects(m);
        CHECK(fx[0].runtimeShader == 0x8001);
        CHECK(fx[0].combiner == Combiner::Opaque_Mod2xNA_Alpha);
        CHECK(fx[2].hidden);
    }
    SECTION("a different texture weight keeps the layers apart") {
        m.batches = {batch(0, 0, 1, 0, 0, 0, 0), batch(1, 1, 1, 4, 2, 0, 1)};
        const auto fx = resolveBatchEffects(m);
        CHECK(fx[0].runtimeShader == 0x00);
        CHECK(fx[0].stages == 1);
        CHECK_FALSE(fx[1].hidden);
        CHECK(fx[1].combiner == Combiner::Mod);
        CHECK(fx[1].uv[0] == UvSource::Env);
    }
    SECTION("a layer on UV0 is not an env layer") {
        m.batches = {batch(0, 0, 1, 0, 0), batch(1, 1, 1, 4, 0)};
        const auto fx = resolveBatchEffects(m);
        CHECK_FALSE(fx[1].hidden);
        CHECK(fx[0].stages == 1);
    }
}

TEST_CASE("a two-texture env shine followed by its alpha layer is Opaque_Mod2xNA_Alpha",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.globalFlags = 0x08;
    m.textureCombinerCombos = {0, 6};  // stage 2 Mod2xNA
    m.materials = {M2Material{0, 0}, M2Material{0, 2}, M2Material{1, 2}};
    SECTION("same unlit flag, texture and weight: folded") {
        m.batches = {batch(0, 0, 2, 0, 3), batch(1, 1, 1, 0, 0)};
        const auto fx = resolveBatchEffects(m);
        CHECK(fx[0].runtimeShader == 0x8001);
        CHECK(fx[0].combiner == Combiner::Opaque_Mod2xNA_Alpha);
        CHECK(fx[0].texture[1] == 1);
        CHECK(fx[1].hidden);
    }
    SECTION("an unlit alpha layer over a lit base is drawn on its own") {
        m.batches = {batch(0, 0, 2, 0, 3), batch(2, 1, 1, 0, 0)};
        const auto fx = resolveBatchEffects(m);
        CHECK(fx[0].runtimeShader == 0x0E);
        CHECK(fx[0].combiner == Combiner::Opaque_Mod2xNA);
        CHECK_FALSE(fx[1].hidden);
    }
}

TEST_CASE("a batch on the same material as the one before takes its result",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.materials = {M2Material{0, 0}, M2Material{0, 4}};
    m.batches = {batch(0, 0, 1, 0, 0), batch(1, 1, 1, 4, 2), batch(1, 1, 1, 4, 2)};
    const auto fx = resolveBatchEffects(m);
    REQUIRE(fx.size() == 3);
    CHECK(fx[0].runtimeShader == 0x8002);
    CHECK(fx[1].hidden);
    CHECK(fx[2].hidden);
}

TEST_CASE("one-texture batches on UV set 1 keep their transform, env stages drop it",
          "[m2][effect][stock]") {
    M2Model m = model();
    m.materials = {M2Material{0, 2}};
    m.batches = {batch(0, 0, 1, 0, 1, 0, 0, 2)};
    const auto fx = resolveBatchEffects(m);
    CHECK(fx[0].uv[0] == UvSource::Uv1);
    CHECK(fx[0].transform[0] == 1);
    CHECK(modulateEmissive(6) == 0.5f);
    CHECK(modulateEmissive(5) == 1.0f);
}
