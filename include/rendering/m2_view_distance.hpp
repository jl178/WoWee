#pragma once

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

/// The distance at which one M2 instance stops drawing, squared.
///
/// Three separate inputs each push an instance further out than the scene's
/// base distance: a large model gets a factor above 1 so a cathedral does not
/// pop before a shrub does, an animation-disabled model gets a further 2.6x,
/// and a server game object gets an outright floor so a mailbox stays findable.
/// All three are biases within the drawn world, not licence to leave it. The
/// terrain and the WMOs stop at the view distance the player asked for, so an
/// instance past that ceiling is a tree standing on nothing - which is why the
/// clamp comes last, after every widening rather than before it.
inline float m2InstanceMaxDistSq(float baseMaxDistSq,
                                 float perInstanceFactor,
                                 bool isGameObject,
                                 float gameObjectMinDistance,
                                 float viewDistanceAbsolute,
                                 bool isGroundDetail = false,
                                 float groundDetailMaxDistance = 0.0f) {
    float maxDistSq = baseMaxDistSq * perInstanceFactor;
    if (isGameObject) {
        maxDistSq = std::max(maxDistSq, gameObjectMinDistance * gameObjectMinDistance);
    }
    // Grass and the rest of the ground cover stop far closer than anything
    // else, and are their own setting for it: the original client draws them
    // between 70 and 140 yards while doodads go out to the horizon. A ceiling
    // rather than a scale, because it is a distance the player names outright.
    if (isGroundDetail && groundDetailMaxDistance > 0.0f) {
        maxDistSq = std::min(maxDistSq, groundDetailMaxDistance * groundDetailMaxDistance);
    }
    return std::min(maxDistSq, viewDistanceAbsolute * viewDistanceAbsolute);
}

/// How far one instance's particles are worth stepping and drawing, squared.
///
/// A flame or a wisp of smoke is a few pixels wide well before the model it
/// sits on stops drawing, and the instance distance above runs out to the
/// view distance. Stepping every torch and vent out there filled the particle
/// budget in Goldshire with Burning Steppes vents and Stormwind braziers -
/// 40k live particles, none of them visible. The limit follows the size on
/// screen instead: about 120 radii, where a model is still a dozen pixels
/// tall at 1080p and the stock 54 degree field of view, and never closer than
/// a nearby campfire needs.
constexpr float kParticleRadiiOfRange = 120.0f;
constexpr float kParticleMinRange = 150.0f;

inline float m2ParticleMaxDistSq(float instanceMaxDistSq, float worldRadius) {
    const float range = std::max(kParticleMinRange, kParticleRadiiOfRange * worldRadius);
    return std::min(instanceMaxDistSq, range * range);
}

/// The stock client's map doodad distance fade (CMapDoodadDef, 0x791CB0;
/// docs/re/m2-runtime.md §4). Five size classes by the doodad's largest
/// world-space bounding-box extent (0x7BDB10, thresholds 0xADF350):
/// up to 1, 4, 15 and 100 yards, and larger. Each class has an end distance
/// (0xADF364) and a fade length (0xADF38C); 0x78F570 scales the middle three
/// ends by the environmentDetail CVar (0.5..1.5, default 1). Past the end the
/// doodad is not drawn; over the last fade length its alpha falls linearly;
/// above 0.99 it is 1 and below 0.01 it is not drawn.
constexpr int kStockDoodadClasses = 5;
constexpr float kStockDoodadSizeLimit[4] = {1.0f, 4.0f, 15.0f, 100.0f};
constexpr float kStockDoodadEnd[kStockDoodadClasses] = {30.0f, 100.0f, 200.0f, 750.0f, 1250.0f};
constexpr float kStockDoodadFadeLength[kStockDoodadClasses] = {5.0f, 10.0f, 15.0f, 20.0f, 50.0f};

inline int stockDoodadSizeClass(float maxExtent) {
    for (int i = 0; i < 4; ++i)
        if (maxExtent <= kStockDoodadSizeLimit[i]) return i;
    return 4;
}

/// A class's end distance. `detail` is environmentDetail, clamped as stock
/// clamps it, on the middle three classes. `viewScale` is Wowee's view
/// distance over its 1200-yard default, on every class but the smallest: at
/// the default it is 1 and the table is stock's; the slider still widens the
/// doodads with the terrain.
inline float stockDoodadEndDistance(int sizeClass, float detail, float viewScale) {
    const int c = std::clamp(sizeClass, 0, kStockDoodadClasses - 1);
    float end = kStockDoodadEnd[c];
    if (c >= 1 && c <= 3) end *= std::clamp(detail, 0.5f, 1.5f);
    if (c >= 1) end *= std::max(viewScale, 0.0f);
    return end;
}

/// Alpha at `distance` for a class whose end is `end`: 1 inside the fade,
/// 0 when not drawn.
inline float stockDoodadFadeAlpha(int sizeClass, float end, float distance) {
    const int c = std::clamp(sizeClass, 0, kStockDoodadClasses - 1);
    if (distance > end) return 0.0f;
    const float fadeLen = kStockDoodadFadeLength[c];
    const float start = end - fadeLen;
    if (distance <= start) return 1.0f;
    const float a = 1.0f - (distance - start) / fadeLen;
    if (a > 0.99f) return 1.0f;
    if (a < 0.01f) return 0.0f;
    return a;
}

} // namespace rendering
} // namespace wowee
