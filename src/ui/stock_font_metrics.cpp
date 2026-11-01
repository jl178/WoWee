#include "ui/stock_font_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// The TrueType reader ImGui ships, compiled privately here: ImGui's own copy
// is static to imgui_draw.cpp, and this needs the kerning lookup it keeps
// internal. A second static copy costs a few kilobytes and no symbol clashes.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imgui/imstb_truetype.h>

namespace wowee::ui {

FontOutline fontOutlineFromName(std::string_view name) {
    if (name == "THICK") return FontOutline::Thick;
    if (name == "NORMAL") return FontOutline::Normal;
    return FontOutline::None;
}

namespace stockfont {

int pixelSize(float height, float pixelsPerUnit) {
    if (!(pixelsPerUnit > 0.0f)) pixelsPerUnit = 1.0f;
    const long px = std::lround(height * pixelsPerUnit);
    return px < 1 ? 1 : static_cast<int>(px);
}

int rasterSize(int px) { return std::min(px, kMaxRasterPx); }

int outlinePad(FontOutline outline) {
    switch (outline) {
        case FontOutline::Thick:  return 4;
        case FontOutline::Normal: return 2;
        case FontOutline::None:   break;
    }
    return 0;
}

int32_t freetypeScale(int ppem, int unitsPerEm) {
    // FT_DivFix(ppem << 6, units_per_EM), rounded to nearest as FreeType does.
    if (unitsPerEm <= 0) unitsPerEm = 1000;
    const int64_t num = (static_cast<int64_t>(ppem) << 6) << 16;
    return static_cast<int32_t>((num + unitsPerEm / 2) / unitsPerEm);
}

int32_t toF26Dot6(int units, int32_t scale) {
    // FT_MulFix: round to nearest, symmetric about zero.
    const int64_t a = units < 0 ? -static_cast<int64_t>(units) : units;
    const int64_t b = scale < 0 ? -static_cast<int64_t>(scale) : scale;
    const int64_t r = (a * b + 0x8000) >> 16;
    return static_cast<int32_t>(((units < 0) != (scale < 0)) ? -r : r);
}

namespace {
/// x >> 6 rounded toward zero, which is what the client's (x + (x >> 31 &
/// 63)) >> 6 does to a 26.6 advance.
int truncPixels(int32_t v26) { return (v26 + ((v26 >> 31) & 63)) >> 6; }
} // namespace

int advancePx(const GlyphUnits& g, int rasterPx, FontOutline outline) {
    const int32_t scale = freetypeScale(rasterPx, g.unitsPerEm);
    const int adv = truncPixels(toF26Dot6(g.advance, scale)) + 1;
    return outline == FontOutline::Thick ? adv + 1 : adv;
}

int pairAdvancePx(const GlyphUnits& prev, int kernUnits, int rasterPx,
                  FontOutline outline) {
    // Only kerning that tightens a pair; the client masks a positive value
    // away before it scales it.
    const int kern = std::min(kernUnits, 0);
    const float em = static_cast<float>(prev.unitsPerEm > 0 ? prev.unitsPerEm : 1000);
    const float moved = static_cast<float>(advancePx(prev, rasterPx, outline)) +
                        static_cast<float>(kern) * static_cast<float>(rasterPx) / em;
    return static_cast<int>(std::ceil(moved));
}

int lastGlyphExtentPx(const GlyphUnits& g, int rasterPx, FontOutline outline) {
    const int pad = outlinePad(outline);
    if (!g.hasContours) {
        // No bitmap: the client sizes the cell a quarter em wide instead.
        return ((rasterPx + 3) >> 2) + pad;
    }
    const int32_t scale = freetypeScale(rasterPx, g.unitsPerEm);
    const int32_t x0 = toF26Dot6(g.xMin, scale);
    const int32_t x1 = toF26Dot6(g.xMax, scale);
    // The smooth rasteriser's bitmap: the control box out to whole pixels.
    const int bitmapW = (((x1 + 63) & -64) - (x0 & -64)) >> 6;
    const int cell = (bitmapW > 0 ? bitmapW : ((rasterPx + 3) >> 2)) + pad;
    const int bearing = static_cast<int>(std::ceil(static_cast<float>(x0) / 64.0f));
    return cell + bearing;
}

} // namespace stockfont

StockPen::StockPen(const GlyphMetricsSource& source, int rasterPx, FontOutline outline)
    : source_(source), rasterPx_(rasterPx), outline_(outline),
      after_(static_cast<float>(stockfont::outlinePad(outline))) {}

float StockPen::place(uint32_t codepoint) {
    GlyphUnits g;
    if (!source_.glyph(codepoint, g) && !source_.glyph('?', g)) g = GlyphUnits{};
    float origin = after_;
    if (havePrev_) {
        origin = origin_ + static_cast<float>(stockfont::pairAdvancePx(
                               prev_, source_.kerning(prevCode_, codepoint),
                               rasterPx_, outline_));
    }
    origin_ = origin;
    prev_ = g;
    prevCode_ = codepoint;
    havePrev_ = true;
    any_ = true;
    return origin;
}

void StockPen::skip(float rasterPx) {
    after_ = cursor() + rasterPx;
    havePrev_ = false;
    any_ = true;
}

float StockPen::cursor() const {
    if (!havePrev_) return after_;
    return origin_ + static_cast<float>(stockfont::advancePx(prev_, rasterPx_, outline_));
}

float StockPen::width() const {
    if (!any_) return 0.0f;
    if (!havePrev_) return after_;
    return origin_ + static_cast<float>(
                         stockfont::lastGlyphExtentPx(prev_, rasterPx_, outline_));
}

uint32_t nextCodepoint(std::string_view text, size_t& at) {
    const auto c = static_cast<unsigned char>(text[at]);
    int extra = 0;
    uint32_t cp = c;
    if ((c & 0xE0u) == 0xC0u)      { extra = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0u) == 0xE0u) { extra = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8u) == 0xF0u) { extra = 3; cp = c & 0x07u; }
    // ASCII, a stray continuation byte, or a sequence cut off by the end of
    // the text: one byte, as itself.
    if (extra == 0 || at + static_cast<size_t>(extra) >= text.size()) {
        ++at;
        return c;
    }
    for (int i = 1; i <= extra; ++i) {
        const auto cc = static_cast<unsigned char>(text[at + static_cast<size_t>(i)]);
        if ((cc & 0xC0u) != 0x80u) {
            ++at;
            return c;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    at += static_cast<size_t>(extra) + 1;
    return cp;
}

namespace {
/// Runs `each(lineText)` over the '\n'-separated lines of text.
template <typename Each>
void forEachLine(std::string_view text, Each each) {
    size_t start = 0;
    while (true) {
        const size_t brk = text.find('\n', start);
        each(text.substr(start, brk == std::string_view::npos ? std::string_view::npos
                                                               : brk - start));
        if (brk == std::string_view::npos) break;
        start = brk + 1;
    }
}

float lineRaster(const GlyphMetricsSource& source, std::string_view line,
                 int rasterPx, FontOutline outline, bool advanceOnly) {
    StockPen pen(source, rasterPx, outline);
    size_t at = 0;
    while (at < line.size()) {
        const uint32_t cp = nextCodepoint(line, at);
        // Control characters draw nothing and move nothing; a '\r' left by a
        // DOS line ending is the one that turns up.
        if (cp < 0x20) continue;
        pen.place(cp);
    }
    return advanceOnly ? pen.cursor() : pen.width();
}
} // namespace

float stockTextWidth(const GlyphMetricsSource& source, std::string_view text,
                     float height, float pixelsPerUnit, FontOutline outline) {
    if (text.empty()) return 0.0f;
    if (!(pixelsPerUnit > 0.0f)) pixelsPerUnit = 1.0f;
    const int px = stockfont::pixelSize(height, pixelsPerUnit);
    const int raster = stockfont::rasterSize(px);
    float widest = 0.0f;
    forEachLine(text, [&](std::string_view line) {
        widest = std::max(widest, lineRaster(source, line, raster, outline, false));
    });
    return widest * static_cast<float>(px) / static_cast<float>(raster) / pixelsPerUnit;
}

float stockTextAdvance(const GlyphMetricsSource& source, std::string_view text,
                       float height, float pixelsPerUnit, FontOutline outline) {
    if (!(pixelsPerUnit > 0.0f)) pixelsPerUnit = 1.0f;
    const int px = stockfont::pixelSize(height, pixelsPerUnit);
    const int raster = stockfont::rasterSize(px);
    const size_t brk = text.rfind('\n');
    const std::string_view last =
        brk == std::string_view::npos ? text : text.substr(brk + 1);
    return lineRaster(source, last, raster, outline, true) * static_cast<float>(px) /
           static_cast<float>(raster) / pixelsPerUnit;
}

float stockTextHeight(int lines, float height, float spacing) {
    if (lines < 1) lines = 1;
    return static_cast<float>(lines) * height +
           static_cast<float>(lines - 1) * spacing;
}

struct TrueTypeGlyphSource::Impl {
    struct Face {
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        stbtt_fontinfo info{};
        int unitsPerEm = 1000;
    };
    // stbtt_fontinfo points into `bytes`, which the shared pointer pins.
    std::vector<std::unique_ptr<Face>> faces;

    /// A code point's glyph, with the face it came from; -1 for none.
    struct Entry {
        GlyphUnits units;
        int face = -1;
        int index = 0;
    };
    mutable std::unordered_map<uint32_t, Entry> glyphs;
    mutable std::unordered_map<uint64_t, int> kerns;

    const Entry& lookup(uint32_t cp) const {
        const auto it = glyphs.find(cp);
        if (it != glyphs.end()) return it->second;
        Entry e;
        for (size_t f = 0; f < faces.size(); ++f) {
            const Face& face = *faces[f];
            const int index = stbtt_FindGlyphIndex(&face.info, static_cast<int>(cp));
            if (index == 0) continue;
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&face.info, index, &adv, &lsb);
            e.units.advance = adv;
            e.units.unitsPerEm = face.unitsPerEm;
            e.units.hasContours = stbtt_IsGlyphEmpty(&face.info, index) == 0;
            if (e.units.hasContours) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphBox(&face.info, index, &x0, &y0, &x1, &y1);
                e.units.xMin = x0;
                e.units.xMax = x1;
            }
            e.face = static_cast<int>(f);
            e.index = index;
            break;
        }
        return glyphs.emplace(cp, e).first->second;
    }
};

TrueTypeGlyphSource::TrueTypeGlyphSource() : impl_(std::make_unique<Impl>()) {}
TrueTypeGlyphSource::~TrueTypeGlyphSource() = default;

bool TrueTypeGlyphSource::addFace(std::shared_ptr<const std::vector<uint8_t>> bytes) {
    if (!bytes || bytes->size() < 64) return false;
    auto face = std::make_unique<Impl::Face>();
    face->bytes = std::move(bytes);
    const unsigned char* data = face->bytes->data();
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&face->info, data, offset)) return false;
    // head.unitsPerEm; stb keeps the table's offset but not the field.
    const auto head = static_cast<size_t>(face->info.head);
    if (head != 0 && head + 20 <= face->bytes->size()) {
        const int upem = (data[head + 18] << 8) | data[head + 19];
        if (upem > 0) face->unitsPerEm = upem;
    }
    impl_->faces.push_back(std::move(face));
    // A face added later answers code points the earlier ones lacked, and a
    // miss may have been remembered before it arrived.
    impl_->glyphs.clear();
    impl_->kerns.clear();
    return true;
}

bool TrueTypeGlyphSource::empty() const { return impl_->faces.empty(); }

bool TrueTypeGlyphSource::glyph(uint32_t codepoint, GlyphUnits& out) const {
    const Impl::Entry& e = impl_->lookup(codepoint);
    if (e.face < 0) return false;
    out = e.units;
    return true;
}

int TrueTypeGlyphSource::kerning(uint32_t left, uint32_t right) const {
    const uint64_t key = (static_cast<uint64_t>(left) << 32) | right;
    const auto it = impl_->kerns.find(key);
    if (it != impl_->kerns.end()) return it->second;
    const Impl::Entry& a = impl_->lookup(left);
    const Impl::Entry& b = impl_->lookup(right);
    int kern = 0;
    // The 'kern' table only, which is all FT_Get_Kerning reads; stb's public
    // call would prefer GPOS where a face has both. Never across faces.
    if (a.face >= 0 && a.face == b.face) {
        kern = stbtt__GetGlyphKernInfoAdvance(
            &impl_->faces[static_cast<size_t>(a.face)]->info, a.index, b.index);
    }
    impl_->kerns.emplace(key, kern);
    return kern;
}

} // namespace wowee::ui
