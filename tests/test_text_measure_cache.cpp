#include <catch_amalgamated.hpp>

#include "ui/text_measure_cache.hpp"

#include <limits>
#include <string>

using namespace wowee::ui;

// The renderer answers a label's size from this cache instead of measuring
// when the key matches, so a key that matches when it should not is a label
// drawn at another label's size. Pin what is part of the key.

namespace {
TextMeasureKey key(const void* face, float size, const std::string& text,
                   bool paragraph = false, float wrapWidth = 0.0f,
                   bool nonSpaceWrap = false) {
    TextMeasureKey k;
    k.face = face;
    k.size = size;
    k.text = text;
    k.paragraph = paragraph;
    k.wrapWidth = wrapWidth;
    k.nonSpaceWrap = nonSpaceWrap;
    return k;
}
}  // namespace

TEST_CASE("A remembered measurement is found only under every input it was made with",
          "[ui][textmeasure]") {
    int faceA = 0, faceB = 0;
    TextMeasureCache cache;
    cache.insert(key(&faceA, 12.0f, "Goldshire"), {.width = 57.5f, .rows = 1});

    const TextMeasure* hit = cache.find(key(&faceA, 12.0f, "Goldshire"));
    REQUIRE(hit != nullptr);
    CHECK(hit->width == 57.5f);
    CHECK(hit->rows == 1);

    // Each input on its own is enough to miss.
    CHECK(cache.find(key(&faceB, 12.0f, "Goldshire")) == nullptr);
    CHECK(cache.find(key(&faceA, 12.5f, "Goldshire")) == nullptr);
    CHECK(cache.find(key(&faceA, 12.0f, "Goldshire ")) == nullptr);
    CHECK(cache.find(key(&faceA, 12.0f, "Goldshire", true)) == nullptr);
    CHECK(cache.find(key(&faceA, 12.0f, "Goldshire", false, 100.0f)) == nullptr);
    CHECK(cache.find(key(&faceA, 12.0f, "Goldshire", false, 0.0f, true)) == nullptr);
}

TEST_CASE("A paragraph is remembered per wrap width", "[ui][textmeasure]") {
    int face = 0;
    TextMeasureCache cache;
    const std::string prose = "Bring me the heads of ten kobolds.";
    cache.insert(key(&face, 12.0f, prose, true, 285.0f), {.width = 0.0f, .rows = 2});
    cache.insert(key(&face, 12.0f, prose, true, 100.0f), {.width = 0.0f, .rows = 4});
    REQUIRE(cache.find(key(&face, 12.0f, prose, true, 285.0f)) != nullptr);
    CHECK(cache.find(key(&face, 12.0f, prose, true, 285.0f))->rows == 2);
    CHECK(cache.find(key(&face, 12.0f, prose, true, 100.0f))->rows == 4);
    // The last place counts: a width a hair narrower may wrap differently.
    CHECK(cache.find(key(&face, 12.0f, prose, true, 284.99997f)) == nullptr);
}

TEST_CASE("Float inputs compare by their bits", "[ui][textmeasure]") {
    int face = 0;
    TextMeasureCache cache;
    // A NaN width still finds itself rather than missing for ever and
    // filling the cache with copies of the same entry.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    cache.insert(key(&face, 12.0f, "x", true, nan), {.width = 0.0f, .rows = 1});
    CHECK(cache.find(key(&face, 12.0f, "x", true, nan)) != nullptr);
    cache.insert(key(&face, 12.0f, "x", true, nan), {.width = 0.0f, .rows = 1});
    CHECK(cache.size() == 1);
    // Zero and negative zero are different keys: nothing is assumed equal
    // that a measurement was not actually made for.
    cache.insert(key(&face, 12.0f, "y", true, 0.0f), {.width = 0.0f, .rows = 1});
    CHECK(cache.find(key(&face, 12.0f, "y", true, -0.0f)) == nullptr);
}

TEST_CASE("The cache empties when full and on request", "[ui][textmeasure]") {
    int face = 0;
    TextMeasureCache cache(3);
    cache.insert(key(&face, 12.0f, "1"), {.width = 6.0f, .rows = 1});
    cache.insert(key(&face, 12.0f, "2"), {.width = 6.0f, .rows = 1});
    cache.insert(key(&face, 12.0f, "3"), {.width = 6.0f, .rows = 1});
    CHECK(cache.size() == 3);
    // The fourth empties it first, so it never holds more than its bound.
    cache.insert(key(&face, 12.0f, "4"), {.width = 7.0f, .rows = 1});
    CHECK(cache.size() == 1);
    CHECK(cache.find(key(&face, 12.0f, "1")) == nullptr);
    REQUIRE(cache.find(key(&face, 12.0f, "4")) != nullptr);
    CHECK(cache.find(key(&face, 12.0f, "4"))->width == 7.0f);

    // Replacing an entry keeps one copy with the new answer.
    cache.insert(key(&face, 12.0f, "4"), {.width = 8.0f, .rows = 2});
    CHECK(cache.size() == 1);
    CHECK(cache.find(key(&face, 12.0f, "4"))->rows == 2);

    // A face rebuilt at the same address: everything goes.
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.find(key(&face, 12.0f, "4")) == nullptr);
}
