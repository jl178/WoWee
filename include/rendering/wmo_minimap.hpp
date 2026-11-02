// wmo_minimap.hpp - the indoor minimap: which of a WMO's own minimap tiles
// to draw, and where each one lands.
//
// Inside an indoor WMO group (MOGP flag 0x2000) the stock minimap stops
// drawing the terrain composite and draws the building's own tiles instead:
// Ironforge's halls, the Undercity's rings, an inn's rooms, a cave. They live
// beside the terrain tiles in Textures\Minimap, named through md5translate.trs
// by the root's path less "World\" and its extension, then the group's index
// in the root file and the tile's column and row:
//
//     WMO\KhazModan\Cities\Ironforge\ironforge_044_00_01.blp
//
// Each tile covers 128 yards of the group's own space at 2 pixels a yard,
// counted from the MOGP header's bounding box minimum, and is cropped to the
// power of two that holds what is left of the group: Ironforge's group 44,
// 51 x 201 yards, is two 128 x 256 tiles stacked along y.
//
// The image's columns run along +x. Its rows run along -y: row 0 is the
// tile's high-y edge and the last row its low-y edge, so a tile cropped short
// in y keeps its low-y side. That was measured, not assumed - see
// tests/test_wmo_minimap.cpp, where the reasoning and the numbers are kept.
#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace rendering {
namespace wmo_minimap {

inline constexpr float kTileYards = 128.0f;
inline constexpr float kPixelsPerYard = 2.0f;
/// MOGP flag: the group is indoors, which is what puts the minimap into its
/// WMO mode.
inline constexpr uint32_t kGroupIndoor = 0x2000;

/// The tile key prefix for a root WMO path: lower case, backslashes, "World\"
/// and ".wmo" taken off. "World\wmo\KhazModan\Cities\Ironforge\Ironforge.wmo"
/// gives "wmo\khazmodan\cities\ironforge\ironforge". The trs keys are matched
/// in lower case because the two files disagree on it (MWMO says
/// "Ironforge.wmo", md5translate "ironforge_001_00_00.blp").
inline std::string tileRoot(std::string_view rootPath) {
    std::string s(rootPath);
    for (char& c : s) {
        c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    constexpr std::string_view kWorld = "world\\";
    if (s.compare(0, kWorld.size(), kWorld) == 0) s.erase(0, kWorld.size());
    constexpr std::string_view kExt = ".wmo";
    if (s.size() > kExt.size() && s.compare(s.size() - kExt.size(), kExt.size(), kExt) == 0) {
        s.resize(s.size() - kExt.size());
    }
    return s;
}

/// The md5translate key for one tile, without ".blp": root, then the group's
/// index in the root file in three digits and the column and row in two.
inline std::string tileKey(const std::string& root, uint32_t groupIndex, int x, int y) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%03u_%02d_%02d", groupIndex, x, y);
    return root + suffix;
}

/// How many tiles a group's box needs along one axis: 128 yards each, at
/// least one. Ironforge has 185.6 and 128.4 yards at two, 107.7 at one.
inline int tileCount(float extentYards) {
    return std::max(1, static_cast<int>(std::ceil(extentYards / kTileYards - 1e-4f)));
}

/// Where one tile's image corners are, in the WMO's local x/y. uv00 is image
/// (0,0) - the top-left texel corner - uv10 is (1,0), uv01 (0,1), uv11 (1,1).
/// A tile w x h pixels across covers w/2 x h/2 yards.
struct TileCorners {
    glm::vec2 uv00, uv10, uv01, uv11;
};
inline TileCorners tileCorners(const glm::vec2& groupBoxMin, int x, int y,
                               uint32_t widthPx, uint32_t heightPx) {
    const float x0 = groupBoxMin.x + static_cast<float>(x) * kTileYards;
    const float y0 = groupBoxMin.y + static_cast<float>(y) * kTileYards;
    const float x1 = x0 + static_cast<float>(widthPx) / kPixelsPerYard;
    const float y1 = y0 + static_cast<float>(heightPx) / kPixelsPerYard;
    // Rows run toward -y: the top row is the tile's high-y edge.
    return {{x0, y1}, {x1, y1}, {x0, y0}, {x1, y0}};
}

/// One group of the WMO the player is in, as the minimap needs it.
struct Group {
    uint32_t fileIndex = 0;   ///< index in the root file, the tile key's number
    glm::vec3 boxMin{0.0f};   ///< MOGP header bounds, local space
    glm::vec3 boxMax{0.0f};
    uint32_t flags = 0;       ///< MOGP flags
};

/// The groups to draw, in draw order: the indoor groups whose height range
/// holds the player's (local z, a yard above the feet), lowest first, and the
/// group the player is in last so it is never covered by another floor's.
/// `current` indexes `groups`; out of range draws the level only.
inline std::vector<size_t> groupsToDraw(const std::vector<Group>& groups, size_t current,
                                        float playerLocalZ) {
    std::vector<size_t> out;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i == current) continue;
        const Group& g = groups[i];
        if (!(g.flags & kGroupIndoor)) continue;
        if (playerLocalZ < g.boxMin.z || playerLocalZ > g.boxMax.z) continue;
        out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        return groups[a].boxMin.z < groups[b].boxMin.z;
    });
    if (current < groups.size()) out.push_back(current);
    return out;
}

/// A render-space point (x north, y west) in the minimap composite's 0..1
/// space, for a composite `span` yards across centred on `center`. Matches the
/// terrain composite and the display shader: u grows southward (-x), v
/// eastward (-y), and the display reads the map with the screen's right as +v
/// and down as +u.
inline glm::vec2 renderToComposite(const glm::vec2& renderXY, const glm::vec2& center, float span) {
    return {0.5f + (center.x - renderXY.x) / span, 0.5f + (center.y - renderXY.y) / span};
}

}  // namespace wmo_minimap
}  // namespace rendering
}  // namespace wowee
