// m2_blend_order.hpp - the stock client's order for an M2 scene's blended
// elements (batches, ribbons, particles): CM2Scene 0x821A20 builds the keys,
// comparator 0x81EF30 sorts them. docs/re/m2-runtime.md #25.
#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {

/// A batch's sort distance, squared, from `v`, its sort point (sortCenter
/// through the centre bone) less the camera position. Batch flag 0x1 pulls
/// the point `radius` (sortRadius times the bone's scale) towards the
/// camera, 0x2 pushes it away; with either, a point that ends up behind the
/// camera is negative, so it sorts last. Without them it is |v|^2.
inline float m2BatchSortDistanceSq(glm::vec3 v, float radius, uint8_t batchFlags,
                                   const glm::vec3& camForward) {
    float d = glm::dot(v, v);
    if ((batchFlags & 0x3) == 0 || radius == 0.0f || d <= 1e-12f) return d;
    const glm::vec3 n = v * (1.0f / std::sqrt(d));
    v += (batchFlags & 0x1) ? -radius * n : radius * n;
    d = glm::dot(v, v);
    return glm::dot(v, camForward) < 0.0f ? -d : d;
}

/// 0x81EF30 for elements with primary, secondary, priority, instance, type,
/// layer and item members: primary distance far first, priorityPlane
/// ascending, secondary distance far first, then the model, the element
/// type (batches 0, ribbons 3, particles 4) and materialLayer ascending.
/// `item` keeps the rest stable (stock falls back to its state compare).
template <class E>
inline bool m2BlendOrderLess(const E& a, const E& b) {
    if (a.primary != b.primary) return a.primary > b.primary;
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.secondary != b.secondary) return a.secondary > b.secondary;
    if (a.instance != b.instance) return a.instance < b.instance;
    if (a.type != b.type) return a.type < b.type;
    if (a.layer != b.layer) return a.layer < b.layer;
    return a.item < b.item;
}

}  // namespace rendering
}  // namespace wowee
