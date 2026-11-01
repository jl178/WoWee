#pragma once

/// Frame timing, in integer nanoseconds, and the wait that holds a frame to
/// its cap.
///
/// Both halves exist because of high refresh rates. At 60Hz a frame is 16.7ms
/// and a millisecond of slop in either is noise; at 144Hz the whole budget is
/// 6.94ms, and the same millisecond is fourteen per cent of it. Two things
/// were producing that slop:
///
///   - delta came from high_resolution_clock, which is an alias for
///     system_clock on libstdc++. That clock is not monotonic: an NTP
///     correction steps it, and a frame either jumps or comes out negative.
///   - the cap was a plain sleep_for to the target. Sleep overshoots by
///     around a millisecond on a good platform, and on Windows by the timer
///     resolution, which defaults to 15.6ms - more than two whole frames at
///     144Hz.
///
/// Delta is kept as integer nanoseconds and converted to seconds once, at
/// the end, so nothing accumulates in a float.

#include <cstdint>

namespace wowee::core {

class FramePacer {
public:
    /// Nanoseconds since the last call, from a monotonic clock. The first
    /// call establishes the baseline and returns 0.
    [[nodiscard]] std::int64_t tickNs();

    /// Seconds since the last tickNs, capped so that a stall - a breakpoint,
    /// a swapchain rebuild, a machine coming back from sleep - does not
    /// teleport everything that integrates over it.
    [[nodiscard]] static float toSeconds(std::int64_t deltaNs, float capSeconds = 0.1f);

    /// Hold until one frame at `fps` after the last tickNs. Sleeps for most
    /// of the wait and spins the last of it, because a sleep cannot be
    /// trusted to the precision a short frame needs. Does nothing when
    /// fps <= 0.
    ///
    /// How much to spin is learned rather than guessed: a sleep overshoots
    /// by an amount that differs by platform, by timer resolution and by
    /// load - half a millisecond on a good macOS run, over fifteen on
    /// Windows at the default tick - and a fixed margin is either late
    /// every frame or burns a core for nothing.
    void waitForCap(int fps);

    /// The clock reading the last tickNs took. The frame's start, without
    /// asking the clock a second time and getting a slightly different
    /// answer than the delta was measured against.
    [[nodiscard]] std::int64_t lastTickNs() const { return lastNs_; }

    /// The monotonic clock these all read, in nanoseconds.
    [[nodiscard]] static std::int64_t nowNs();

private:
    std::int64_t lastNs_ = 0;
    bool started_ = false;
    /// The running estimate of how late a sleep returns, which is what the
    /// spin has to cover. Starts at half a millisecond and climbs to
    /// whatever the platform is actually doing.
    std::int64_t sleepSlackNs_ = 500'000;
};

/// The delta the simulation steps by, made from the raw frame deltas.
///
/// A raw delta is how long the CPU took over the last frame, which is not
/// how long that frame was on screen. Under vsync at 60Hz a client running
/// at 30-35fps shows each frame for two refreshes or three, and a frame
/// that took 26ms and one that took 38ms can be shown for the same 33ms -
/// so stepping the camera, the character and every animation by the raw
/// figure moves them unevenly against a display that advances evenly, and
/// a single slow frame lands as a visible jump.
///
/// Three things, in order:
///   - an outlier is clipped to a multiple of the recent median before it
///     joins the history, so one hitch does not raise the average for the
///     next several frames;
///   - the step is the mean of the recent (clipped) deltas;
///   - whatever the clipping and averaging held back - or handed out early -
///     is owed, and a quarter of what is owed is paid each frame. Over any
///     stretch longer than a few frames the simulation advances by exactly
///     the wall time that passed, so nothing drifts against the server's
///     clock; only the shape of a spike is spread out.
///
/// Mouse-look is not stepped by this at all: it turns by the pointer's own
/// motion, so it stays one-to-one with the hand whatever the frame time.
class DeltaSmoother {
public:
    /// The step for this frame, given this frame's raw delta in seconds.
    /// The raw delta is expected to be capped already (FramePacer::toSeconds).
    [[nodiscard]] float smooth(float rawSeconds);
    /// Forget the history: after a loading screen, a stall or a toggle.
    void reset();

    /// How much time the smoother is holding back (positive) or has handed
    /// out early (negative). For the spike log.
    [[nodiscard]] float owedSeconds() const { return owed_; }

private:
    static constexpr int kWindow = 6;
    float history_[kWindow] = {};
    int count_ = 0;
    int next_ = 0;
    float owed_ = 0.0f;
};

/// Ask the platform for the finest sleep granularity it will give, for as
/// long as this object lives.
///
/// Windows only in substance: timeBeginPeriod. Without it a sleep rounds up
/// to the system timer tick, which defaults to 15.6ms - so a 144Hz cap
/// cannot be held at all, and even a 60Hz one is paced in lumps. Elsewhere
/// this is an empty object, since POSIX nanosleep already does better.
class SleepPrecisionScope {
public:
    SleepPrecisionScope();
    ~SleepPrecisionScope();
    SleepPrecisionScope(const SleepPrecisionScope&) = delete;
    SleepPrecisionScope& operator=(const SleepPrecisionScope&) = delete;

private:
#ifdef _WIN32
    // Only Windows has anything to raise or put back, and an unused member
    // is an error under this build's warning settings.
    bool raised_ = false;
#endif
};

}  // namespace wowee::core
