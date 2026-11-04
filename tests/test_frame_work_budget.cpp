// The per-frame share of main-thread spawn work (core/frame_work_budget.hpp):
// the first item of a frame always runs, later ones only while the frame has
// spent less than the budget on them, and the budget is per frame.

#include <catch_amalgamated.hpp>
#include "core/frame_work_budget.hpp"

#include <algorithm>

using wowee::core::FrameWorkBudget;
using Clock = FrameWorkBudget::Clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;

TEST_CASE("the first item of a frame always runs", "[frame_budget]") {
    const auto t0 = Clock::now();
    const FrameWorkBudget budget(3.0f, t0);
    // Even when the frame has already overrun - an item larger than the
    // budget must not stall its queue for good.
    CHECK(budget.allowNext(0, t0));
    CHECK(budget.allowNext(0, t0 + milliseconds(50)));
    const FrameWorkBudget none(0.0f, t0);
    CHECK(none.allowNext(0, t0 + milliseconds(1)));
    CHECK_FALSE(none.allowNext(1, t0));
}

TEST_CASE("later items run while time is left", "[frame_budget]") {
    const auto t0 = Clock::now();
    const FrameWorkBudget budget(3.0f, t0);
    CHECK(budget.allowNext(1, t0 + microseconds(500)));
    CHECK(budget.allowNext(5, t0 + microseconds(2999)));
    CHECK_FALSE(budget.allowNext(1, t0 + milliseconds(3)));
    CHECK_FALSE(budget.allowNext(2, t0 + milliseconds(20)));
}

TEST_CASE("a crowd is spread over frames, never dropped", "[frame_budget]") {
    // Twenty items of 1.2 ms each against 3 ms a frame: three a frame (the
    // third starts at 2.4 ms), seven frames, every item done.
    int remaining = 20;
    int frames = 0;
    int maxPerFrame = 0;
    while (remaining > 0 && frames < 100) {
        auto now = Clock::now();
        const FrameWorkBudget budget(3.0f, now);
        int done = 0;
        while (remaining > 0 && budget.allowNext(done, now)) {
            now += microseconds(1200);
            --remaining;
            ++done;
        }
        maxPerFrame = std::max(maxPerFrame, done);
        ++frames;
    }
    CHECK(remaining == 0);
    CHECK(maxPerFrame == 3);
    CHECK(frames == 7);
}

TEST_CASE("elapsed time is measured from the frame's start", "[frame_budget]") {
    const auto t0 = Clock::now();
    const FrameWorkBudget budget(2.0f, t0);
    CHECK(budget.elapsedMs(t0 + milliseconds(1)) == Catch::Approx(1.0f).margin(1e-3));
    CHECK(budget.budgetMs() == 2.0f);
}

// ── SharedFrameBudget: several queues under one frame budget ──

using wowee::core::SharedFrameBudget;

TEST_CASE("queues share one budget for the frame", "[frame_budget]") {
    SharedFrameBudget pacer(6.0f, 3);
    const auto t0 = Clock::now();
    pacer.beginFrame(t0);
    // Under the budget every queue may go, any number of items.
    CHECK(pacer.allow(0, 0, t0 + milliseconds(1)));
    CHECK(pacer.allow(0, 4, t0 + milliseconds(2)));
    CHECK(pacer.allow(1, 0, t0 + milliseconds(5)));
    // Once the frame's spawn work passes it, nobody starts another.
    CHECK_FALSE(pacer.allow(1, 1, t0 + milliseconds(6)));
    CHECK_FALSE(pacer.allow(2, 0, t0 + milliseconds(7)));
    pacer.endFrame();
    CHECK(pacer.starvedFrames(1) == 1);
    CHECK(pacer.starvedFrames(2) == 1);
    CHECK(pacer.starvedFrames(0) == 0);
}

TEST_CASE("a queue refused frame after frame gets one item", "[frame_budget]") {
    SharedFrameBudget pacer(6.0f, 3);
    auto now = Clock::now();
    // Queue 0 eats the budget every frame; queue 1, after it, is refused...
    for (int frame = 0; frame < 3; ++frame) {
        pacer.beginFrame(now);
        CHECK(pacer.allow(0, 0, now));
        CHECK_FALSE(pacer.allow(1, 0, now + milliseconds(8)));
        pacer.endFrame();
        now += milliseconds(16);
    }
    CHECK(pacer.starvedFrames(1) == 3);
    // ...until it has waited three frames: then its first item goes through,
    // over the budget, and only the first.
    pacer.beginFrame(now);
    CHECK(pacer.allow(0, 0, now));
    CHECK(pacer.allow(1, 0, now + milliseconds(8)));
    CHECK_FALSE(pacer.allow(1, 1, now + milliseconds(9)));
    pacer.endFrame();
}

TEST_CASE("a queue that was let through is no longer starved", "[frame_budget]") {
    SharedFrameBudget pacer(6.0f, 2);
    auto now = Clock::now();
    pacer.beginFrame(now);
    CHECK_FALSE(pacer.allow(3, 0, now + milliseconds(10)));
    pacer.endFrame();
    now += milliseconds(16);
    // Under the budget this frame: allowed, and not refused, so the count resets.
    pacer.beginFrame(now);
    CHECK(pacer.allow(3, 0, now + milliseconds(1)));
    pacer.endFrame();
    CHECK(pacer.starvedFrames(3) == 0);
}

TEST_CASE("an unknown queue is never held back", "[frame_budget]") {
    SharedFrameBudget pacer(1.0f, 3);
    const auto t0 = Clock::now();
    pacer.beginFrame(t0);
    CHECK(pacer.allow(-1, 5, t0 + milliseconds(50)));
    CHECK(pacer.allow(SharedFrameBudget::kMaxQueues, 5, t0 + milliseconds(50)));
}
