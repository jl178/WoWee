#pragma once

// The stock 3.3.5a character skin composite (CCharacterComponent, build 12340)
// for the parts that are pure pixel arithmetic. See docs/re/characters.md.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {
namespace composite {

/// An item texture authored for the 256 composite drawn on a 512 one is grown
/// 2x with neighbour averaging (0x4E89F0, 0x4EC690, 0x4ECC20, 0x4ED200), not
/// nearest: for destination (x, y) and source (w, h)
///   even x, even y: src[y/2][x/2]
///   odd x:  (src[r][x/2] + src[r][min(x/2+1, w-1)]) * 128 >> 8
///   odd y:  the same between rows y/2 and min(y/2+1, h-1)
///   both:   the four-tap sum * 64 >> 8
/// on every channel, alpha included. RGBA8 in, RGBA8 (2w x 2h) out.
inline std::vector<uint8_t> stockUpscale2x(const uint8_t* src, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 16);
    if (w <= 0 || h <= 0) return out;
    auto at = [&](int x, int y, int c) -> unsigned { return src[(static_cast<size_t>(y) * w + x) * 4 + c]; };
    const int W = w * 2, H = h * 2;
    for (int y = 0; y < H; ++y) {
        const int r0 = y / 2, r1 = std::min(r0 + 1, h - 1);
        for (int x = 0; x < W; ++x) {
            const int c0 = x / 2, c1 = std::min(c0 + 1, w - 1);
            uint8_t* d = &out[(static_cast<size_t>(y) * W + x) * 4];
            for (int c = 0; c < 4; ++c) {
                unsigned v;
                if (!(x & 1) && !(y & 1)) v = at(c0, r0, c);
                else if ((x & 1) && !(y & 1)) v = ((at(c0, r0, c) + at(c1, r0, c)) * 128u) >> 8;
                else if (!(x & 1)) v = ((at(c0, r0, c) + at(c0, r1, c)) * 128u) >> 8;
                else v = ((at(c0, r0, c) + at(c1, r0, c) + at(c0, r1, c) + at(c1, r1, c)) * 64u) >> 8;
                d[c] = static_cast<uint8_t>(v);
            }
        }
    }
    return out;
}

}  // namespace composite
}  // namespace rendering
}  // namespace wowee
