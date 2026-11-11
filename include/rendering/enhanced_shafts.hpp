#pragma once

// Enhanced's canopy shafts: beams of sunlit (or moonlit) air from the gaps in
// a canopy down to the ground, bright against the shaded air beside them
// (docs/re/enhanced-lighting.md section 5c). The froxel mist ("Light shafts")
// already reads the shadow map once per cell; under Enhanced's light a lit
// cell with shade round it scatters this much more (VolumetricFog::
// FrameInputs::shaftStrength, volumetric_fog_inject.comp.glsl). Nothing
// changes in the open, and with Enhanced's light off the mist is stock.

#include <cstdlib>

namespace wowee {
namespace rendering {
namespace enhanced_shafts {

/// How much more a sunlit cell beside shade scatters, against the mist's own
/// sun scatter (0.8). 9 laid a gold veil over Goldshire's crowns facing the
/// 9:22 sun (top third saturation 0.46 against Enhanced's 0.36); at 3.5, with
/// the beams paler by day, it is 0.38 and the 15:23 gold still Enhanced's.
inline constexpr float kStrength = 3.5f;

/// The moonlight's shafts against the sun's, before its own (dim, blue)
/// colour: the night light is a fraction of the day's. Strength times this
/// is 17.5, about the moonbeams' strength from the start (18).
inline constexpr float kNightScale = 5.0f;

/// How far round a cell, on a level ring, it looks for shade beside it:
/// about a gap in the leaves.
inline constexpr float kProbeYards = 2.5f;

/// Full to this many yards from the camera, gone at twice it: the shafts a
/// player sees are the ones in the trees round them.
inline constexpr float kReachYards = 60.0f;

struct Tuning {
    float strength = kStrength;
    float nightScale = kNightScale;
    float probeYards = kProbeYards;
    float reachYards = kReachYards;
};

/// The constants, or WOWEE_ENHANCED_SHAFTS=strength[,night,probe,reach]
/// (read once; a strength of 0 turns the shafts off).
inline const Tuning& tuning() {
    static const Tuning t = [] {
        Tuning r;
        const char* e = std::getenv("WOWEE_ENHANCED_SHAFTS");
        if (e && *e) {
            float v[4] = {r.strength, r.nightScale, r.probeYards, r.reachYards};
            const char* p = e;
            for (int i = 0; i < 4 && *p; ++i) {
                char* end = nullptr;
                const float x = std::strtof(p, &end);
                if (end == p) break;
                v[i] = x;
                p = (*end == ',') ? end + 1 : end;
            }
            r = {v[0], v[1], v[2], v[3]};
        }
        return r;
    }();
    return t;
}

}  // namespace enhanced_shafts
}  // namespace rendering
}  // namespace wowee
