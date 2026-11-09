// The "HD characters" appearance table: what tools/hd_characters/convert.py
// writes beside a converted model, and how a 3.3.5 character is read through it.
#include <catch_amalgamated.hpp>
#include "pipeline/hd_characters.hpp"

#include <algorithm>

using namespace wowee::pipeline;

namespace {
// A cut-down HumanMale.hdlook, the lines as the converter writes them.
// Enhanced options: 9 skin, 10 face, 11 hair style, 12 hair colour,
// 9494 facial hair, 890 eyebrows, 525 face shape.
const char* kLook =
    "# HumanMale from Enhanced\n"
    "order 1 4 5 11 8 13\n"
    "hairtex 10\n"
    "choice 1 0 1 9\n"
    "choice 1 1 2 9\n"
    "choice 2 0 20 10\n"
    "choice 2 1 21 10\n"
    "choice 2 12 20 10 1 1\n"     // a face that only exists with skin 1
    "choice 3 0 44 11\n"
    "choice 3 1 45 11\n"
    "choice 4 0 61 12\n"
    "choice 4 1 62 12\n"
    "choice 5 0 78934 9494\n"
    "choice 5 8 78942 9494\n"
    "default 1 9\n"
    "default 9915 890\n"
    "default 5056 525\n"
    "tex 1 0 1 HD\\skin00.blp\n"
    "tex 1 0 13 HD\\pelvis00.blp\n"
    "tex 2 0 1 HD\\skin01.blp\n"
    "tex 20 1 5 HD\\faceupper00_00.blp\n"
    "tex 20 2 5 HD\\faceupper00_01.blp\n"
    "tex 21 1 5 HD\\faceupper01_00.blp\n"
    "tex 45 61 11 HD\\scalpupper00_00.blp\n"
    "tex 61 0 10 HD\\hair00.blp\n"
    "tex 62 0 10 HD\\hair01.blp\n"
    "tex 9915 62 8 HD\\eyebrows01.blp\n"
    "geo 44 0\n"
    "geo 45 2\n"
    "geo 78934 106\n"
    "geo 78934 307\n"
    "geo 78934 202\n"
    "geo 78942 100\n"
    "geo 78942 300\n"
    "geo 78942 200\n"
    "geo 5056 3202\n"
    "geo 9915 3400\n"
    "geo 0 3201\n"
    "geo 0 3301\n"
    "geo 0 3401\n";

CharacterAppearance who(int skin, int face, int style, int color, int facial) {
    CharacterAppearance a;
    a.skinId = static_cast<uint8_t>(skin);
    a.faceId = static_cast<uint8_t>(face);
    a.hairStyleId = static_cast<uint8_t>(style);
    a.hairColorId = static_cast<uint8_t>(color);
    a.facialHairId = static_cast<uint8_t>(facial);
    return a;
}

bool has(const std::vector<uint32_t>& v, uint32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
}  // namespace

TEST_CASE("the table parses as written", "[hd_characters]") {
    const auto look = parseHdCharacterLook(kLook);
    CHECK(look.bodyOrder == std::vector<int>{1, 4, 5, 11, 8, 13});
    CHECK(look.hairTarget == 10);
    CHECK(look.choices.size() == 11);
    CHECK(look.defaults.size() == 3);
    CHECK(look.textures.size() == 10);
    CHECK(look.geosets.size() == 13);
    const auto& dep = look.choices[4];
    CHECK(dep.option == HdCharacterLook::kFace);
    CHECK(dep.index == 12);
    CHECK(dep.dependsOn == HdCharacterLook::kSkin);
    CHECK(dep.dependsIndex == 1);
}

TEST_CASE("the five bytes pick choices, defaults fill the rest", "[hd_characters]") {
    const auto look = parseHdCharacterLook(kLook);
    const auto c = selectHdCharacterChoices(look, who(1, 0, 1, 1, 0));
    CHECK(has(c, 2));       // skin 1
    CHECK(has(c, 20));      // face 0
    CHECK(has(c, 45));      // hair style 1
    CHECK(has(c, 62));      // hair colour 1
    CHECK(has(c, 78934));   // facial 0
    CHECK(has(c, 9915));    // eyebrows: no 3.3.5 option, the default
    CHECK(!has(c, 1));      // skin's default lost to the 3.3.5 skin

    // A face number that depends on the skin: only with that skin.
    CHECK(has(selectHdCharacterChoices(look, who(1, 12, 0, 0, 0)), 20));
    // Unknown index: the option's index 0.
    CHECK(has(selectHdCharacterChoices(look, who(14, 0, 0, 0, 0)), 1));
}

TEST_CASE("textures come out in layer order, related choices matched", "[hd_characters]") {
    const auto look = parseHdCharacterLook(kLook);
    const auto t = resolveHdCharacterTextures(look, who(1, 0, 1, 1, 0));
    CHECK(t.bodySkin == "HD\\skin01.blp");
    // The face for skin 1; no scalp for hair colour 1 here; the brows for
    // hair colour 1; skin 1 names no pelvis.
    CHECK(t.underwear == std::vector<std::string>{"HD\\faceupper00_01.blp", "HD\\eyebrows01.blp"});
    CHECK(t.hair == "HD\\hair01.blp");

    const auto t0 = resolveHdCharacterTextures(look, who(0, 1, 1, 0, 0));
    CHECK(t0.bodySkin == "HD\\skin00.blp");
    CHECK(t0.underwear == std::vector<std::string>{"HD\\faceupper01_00.blp", "HD\\scalpupper00_00.blp",
                                                   "HD\\pelvis00.blp"});
}

TEST_CASE("geosets in the 3.3.5 tables' shape", "[hd_characters]") {
    const auto look = parseHdCharacterLook(kLook);
    const auto g = resolveHdCharacterGeosets(look, who(0, 0, 1, 0, 0));
    CHECK(g.hairScalp == 2);
    CHECK(g.facial100 == 6);
    CHECK(g.facial200 == 2);
    CHECK(g.facial300 == 7);
    // Groups above 20: the face shape replaces 3201, the brows' 3400 removes
    // 3401, the eyes stay.
    CHECK(g.extra == std::vector<uint16_t>{3202, 3301});

    // Bald and clean-shaven: no hair geoset at all, no beard.
    const auto bald = resolveHdCharacterGeosets(look, who(0, 0, 0, 0, 8));
    CHECK(bald.hairScalp == kHdNoHairGeoset);
    CHECK(bald.facial100 == 0);
    CHECK(bald.facial200 == 0);
    CHECK(bald.facial300 == 0);
}

TEST_CASE("a face's .bone file reads as bone ids and matrices", "[hd_characters]") {
    // version 1, BIDA with two ids, BOMT with two column-major matrices.
    std::vector<uint8_t> d = {1, 0, 0, 0, 'B', 'I', 'D', 'A', 4, 0, 0, 0, 52, 0, 60, 0,
                              'B', 'O', 'M', 'T', 128, 0, 0, 0};
    auto pushMatrix = [&](float tz) {
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, tz, 1};
        const auto* b = reinterpret_cast<const uint8_t*>(m);
        d.insert(d.end(), b, b + sizeof(m));
    };
    pushMatrix(0.25f);
    pushMatrix(-0.5f);
    const auto adj = parseHdBoneFile(d);
    REQUIRE(adj.size() == 2);
    CHECK(adj[0].bone == 52);
    CHECK(adj[1].bone == 60);
    CHECK(adj[0].matrix[3][2] == 0.25f);
    CHECK(adj[1].matrix[3][2] == -0.5f);
    CHECK(parseHdBoneFile({2, 0, 0, 0}).empty());
}
