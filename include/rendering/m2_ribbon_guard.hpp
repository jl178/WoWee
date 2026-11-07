// m2_ribbon_guard.hpp - the limits that keep one ribbon or effect-strip edge
// from spanning the map.
//
// A trail joins each new edge to the last one (SetMatrix 0x97F940), so one
// bad emitter position - a missile created before its launch point was
// known, a bone palette not yet filled (all zeros, which puts the emitter at
// the world origin), a carrier moved or reused without its trail reset -
// draws a strip from wherever it had been to wherever it is now. With many
// casters at once that is a fan of textured lines across the map. Stock has
// no such inputs; these turn each into a break in the trail instead.
#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace wowee {
namespace rendering {

/// The farthest one ribbon emitter may move between two updates and stay one
/// trail, in yards. A missile at 60 yd/s over a quarter-second hitch is 15.
constexpr float kRibbonMaxJump = 40.0f;
/// The farthest two neighbouring edges of a drawn strip may lie apart.
constexpr float kRibbonMaxEdgeSpan = 40.0f;

inline bool ribbonPointFinite(const glm::vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

/// An emitter frame that cannot be a real placement: a non-finite position,
/// or a degenerate basis (a bone palette entry still zero).
inline bool ribbonFrameDegenerate(const glm::mat4& xf) {
    const glm::vec3 pos(xf[3]);
    if (!ribbonPointFinite(pos)) return true;
    const float y = glm::dot(glm::vec3(xf[1]), glm::vec3(xf[1]));
    const float z = glm::dot(glm::vec3(xf[2]), glm::vec3(xf[2]));
    return !(y > 1e-10f) || !(z > 1e-10f) || !std::isfinite(y) || !std::isfinite(z);
}

/// Whether the emitter moved too far since its last update to be the same trail.
inline bool ribbonJumped(const glm::vec3& from, const glm::vec3& to, float maxJump = kRibbonMaxJump) {
    const glm::vec3 d = to - from;
    return !ribbonPointFinite(to) || glm::dot(d, d) > maxJump * maxJump;
}

/// The runs of a strip's points that may be drawn joined: [begin, end) index
/// ranges, split wherever a point is not finite (dropped) or lies farther
/// than maxSpan from the one before. Runs shorter than minRun are left out.
inline std::vector<std::pair<size_t, size_t>> ribbonStripRuns(const std::vector<glm::vec3>& points,
                                                             size_t minRun = 2,
                                                             float maxSpan = kRibbonMaxEdgeSpan) {
    std::vector<std::pair<size_t, size_t>> runs;
    size_t begin = 0;
    const auto close = [&](size_t end) {
        if (end > begin && end - begin >= minRun) runs.emplace_back(begin, end);
    };
    for (size_t i = 0; i < points.size(); ++i) {
        if (!ribbonPointFinite(points[i])) {
            close(i);
            begin = i + 1;
            continue;
        }
        if (i > begin) {
            const glm::vec3 d = points[i] - points[i - 1];
            if (glm::dot(d, d) > maxSpan * maxSpan) {
                close(i);
                begin = i;
            }
        }
    }
    close(points.size());
    return runs;
}

}  // namespace rendering
}  // namespace wowee
