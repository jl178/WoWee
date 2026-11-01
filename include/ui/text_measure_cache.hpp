// text_measure_cache.hpp - Remembered measurements of a label's text.
//
// Sizing a FontString measures its text with the face it draws in: one pass
// over the glyphs for the width, and a markup parse and a wrap for the number
// of lines. The answer depends on nothing but the face, the point size, the
// text and how it wraps, so the same question asked again - a timer going back
// to "1:00", forty action buttons labelled with the same hotkey digits, every
// label in the interface on a reload - can be answered from here.
//
// Free of ImGui so it can be tested without a font: the face is only an
// identity here, and the renderer does the measuring on a miss.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace wowee::ui {

/// Everything a measurement reads. Two keys are equal only when a measurement
/// of one would give the other's answer bit for bit, so the floats compare by
/// their bits: a NaN width still finds itself, and nothing that differs in the
/// last place is taken for something it is not.
struct TextMeasureKey {
    const void* face = nullptr;   ///< the ImFont the text is measured with
    float size = 0.0f;            ///< point size
    float wrapWidth = 0.0f;       ///< the box a paragraph wraps in; 0 for a label
    bool paragraph = false;       ///< wraps inside wrapWidth rather than sizing itself
    bool nonSpaceWrap = false;
    std::string text;

    bool operator==(const TextMeasureKey& o) const {
        return face == o.face &&
               std::bit_cast<uint32_t>(size) == std::bit_cast<uint32_t>(o.size) &&
               std::bit_cast<uint32_t>(wrapWidth) == std::bit_cast<uint32_t>(o.wrapWidth) &&
               paragraph == o.paragraph && nonSpaceWrap == o.nonSpaceWrap &&
               text == o.text;
    }
};

struct TextMeasureKeyHash {
    size_t operator()(const TextMeasureKey& k) const {
        size_t h = std::hash<std::string>()(k.text);
        const auto mix = [&h](size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        };
        mix(std::hash<const void*>()(k.face));
        mix(std::bit_cast<uint32_t>(k.size));
        mix(std::bit_cast<uint32_t>(k.wrapWidth));
        mix((k.paragraph ? 1u : 0u) | (k.nonSpaceWrap ? 2u : 0u));
        return h;
    }
};

/// What sizing takes from a measurement: the width of the text on one line
/// (unused for a paragraph, whose width is declared) and how many lines it
/// comes to.
struct TextMeasure {
    float width = 0.0f;
    int rows = 0;
};

/// A bounded map of measurements. Full means emptied, not evicted one at a
/// time: the working set of an interface is a few thousand strings, the bound
/// is well above that, and a cache that never answers wrongly does not need to
/// be clever about which answers it keeps.
class TextMeasureCache {
public:
    explicit TextMeasureCache(size_t capacity = 8192) : capacity_(capacity) {}

    /// The remembered answer, or null.
    [[nodiscard]] const TextMeasure* find(const TextMeasureKey& key) const {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }

    void insert(const TextMeasureKey& key, const TextMeasure& value) {
        if (map_.size() >= capacity_) map_.clear();
        map_.insert_or_assign(key, value);
    }

    /// Forget everything. For when a face may have been rebuilt at the same
    /// address, which would make every key that names it a lie.
    void clear() { map_.clear(); }

    [[nodiscard]] size_t size() const { return map_.size(); }
    [[nodiscard]] size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    std::unordered_map<TextMeasureKey, TextMeasure, TextMeasureKeyHash> map_;
};

} // namespace wowee::ui
