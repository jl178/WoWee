#pragma once

/// A frame's share of main-thread work that can wait for the next frame.
///
/// The spawn queues finish work the workers have prepared - a player's model
/// and textures uploaded, a creature's instance built - and each item costs
/// a millisecond or two. Done all at once, a crowd arriving (Dalaran, a
/// battleground's start) turns into one 40 ms frame; done one item per frame,
/// the crowd takes seconds to appear. This is the middle: keep taking items
/// while the frame has spent less than the budget on them, and always take
/// the first, so a queue never stalls behind a budget smaller than one item.
///
/// The clock is passed in so the rule can be tested without sleeping.

#include <chrono>

namespace wowee {
namespace core {

class FrameWorkBudget {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameWorkBudget(float budgetMs, Clock::time_point start = Clock::now())
        : start_(start), budgetMs_(budgetMs) {}

    /// Milliseconds spent since the budget was opened.
    [[nodiscard]] float elapsedMs(Clock::time_point now = Clock::now()) const {
        return std::chrono::duration<float, std::milli>(now - start_).count();
    }

    /// Whether another item may start, given how many this frame has done.
    /// The first is always allowed; after it, only while time is left.
    [[nodiscard]] bool allowNext(int doneSoFar, Clock::time_point now = Clock::now()) const {
        if (doneSoFar <= 0) return true;
        return elapsedMs(now) < budgetMs_;
    }

    [[nodiscard]] float budgetMs() const { return budgetMs_; }

private:
    Clock::time_point start_;
    float budgetMs_;
};

}  // namespace core
}  // namespace wowee

namespace wowee {
namespace core {

/// One frame's budget shared by several queues, with a floor for each.
///
/// The spawn queues each had a budget of their own - two milliseconds for the
/// creatures, two for the NPC skins, three for the players, three for the
/// equipment, six for a game object's building - and each always took one
/// item, so a crowd arriving with everything in flight could still add their
/// sum to one frame. This puts them under one budget for the frame: a queue
/// starts an item only while the frame's spawn work is under it. So that a
/// queue late in the order is not starved for good by the ones before it, a
/// queue that was refused for `maxStarvedFrames` frames in a row is let
/// through for one item regardless.
class SharedFrameBudget {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxQueues = 16;

    explicit SharedFrameBudget(float budgetMs = 6.0f, int maxStarvedFrames = 3)
        : budgetMs_(budgetMs), maxStarved_(maxStarvedFrames) {}

    void beginFrame(Clock::time_point now = Clock::now()) {
        start_ = now;
        for (int i = 0; i < kMaxQueues; ++i) refusedThisFrame_[i] = false;
    }

    /// Whether queue `q` may start another item, given how many it has done
    /// this frame. Call only when the queue has work.
    bool allow(int q, int doneSoFar, Clock::time_point now = Clock::now()) {
        if (q < 0 || q >= kMaxQueues) return true;
        const float spent = std::chrono::duration<float, std::milli>(now - start_).count();
        if (spent < budgetMs_) return true;
        // Over the budget: one item for a queue that has waited long enough.
        if (doneSoFar == 0 && starved_[q] >= maxStarved_) return true;
        refusedThisFrame_[q] = true;
        return false;
    }

    /// Close the frame: a queue refused in it has waited one frame more; any
    /// other has not waited at all.
    void endFrame() {
        for (int i = 0; i < kMaxQueues; ++i) {
            starved_[i] = refusedThisFrame_[i] ? starved_[i] + 1 : 0;
        }
    }

    [[nodiscard]] int starvedFrames(int q) const { return (q >= 0 && q < kMaxQueues) ? starved_[q] : 0; }
    [[nodiscard]] float budgetMs() const { return budgetMs_; }

private:
    Clock::time_point start_{};
    float budgetMs_;
    int maxStarved_;
    int starved_[kMaxQueues] = {};
    bool refusedThisFrame_[kMaxQueues] = {};
};

}  // namespace core
}  // namespace wowee
