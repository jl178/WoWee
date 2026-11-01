// The stock client's font string metrics, from Wow.exe 3.3.5a.
//
// Every number FrameXML lays out from - GetStringWidth, a self-sized label's
// width, where a paragraph wraps - comes out of these rules, and none of them
// is the font's own fractional advance. Each rule is pinned on its own with
// made-up glyphs, so a change to one is a failure that names it, and then the
// lot is checked against two strings measured in the stock client.
//
// The FRIZQT__ figures in the last cases are copied out of the font (hmtx,
// glyf bounding boxes, the kern table) rather than read off disk: Data/* is
// not in the repository, and a test that opened it would be skipped on every
// machine without the game.
#include <catch_amalgamated.hpp>

#include "ui/stock_font_metrics.hpp"

#include <map>
#include <utility>

using namespace wowee::ui;
using namespace wowee::ui::stockfont;

namespace {

class TableSource final : public GlyphMetricsSource {
public:
    std::map<uint32_t, GlyphUnits> glyphs;
    std::map<std::pair<uint32_t, uint32_t>, int> kerns;

    bool glyph(uint32_t cp, GlyphUnits& out) const override {
        const auto it = glyphs.find(cp);
        if (it == glyphs.end()) return false;
        out = it->second;
        return true;
    }
    int kerning(uint32_t l, uint32_t r) const override {
        const auto it = kerns.find({l, r});
        return it == kerns.end() ? 0 : it->second;
    }
};

GlyphUnits g(int advance, int xMin, int xMax, bool contours = true) {
    GlyphUnits u;
    u.advance = advance;
    u.xMin = xMin;
    u.xMax = xMax;
    u.hasContours = contours;
    u.unitsPerEm = 1000;
    return u;
}

/// FRIZQT__.TTF: advance, xMin, xMax for the glyphs the two strings use, and
/// the kern pairs among them. units per em 1000.
TableSource frizqt() {
    TableSource s;
    s.glyphs = {
        {' ', g(261, 0, 0, false)}, {'?', g(532, 70, 470)},
        {'C', g(670, 40, 657)},     {'I', g(304, 52, 252)},
        {'S', g(580, 37, 541)},     {'V', g(655, -5, 648)},
        {'a', g(545, 35, 539)},     {'c', g(521, 40, 525)},
        {'e', g(582, 40, 542)},     {'f', g(333, 34, 386)},
        {'g', g(619, 15, 616)},     {'h', g(589, 38, 550)},
        {'l', g(252, 38, 213)},     {'n', g(593, 42, 555)},
        {'o', g(631, 40, 591)},     {'r', g(349, 42, 340)},
        {'t', g(347, 27, 338)},
    };
    s.kerns = {
        {{'S', 't'}, -15}, {{'r', 'a'}, -23}, {{'e', 't'}, -17},
        {{'t', 'h'}, -1},  {{'r', 'n'}, 12},  {{'V', 'a'}, -59},
        {{'t', 'e'}, -8},  {{'f', 'o'}, -13},
    };
    return s;
}

}  // namespace

TEST_CASE("A font height is an em, rounded to whole pixels", "[fonts][stock]") {
    // 12 units at 1080p: 12 * 1080 / 768 = 16.875.
    CHECK(pixelSize(12.0f, 1080.0f / 768.0f) == 17);
    CHECK(pixelSize(62.0f, 1080.0f / 768.0f) == 87);
    CHECK(pixelSize(12.0f, 1.0f) == 12);
    // Nothing measures at zero pixels, even before the screen is known.
    CHECK(pixelSize(0.1f, 1.0f) == 1);
    CHECK(pixelSize(12.0f, 0.0f) == 12);
}

TEST_CASE("Glyphs are rasterised at no more than 32 pixels", "[fonts][stock]") {
    CHECK(rasterSize(17) == 17);
    CHECK(rasterSize(32) == 32);
    CHECK(rasterSize(87) == 32);
}

TEST_CASE("An advance is floor(horiAdvance) + 1, and THICK adds one more",
          "[fonts][stock]") {
    // 589 units at 17 px is 10.013 px.
    const GlyphUnits h = g(589, 38, 550);
    CHECK(advancePx(h, 17, FontOutline::None) == 11);
    CHECK(advancePx(h, 17, FontOutline::Normal) == 11);
    CHECK(advancePx(h, 17, FontOutline::Thick) == 12);
    // Exactly on a pixel. FreeType's scale is rounded up a hair at 16 px, so
    // 500 units comes to 512/64 = 8 exactly, and the floor stays at 8.
    CHECK(freetypeScale(16, 1000) == 67109);
    CHECK(toF26Dot6(500, freetypeScale(16, 1000)) == 512);
    CHECK(advancePx(g(500, 0, 500), 16, FontOutline::None) == 9);
}

TEST_CASE("Kerning is added before the ceiling, and only when it tightens",
          "[fonts][stock]") {
    const GlyphUnits r = g(349, 42, 340);  // advance 5.933 -> 6 at 17 px
    CHECK(pairAdvancePx(r, 0, 17, FontOutline::None) == 6);
    // -23 * 17 / 1000 = -0.391: ceil(5.609) is still 6.
    CHECK(pairAdvancePx(r, -23, 17, FontOutline::None) == 6);
    // -100 * 17 / 1000 = -1.7: ceil(4.3) is 5.
    CHECK(pairAdvancePx(r, -100, 17, FontOutline::None) == 5);
    // The client zeroes a positive kern before it scales it.
    CHECK(pairAdvancePx(r, 100, 17, FontOutline::None) == 6);
}

TEST_CASE("The last glyph adds its cell and its bearing, not its advance",
          "[fonts][stock]") {
    // 'o' at 17 px: x from 0.68 to 10.047, so a bitmap 11 wide (0 to 11) and
    // a bearing that rounds up to 1.
    const GlyphUnits o = g(631, 40, 591);
    CHECK(lastGlyphExtentPx(o, 17, FontOutline::None) == 12);
    CHECK(lastGlyphExtentPx(o, 17, FontOutline::Normal) == 14);
    CHECK(lastGlyphExtentPx(o, 17, FontOutline::Thick) == 16);
    // A space has no bitmap: its cell is (px + 3) / 4 wide.
    CHECK(lastGlyphExtentPx(g(261, 0, 0, false), 17, FontOutline::None) == 5);
    // A negative bearing: x from -1.016 to 11.016 is a bitmap from -2 to 12,
    // 14 wide, and the bearing rounds up to -1.
    CHECK(lastGlyphExtentPx(g(655, -60, 648), 17, FontOutline::None) == 13);
}

TEST_CASE("A line starts with the outline's pad in hand", "[fonts][stock]") {
    TableSource s;
    s.glyphs = {{'o', g(631, 40, 591)}};
    // One glyph: pad, then the glyph's extent.
    CHECK(stockTextWidth(s, "o", 17.0f, 1.0f, FontOutline::None) == 12.0f);
    CHECK(stockTextWidth(s, "o", 17.0f, 1.0f, FontOutline::Normal) == 2.0f + 14.0f);
    CHECK(stockTextWidth(s, "o", 17.0f, 1.0f, FontOutline::Thick) == 4.0f + 16.0f);
    // Two: the first one's advance (10.727 -> 11, 12 THICK) between them.
    CHECK(stockTextWidth(s, "oo", 17.0f, 1.0f, FontOutline::None) == 11.0f + 12.0f);
    CHECK(stockTextWidth(s, "oo", 17.0f, 1.0f, FontOutline::Thick) ==
          4.0f + 12.0f + 16.0f);
}

TEST_CASE("The pen places each glyph where the measurement put it",
          "[fonts][stock]") {
    TableSource s = frizqt();
    StockPen pen(s, 17, FontOutline::Thick);
    CHECK(pen.width() == 0.0f);
    CHECK(pen.place('r') == 4.0f);           // the pad
    CHECK(pen.place('a') == 4.0f + 7.0f);    // ceil(5 + 1 + 1 - 0.391)
    CHECK(pen.cursor() == 11.0f + 11.0f);    // 'a' 9.265 -> 9 + 2
    // A picture: the pen moves on from the cursor and kerning stops at it.
    pen.skip(16.0f);
    CHECK(pen.place('r') == 22.0f + 16.0f);
    CHECK(pen.width() == 38.0f + static_cast<float>(lastGlyphExtentPx(
                                     s.glyphs['r'], 17, FontOutline::Thick)));
}

TEST_CASE("A code point no face has is measured as '?'", "[fonts][stock]") {
    TableSource s = frizqt();
    CHECK(stockTextWidth(s, "\xE4\xB8\xAD", 17.0f, 1.0f, FontOutline::None) ==
          stockTextWidth(s, "?", 17.0f, 1.0f, FontOutline::None));
}

TEST_CASE("Above 32 pixels the 32 pixel layout is scaled up", "[fonts][stock]") {
    TableSource s = frizqt();
    const float at32 = stockTextWidth(s, "Vale", 32.0f, 1.0f, FontOutline::None);
    // 64 px is the 32 px layout twice over, not a layout of its own.
    CHECK(stockTextWidth(s, "Vale", 64.0f, 1.0f, FontOutline::None) == at32 * 2.0f);
    // And a width in units divides the pixels back out.
    CHECK(stockTextWidth(s, "Vale", 32.0f, 2.0f, FontOutline::None) ==
          Catch::Approx(at32));
}

TEST_CASE("A text is as wide as its widest line", "[fonts][stock]") {
    TableSource s = frizqt();
    const float a = stockTextWidth(s, "Vale", 17.0f, 1.0f, FontOutline::None);
    const float b = stockTextWidth(s, "Character", 17.0f, 1.0f, FontOutline::None);
    CHECK(stockTextWidth(s, "Vale\nCharacter", 17.0f, 1.0f, FontOutline::None) ==
          std::max(a, b));
    CHECK(stockTextWidth(s, "", 17.0f, 1.0f, FontOutline::None) == 0.0f);
}

TEST_CASE("The advance is where a caret goes after the text", "[fonts][stock]") {
    TableSource s = frizqt();
    // "ra": the pad, the kerned step to 'a', then 'a''s own advance.
    CHECK(stockTextAdvance(s, "ra", 17.0f, 1.0f, FontOutline::None) == 6.0f + 10.0f);
    CHECK(stockTextAdvance(s, "", 17.0f, 1.0f, FontOutline::Normal) == 2.0f);
}

TEST_CASE("One line is exactly the font height", "[fonts][stock]") {
    CHECK(stockTextHeight(1, 12.0f, 3.0f) == 12.0f);
    CHECK(stockTextHeight(3, 12.0f, 3.0f) == 3.0f * 12.0f + 2.0f * 3.0f);
    CHECK(stockTextHeight(0, 12.0f, 3.0f) == 12.0f);
}

TEST_CASE("UTF-8 is read a code point at a time", "[fonts][stock]") {
    const std::string text = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xFF";
    size_t at = 0;
    CHECK(nextCodepoint(text, at) == 'a');
    CHECK(nextCodepoint(text, at) == 0xE9u);
    CHECK(nextCodepoint(text, at) == 0x4E2Du);
    CHECK(nextCodepoint(text, at) == 0x1F600u);
    CHECK(nextCodepoint(text, at) == 0xFFu);  // malformed: the byte itself
    CHECK(at == text.size());
    // Cut off by the end of the text.
    const std::string cut = "\xE4\xB8";
    at = 0;
    CHECK(nextCodepoint(cut, at) == 0xE4u);
    CHECK(at == 1);
}

TEST_CASE("FRIZQT__ measures what the stock client measures", "[fonts][stock]") {
    TableSource s = frizqt();
    const float ppu1080 = 1080.0f / 768.0f;
    // A zone title: 62 units, THICK. 87 px rasterised at 32 and scaled up.
    // The stock client draws it about 601 units wide at 1080p.
    CHECK(stockTextWidth(s, "Stranglethorn Vale", 62.0f, ppu1080, FontOutline::Thick) ==
          Catch::Approx(601.27f).margin(0.01f));
    // The same width at any height of screen above the clamp, since the
    // layout is the 32 px one either way.
    CHECK(stockTextWidth(s, "Stranglethorn Vale", 62.0f, 1440.0f / 768.0f,
                         FontOutline::Thick) == Catch::Approx(601.27f).margin(0.01f));
    // A 12-unit caption at 17 px: 108 px of steps, then 'o''s 11 px bitmap
    // and its 0.68 px bearing rounded up - 120 px, 85.33 units.
    CHECK(stockTextWidth(s, "Character Info", 12.0f, ppu1080, FontOutline::None) ==
          Catch::Approx(85.333f).margin(0.01f));
}
