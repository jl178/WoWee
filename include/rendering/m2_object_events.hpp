#pragma once

// Which of a sequence's events a frame passes, the stock way (CM2Model
// 0x832260 -> 0x830fb0): an event fires when its time in the playing
// sequence falls in (last frame, this frame] - so a sequence just started
// fires its stamp at 0 - every lap of a looping one, and in the direction it
// plays. Pure; tests/test_game_object_anim.cpp.

#include "pipeline/m2_loader.hpp"

#include <vector>

namespace wowee::rendering {

/// `from` and `t` are the sequence times of the last frame and this one,
/// `dur` its length; `fresh` when the sequence started since the last frame
/// (nothing of it was passed yet). `speed` is the playing speed: above 0
/// forward, below 0 backward, 0 held - a held pose fires nothing new, but a
/// one-shot that reached its end this frame (held there now) still passes its
/// last stretch, in the way it was going.
template <class Fire>
void passObjectEvents(const std::vector<pipeline::M2Model::ObjectEvent>& events, float from,
                      float t, float dur, bool fresh, float speed, Fire&& fire) {
    if (events.empty()) return;
    auto forward = [&](float lo, float hi) {
        for (const auto& ev : events) {
            const float ts = static_cast<float>(ev.timeMs);
            if (ts > lo && ts <= hi) fire(ev);
        }
    };
    auto back = [&](float hi, float lo) {
        for (auto e = events.rbegin(); e != events.rend(); ++e) {
            const float ts = static_cast<float>(e->timeMs);
            if (ts >= lo && ts < hi) fire(*e);
        }
    };
    if (fresh) {
        if (speed > 0.0f) forward(-1.0f, t);
        else if (speed < 0.0f) back(dur + 1.0f, t);
        return;
    }
    const int dir = speed > 0.0f ? 1 : speed < 0.0f ? -1 : (t > from ? 1 : t < from ? -1 : 0);
    if (dir > 0) {
        if (t >= from) {
            forward(from, t);
        } else {
            // A lap ended: the rest of the old lap, then the new one's start.
            forward(from, dur);
            forward(-1.0f, t);
        }
    } else if (dir < 0) {
        if (t <= from) {
            back(from, t);
        } else {
            back(from, 0.0f);
            back(dur + 1.0f, t);
        }
    }
}

}  // namespace wowee::rendering
