// The stock client's WMO portal walk and WMO doodad light, held to Wow.exe
// 3.3.5a at the addresses rendering/stock_wmo_portal.hpp names.
// docs/re/wmo-rendering.md §5, §6 and §10 have the walk-through. Header-only.
#include <catch_amalgamated.hpp>

#include "rendering/stock_wmo_portal.hpp"

#include <glm/gtc/matrix_transform.hpp>

using Catch::Approx;
namespace sp = wowee::rendering::stock_wmo_portal;

namespace {

// A row of rooms along +x, each 10 wide, with a doorway portal in the
// plane x = 10 * k between room k-1 and room k. Room 0 is exterior.
//   room 0 (exterior) | room 1 | room 2
// Portal k-1 sits at x = 10k, normal +x, so n.p + d = x - 10k.
sp::Graph corridor(uint32_t rooms, uint32_t room0Flags = sp::kGroupExterior) {
    sp::Graph g;
    g.groupFlags.assign(rooms, 0x2000);  // interior
    g.groupFlags[0] = room0Flags;
    for (uint32_t k = 1; k < rooms; ++k) {
        const float x = 10.0f * static_cast<float>(k);
        sp::Portal p;
        p.firstVertex = static_cast<uint32_t>(g.vertices.size());
        p.vertexCount = 4;
        p.normal = glm::vec3(1.0f, 0.0f, 0.0f);
        p.d = -x;
        g.vertices.push_back({x, -2.0f, 0.0f});
        g.vertices.push_back({x, 2.0f, 0.0f});
        g.vertices.push_back({x, 2.0f, 4.0f});
        g.vertices.push_back({x, -2.0f, 4.0f});
        g.portals.push_back(p);
    }
    // Refs: room k sees portal k-1 (behind it, side -1: room k is on the
    // positive side... no - room k is at x > 10k, the positive side) and
    // portal k (ahead of it, room k on the negative side).
    g.groupRefs.resize(rooms);
    for (uint32_t k = 0; k < rooms; ++k) {
        const uint32_t start = static_cast<uint32_t>(g.refs.size());
        if (k > 0) g.refs.push_back({static_cast<uint16_t>(k - 1), static_cast<uint16_t>(k - 1), 1});
        if (k + 1 < rooms) g.refs.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(k + 1), -1});
        g.groupRefs[k] = {start, static_cast<uint32_t>(g.refs.size()) - start};
    }
    return g;
}

glm::mat4 viewProj(const glm::vec3& eye, const glm::vec3& at) {
    return glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f) *
           glm::lookAt(eye, at, glm::vec3(0.0f, 0.0f, 1.0f));
}

}  // namespace

TEST_CASE("the walk clips as 0x7ac060 does, one maximum left unclamped", "[stockwmoportal]") {
    const sp::Rect portal{-0.5f, -0.5f, 2.0f, 2.0f};
    const sp::Rect current{-1.0f, -1.0f, 1.0f, 1.0f};
    sp::Rect out;
    REQUIRE(sp::clipStock(portal, current, out));
    CHECK(out.minX == Approx(-0.5f));
    CHECK(out.minY == Approx(-0.5f));
    CHECK(out.maxY == Approx(1.0f));
    // 0x7ac32a..0x7ac363 clamp the other maximum twice; this one stays.
    CHECK(out.maxX == Approx(2.0f));
    // A sliver is dropped (0x482870 with 0x9e1134 = 0.001).
    CHECK_FALSE(sp::clipStock(sp::Rect{-0.5f, 0.9995f, 0.5f, 2.0f}, current, out));
    CHECK(sp::overlaps(portal, current));
    CHECK_FALSE(sp::overlaps(sp::Rect{1.5f, 0.0f, 2.0f, 0.5f}, current));
}

TEST_CASE("a portal faces the camera by its plane and the ref's side", "[stockwmoportal]") {
    sp::Portal p;
    p.normal = glm::vec3(1.0f, 0.0f, 0.0f);
    p.d = -10.0f;
    CHECK(sp::facing(p, 1, {12.0f, 0.0f, 0.0f}) > 0.0f);
    CHECK(sp::facing(p, -1, {12.0f, 0.0f, 0.0f}) < 0.0f);
    CHECK(sp::facing(p, -1, {8.0f, 0.0f, 0.0f}) > 0.0f);
    // NaN planes become (0, 0, 1) at load (0x7d7470).
    sp::Portal bad;
    bad.normal = glm::vec3(std::nanf(""), 0.0f, 0.0f);
    CHECK(sp::sanitisePortal(bad).normal == glm::vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("a portal projects to its screen bounds, or the whole screen when the camera is in it",
          "[stockwmoportal]") {
    const sp::Graph g = corridor(2);
    const glm::vec3 eye(2.0f, 0.0f, 2.0f);
    const glm::mat4 vp = viewProj(eye, {20.0f, 0.0f, 2.0f});
    const sp::Rect r = sp::projectPortal(g, g.portals[0], vp, eye);
    REQUIRE_FALSE(r.empty());
    // Centred, and smaller than the screen: 4 wide at 8 yards.
    CHECK(r.minX == Approx(-r.maxX).margin(1e-4));
    CHECK(r.maxX < 1.0f);
    CHECK(r.maxX > 0.2f);
    // Standing in the doorway (within 0.01 of the plane, over the polygon).
    const glm::vec3 inDoor(10.005f, 0.0f, 2.0f);
    const sp::Rect full = sp::projectPortal(g, g.portals[0], viewProj(inDoor, {20.0f, 0.0f, 2.0f}), inDoor);
    CHECK(full.minX == Approx(-1.0f));
    CHECK(full.maxY == Approx(1.0f));
    // Wholly behind the camera: nothing.
    const glm::vec3 past(30.0f, 0.0f, 2.0f);
    CHECK(sp::projectPortal(g, g.portals[0], viewProj(past, {40.0f, 0.0f, 2.0f}), past).empty());
}

TEST_CASE("from outside, interiors are reached only through portals that face the camera",
          "[stockwmoportal]") {
    const sp::Graph g = corridor(3);
    const auto all = [](uint32_t) { return true; };
    sp::WalkResult res;
    // In the exterior room looking down the corridor: both rooms behind it.
    {
        const glm::vec3 eye(2.0f, 0.0f, 2.0f);
        sp::walkFromOutside(g, viewProj(eye, {30.0f, 0.0f, 2.0f}), eye, sp::Rect::full(), all, res);
        CHECK(res.drawn[0]);
        CHECK(res.drawn[1]);
        CHECK(res.drawn[2]);
        CHECK(res.rects[2].maxX < 1.0f);
    }
    // Looking away: the doorway is behind the camera, the rooms are not drawn.
    {
        const glm::vec3 eye(2.0f, 0.0f, 2.0f);
        sp::walkFromOutside(g, viewProj(eye, {-30.0f, 0.0f, 2.0f}), eye, sp::Rect::full(), all, res);
        CHECK(res.drawn[0]);
        CHECK_FALSE(res.drawn[1]);
        CHECK_FALSE(res.drawn[2]);
    }
    // An exterior group out of the frustum starts no walk.
    {
        const glm::vec3 eye(2.0f, 0.0f, 2.0f);
        sp::walkFromOutside(g, viewProj(eye, {30.0f, 0.0f, 2.0f}), eye, sp::Rect::full(),
                            [](uint32_t gi) { return gi != 0; }, res);
        CHECK_FALSE(res.drawn[0]);
        CHECK_FALSE(res.drawn[1]);
    }
}

TEST_CASE("from inside, a portal into an exterior group opens the outdoor view", "[stockwmoportal]") {
    sp::Graph g = corridor(3);
    g.groupFlags[2] |= sp::kGroupShowSkybox;
    const auto all = [](uint32_t) { return true; };
    sp::WalkResult res;
    // In room 1 looking back at the exterior: it is not walked into, the
    // outdoor view opens over its portal.
    {
        const glm::vec3 eye(15.0f, 0.0f, 2.0f);
        sp::walkFromInside(g, viewProj(eye, {-20.0f, 0.0f, 2.0f}), eye, 1, all, res);
        CHECK(res.interior);
        CHECK(res.drawn[1]);
        CHECK_FALSE(res.drawn[0]);
        CHECK_FALSE(res.drawn[2]);
        CHECK_FALSE(res.outdoor.empty());
        CHECK(res.outdoor.maxX < 1.0f);
        CHECK_FALSE(res.skybox);
    }
    // Looking ahead into room 2, which carries 0x40000: the MOSB sky.
    {
        const glm::vec3 eye(15.0f, 0.0f, 2.0f);
        sp::walkFromInside(g, viewProj(eye, {40.0f, 0.0f, 2.0f}), eye, 1, all, res);
        CHECK(res.drawn[2]);
        CHECK(res.skybox);
        // 0x40000 is one of 0x50148: the outdoors opens over that portal too,
        // and the walk still goes in, the group being neither 0x8 nor 0x10000.
        CHECK_FALSE(res.outdoor.empty());
    }
    // 0x10000 groups are drawn in the frustum, never walked into.
    {
        sp::Graph h = corridor(3);
        h.groupFlags[2] |= sp::kGroupAlwaysDraw;
        const glm::vec3 eye(15.0f, 0.0f, 2.0f);
        sp::walkFromInside(h, viewProj(eye, {-20.0f, 0.0f, 2.0f}), eye, 1, all, res);
        CHECK(res.drawn[2]);
        CHECK(res.rects[2].minX == Approx(-1.0f));
    }
    // A seed group lit as exterior opens the outdoors at once (0x795d40).
    {
        sp::Graph h = corridor(3);
        h.groupFlags[1] |= sp::kGroupExteriorLit;
        const glm::vec3 eye(15.0f, 0.0f, 2.0f);
        sp::walkFromInside(h, viewProj(eye, {40.0f, 0.0f, 2.0f}), eye, 1, all, res);
        CHECK_FALSE(res.interior);
        CHECK(res.outdoor.minX == Approx(-1.0f));
    }
}

TEST_CASE("from inside, the second seed group is drawn and walked from too", "[stockwmoportal]") {
    const sp::Graph g = corridor(3);
    const auto all = [](uint32_t) { return true; };
    sp::WalkResult res;
    // In room 1 looking back at the exterior, seeded from room 2 (the floor
    // under the camera is the room beyond): room 2 alone would leave room 1,
    // where the camera stands, undrawn - the second seed draws it, and the
    // outdoors opens through its portal as before.
    const glm::vec3 eye(15.0f, 0.0f, 2.0f);
    sp::walkFromInside(g, viewProj(eye, {-20.0f, 0.0f, 2.0f}), eye, 2, all, res);
    CHECK_FALSE(res.drawn[1]);
    sp::walkFromInside(g, viewProj(eye, {-20.0f, 0.0f, 2.0f}), eye, 2, all, res, 1);
    CHECK(res.drawn[1]);
    CHECK(res.drawn[2]);
    CHECK_FALSE(res.outdoor.empty());
    // The same group twice is one seed.
    sp::walkFromInside(g, viewProj(eye, {-20.0f, 0.0f, 2.0f}), eye, 1, all, res, 1);
    CHECK(res.drawn[1]);
    CHECK_FALSE(res.drawn[0]);
}

TEST_CASE("a rectangle's planes cull boxes to that part of the screen", "[stockwmoportal]") {
    const glm::vec3 eye(0.0f, 0.0f, 0.0f);
    const glm::mat4 vp = viewProj(eye, {10.0f, 0.0f, 0.0f});
    const sp::Planes left = sp::rectPlanes(vp, sp::Rect{-1.0f, -1.0f, 0.0f, 1.0f});
    const sp::Planes all = sp::rectPlanes(vp, sp::Rect::full());
    // In glm's view, +y in the world is screen left here (looking down +x).
    const glm::vec3 onLeft(10.0f, 3.0f, 0.0f), onRight(10.0f, -3.0f, 0.0f);
    CHECK(sp::boxVisible(all, onLeft - 0.5f, onLeft + 0.5f));
    CHECK(sp::boxVisible(all, onRight - 0.5f, onRight + 0.5f));
    CHECK(sp::boxVisible(left, onLeft - 0.5f, onLeft + 0.5f) !=
          sp::boxVisible(left, onRight - 0.5f, onRight + 0.5f));
    // Behind the eye.
    CHECK_FALSE(sp::boxVisible(all, glm::vec3(-10.5f, -0.5f, -0.5f), glm::vec3(-9.5f, 0.5f, 0.5f)));
    CHECK(sp::sphereVisible(all, {10.0f, 0.0f, 0.0f}, 1.0f));
    CHECK_FALSE(sp::sphereVisible(all, {-10.0f, 0.0f, 0.0f}, 1.0f));
}

TEST_CASE("a WMO doodad's MODD colour becomes its two lights as 0x7c1ad0 makes them",
          "[stockwmoportal]") {
    // Dark grey: the diffuse is raised to 0x70, the ambient left as it is.
    {
        const auto l = sp::doodadLight(0xff202020u);
        CHECK(l.diffuse.r == Approx(0x70 / 255.0f));
        CHECK(l.diffuse.g == Approx(0x70 / 255.0f));
        CHECK(l.ambient.r == Approx(0x20 / 255.0f));
    }
    // Orange (r 255, g 128, b 0): the diffuse kept, the ambient scaled so its
    // brightest channel is 0x60: k = round(0x60 * 255 / 255 - 0.5) = 96,
    // (255 * 96 + 255) >> 8 = 96, (128 * 96 + 255) >> 8 = 48, (0 + 255) >> 8 = 0.
    {
        const auto l = sp::doodadLight(0xffff8000u);
        CHECK(l.diffuse.r == Approx(1.0f));
        CHECK(l.diffuse.g == Approx(128 / 255.0f));
        CHECK(l.ambient.r == Approx(96 / 255.0f));
        CHECK(l.ambient.g == Approx(48 / 255.0f));
        CHECK(l.ambient.b == Approx(0.0f));
    }
    // Black stays black.
    {
        const auto l = sp::doodadLight(0xff000000u);
        CHECK(l.diffuse == glm::vec3(0.0f));
        CHECK(l.ambient == glm::vec3(0.0f));
    }
    CHECK(sp::doodadLitAsInterior(0x2000));
    CHECK_FALSE(sp::doodadLitAsInterior(0x2000 | 0x40));
    CHECK_FALSE(sp::doodadLitAsInterior(0x8));
}
