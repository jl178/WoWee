#pragma once

/// Wowee's own enhanced lighting table: 3.3.5's Light.dbc re-graded per zone
/// class and time of day by the parameters in
/// assets/enhanced_lighting_params.json (docs/re/enhanced-lighting.md
/// section 2f). Built at start-up from the client's own DBCs, in the same
/// layout as a local table (enhanced_lighting_table.hpp), so the "Enhanced
/// daylight" row reads either the same way. Nothing here comes from another
/// client: the parameters are a few numbers per class and band, fitted by
/// tools/enhanced_lighting_build.py (which mirrors this recipe).
///
/// Per 3.3.5 light on maps 0 and 1 and per weather slot (clear, storm):
///   - its class: the smallest WorldMapArea box on its map holding the
///     light's centre (outside every box, the nearest), that box's zone
///     looked up in the classes' zone lists; a map default light, or a zone
///     no class lists, is the first class ("default");
///   - its keys: every time 3.3.5's bands for the set have a key, plus the
///     band times, at most 16 (the key nearest the one before it goes first,
///     never a band time);
///   - at each key, 3.3.5's colour there re-graded:
///       out = level*Y + saturation*(rgb - Y) + Y*(warmth*W + tint*G)
///     Y the luminance, W orange-vs-blue and G green-vs-magenta, both of no
///     luminance, the parameters eased linearly between the band times; the
///     sun sprite moved toward white; the sky channels carry the table's sky
///     exposure (kSkyExposure) as a local table's do;
///   - the fog end scaled and the fog start fraction offset; the class's air.
/// No renderer includes.

#include "rendering/enhanced_lighting_table.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace rendering {
namespace enhanced_lighting_emulation {

inline constexpr const char* kParamsFile = "assets/enhanced_lighting_params.json";

/// level, saturation, warmth, tint; the identity is {1, 1, 0, 0}.
struct Grade {
    float level = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f;
    float tint = 0.0f;
};

/// One class's parameters at one band time.
struct ClassBand {
    Grade sun, ambient, sky, fogColor;
    float fogEndScale = 1.0f;
    float fogStartOffset = 0.0f;
};

struct ZoneClass {
    std::string name;
    std::vector<uint32_t> zones;   ///< AreaTable zone ids
    std::vector<ClassBand> bands;  ///< one per Params::bandHours
    float airDensity = 0.0f;
};

struct Params {
    std::vector<float> bandHours;
    std::vector<ZoneClass> classes;  ///< the first is the default
    std::vector<float> spriteWhite;  ///< per band: the sun sprite toward white
    std::vector<Grade> clouds;       ///< per band: CloudSun and the first cloud layer
};

/// Reads the parameter file (JSON; `//` comments allowed). False, with
/// `error` set, if it is not one or its lists do not match its bands.
bool parseParams(const std::string& text, Params& out, std::string& error);

/// A 3.3.5 band as LightIntBand / LightFloatBand give it.
struct ColorBand {
    int count = 0;
    const uint16_t* times = nullptr;
    const uint32_t* packed = nullptr;  ///< 0x00RRGGBB
};
struct FloatBand {
    int count = 0;
    const uint16_t* times = nullptr;
    const float* values = nullptr;
};
/// A LightParams set's bands (the 18 colour and the first two float
/// channels); count 0 where 3.3.5 has none.
struct StockSet {
    std::array<ColorBand, enhanced_lighting_table::kColorChannels> colors{};
    std::array<FloatBand, 2> floats{};
};

struct Light {
    uint32_t id = 0;
    uint32_t mapId = 0;
    bool isMapDefault = false;
    float worldX = 0.0f, worldY = 0.0f;
    uint32_t clearParams = 0, stormParams = 0;
};

/// A WorldMapArea row: the box bounds world Y by left (high) and right, and
/// world X by top (high) and bottom.
struct ZoneBox {
    uint32_t mapId = 0;
    uint32_t areaId = 0;
    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
};

uint32_t zoneOf(const Light& light, const std::vector<ZoneBox>& boxes);
/// Index into params.classes.
size_t classOf(const Light& light, const std::vector<ZoneBox>& boxes, const Params& params);

/// The colour after a grade (0-255 per channel, not clamped).
std::array<float, 3> applyGrade(const std::array<float, 3>& rgb, const Grade& g);

/// The table for every light on maps 0 and 1 whose set `stockSet` knows.
/// `stockSet(paramsId, out)` fills `out` and returns false for an unknown set.
enhanced_lighting_table::Table build(const Params& params, const std::vector<Light>& lights,
                                     const std::vector<ZoneBox>& boxes,
                                     const std::function<bool(uint32_t, StockSet&)>& stockSet);

/// The table as WFL2 bytes (enhanced_lighting_table::parse reads them back).
std::vector<uint8_t> serialize(const enhanced_lighting_table::Table& table);

}  // namespace enhanced_lighting_emulation
}  // namespace rendering
}  // namespace wowee
