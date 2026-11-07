// The stock 3.3.5a camera input rules (rendering/stock_camera.hpp): the
// mouse-look scale, the pointer speed mouseSpeed sets, and the slider pair.
// Values are the ones read from Wow.exe 12340; docs/re/movement-camera.md
// sections 4 and 10 have the addresses.
#include <catch_amalgamated.hpp>
#include "rendering/stock_camera.hpp"

namespace sc = wowee::rendering::stock_camera;
using Catch::Approx;

TEST_CASE("the DDC rectangle has a unit diagonal") {
    // 0x47BF90: 0.8 x 0.6 at 4:3.
    CHECK(sc::ddcWidth(4.0f / 3.0f) == Approx(0.8f));
    CHECK(sc::ddcHeight(4.0f / 3.0f) == Approx(0.6f));
    const float a = 16.0f / 9.0f;
    CHECK(sc::ddcWidth(a) * sc::ddcWidth(a) + sc::ddcHeight(a) * sc::ddcHeight(a) ==
          Approx(1.0f));
}

TEST_CASE("mouse look at the defaults turns 0.28125 and 0.25 degrees a pixel at 4:3") {
    // dx / 0.8 / 800 x 180, dy / 0.6 / 600 x 90 (0x6020B0).
    const auto d = sc::mouseLook(1.0f, 1.0f, 4.0f / 3.0f, sc::kDefaultYawMoveSpeed,
                                 sc::kDefaultPitchMoveSpeed, sc::kDefaultMouseSpeed);
    CHECK(d.yawDeg == Approx(0.28125f));
    CHECK(d.pitchDeg == Approx(0.25f));
    // 800 pixels across a 4:3 screen turn 225 degrees.
    CHECK(sc::mouseLook(800.0f, 0.0f, 4.0f / 3.0f, 180.0f, 90.0f, 1.0f).yawDeg ==
          Approx(225.0f));
}

TEST_CASE("a wider screen turns less a pixel across and more a pixel down") {
    const auto wide = sc::mouseLook(1.0f, 1.0f, 16.0f / 9.0f, 180.0f, 90.0f, 1.0f);
    const auto narrow = sc::mouseLook(1.0f, 1.0f, 4.0f / 3.0f, 180.0f, 90.0f, 1.0f);
    CHECK(wide.yawDeg < narrow.yawDeg);
    CHECK(wide.pitchDeg > narrow.pitchDeg);
    CHECK(wide.yawDeg == Approx(0.25810f).epsilon(1e-3));
}

TEST_CASE("the look scales with cameraYawMoveSpeed and cameraPitchMoveSpeed") {
    const auto d = sc::mouseLook(10.0f, -10.0f, 4.0f / 3.0f, 270.0f, 135.0f, 1.0f);
    CHECK(d.yawDeg == Approx(10.0f * 0.28125f * 1.5f));
    CHECK(d.pitchDeg == Approx(-10.0f * 0.25f * 1.5f));
    // The Mouse Look Speed slider writes the pitch at half the yaw.
    CHECK(sc::pitchMoveSpeedForSlider(270.0f) == Approx(135.0f));
    CHECK(sc::pitchMoveSpeedForSlider(sc::kDefaultYawMoveSpeed) == sc::kDefaultPitchMoveSpeed);
}

TEST_CASE("mouseSpeed is the Windows pointer speed over ten") {
    // 0x86A070: clamp to [0.1, 2.0], x10, rounded.
    CHECK(sc::osPointerSpeed(1.0f) == 10);
    CHECK(sc::osPointerSpeed(0.5f) == 5);
    CHECK(sc::osPointerSpeed(1.5f) == 15);
    CHECK(sc::osPointerSpeed(0.0f) == 1);
    CHECK(sc::osPointerSpeed(5.0f) == 20);
    // Windows' table, enhance pointer precision off.
    CHECK(sc::osPointerMultiplier(10) == 1.0f);
    CHECK(sc::osPointerMultiplier(1) == Approx(1.0f / 32.0f));
    CHECK(sc::osPointerMultiplier(3) == Approx(0.125f));
    CHECK(sc::osPointerMultiplier(5) == Approx(0.375f));
    CHECK(sc::osPointerMultiplier(15) == Approx(2.25f));
    CHECK(sc::osPointerMultiplier(20) == Approx(3.5f));
    // and the look carries it, both axes alike.
    const auto slow = sc::mouseLook(4.0f, 4.0f, 4.0f / 3.0f, 180.0f, 90.0f, 0.5f);
    CHECK(slow.yawDeg == Approx(4.0f * 0.28125f * 0.375f));
    CHECK(slow.pitchDeg == Approx(4.0f * 0.25f * 0.375f));
}

TEST_CASE("held MoveView turns at the look speeds, Right and Up the stock way round") {
    // 0x6000E0: Right raises the stock yaw (as a mouse moved left), Up raises
    // the stock pitch (looks further down); 180 and 90 deg/s at the defaults.
    const auto right = sc::viewMoveStep(0, 0, 1, 0, 0, 0, 8.33f, 180.0f, 90.0f, 0.5f);
    CHECK(right.yawDeg == Approx(90.0f));
    CHECK(right.pitchDeg == 0.0f);
    const auto up = sc::viewMoveStep(0, 0, 0, 0, 1, 0, 8.33f, 180.0f, 90.0f, 1.0f);
    CHECK(up.pitchDeg == Approx(-90.0f));  // this client's pitch: looking down
    const auto left2 = sc::viewMoveStep(0, 0, 0, 2, 0, 0, 8.33f, 270.0f, 135.0f, 0.1f);
    CHECK(left2.yawDeg == Approx(-54.0f));  // speed 2 at a 270 look speed
    const auto out = sc::viewMoveStep(0, 1, 0, 0, 0, 0, 8.33f, 180.0f, 90.0f, 0.3f);
    CHECK(out.zoomOut == Approx(2.499f));
    const auto both = sc::viewMoveStep(1, 1, 1, 1, 1, 1, 8.33f, 180.0f, 90.0f, 1.0f);
    CHECK(both.zoomOut == 0.0f);
    CHECK(both.yawDeg == 0.0f);
    CHECK(both.pitchDeg == 0.0f);
}

TEST_CASE("the water line: a swimmer at the float depth is at the surface") {
    // 0x6049C0: depth <= h - 2/9 is Shallow. A human (2.031) floats 1.52 under.
    CHECK(sc::waterLine(false, 0.0f, 2.031f) == sc::WaterLine::None);
    CHECK(sc::waterLine(true, 0.75f * 2.031f, 2.031f) == sc::WaterLine::Shallow);
    CHECK(sc::waterLine(true, 1.80f, 2.031f) == sc::WaterLine::Submerged);
    // A gnome (1.056) too.
    CHECK(sc::waterLine(true, 0.75f * 1.056f, 1.056f) == sc::WaterLine::Shallow);
}

TEST_CASE("cameraDive holds a swimmer level at the surface unless steered 18 degrees down") {
    using W = sc::WaterLine;
    // Keys alone at the surface: level.
    CHECK(sc::diveHoldsLevel(W::Shallow, false, 40.0f));
    // Steering, looking down 10: level; 20: dives; looking up: not held.
    CHECK(sc::diveHoldsLevel(W::Shallow, true, 10.0f));
    CHECK_FALSE(sc::diveHoldsLevel(W::Shallow, true, 20.0f));
    CHECK_FALSE(sc::diveHoldsLevel(W::Shallow, true, -10.0f));
    // Under water nothing holds at the default cameraSurfacePitch 0.
    CHECK_FALSE(sc::diveHoldsLevel(W::Submerged, false, 10.0f));
    // With a negative surface pitch, looking up less than it holds.
    CHECK(sc::diveHoldsLevel(W::Submerged, true, -5.0f, -10.0f));
    CHECK_FALSE(sc::diveHoldsLevel(W::Submerged, true, -15.0f, -10.0f));
    CHECK_FALSE(sc::diveHoldsLevel(W::None, false, 30.0f));
}

TEST_CASE("crossing the water line eases the camera to 5 degrees down") {
    using W = sc::WaterLine;
    bool fires = false;
    CHECK(sc::waterLineFinalPitch(W::Submerged, W::Shallow, &fires) == 5.0f);
    CHECK(fires);
    CHECK(sc::waterLineFinalPitch(W::Shallow, W::Submerged, &fires) == 5.0f);
    CHECK(fires);
    sc::waterLineFinalPitch(W::None, W::Shallow, &fires);
    CHECK_FALSE(fires);  // walking into the water is not a crossing
    sc::waterLineFinalPitch(W::Shallow, W::Shallow, &fires);
    CHECK_FALSE(fires);
    sc::waterLineFinalPitch(W::Shallow, W::Submerged, &fires, 5.0f, 0.0f);
    CHECK_FALSE(fires);  // a final pitch of 0 is off
}

TEST_CASE("the player model fades between 1.83 yd and the near clip") {
    // Opaque at the default view, gone at first person.
    CHECK(sc::playerModelAlpha(5.55f, 10.0f, 1.6f) == 1.0f);
    CHECK(sc::playerModelAlpha(0.0f, 10.0f, 1.6f) == 0.0f);
    CHECK(sc::playerModelAlpha(0.2f, 10.0f, 1.6f) == 0.0f);
    // Halfway along the cosine at 0.2 + (1.8315 + 1/360) / 2.
    CHECK(sc::playerModelAlpha(0.2f + (1.8315f + 1.0f / 360.0f) * 0.5f, 10.0f, 1.6f) ==
          Approx(0.5f).margin(1e-3));
    CHECK(sc::playerModelAlpha(2.0f, 10.0f, 1.6f) == Approx(0.99927f).margin(1e-3));
    CHECK(sc::playerModelAlpha(1.5f, 10.0f, 1.6f) == Approx(0.80566f).margin(1e-3));
    // From under the character looking up steeply it fades from further out.
    CHECK(sc::playerModelAlpha(1.5f, -80.0f, 1.6f) < sc::playerModelAlpha(1.5f, 10.0f, 1.6f));
    // ... but not when the camera is further than the pivot height.
    CHECK(sc::playerModelAlpha(2.5f, -80.0f, 1.6f) == 1.0f);
}
