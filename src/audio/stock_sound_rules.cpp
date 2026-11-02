#include "audio/stock_sound_rules.hpp"

#include <glm/geometric.hpp>

#include <algorithm>

namespace wowee {
namespace audio {
namespace stock {

namespace {
// 0x9f2268: the per-type limit PlaySoundKit compares with the live count at
// 0xb4a340 (0x7fffffff for the named groups).
constexpr std::array<int, static_cast<size_t>(Category::Count)> kCaps = {
    0, 0, 0, 0, 0, 0, 1,  // SFX, MUSIC, AMBIENCE, CINEMATIC, SCRIPTSOUND, SCRIPTMUSIC, RACIALCINEMATIC
    1, 2, 1, 2, 2, 1, 6, 4, 1, 2, 4,
};
}  // namespace

int unitSoundChance(UnitSoundKind kind, bool player) {
    static constexpr int kCreature[13] = {70, 100, 60, 100, 100, 40, 100, 100, 100, 100, 100, 100, 100};
    static constexpr int kPlayer[13] = {35, 100, 30, 100, 100, 40, 100, 100, 100, 100, 100, 100, 100};
    const auto i = static_cast<size_t>(kind);
    if (i >= 13) return 100;
    return player ? kPlayer[i] : kCreature[i];
}

Category unitSoundCategory(UnitSoundKind kind, bool ownPlayer) {
    switch (kind) {
        case UnitSoundKind::Exertion:
        case UnitSoundKind::ExertionCritical:
            return Category::CreatureExertion;
        case UnitSoundKind::Injury:
        case UnitSoundKind::InjuryCritical:
        case UnitSoundKind::InjuryCrushingBlow:
            return ownPlayer ? Category::PlayerInjury : Category::CreatureInjury;
        default:
            return Category::Sfx;
    }
}

int categoryCap(Category c) {
    const auto i = static_cast<size_t>(c);
    return i < kCaps.size() ? kCaps[i] : 0;
}

Category volumeGroup(Category c) {
    return static_cast<uint8_t>(c) >= static_cast<uint8_t>(Category::ErrorSpeech) ? Category::Sfx : c;
}

float rolloffGain(float distance, float minDistance, float cutoff) {
    // 0x8782b0, argument order (min, max, distance).
    if (cutoff <= distance) return 0.0f;
    if (distance <= minDistance) return 1.0f;
    float gain = minDistance / ((distance - minDistance) * kRolloffScale + minDistance);
    const float fadeStart = cutoff * kCutoffFadeStart;
    if (fadeStart < distance) {
        gain *= 1.0f - (distance - fadeStart) / (cutoff - fadeStart);
    }
    return gain;
}

glm::vec3 listenerPosition(const glm::vec3& characterPosition, const glm::vec3& cameraForward) {
    return characterPosition - cameraForward * kListenerBackDist + glm::vec3(0.0f, 0.0f, kListenerUpDist);
}

float kitVolume(float rowVolume, uint32_t flags, float multiplier, std::mt19937& rng) {
    float v = rowVolume * multiplier;
    if (flags & kFlagRandomVolume) {
        v += std::uniform_real_distribution<float>(-kRandomVolumeSpread, kRandomVolumeSpread)(rng);
    }
    return std::clamp(v, 0.0f, 1.0f);  // 0x879710
}

float kitPitch(uint32_t flags, std::mt19937& rng) {
    if (!(flags & kFlagRandomPitch)) return 1.0f;
    return std::uniform_real_distribution<float>(kRandomPitchMin, kRandomPitchMax)(rng);
}

int pickKitFile(KitBag& bag, const std::vector<uint32_t>& freq, std::mt19937& rng) {
    const int count = static_cast<int>(std::min<size_t>(freq.size(), bag.remaining.size()));
    if (count <= 0) return -1;
    if (count == 1) {
        bag.last = 0;
        return 0;
    }
    uint32_t sum = 0;
    for (int pass = 0;; ++pass) {
        // The third look forgets the last pick: a kit whose only weight is on
        // the file it just played may play it again rather than nothing.
        if (pass == 2) bag.last = -1;
        else if (pass > 2) return -1;
        sum = 0;
        for (int i = 0; i < count; ++i) {
            if (i != bag.last) sum += bag.remaining[static_cast<size_t>(i)];
        }
        if (sum != 0) break;
        for (int i = 0; i < count; ++i) bag.remaining[static_cast<size_t>(i)] = freq[static_cast<size_t>(i)];
    }
    // (rand * sum) >> 32: uniform in [0, sum).
    int64_t r = std::uniform_int_distribution<uint32_t>(0, sum - 1)(rng);
    for (int i = 0; i < count; ++i) {
        if (i == bag.last) continue;
        r -= bag.remaining[static_cast<size_t>(i)];
        if (r < 0) {
            --bag.remaining[static_cast<size_t>(i)];
            bag.last = i;
            return i;
        }
    }
    return -1;
}

bool lessImportant(const Voice& a, const Voice& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.audibility < b.audibility;
}

VoiceDecision decideVoice(const std::vector<Voice>& active, const Voice& incoming, int realVoices,
                          bool noDuplicate) {
    VoiceDecision d;
    if (const int cap = categoryCap(incoming.category); cap > 0) {
        int n = 0;
        for (const Voice& v : active) n += v.category == incoming.category;
        if (n >= cap) {
            d.admit = false;
            d.reason = VoiceDecision::Reason::CategoryFull;
            return d;
        }
    }
    if (incoming.kitId != 0) {
        size_t sameKit = 0;
        size_t weakest = active.size();
        for (size_t i = 0; i < active.size(); ++i) {
            if (active[i].kitId != incoming.kitId) continue;
            if (noDuplicate) {
                d.admit = false;
                d.reason = VoiceDecision::Reason::Duplicate;
                return d;
            }
            ++sameKit;
            if (weakest == active.size() || lessImportant(active[i], active[weakest])) weakest = i;
        }
        if (static_cast<int>(sameKit) >= kMaxAudiblePerKit) d.evict.push_back(weakest);
    }
    const int used = static_cast<int>(active.size() - d.evict.size());
    if (used >= realVoices) {
        size_t weakest = active.size();
        for (size_t i = 0; i < active.size(); ++i) {
            if (std::find(d.evict.begin(), d.evict.end(), i) != d.evict.end()) continue;
            if (weakest == active.size() || lessImportant(active[i], active[weakest])) weakest = i;
        }
        if (weakest == active.size() || !lessImportant(active[weakest], incoming)) {
            d.admit = false;
            d.evict.clear();
            d.reason = VoiceDecision::Reason::NoVoice;
            return d;
        }
        d.evict.push_back(weakest);
    }
    return d;
}

bool isDaytime(float hours) {
    return hours >= 5.5f && hours < 21.0f;
}

}  // namespace stock
}  // namespace audio
}  // namespace wowee
