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
