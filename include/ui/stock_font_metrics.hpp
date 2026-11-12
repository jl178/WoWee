// stock_font_metrics.hpp - How wide the stock client says a line of text is.
//
// The numbers come from Wow.exe 3.3.5a, not from the font: the client loads
// each glyph through FreeType without hinting, but then lays text out on
// whole pixels by rules of its own, and those rules are what every
// GetStringWidth, every self-sized label and every wrap decision in FrameXML
// was written against. Measuring with the font's own fractional advances
// came out about 10% narrow at the sizes the interface uses - a caption is
// a few pixels short, a zone title tens of pixels - and everything anchored
// to a caption's right edge landed short by that much.
//
// The rules, per font string:
//
//   - The height is an em size. It becomes px = round(height * pixels per
//     unit) - pixels per unit being screenH * effective scale / 768 - and the
//     glyphs are rasterised at min(px, 32). Above 32 the 32 px layout is
//     scaled up by px / 32, measurement and drawing alike.
//   - A glyph advances floor(horiAdvance) + 1 pixels; a THICK outline adds
//     one more. NORMAL adds nothing to the advance.
//   - Between two glyphs the pen moves ceil(advance + kerning * px / em),
//     and only kerning that pulls the pair together counts: the client
//     zeroes any positive value FreeType reports.
//   - Each line starts with the outline's pad in hand (4 THICK, 2 NORMAL,
//     none without), and its last glyph contributes the width of its cell -
//     bitmap width plus pad, or a quarter em for a glyph with no bitmap -
//     plus ceil(bearing X), instead of its advance. A text's width is its
//     widest line.
//   - A text's height is lines * height + (lines - 1) * spacing. One line is
//     exactly the font height.
//
// Free of ImGui and of any font loader, so it can be tested with made-up
// glyphs: the glyphs come from a GlyphMetricsSource, which the interface
// fills from the TrueType file and a test fills by hand.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee::ui {

/// The outline a font object asks for. It changes the metrics, not just the
/// look: the outline is drawn into the glyph's cell, so the cell is wider.
enum class FontOutline : uint8_t { None, Normal, Thick };

/// "THICK" and "NORMAL" as a font object writes them; anything else, "NONE"
/// and empty included, is no outline.
FontOutline fontOutlineFromName(std::string_view name);

/// One glyph's metrics in font units, as the TrueType tables give them.
struct GlyphUnits {
    int advance = 0;           ///< hmtx advance width
    int xMin = 0, xMax = 0;    ///< the outline's horizontal extent
    bool hasContours = false;  ///< false for a space: no bitmap at all
    int unitsPerEm = 1000;     ///< of the face the glyph came from
};

/// Where glyph metrics come from.
class GlyphMetricsSource {
public:
    virtual ~GlyphMetricsSource() = default;
    /// The glyph for a code point, or false if no face has one.
    virtual bool glyph(uint32_t codepoint, GlyphUnits& out) const = 0;
    /// Kerning between two code points in font units, as the 'kern' table
    /// gives it. Zero for a pair it has nothing for.
    virtual int kerning(uint32_t left, uint32_t right) const = 0;
};

namespace stockfont {

/// The largest size glyphs are rasterised at; see the header comment.
constexpr int kMaxRasterPx = 32;

/// The pixel size of a font `height` units tall at `pixelsPerUnit`. Never
/// less than one, so a measurement taken before the screen is known still
/// lays text out.
int pixelSize(float height, float pixelsPerUnit);

/// The size the glyphs are rasterised and laid out at: px, at most 32.
int rasterSize(int px);

/// The pen's starting offset and the extra cell width an outline needs.
int outlinePad(FontOutline outline);

/// FreeType's 16.16 x scale for a pixel size, and a font-unit value through
/// it into 26.6. The client reads glyph metrics after this rounding, and the
/// floor below lands on the other side of a whole pixel often enough to
/// matter if a float is used instead.
int32_t freetypeScale(int ppem, int unitsPerEm);
int32_t toF26Dot6(int units, int32_t scale);

/// How far the pen moves for a glyph with no kerning after it:
/// floor(horiAdvance) + 1, and + 1 more for THICK.
int advancePx(const GlyphUnits& g, int rasterPx, FontOutline outline);

/// How far the pen moves from `prev` to the next glyph, with the kerning
/// between them: ceil(advance + min(kern, 0) * px / em).
int pairAdvancePx(const GlyphUnits& prev, int kernUnits, int rasterPx,
                  FontOutline outline);

/// What the last glyph of a line adds instead of its advance: its cell (the
/// bitmap's width plus the pad, or (px + 3) / 4 plus the pad for a glyph with
/// no bitmap) and ceil(bearing X).
int lastGlyphExtentPx(const GlyphUnits& g, int rasterPx, FontOutline outline);

} // namespace stockfont

/// One line laid out glyph by glyph, the way the client moves its pen.
///
/// Everything is in raster pixels - multiply by px / rasterPx for screen
/// pixels. The drawing side places each glyph at the origin place() answers,
/// so the glyphs land exactly where the measurement assumed they would.
class StockPen {
public:
    StockPen(const GlyphMetricsSource& source, int rasterPx, FontOutline outline);

    /// Puts the next glyph down and answers its origin. A code point no face
    /// has is measured as '?', which is what ImGui draws in its place.
    float place(uint32_t codepoint);

    /// Moves past something that is not a glyph - an inline picture - this
    /// many raster pixels wide. Kerning does not reach across it.
    void skip(float rasterPx);

    /// Where the next glyph would start without kerning.
    [[nodiscard]] float cursor() const;

    /// The line's width so far, by the rule for the last glyph.
    [[nodiscard]] float width() const;

private:
    const GlyphMetricsSource& source_;
    int rasterPx_;
    FontOutline outline_;
    float origin_ = 0.0f;      ///< origin of the last glyph placed
    float after_ = 0.0f;       ///< the cursor when the last thing was a skip
    bool havePrev_ = false;    ///< whether the last thing placed was a glyph
    bool any_ = false;
    uint32_t prevCode_ = 0;
    GlyphUnits prev_{};
};

/// The next code point of UTF-8 text from `at`, which it moves past it.
/// A malformed byte comes back as itself, so nothing loops endlessly.
uint32_t nextCodepoint(std::string_view text, size_t& at);

/// The width of plain text - no markup - in the caller's units: the widest
/// of its '\n'-separated lines, laid out at the raster size, scaled to px and
/// divided by `pixelsPerUnit`. Pass 1 for `pixelsPerUnit` and the size in
/// pixels to measure in pixels.
float stockTextWidth(const GlyphMetricsSource& source, std::string_view text,
                     float height, float pixelsPerUnit, FontOutline outline);

/// Where the pen stands after plain text, in the caller's units - the
/// advance, not the width. An edit box's caret goes here.
float stockTextAdvance(const GlyphMetricsSource& source, std::string_view text,
                       float height, float pixelsPerUnit, FontOutline outline);

/// GetStringHeight: lines * height + (lines - 1) * spacing.
float stockTextHeight(int lines, float height, float spacing);

/// Glyph metrics read from TrueType files: the face itself, then any faces
/// merged under it for the code points it lacks (the CJK fallback).
class TrueTypeGlyphSource final : public GlyphMetricsSource {
public:
    TrueTypeGlyphSource();
    ~TrueTypeGlyphSource() override;
    TrueTypeGlyphSource(const TrueTypeGlyphSource&) = delete;
    TrueTypeGlyphSource& operator=(const TrueTypeGlyphSource&) = delete;

    /// Adds a face after the ones already held. False, and nothing added, if
    /// the bytes are not a font stb_truetype can read. The bytes are kept for
    /// as long as this is.
    bool addFace(std::shared_ptr<const std::vector<uint8_t>> bytes);
    bool addFace(std::vector<uint8_t> bytes) {
        return addFace(std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
    }
    [[nodiscard]] bool empty() const;

    bool glyph(uint32_t codepoint, GlyphUnits& out) const override;
    int kerning(uint32_t left, uint32_t right) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wowee::ui
