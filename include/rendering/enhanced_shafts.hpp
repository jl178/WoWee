#pragma once

// The reference client's canopy shafts: beams of sunlit (or moonlit) air from the gaps in
// a canopy down to the ground, bright against the shaded air beside them
// (docs/re/enhanced-lighting.md section 5c). The froxel mist ("Light shafts")
// already reads the shadow map once per cell; under the enhanced light a lit
// cell with shade round it scatters this much more (VolumetricFog::
// FrameInputs::shaftStrength, volumetric_fog_inject.comp.glsl). Nothing
// changes in the open, and with the reference client's light off the mist is stock.

#include <cstdlib>

namespace wowee {
namespace rendering {
namespace enhanced_shafts {

/// How much more a sunlit cell beside shade scatters, against the mist's own
/// sun scatter (0.8). 9 laid a gold veil over Goldshire's crowns facing the
/// 9:22 sun (top third saturation 0.46 against the reference client's 0.36); at 3.5, with
/// the beams paler by day, it is 0.38 and the 15:23 gold still the reference client's.
inline constexpr float kStrength = 6.0f;

/// The moonlight's shafts against the sun's, before its own (dim, blue)
/// colour: the night light is a fraction of the day's. Strength times this
/// is 17.5, about the moonbeams' strength from the start (18).
inline constexpr float kNightScale = 4.0f;

/// How far round a cell, on a level ring, it looks for shade beside it:
/// about a gap in the leaves.
inline constexpr float kProbeYards = 2.5f;

/// Full to this many yards from the camera, gone at twice it: the shafts a
/// player sees are the ones in the trees round them.
inline constexpr float kReachYards = 60.0f;

/// The Light shaft strength row (Enhancements::lightShaftStrength): Subtle,
/// Normal, Strong.
inline float strengthScale(int setting) {
    return setting <= 0 ? 0.5f : (setting == 1 ? 1.0f : 1.8f);
}

/// The same row on the screen-space sun shafts (SunShafts): the streaks from
/// the sun's place through the gaps in what stands before it.
inline float screenScale(int setting) {
    return setting <= 0 ? 0.6f : (setting == 1 ? 1.0f : 1.4f);
}

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

/// The shafts as lines, not a glow (Jered on 046a047: "moonlight feels
/// hazy, not like strong lines"). By night the air keeps this share of its
/// sky light, so the beams stand on dark air; a beam is held to a thin one
/// (half or more of the inner ring shaded) by night and, this far, by day;
/// and the beams that are left take this gain as far as they are held.
/// WOWEE_SHAFT_LINES=nightSkyAir[,nightHold,dayHold,gain]; 1,0,0,1 is the
/// mist before (046a047).
struct Lines {
    float nightSkyAir = 0.25f;
    float nightHold = 1.0f;
    float dayHold = 0.6f;
    float gain = 2.2f;
};

inline const Lines& lines() {
    static const Lines l = [] {
        Lines r;
        const char* e = std::getenv("WOWEE_SHAFT_LINES");
        if (e && *e) {
            float v[4] = {r.nightSkyAir, r.nightHold, r.dayHold, r.gain};
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
    return l;
}

/// The moonbeams on the screen (SunShafts toward the moon, Renderer::
/// recordSunShafts): strength before the Light shaft strength row, and where
/// the frame starts to and fully counts as sky by the moon (its brightness),
/// and how far from it, in screen heights, the sources fade out. The night
/// sky by the moon is about a fifth of the day's, so a source starts low;
/// the sky away from it is darker still and stays out.
/// A source must also be overSky times the frame's brightness round the
/// moon. WOWEE_MOON_SHAFTS=strength[,low,high,radius,overSky] (0 turns them
/// off).
struct MoonRays {
    float strength = 3.0f;
    float sourceLow = 0.08f;
    float sourceHigh = 0.30f;
    float sourceRadius = 0.35f;
    float overSky = 1.3f;
};

inline const MoonRays& moonRays() {
    static const MoonRays m = [] {
        MoonRays r;
        const char* e = std::getenv("WOWEE_MOON_SHAFTS");
        if (e && *e) {
            float v[5] = {r.strength, r.sourceLow, r.sourceHigh, r.sourceRadius, r.overSky};
            const char* p = e;
            for (int i = 0; i < 5 && *p; ++i) {
                char* end = nullptr;
                const float x = std::strtof(p, &end);
                if (end == p) break;
                v[i] = x;
                p = (*end == ',') ? end + 1 : end;
            }
            r = {v[0], v[1], v[2], v[3], v[4]};
        }
        return r;
    }();
    return m;
}

}  // namespace enhanced_shafts
}  // namespace rendering
}  // namespace wowee
