#pragma once

// SoundEntriesAdvanced: what the stock 3.3.5a client (build 12340) does with
// a kit whose SoundEntries row names an advanced row - the 2D/3D blend by
// distance, the time-of-day window, the replay interval and the ducking of
// the other sounds. Read out of SoundInterface2AdvancedKitProperties.cpp in
// Wow.exe; the addresses are in docs/re/sound-engine.md. Pure, like
// stock_sound_rules.hpp: AudioEngine and the emitters apply it.

#include "audio/stock_sound_rules.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace wowee {
namespace audio {
namespace stock {

/// The game clock wraps here (ms in a day).
constexpr int32_t kDayMs = 86400000;
/// Usage 0 (loop) instances play with these fades (0x4c9dc0: 0x9ebbc4, or
/// 0x9e2ec0 when the caller asks for the short one).
constexpr float kAdvancedLoopFadeSeconds = 3.0f;
constexpr float kAdvancedLoopShortFadeSeconds = 0.7f;
/// Usage 1 and 2 (one-shots) fade over this: at once (0x9f1968).
constexpr float kAdvancedOneShotFadeSeconds = 0.01f;
/// A positional instance of a kit is refused while another instance of the
/// same kit is closer than this - squared yards (0x4ca130, 0x9e8cf8).
constexpr float kAdvancedDuplicateDistanceSq = 6.0f;

/// SoundEntriesAdvanced.Usage (instance +0x58).
enum class AdvancedUsage : uint8_t {
    Loop = 0,      ///< loops while in its time window; a new file every interval
    Interval = 1,  ///< a one-shot replayed every TimeIntervalMin..Max while in the window
    Once = 2,      ///< played at once, a single time
};

/// The channel groups an advanced row can pull down. The volume callback
/// (0x4cb220) maps the category of every playing sound onto one of these.
enum class DuckGroup : uint8_t { Sfx = 0, Music = 1, Ambience = 2, None = 3 };
/// Category 0 and 7..17 mix in SFX, 1 MUSIC, 2 AMBIENCE; 3..6 (cinematic,
/// script sound and music, racial cinematic) are never ducked.
DuckGroup duckGroup(Category c);

/// One SoundEntriesAdvanced row as an instance holds it (0x4cb580 copies the
/// row into the instance, normalising as it goes - see normaliseAdvancedKit).
struct AdvancedKit {
    uint32_t id = 0;
    uint32_t soundEntryId = 0;
    /// Pan level 0 (unpanned, in the head) inside InnerRadius2D, 1 (full 3D)
    /// beyond OuterRadius2D, linear between. No blend when OuterRadius2D <= 0.
    float innerRadius2D = 0.0f;
    float outerRadius2D = 0.0f;
    /// TimeA..D, game-clock ms of the day. After normalising each is at or
    /// after the one before, a day added where a time wraps past midnight.
    int32_t time[4] = {0, 0, 0, 0};
    /// The window is shifted by a random [-range, range) ms per instance.
    int32_t randomOffsetRange = 0;
    AdvancedUsage usage = AdvancedUsage::Once;
    int32_t intervalMinMs = 0;
    int32_t intervalMaxMs = 0;
    /// VolumeSliderCategory: the category the instance plays in (props[0]).
    Category category = Category::Sfx;
    /// DuckToSFX, DuckToMusic, DuckToAmbience. A value outside [0, 1] is 1.
    float duck[3] = {1.0f, 1.0f, 1.0f};
    /// Ducking starts inside InnerRadiusOfInfluence and holds out to
    /// OuterRadiusOfInfluence once started. Inner is clamped to outer.
    float innerRadiusOfInfluence = 0.0f;
    float outerRadiusOfInfluence = 0.0f;
    int32_t timeToDuckMs = 0;
    int32_t timeToUnduckMs = 0;
    /// FMOD cone: InsideAngle (clamped to OutsideAngle), OutsideAngle,
    /// OutsideVolume. Needs an orientation the callers do not supply.
    float insideAngle = 180.0f;
    float outsideAngle = 270.0f;
    float outsideVolume = 0.0f;

    /// Any of TimeA..D set: the instance has a time window.
    [[nodiscard]] bool hasTimeWindow() const { return time[0] || time[1] || time[2] || time[3]; }
    /// Some group would be pulled down: a duck level strictly inside (0, 1)
    /// (0x4ca1e0 makes no ducker otherwise).
    [[nodiscard]] bool ducks() const;
};

/// The row as 0x4cb580 copies it into an instance: B..D each moved a day on
/// when before the time preceding it, duck levels outside [0, 1] made 1,
/// InnerRadiusOfInfluence and InsideAngle clamped to their outer partners.
/// `callerLoops` is the caller forcing a loop (props[7] == 1), which makes
/// the instance usage 0 whatever the row says.
AdvancedKit normaliseAdvancedKit(AdvancedKit raw, bool callerLoops = false);

/// The 3D pan level at `distance` yards from the listener (0x4ca7b0 ->
/// Channel::set3DPanLevel 0x8787b0).
float advancedPanLevel(const AdvancedKit& kit, float distance);

/// True when a playing instance at `distance` starts ducking this frame:
/// only rows with an OuterRadius2D look, and only inside
/// InnerRadiusOfInfluence (0x4ca7b0). Usage 2 instances also duck the
/// moment they start, at any distance (0x4cb580).
bool advancedDuckStarts(const AdvancedKit& kit, float distance);

/// Inside the instance's time window at `nowMs` (0x4c9cf0). No window is
/// always inside.
bool advancedInTimeWindow(const AdvancedKit& kit, int32_t offsetMs, int32_t nowMs);

/// The instance's time-of-day volume (instance +0x54, 0x4ca7b0): ramps
/// 0 -> 1 from TimeA to TimeB, holds to TimeC, ramps 1 -> 0 to TimeD, and 0
/// outside. 1 for a row with no window. As in the client the window's start
/// and end are shifted by the offset and the ramps themselves are not.
float advancedTimeVolume(const AdvancedKit& kit, int32_t offsetMs, int32_t nowMs);

/// The two random draws an instance makes (0x4c9d90: min + rand * (max -
/// min) >> 32, or max when max <= min): the window offset in
/// [-range, range) and the next replay interval in [min, max).
int32_t advancedRandomOffset(const AdvancedKit& kit, std::mt19937& rng);
int32_t advancedRandomInterval(const AdvancedKit& kit, std::mt19937& rng);

/// What an instance's owner should do this frame (0x4ca7b0's state
/// machine, for the sounds an owner keeps - the SoundEmitters).
struct AdvancedSchedule {
    int32_t countdownMs = 0;  ///< instance +0x5c
    bool started = false;     ///< instance +0xc: a usage-0 instance played once
};
enum class AdvancedStep : uint8_t {
    Nothing,   ///< leave it
    Start,     ///< start the next file
    Restart,   ///< stop what plays (its own fade) and start the next file
    Stop,      ///< out of the window: fade out what plays
};
/// `playing`: the instance's sound is sounding; `fileCount`: files in the
/// kit; `inWindow`: advancedInTimeWindow. Usage 0 starts at once inside the
/// window and, with more than one file and an interval, changes file every
/// interval; usage 1 waits an interval between one-shots, counting only
/// while nothing plays; usage 2 is played by whoever asked and never here.
AdvancedStep advancedStep(const AdvancedKit& kit, AdvancedSchedule& s, bool playing, bool inWindow,
                          size_t fileCount, int32_t dtMs, std::mt19937& rng);

/// The duckers (0x4ca1e0 makes one, 0x4ca4f0 moves them each frame,
/// 0x4c9fe0 reads them, 0x4ca080 lets go of one). Each holds a level per
/// group that starts at 1, falls to the row's DuckTo* over TimeToDuck while
/// its instance is in range and climbs back to 1 over TimeToUnduck once the
/// instance is gone or out of range. Every sound is mixed at the lowest
/// level any ducker holds for its group - except that an instance is never
/// ducked by its own ducker.
class DuckMixer {
public:
    /// Start ducking for instance `owner` (non-zero), unless the row pulls
    /// nothing down or the owner already has a ducker.
    void duck(uint32_t owner, const AdvancedKit& kit);
    /// The instance is gone: its ducker lets go and climbs back up.
    void release(uint32_t owner);
    /// Advance by dtMs. `distance(owner)` is the instance's distance from the
    /// listener in yards, or negative for a 2D instance, which is always in
    /// range.
    void update(int32_t dtMs, const std::function<float(uint32_t owner)>& distance);
    /// The level a sound of `group` owned by instance `self` (0 for a sound
    /// with no instance) is mixed at. 1 for DuckGroup::None.
    [[nodiscard]] float level(DuckGroup group, uint32_t self = 0) const;
    [[nodiscard]] bool empty() const { return duckers_.empty(); }
    [[nodiscard]] size_t size() const { return duckers_.size(); }
    void clear() { duckers_.clear(); }

private:
    struct Ducker {
        uint32_t owner = 0;
        float level[3] = {1.0f, 1.0f, 1.0f};
        float target[3] = {1.0f, 1.0f, 1.0f};
        int32_t duckMs = 0;
        int32_t unduckMs = 1000;
        float inner = 0.0f;
        float outer = 0.0f;
        bool ducking = false;  ///< instance +0x80
    };
    std::vector<Ducker> duckers_;
};

}  // namespace stock
}  // namespace audio
}  // namespace wowee
