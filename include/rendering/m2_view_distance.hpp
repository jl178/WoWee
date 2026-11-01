#pragma once

#include <algorithm>

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

} // namespace rendering
} // namespace wowee
