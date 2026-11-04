// The local lights culled to a screen grid (rendering/light_tiles.hpp).
//
// The grid may only leave a light out of a tile when no pixel in the tile can
// be inside the light's sphere - otherwise a torch's pool of light would be cut
// off at a tile edge. This throws random cameras and lights at the culling and
// checks, for points inside each sphere that land on the screen in front of
// the camera, that the tile the shader would read for that pixel has the bit.

#include <catch_amalgamated.hpp>
#include "rendering/light_tiles.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <vector>

using namespace wowee::rendering;

namespace {

bool tileHasLight(const std::vector<glm::uvec4>& tiles, int tx, int ty, uint32_t light) {
    const int idx = ty * static_cast<int>(LIGHT_TILES_X) + tx;
    const glm::uvec4 w = tiles[idx >> 1];
    const uint32_t lo = (idx & 1) == 0 ? w.x : w.z;
    const uint32_t hi = (idx & 1) == 0 ? w.y : w.w;
    return ((light < 32 ? lo : hi) >> (light & 31u)) & 1u;
}

glm::mat4 vulkanProjection(float fovDeg, float aspect, float nearZ, float farZ) {
    glm::mat4 p = glm::perspective(glm::radians(fovDeg), aspect, nearZ, farZ);
    p[1][1] *= -1.0f;  // as Camera does
    return p;
}

}  // namespace

namespace {

// Random cameras and lights; for points inside each sphere that land on the
// screen in front of the camera, the tile the shader reads for that pixel -
// and for any pixel up to `slackPx` from it (pixel centres, sample positions)
// - must hold the light. `tight` culls with the view and projection apart and
// a two-pixel margin, else with their product and the one-tile margin.
int checkCoverage(bool tight, float slackPx, bool jitter) {
    std::mt19937 rng(tight ? 4321 : 1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<glm::uvec4> tiles(LIGHT_TILE_WORDS);
    const float width = 1251.0f, height = 986.0f;
    int checked = 0;
    for (int scene = 0; scene < 200; ++scene) {
        const glm::vec3 eye(u(rng) * 50.0f, u(rng) * 50.0f, u(rng) * 10.0f);
        const glm::vec3 target = eye + glm::vec3(u(rng), u(rng), u(rng) * 0.4f);
        const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 0.0f, 1.0f));
        const float aspect = 1.0f + 1.5f * (u(rng) * 0.5f + 0.5f);
        glm::mat4 proj = vulkanProjection(50.0f + 20.0f * u(rng), aspect, 0.3f, 2000.0f);
        if (jitter) {
            // A sub-pixel jitter as the upscalers put it in: a shift in NDC.
            proj[2][0] += u(rng) * 2.0f / width;
            proj[2][1] += u(rng) * 2.0f / height;
        }
        const glm::mat4 vp = proj * view;

        std::vector<glm::vec4> lights(64);
        for (auto& l : lights) {
            l = glm::vec4(eye + glm::vec3(u(rng), u(rng), u(rng) * 0.3f) * 60.0f,
                          3.0f + 10.0f * (u(rng) * 0.5f + 0.5f));
        }
        if (tight) {
            cullLightsToTiles(view, proj, lights.data(), 64, tiles.data(),
                              lightTileMarginPixels(2.0f, width, height));
        } else {
            cullLightsToTiles(vp, lights.data(), 64, tiles.data());
        }

        for (uint32_t i = 0; i < 64; ++i) {
            for (int s = 0; s < 60; ++s) {
                glm::vec3 d(u(rng), u(rng), u(rng));
                if (glm::dot(d, d) > 1.0f) continue;
                const glm::vec3 p = glm::vec3(lights[i]) + d * (lights[i].w * 0.999f);
                const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
                if (clip.w < 0.3f || clip.z < 0.0f || clip.z > clip.w) continue;  // not drawn
                glm::vec2 ndc = glm::vec2(clip) / clip.w;
                ndc += glm::vec2(u(rng), u(rng)) * glm::vec2(2.0f * slackPx / width, 2.0f * slackPx / height);
                if (std::abs(ndc.x) >= 1.0f || std::abs(ndc.y) >= 1.0f) continue;
                // gl_FragCoord * tiles / extent, as the shader reads it.
                const int tx = static_cast<int>((ndc.x * 0.5f + 0.5f) * LIGHT_TILES_X);
                const int ty = static_cast<int>((ndc.y * 0.5f + 0.5f) * LIGHT_TILES_Y);
                REQUIRE(tileHasLight(tiles, tx, ty, i));
                ++checked;
            }
        }
    }
    return checked;
}

int markedTiles(const std::vector<glm::uvec4>& tiles, uint32_t light) {
    int n = 0;
    for (int ty = 0; ty < static_cast<int>(LIGHT_TILES_Y); ++ty)
        for (int tx = 0; tx < static_cast<int>(LIGHT_TILES_X); ++tx) n += tileHasLight(tiles, tx, ty, light);
    return n;
}

}  // namespace

TEST_CASE("Light tiles keep every light a visible pixel in the tile can be within", "[light_tiles]") {
    REQUIRE(checkCoverage(false, 0.0f, false) > 10000);
}

TEST_CASE("Tangent-bounded light tiles with a two-pixel margin keep every light", "[light_tiles]") {
    // A pixel's centre lies up to half a pixel from where its surface point
    // projects, a sample position or a jitter a little more: one pixel of
    // slack in the check, two of margin in the cull.
    REQUIRE(checkCoverage(true, 1.0f, false) > 10000);
    REQUIRE(checkCoverage(true, 1.0f, true) > 10000);
}

TEST_CASE("Tangent-bounded light tiles mark fewer tiles than the box and a tile of margin",
          "[light_tiles]") {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 proj = vulkanProjection(60.0f, 1251.0f / 986.0f, 0.3f, 2000.0f);
    // A torch's reach ahead, near and far, and one off to the side.
    const glm::vec4 lights[3] = {glm::vec4(25.0f, 0.0f, -2.0f, 5.0f), glm::vec4(120.0f, 10.0f, 0.0f, 5.0f),
                                 glm::vec4(30.0f, -14.0f, 3.0f, 4.0f)};
    std::vector<glm::uvec4> box(LIGHT_TILE_WORDS), tight(LIGHT_TILE_WORDS);
    cullLightsToTiles(proj * view, lights, 3, box.data());
    cullLightsToTiles(view, proj, lights, 3, tight.data(), lightTileMarginPixels(2.0f, 1251.0f, 986.0f));
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(markedTiles(tight, i) > 0);
        CHECK(markedTiles(tight, i) < markedTiles(box, i));
        // Every tile the tight cull marks, the old one did.
        for (int ty = 0; ty < static_cast<int>(LIGHT_TILES_Y); ++ty)
            for (int tx = 0; tx < static_cast<int>(LIGHT_TILES_X); ++tx)
                if (tileHasLight(tight, tx, ty, i)) CHECK(tileHasLight(box, tx, ty, i));
    }
    // The far one, which covers a tile or two: the margin of a tile all round
    // was most of what it marked.
    CHECK(markedTiles(tight, 1) * 2 <= markedTiles(box, 1));
}

TEST_CASE("A projection that is not a plain perspective falls back to the box", "[light_tiles]") {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 ortho = glm::ortho(-50.0f, 50.0f, -40.0f, 40.0f, 0.1f, 500.0f);
    const glm::vec4 light(60.0f, 5.0f, 2.0f, 6.0f);
    std::vector<glm::uvec4> a(LIGHT_TILE_WORDS), b(LIGHT_TILE_WORDS);
    cullLightsToTiles(ortho * view, &light, 1, a.data(), lightTileMarginPixels(2.0f, 1251.0f, 986.0f));
    cullLightsToTiles(view, ortho, &light, 1, b.data(), lightTileMarginPixels(2.0f, 1251.0f, 986.0f));
    CHECK(markedTiles(a, 0) == markedTiles(b, 0));
    CHECK(markedTiles(b, 0) > 0);
}

TEST_CASE("Light tiles leave far lights out of most of the screen", "[light_tiles]") {
    // The point of the grid: a small light ahead marks a few tiles, not all.
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 vp = vulkanProjection(60.0f, 16.0f / 9.0f, 0.3f, 2000.0f) * view;
    const glm::vec4 lights[2] = {glm::vec4(40.0f, 0.0f, 0.0f, 4.0f),     // ahead
                                 glm::vec4(-40.0f, 0.0f, 0.0f, 4.0f)};   // behind
    std::vector<glm::uvec4> tiles(LIGHT_TILE_WORDS);
    cullLightsToTiles(vp, lights, 2, tiles.data());
    int ahead = 0, behind = 0;
    for (int ty = 0; ty < static_cast<int>(LIGHT_TILES_Y); ++ty)
        for (int tx = 0; tx < static_cast<int>(LIGHT_TILES_X); ++tx) {
            ahead += tileHasLight(tiles, tx, ty, 0);
            behind += tileHasLight(tiles, tx, ty, 1);
        }
    CHECK(ahead > 0);
    CHECK(ahead < static_cast<int>(LIGHT_TILES_X * LIGHT_TILES_Y) / 8);
    CHECK(behind == 0);
}
