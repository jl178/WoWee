#include "audio/stock_sound_advanced.hpp"

#include <algorithm>

namespace wowee {
namespace audio {
namespace stock {

DuckGroup duckGroup(Category c) {
    // 0x4cb220: type 0 or above 6 reads the SFX level, 1 music, 2 ambience;
    // 3..6 take the instance volume alone.
    const auto t = static_cast<uint8_t>(c);
    if (t == 0 || t > 6) return DuckGroup::Sfx;
    if (t == 1) return DuckGroup::Music;
    if (t == 2) return DuckGroup::Ambience;
    return DuckGroup::None;
}

bool AdvancedKit::ducks() const {
    for (float d : duck) {
        if (d > 0.0f && d < 1.0f) return true;
    }
    return false;
}

AdvancedKit normaliseAdvancedKit(AdvancedKit k, bool callerLoops) {
    // 0x4cb580, in the order it does them: each of B, C, D before the time
    // preceding it is taken to be on the next day.
    for (int i = 1; i < 4; ++i) {
        if (k.time[i] < k.time[i - 1]) k.time[i] += kDayMs;
    }
    if (callerLoops) k.usage = AdvancedUsage::Loop;
    for (float& d : k.duck) {
        if (d < 0.0f || d > 1.0f) d = 1.0f;
    }
    if (k.outerRadiusOfInfluence < k.innerRadiusOfInfluence) k.innerRadiusOfInfluence = k.outerRadiusOfInfluence;
    if (k.outsideAngle < k.insideAngle) k.insideAngle = k.outsideAngle;
    return k;
}

float advancedPanLevel(const AdvancedKit& k, float distance) {
    if (k.outerRadius2D <= 0.0f) return 1.0f;
    // Compared squared in the client; the same for distances >= 0.
    if (distance <= k.innerRadius2D) return 0.0f;
    if (distance > k.outerRadius2D) return 1.0f;
    return (distance - k.innerRadius2D) / (k.outerRadius2D - k.innerRadius2D);
}

bool advancedDuckStarts(const AdvancedKit& k, float distance) {
    return k.outerRadius2D > 0.0f && distance <= k.innerRadiusOfInfluence && k.ducks();
}

namespace {
// Where the clock stands against a window that may run past midnight: a
// window whose shifted end is in the next day and whose shifted start is
// still ahead is looked at from the next day.
int32_t windowClock(const AdvancedKit& k, int32_t offset, int32_t now) {
    if (k.time[3] + offset > kDayMs - 1 && now < k.time[0] + offset) return now + kDayMs;
    return now;
}
}  // namespace

bool advancedInTimeWindow(const AdvancedKit& k, int32_t offset, int32_t now) {
    if (!k.hasTimeWindow()) return true;
    const int32_t t = windowClock(k, offset, now);
    return !(t < k.time[0] + offset || k.time[3] + offset < t);
}

float advancedTimeVolume(const AdvancedKit& k, int32_t offset, int32_t now) {
    if (!k.hasTimeWindow() || k.usage == AdvancedUsage::Once) return 1.0f;
    const int32_t t = windowClock(k, offset, now);
    float v;
    if (t < k.time[1] + offset) {
        const int32_t span = k.time[1] - k.time[0];
        v = span > 0 ? static_cast<float>(t - k.time[0]) / static_cast<float>(span) : 1.0f;
    } else if (t < k.time[2] + offset) {
        v = 1.0f;
    } else {
        const int32_t span = k.time[3] - k.time[2];
        v = span > 0 ? 1.0f - static_cast<float>(t - k.time[2]) / static_cast<float>(span) : 0.0f;
    }
    return std::clamp(v, 0.0f, 1.0f);
}

namespace {
int32_t randomBetween(int32_t lo, int32_t hi, std::mt19937& rng) {
    // 0x4c9d90: lo + (rand32 * (hi - lo)) >> 32, or hi when hi <= lo.
    if (hi <= lo) return hi;
    const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo);
    const uint64_t r = static_cast<uint64_t>(rng()) & 0xffffffffu;
    return lo + static_cast<int32_t>((r * span) >> 32);
}
}  // namespace

int32_t advancedRandomOffset(const AdvancedKit& k, std::mt19937& rng) {
    return randomBetween(-k.randomOffsetRange, k.randomOffsetRange, rng);
}

int32_t advancedRandomInterval(const AdvancedKit& k, std::mt19937& rng) {
    return randomBetween(k.intervalMinMs, k.intervalMaxMs, rng);
}

AdvancedStep advancedStep(const AdvancedKit& k, AdvancedSchedule& s, bool playing, bool inWindow,
                          size_t fileCount, int32_t dtMs, std::mt19937& rng) {
    if (k.usage == AdvancedUsage::Once) return AdvancedStep::Nothing;
    if (playing) {
        if (!inWindow) {
            s.started = true;
            return AdvancedStep::Stop;
        }
        // A loop with several files and an interval moves on to its next
        // file every interval, stopping the one it plays (0x4c9d50) first.
        if (k.usage == AdvancedUsage::Loop && fileCount > 1 && k.intervalMinMs > 0 && k.intervalMaxMs > 0) {
            s.countdownMs -= dtMs;
            if (s.countdownMs < 0) {
                s.countdownMs = advancedRandomInterval(k, rng);
                return AdvancedStep::Restart;
            }
        }
        return AdvancedStep::Nothing;
    }
    // A loop stopped by its window closing is over (0x4ca7b0 deletes an
    // instance that is not playing once usage 0 has set +0xc).
    if (k.usage == AdvancedUsage::Loop && s.started) return AdvancedStep::Nothing;
    if (!inWindow) return AdvancedStep::Nothing;
    if (k.usage == AdvancedUsage::Loop) return AdvancedStep::Start;
    // Usage 1: the interval runs while nothing plays.
    s.countdownMs -= dtMs;
    if (s.countdownMs < 0) {
        s.countdownMs = advancedRandomInterval(k, rng);
        return AdvancedStep::Start;
    }
    return AdvancedStep::Nothing;
}

void DuckMixer::duck(uint32_t owner, const AdvancedKit& kit) {
    if (owner == 0 || !kit.ducks()) return;
    for (const Ducker& d : duckers_) {
        if (d.owner == owner) return;
    }
    Ducker d;
    d.owner = owner;
    for (int g = 0; g < 3; ++g) d.target[g] = kit.duck[g];
    d.duckMs = kit.timeToDuckMs;
    d.unduckMs = kit.timeToUnduckMs;
    d.inner = kit.innerRadiusOfInfluence;
    d.outer = kit.outerRadiusOfInfluence;
    duckers_.push_back(d);
}

void DuckMixer::release(uint32_t owner) {
    if (owner == 0) return;
    for (Ducker& d : duckers_) {
        if (d.owner == owner) {
            d.owner = 0;
            d.ducking = false;
        }
    }
}

void DuckMixer::update(int32_t dtMs, const std::function<float(uint32_t)>& distance) {
    const float dt = static_cast<float>(std::max(dtMs, 0));
    for (auto it = duckers_.begin(); it != duckers_.end();) {
        Ducker& d = *it;
        for (int g = 0; g < 3; ++g) {
            float& level = d.level[g];
            if (d.owner == 0) {
                // Let go: back up to 1 over TimeToUnduck.
                if (level != 1.0f) {
                    if (d.unduckMs < 1) level = 1.0f;
                    else level = std::min(1.0f, level + dt / static_cast<float>(d.unduckMs));
                }
                continue;
            }
            const float dist = distance ? distance(d.owner) : -1.0f;
            if (dist < 0.0f || dist < d.inner || (dist < d.outer && d.ducking)) {
                if (d.target[g] != level) {
                    if (d.duckMs < 1) {
                        level = d.target[g];
                    } else {
                        level -= dt / static_cast<float>(d.duckMs);
                        if (level < d.target[g]) level = d.target[g];
                    }
                }
                d.ducking = true;
            } else {
                // Out of range: the instance lets go of its ducker, and the
                // groups after this one start back up in the same pass.
                d.ducking = false;
                release(d.owner);
            }
        }
        if (d.owner == 0 && d.level[0] == 1.0f && d.level[1] == 1.0f && d.level[2] == 1.0f) {
            it = duckers_.erase(it);
        } else {
            ++it;
        }
    }
}

float DuckMixer::level(DuckGroup group, uint32_t self) const {
    if (group == DuckGroup::None) return 1.0f;
    const auto g = static_cast<size_t>(group);
    float lowest = 1.0f;
    for (const Ducker& d : duckers_) {
        if (d.owner != 0 && d.owner == self) continue;
        lowest = std::min(lowest, d.level[g]);
    }
    return lowest;
}

}  // namespace stock
}  // namespace audio
}  // namespace wowee
