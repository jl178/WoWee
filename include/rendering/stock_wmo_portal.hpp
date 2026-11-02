#pragma once

/// Which groups of a WMO the stock 3.3.5a client (12340) draws, and through
/// which part of the screen: the portal walk and the doodad light. Pure
/// functions; docs/re/wmo-rendering.md §5, §6 and §10 have the walk-through
/// and the addresses.
///
/// - Camera outside an instance (0x7b3a10): every group whose box is in the
///   frustum is drawn at once when it has MOGP 0x10000; an exterior group
///   (0x8) starts a walk (0x7ad350 -> 0x7ac060) with the whole screen.
///   Interior groups are reached only through portals.
/// - Camera inside (0x795d40 finds the group under the camera with a ray
///   1760 yards down; a hit on an exterior group means "outside"): the walk
///   (0x7ad1f0) starts from that group with the whole screen, then the
///   0x10000 groups in the frustum are drawn too.
/// - The walk (0x7ac060), depth at most 10 (0xd1bee4 = 0xadfe40): skips a
///   group with 0x10000, draws the group, then for each MOPR ref of it that
///   does not lead back where the walk came from, whose portal faces the
///   camera (n.c + d, negated for a negative ref side, >= 0) and whose
///   projected rectangle (0x7a9090) overlaps the current one, clips the
///   rectangle and goes on into the group behind - except:
///     camera outside: not into a group with 0x10008;
///     camera inside: a group with any of 0x50148 opens the outdoor view
///       through that portal (0x7a8f20), and the walk goes on into it only
///       when it has neither 0x8 nor 0x10000.
/// - With the camera inside, a group reached with 0x40000 sets the MOSB sky.
/// - The portal's rectangle (0x7a9090): the whole screen when the camera is
///   within 0.01 of the portal plane and over the polygon (0x7a7210); else
///   the bounds of the polygon clipped (0x7a72a0) and projected (0x7a85e0).
/// - The clip keeps one axis' maximum unclamped (0x7ac2f6..0x7ac366 compare
///   the maximum of the other axis twice); that is reproduced.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace wowee::rendering::stock_wmo_portal {

/// MOGP flags the walk reads.
inline constexpr uint32_t kGroupExterior = 0x8;
inline constexpr uint32_t kGroupExteriorLit = 0x40;
inline constexpr uint32_t kGroupAlwaysDraw = 0x10000;   ///< drawn whenever in the frustum
inline constexpr uint32_t kGroupShowSkybox = 0x40000;    ///< the root's MOSB, camera inside
inline constexpr uint32_t kGroupNoWalkOutside = 0x10008; ///< not walked into, camera outside
inline constexpr uint32_t kGroupOpensOutdoors = 0x50148; ///< 0x7a8f20 from inside
/// Groups 0x795d40 ignores when it looks for the group under the camera.
inline constexpr uint32_t kGroupNotUnderCamera = 0x410080;
/// The seed groups' flags that open the outdoor view at once (0x795d40).
inline constexpr uint32_t kGroupSeedOpensOutdoors = 0x40140;
/// 0xadfe40, copied into 0xd1bee4 at start-up.
inline constexpr uint32_t kMaxPortalDepth = 10;
/// 0x9f1968 / 0xa104b4: how near the portal plane the camera counts as in it.
inline constexpr float kPortalPlaneEpsilon = 0.01f;
/// 0x9e1134: a clipped rectangle thinner than this is dropped (0x482870).
inline constexpr float kRectEpsilon = 0.001f;
/// 0xa3e6e8: how far down 0x795d40 looks for the camera's group.
inline constexpr float kCameraRayLength = 1760.0f;

/// A screen rectangle in normalised device coordinates, the way the client
/// keeps it: index 0/2 are one axis' min/max, 1/3 the other's. Here x is
/// 0/2 and y 1/3; only the clip below cares which is which.
struct Rect {
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = -std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();

    static Rect full() { return {-1.0f, -1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] bool empty() const { return !(minX <= maxX && minY <= maxY); }
    void unite(const Rect& o) {
        if (o.empty()) return;
        minX = std::min(minX, o.minX);
        minY = std::min(minY, o.minY);
        maxX = std::max(maxX, o.maxX);
        maxY = std::max(maxY, o.maxY);
    }
};

/// The overlap test of 0x7ac060: both axes, touching counts.
inline bool overlaps(const Rect& portal, const Rect& current) {
    return portal.minY <= current.maxY && current.minY <= portal.maxY &&
           portal.minX <= current.maxX && current.minX <= portal.maxX;
}

/// The clip of 0x7ac060 as the binary does it: minX, minY and maxY are
/// clamped to the current rectangle, maxX is not (the code compares maxY
/// twice). A rectangle thinner than 0.001 on either axis is dropped.
inline bool clipStock(const Rect& portal, const Rect& current, Rect& out) {
    out = portal;
    out.minY = std::max(out.minY, current.minY);
    out.maxY = std::min(out.maxY, current.maxY);
    out.minX = std::max(out.minX, current.minX);
    out.maxY = std::min(out.maxY, current.maxY);
    if (std::fabs(out.minY - out.maxY) < kRectEpsilon) return false;
    if (std::fabs(out.minX - out.maxX) < kRectEpsilon) return false;
    return true;
}

/// One MOPT portal: its polygon in MOPV and its plane, n.p + d = 0.
struct Portal {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float d = 0.0f;
};

/// One MOPR ref: the portal, the group on its far side, and the side of the
/// plane the owning group is on.
struct Ref {
    uint16_t portal = 0;
    uint16_t group = 0;
    int16_t side = 1;
};

/// A root's portal graph, by file group index.
struct Graph {
    std::vector<uint32_t> groupFlags;
    /// MOGP portalStart / portalCount, into refs.
    std::vector<std::pair<uint32_t, uint32_t>> groupRefs;
    std::vector<Ref> refs;
    std::vector<Portal> portals;
    std::vector<glm::vec3> vertices;
};

/// MOPT's plane as the root loader keeps it (0x7d7470): a NaN plane becomes
/// (0, 0, 1) with its distance.
inline Portal sanitisePortal(Portal p) {
    if (std::isnan(p.normal.x) || std::isnan(p.normal.y) || std::isnan(p.normal.z)) {
        p.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    if (std::isnan(p.d)) p.d = 0.0f;
    return p;
}

/// The signed distance the walk tests (0x7ac060): n.c + d, negated for a ref
/// whose side is negative. The portal is walked through when it is >= 0.
inline float facing(const Portal& p, int16_t side, const glm::vec3& cam) {
    const float dist = glm::dot(p.normal, cam) + p.d;
    return side < 0 ? -dist : dist;
}

/// Whether `pt`, on the portal plane, lies inside its (convex) polygon.
inline bool overPolygon(const Graph& g, const Portal& p, const glm::vec3& pt) {
    if (p.vertexCount < 3 || p.firstVertex + p.vertexCount > g.vertices.size()) return false;
    const glm::vec3* v = g.vertices.data() + p.firstVertex;
    float sign = 0.0f;
    for (uint32_t i = 0; i < p.vertexCount; ++i) {
        const glm::vec3& a = v[i];
        const glm::vec3& b = v[(i + 1) % p.vertexCount];
        const float s = glm::dot(glm::cross(b - a, pt - a), p.normal);
        if (s == 0.0f) continue;
        if (sign == 0.0f) sign = s;
        else if ((s > 0.0f) != (sign > 0.0f)) return false;
    }
    return true;
}

/// The portal's screen rectangle (0x7a9090). `localToClip` takes the
/// model's own coordinates to clip space. The polygon is clipped against
/// w >= kNearW (0x7a72a0) and its points projected; the rectangle is their
/// bounds, unclamped. Full screen when the camera stands in the portal.
inline Rect projectPortal(const Graph& g, const Portal& p, const glm::mat4& localToClip,
                          const glm::vec3& camLocal) {
    const float planeDist = glm::dot(p.normal, camLocal) + p.d;
    if (planeDist > -kPortalPlaneEpsilon && planeDist < kPortalPlaneEpsilon) {
        const glm::vec3 onPlane = camLocal - p.normal * planeDist;
        if (overPolygon(g, p, onPlane)) return Rect::full();
    }
    Rect r;
    if (p.vertexCount < 3 || p.firstVertex + p.vertexCount > g.vertices.size()) return r;
    constexpr float kNearW = 1e-4f;  // 0x9e8cd0, the floor of w at 0x7a85e0
    // Clip the polygon to w >= kNearW (Sutherland-Hodgman, one plane).
    std::array<glm::vec4, 32> in{};
    const uint32_t n = std::min<uint32_t>(p.vertexCount, 12);  // 0x7a85e0 caps at 12
    for (uint32_t i = 0; i < n; ++i) {
        in[i] = localToClip * glm::vec4(g.vertices[p.firstVertex + i], 1.0f);
    }
    std::array<glm::vec4, 32> out{};
    uint32_t outCount = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec4& a = in[i];
        const glm::vec4& b = in[(i + 1) % n];
        const bool aIn = a.w >= kNearW;
        const bool bIn = b.w >= kNearW;
        if (aIn) out[outCount++] = a;
        if (aIn != bIn) {
            const float t = (kNearW - a.w) / (b.w - a.w);
            out[outCount++] = a + (b - a) * t;
        }
    }
    if (outCount < 3) return r;
    for (uint32_t i = 0; i < outCount; ++i) {
        const float w = std::max(out[i].w, kNearW);
        const float x = out[i].x / w;
        const float y = out[i].y / w;
        r.minX = std::min(r.minX, x);
        r.maxX = std::max(r.maxX, x);
        r.minY = std::min(r.minY, y);
        r.maxY = std::max(r.maxY, y);
    }
    return r;
}

/// What a walk found.
struct WalkResult {
    /// Per group: drawn this frame, and the union of the rectangles it was
    /// reached with (the batches and doodads are culled to it).
    std::vector<uint8_t> drawn;
    std::vector<Rect> rects;
    /// The union of the portals that opened the outdoor view (0x7a8f20,
    /// and 0x795d40 for a seed group with 0x40140). Empty when none did.
    Rect outdoor;
    /// A group with 0x40000 was reached with the camera inside.
    bool skybox = false;
    /// The camera's seed group is interior (0xcd8620).
    bool interior = false;

    void reset(size_t groups) {
        drawn.assign(groups, 0);
        rects.assign(groups, Rect{});
        outdoor = Rect{};
        skybox = false;
        interior = false;
    }
    void draw(uint32_t g, const Rect& r) {
        if (g >= drawn.size()) return;
        drawn[g] = 1;
        rects[g].unite(r);
    }
};

/// The walk itself, 0x7ac060. `portalRects` caches each portal's projected
/// rectangle for the frame; pass it empty and it is filled as needed.
class Walker {
public:
    Walker(const Graph& graph, const glm::mat4& localToClip, const glm::vec3& camLocal,
           bool cameraInside, WalkResult& result)
        : g_(graph), clip_(localToClip), cam_(camLocal), inside_(cameraInside), res_(result) {
        cache_.assign(graph.portals.size(), Rect{});
        cached_.assign(graph.portals.size(), 0);
    }

    void walk(uint32_t group, uint32_t from, const Rect& rect, uint32_t depth = 0) {
        if (depth > kMaxPortalDepth || group >= g_.groupFlags.size()) return;
        const uint32_t flags = g_.groupFlags[group];
        if (flags & kGroupAlwaysDraw) return;
        if (inside_ && (flags & kGroupShowSkybox)) res_.skybox = true;
        res_.draw(group, rect);
        if (group >= g_.groupRefs.size()) return;
        const auto [start, count] = g_.groupRefs[group];
        for (uint32_t r = start; r < start + count && r < g_.refs.size(); ++r) {
            const Ref& ref = g_.refs[r];
            if (ref.group == from || ref.portal >= g_.portals.size()) continue;
            const Portal& p = g_.portals[ref.portal];
            if (facing(p, ref.side, cam_) < 0.0f) continue;
            const Rect& pr = portalRect(ref.portal);
            if (pr.empty() || !overlaps(pr, rect)) continue;
            Rect clipped;
            if (!clipStock(pr, rect, clipped)) continue;
            const uint32_t target = ref.group < g_.groupFlags.size() ? g_.groupFlags[ref.group] : 0;
            if (!inside_) {
                if (target & kGroupNoWalkOutside) continue;
            } else if (target & kGroupOpensOutdoors) {
                res_.outdoor.unite(pr);
                if (target & kGroupNoWalkOutside) continue;
            }
            walk(ref.group, group, clipped, depth + 1);
        }
    }

private:
    const Rect& portalRect(uint32_t portal) {
        if (!cached_[portal]) {
            cached_[portal] = 1;
            cache_[portal] = projectPortal(g_, g_.portals[portal], clip_, cam_);
        }
        return cache_[portal];
    }

    const Graph& g_;
    glm::mat4 clip_;
    glm::vec3 cam_;
    bool inside_;
    WalkResult& res_;
    std::vector<Rect> cache_;
    std::vector<uint8_t> cached_;
};

/// The camera is outside the instance (0x7b3a10). `groupInFrustum(g)` is the
/// box test of 0x78fb20.
template <typename InFrustum>
void walkFromOutside(const Graph& g, const glm::mat4& localToClip, const glm::vec3& camLocal,
                     const Rect& screen, InFrustum&& groupInFrustum, WalkResult& res) {
    res.reset(g.groupFlags.size());
    Walker w(g, localToClip, camLocal, false, res);
    for (uint32_t gi = 0; gi < g.groupFlags.size(); ++gi) {
        if (!groupInFrustum(gi)) continue;
        const uint32_t f = g.groupFlags[gi];
        if (f & kGroupAlwaysDraw) {
            res.draw(gi, screen);
        } else if (f & kGroupExterior) {
            w.walk(gi, 0xffff, screen);
        }
    }
}

/// The camera stands in `seed` of this instance (0x795d40 + 0x7ad1f0).
template <typename InFrustum>
void walkFromInside(const Graph& g, const glm::mat4& localToClip, const glm::vec3& camLocal,
                    uint32_t seed, InFrustum&& groupInFrustum, WalkResult& res) {
    res.reset(g.groupFlags.size());
    if (seed < g.groupFlags.size()) {
        const uint32_t f = g.groupFlags[seed];
        res.interior = (f & (kGroupExterior | kGroupExteriorLit)) == 0;
        if (f & kGroupSeedOpensOutdoors) res.outdoor.unite(Rect::full());
    }
    Walker w(g, localToClip, camLocal, true, res);
    w.walk(seed, 0xffff, Rect::full());
    for (uint32_t gi = 0; gi < g.groupFlags.size(); ++gi) {
        if ((g.groupFlags[gi] & kGroupAlwaysDraw) && groupInFrustum(gi)) res.draw(gi, Rect::full());
    }
}

/// The frustum of a screen rectangle: five planes (left, right, bottom, top,
/// near) of `localToClip` narrowed to `r`, each (n, d) with n.p + d >= 0
/// inside. In the space `localToClip` starts from.
struct Planes {
    std::array<glm::vec4, 5> p{};
};
inline Planes rectPlanes(const glm::mat4& m, const Rect& r) {
    // Rows of the matrix (glm is column-major: m[col][row]).
    const glm::vec4 rx(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 ry(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 rw(m[0][3], m[1][3], m[2][3], m[3][3]);
    Planes out;
    out.p[0] = rx - r.minX * rw;  // x >= minX w
    out.p[1] = r.maxX * rw - rx;  // x <= maxX w
    out.p[2] = ry - r.minY * rw;
    out.p[3] = r.maxY * rw - ry;
    out.p[4] = rw;                // w >= 0: in front of the eye
    return out;
}
/// Whether a box is at least partly inside the planes (the positive-vertex test).
inline bool boxVisible(const Planes& pl, const glm::vec3& mn, const glm::vec3& mx) {
    for (const glm::vec4& p : pl.p) {
        const glm::vec3 v(p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y,
                          p.z >= 0.0f ? mx.z : mn.z);
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0.0f) return false;
    }
    return true;
}
inline bool sphereVisible(const Planes& pl, const glm::vec3& c, float radius) {
    for (const glm::vec4& p : pl.p) {
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len <= 0.0f) continue;
        if ((p.x * c.x + p.y * c.y + p.z * c.z + p.w) < -radius * len) return false;
    }
    return true;
}

/// The two colours a WMO doodad's MODD colour (BGRA, 0xAARRGGBB as read)
/// becomes at creation (0x7bef40 -> 0x7c1ad0(colour, +0x88, 0x70, +0x84, 0x60)).
struct DoodadLight {
    glm::vec3 ambient{0.0f};  ///< +0x84: scaled down so no channel exceeds 0x60
    glm::vec3 diffuse{0.0f};  ///< +0x88: value raised to at least 0x70 (HSV)
};
inline DoodadLight doodadLight(uint32_t bgra) {
    const int b = static_cast<int>(bgra & 0xff);
    const int gch = static_cast<int>((bgra >> 8) & 0xff);
    const int r = static_cast<int>((bgra >> 16) & 0xff);
    int mx = std::max(b, std::max(gch, r));
    if (mx == 0) mx = 1;
    DoodadLight out;
    // +0x88: the colour, or with its HSV value raised to 0x70/255 when the
    // brightest channel is below 0x70 (0x984f60 / 0x985030 / 0x9851a0). The
    // hue and saturation are kept, so this scales every channel by 0x70/max.
    // A black colour (max taken as 1) stays black.
    {
        float fr = r / 255.0f, fg = gch / 255.0f, fb = b / 255.0f;
        if (mx < 0x70) {
            const float k = static_cast<float>(0x70) / static_cast<float>(mx);
            fr = std::round(std::min(fr * k, 1.0f) * 255.0f) / 255.0f;
            fg = std::round(std::min(fg * k, 1.0f) * 255.0f) / 255.0f;
            fb = std::round(std::min(fb * k, 1.0f) * 255.0f) / 255.0f;
        }
        out.diffuse = glm::vec3(fr, fg, fb);
    }
    // +0x84: when the brightest channel is above 0x60, every channel times
    // round(0x60 * 255 / max - 0.5), plus 0xff, shifted down 8.
    {
        int ar = r, ag = gch, ab = b;
        if (mx > 0x60) {
            const int k = static_cast<int>(std::lround(static_cast<double>(static_cast<float>(0x60) * 255.0f /
                                                                           static_cast<float>(mx)) - 0.5));
            ar = (r * k + 0xff) >> 8;
            ag = (gch * k + 0xff) >> 8;
            ab = (b * k + 0xff) >> 8;
        }
        out.ambient = glm::vec3(ar / 255.0f, ag / 255.0f, ab / 255.0f);
    }
    return out;
}

/// The fixed direction an interior doodad's light comes along (0xaeedf0),
/// in the same sense as the DayNight light direction. Its x and y are equal,
/// so it is the same in render and world coordinates.
inline constexpr glm::vec3 kInteriorDoodadLightDir{-0.30822f, -0.30822f, -0.9f};

/// Whether a doodad of a group with these MOGP flags is lit as an interior
/// one (0x7bf740: def flag 2 unless the group is exterior or exterior-lit).
inline bool doodadLitAsInterior(uint32_t groupFlags) {
    return (groupFlags & (kGroupExterior | kGroupExteriorLit)) == 0;
}

}  // namespace wowee::rendering::stock_wmo_portal
