// A game object's animation and model events as the stock 3.3.5a client
// plays them (docs/re/interactions.md section 12): the handler's kinds for
// the GO states (0x70d600, 0x70d690, 0x70d7e0), the sequence a kind plays on
// a model (0x70d1e0 and the AnimationData chain), and which events a frame
// passes (0x830fb0).
#include <catch_amalgamated.hpp>

#include "anim_test_support.hpp"
#include "rendering/animation/game_object_anim.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/m2_object_events.hpp"
#include "pipeline/m2_loader.hpp"

#include <initializer_list>
#include <vector>

namespace a = wowee::rendering::anim;
namespace go = wowee::rendering::anim::goanim;
using wowee::pipeline::M2Model;
using wowee::pipeline::m2EventTag;

namespace {

// AnimationData 145-157 and Stand as 3.3.5a has them (ID, Fallback, Flags).
void loadRows() {
    struct Row { uint32_t id, fallback, flags; };
    static const Row rows[] = {
        {0, 147, 0x3},    {145, 0, 0},      {146, 148, 0x10}, {147, 146, 0x20},
        {148, 146, 0x13}, {149, 148, 0x23}, {150, 148, 0},    {151, 150, 0x20},
        {152, 150, 0x10}, {153, 0, 0x2},    {154, 0, 0},      {155, 0, 0},
        {156, 0, 0},      {157, 0, 0},
    };
    for (const auto& r : rows) a::setDbcFallbackRow(r.id, r.fallback, r.flags);
}

}  // namespace

TEST_CASE("Game object kinds for the states", "[goanim]") {
    // First sight (0x70d600): the state's loop, no motion.
    CHECK(go::kindForState(go::kStateActive) == go::Kind::Opened);
    CHECK(go::kindForState(go::kStateReady) == go::Kind::Closed);
    CHECK(go::kindForState(go::kStateActiveAlternative) == go::Kind::Destroyed);
    // With the descriptor's progress set, the motion.
    CHECK(go::kindForState(go::kStateActive, 0x4000) == go::Kind::Open);

    // Changes (0x70d690).
    CHECK(go::kindForStateChange(go::kStateReady, go::kStateActive) == go::Kind::Open);
    CHECK(go::kindForStateChange(go::kStateActive, go::kStateReady) == go::Kind::Close);
    CHECK(go::kindForStateChange(go::kStateActiveAlternative, go::kStateReady) == go::Kind::Rebuild);
    CHECK(go::kindForStateChange(go::kStateReady, go::kStateActiveAlternative) == go::Kind::Destroy);
    // From the other open state, straight into the loop.
    CHECK(go::kindForStateChange(go::kStateActiveAlternative, go::kStateActive) == go::Kind::Opened);
    CHECK(go::kindForStateChange(go::kStateActive, go::kStateActiveAlternative) == go::Kind::Destroyed);

    // Ends (0x70d7e0).
    CHECK(go::kindAfterEnd(go::Kind::Open) == go::Kind::Opened);
    CHECK(go::kindAfterEnd(go::Kind::Close) == go::Kind::Closed);
    CHECK(go::kindAfterEnd(go::Kind::Rebuild) == go::Kind::Closed);
    CHECK(go::kindAfterEnd(go::Kind::Destroy) == go::Kind::Destroyed);
    CHECK_FALSE(go::kindAfterEnd(go::Kind::Custom0).has_value());
    CHECK_FALSE(go::kindAfterEnd(go::Kind::Spawn).has_value());

    CHECK(go::isReversal(go::Kind::Open, go::Kind::Close));
    CHECK_FALSE(go::isReversal(go::Kind::Open, go::Kind::Destroy));
    CHECK(go::kindAnimation(go::Kind::Custom2) == a::CUSTOM_2);
    CHECK(go::kindAnimation(go::Kind::Despawn) == a::DESPAWN);
}

TEST_CASE("Game object kinds on models", "[goanim]") {
    loadRows();
    // A full door (StormwindDoor: 0, 147, 148, 146, 149): its own sequences.
    const auto door = anim_test::hasIds({0, 147, 148, 146, 149});
    for (auto k : {go::Kind::Open, go::Kind::Opened, go::Kind::Close, go::Kind::Closed}) {
        const auto r = go::resolveKind(door, k, 0);
        CHECK(r.id == go::kindAnimation(k));
        CHECK(r.mode == a::PlayMode::Forward);
    }
    // Destroy falls back to Open on a door (state 2, the other way open).
    CHECK(go::resolveKind(door, go::Kind::Destroy, 0).id == a::OPEN);
    // ...and Destroyed holds Open's last frame through Destroy.
    {
        const auto r = go::resolveKind(door, go::Kind::Destroyed, 0);
        CHECK(r.id == a::OPEN);
        CHECK(r.mode == a::PlayMode::HoldLast);
    }

    // Rebuild on a door: Open run backwards; the id asked for is Rebuild's,
    // which is what is compared with the sequence already playing.
    {
        uint32_t requested = 0;
        const auto r = go::resolveKind(door, go::Kind::Rebuild, 0, &requested);
        CHECK(r.id == a::OPEN);
        CHECK(r.mode == a::PlayMode::Reverse);
        CHECK(requested == a::REBUILD);
    }
    // RazorfenDoor01 has no Close (0, 147, 148, 149): Open plays backwards.
    {
        const auto r = go::resolveKind(anim_test::hasIds({0, 147, 148, 149}), go::Kind::Close, 0);
        CHECK(r.id == a::OPEN);
        CHECK(r.mode == a::PlayMode::Reverse);
    }
    // Only Open: Closed is Open stopped on its first frame, Opened its last.
    {
        const auto m = anim_test::hasIds({0, 148});
        uint32_t requested = 0;
        const auto c = go::resolveKind(m, go::Kind::Closed, 0, &requested);
        CHECK(requested == a::OPEN);
        CHECK(c.id == a::OPEN);
        CHECK(c.mode == a::PlayMode::HoldFirst);
        const auto o = go::resolveKind(m, go::Kind::Opened, 0);
        CHECK(o.id == a::OPEN);
        CHECK(o.mode == a::PlayMode::HoldLast);
    }
    // Only Close: Opened is Close stopped on its first frame.
    {
        const auto r = go::resolveKind(anim_test::hasIds({0, 146}), go::Kind::Opened, 0);
        CHECK(r.id == a::CLOSE);
        CHECK(r.mode == a::PlayMode::HoldFirst);
    }
    // A brazier, a bobber, a lift door: Stand for every state.
    const auto plain = anim_test::hasIds({0});
    for (auto k : {go::Kind::Open, go::Kind::Opened, go::Kind::Close, go::Kind::Closed,
                   go::Kind::Destroyed}) {
        const auto r = go::resolveKind(plain, k, 0);
        CHECK(r.id == a::STAND);
        CHECK(r.mode == a::PlayMode::Forward);
    }
    // A chest (Chest01: 148, 146, 147, 149, 0) closed.
    CHECK(go::resolveKind(anim_test::hasIds({148, 146, 147, 149, 0}), go::Kind::Closed, 148).id == a::CLOSED);
}

TEST_CASE("Object event tags", "[goanim]") {
    using wowee::pipeline::gameObjectSoundSlot;
    using wowee::pipeline::isM2ObjectEventTag;
    CHECK(gameObjectSoundSlot(m2EventTag("$GO0")) == 0);
    CHECK(gameObjectSoundSlot(m2EventTag("$GO5")) == 5);
    CHECK(gameObjectSoundSlot(m2EventTag("$GC0")) == 6);
    CHECK(gameObjectSoundSlot(m2EventTag("$GC3")) == 9);
    CHECK(gameObjectSoundSlot(m2EventTag("$GO6")) == -1);
    CHECK(gameObjectSoundSlot(m2EventTag("$GC4")) == -1);
    CHECK(gameObjectSoundSlot(m2EventTag("$DSL")) == -1);
    CHECK(isM2ObjectEventTag(m2EventTag("$DSL")));
    CHECK(isM2ObjectEventTag(m2EventTag("$DSE")));
    CHECK(isM2ObjectEventTag(m2EventTag("$SHK")));
    CHECK(isM2ObjectEventTag(m2EventTag("$GO4")));
    CHECK_FALSE(isM2ObjectEventTag(m2EventTag("$CSD")));
    CHECK_FALSE(isM2ObjectEventTag(m2EventTag("$FSD")));
}

TEST_CASE("Which events a frame passes", "[goanim]") {
    using wowee::rendering::passObjectEvents;
    std::vector<M2Model::ObjectEvent> ev;
    for (uint32_t t : {0u, 200u, 900u}) {
        M2Model::ObjectEvent e;
        e.timeMs = t;
        ev.push_back(e);
    }
    std::vector<uint32_t> got;
    auto rec = [&](const M2Model::ObjectEvent& e) { got.push_back(e.timeMs); };

    // A sequence just started fires its stamp at 0.
    passObjectEvents(ev, 0.0f, 16.0f, 1000.0f, true, 1.0f, rec);
    CHECK(got == std::vector<uint32_t>{0});
    got.clear();
    // (from, t]
    passObjectEvents(ev, 16.0f, 200.0f, 1000.0f, false, 1.0f, rec);
    CHECK(got == std::vector<uint32_t>{200});
    got.clear();
    passObjectEvents(ev, 200.0f, 210.0f, 1000.0f, false, 1.0f, rec);
    CHECK(got.empty());
    // A lap: the old lap's rest, then the new one's start.
    passObjectEvents(ev, 850.0f, 20.0f, 1000.0f, false, 1.0f, rec);
    CHECK(got == std::vector<uint32_t>{900, 0});
    got.clear();
    // A one-shot held at its end this frame still passes its last stretch.
    passObjectEvents(ev, 850.0f, 1000.0f, 1000.0f, false, 0.0f, rec);
    CHECK(got == std::vector<uint32_t>{900});
    got.clear();
    // Held: nothing more.
    passObjectEvents(ev, 1000.0f, 1000.0f, 1000.0f, false, 0.0f, rec);
    CHECK(got.empty());
    // A held pose shown, not played: nothing.
    passObjectEvents(ev, 0.0f, 1000.0f, 1000.0f, true, 0.0f, rec);
    CHECK(got.empty());
    // Backwards (Close as Open run back): from the end down.
    passObjectEvents(ev, 0.0f, 950.0f, 1000.0f, true, -1.0f, rec);
    CHECK(got.empty());
    passObjectEvents(ev, 950.0f, 100.0f, 1000.0f, false, -1.0f, rec);
    CHECK(got == std::vector<uint32_t>{900, 200});
    got.clear();
    passObjectEvents(ev, 100.0f, 0.0f, 1000.0f, false, 0.0f, rec);
    CHECK(got == std::vector<uint32_t>{0});
}
