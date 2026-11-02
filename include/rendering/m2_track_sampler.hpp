#pragma once

#include "pipeline/m2_loader.hpp"

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::m2_track {

struct SampleTime {
    int sequenceIndex = -1;
    float timeMs = 0.0f;
};

inline SampleTime resolveTime(const pipeline::M2AnimationTrack& track,
                              int animationSequenceIndex, float animationTimeMs,
                              float globalTimeMs,
                              const std::vector<uint32_t>& globalSequenceDurations) {
    if (track.globalSequence >= 0 &&
        static_cast<size_t>(track.globalSequence) < globalSequenceDurations.size()) {
        const float duration =
            static_cast<float>(globalSequenceDurations[track.globalSequence]);
        float time = duration > 0.0f ? std::fmod(globalTimeMs, duration) : 0.0f;
        if (time < 0.0f) time += duration;
        return {.sequenceIndex = 0, .timeMs = time};
    }
    return {.sequenceIndex = animationSequenceIndex, .timeMs = animationTimeMs};
}

/// The key arrays a sample reads, as the stock client picks them (key
/// search 0x8284d0, evaluators 0x82af40/0x82b0a0/0x82b340): a global-sequence
/// track reads array 0; any other reads the playing sequence's array, and
/// array 0 when the track has fewer arrays than that index. Thousands of
/// tracks carry a single array for a model with many sequences (texture
/// weights, colour alphas, ribbon colours, attachment visibility), and the
/// value they hold applies to every sequence. Returns nullptr for no
/// sequence (index < 0) or a track with no arrays: the caller's default.
inline const pipeline::M2AnimationTrack::SequenceKeys*
keysFor(const pipeline::M2AnimationTrack& track, const SampleTime& sampleTime) {
    if (sampleTime.sequenceIndex < 0 || track.sequences.empty()) return nullptr;
    size_t idx = static_cast<size_t>(sampleTime.sequenceIndex);
    if (idx >= track.sequences.size()) idx = 0;
    return &track.sequences[idx];
}

inline size_t lowerKeyIndex(const std::vector<uint32_t>& timestamps,
                            size_t keyCount, float timeMs) {
    keyCount = std::min(keyCount, timestamps.size());
    if (keyCount <= 1 || timeMs <= static_cast<float>(timestamps[0])) return 0;
    const auto end = timestamps.begin() + static_cast<std::ptrdiff_t>(keyCount);
    const auto upper = std::upper_bound(
        timestamps.begin(), end, timeMs,
        [](float time, uint32_t timestamp) {
            return time < static_cast<float>(timestamp);
        });
    if (upper == end) return keyCount - 1;
    return static_cast<size_t>(upper - timestamps.begin() - 1);
}

/// lowerKeyIndex, starting from where the last search on this track ended.
///
/// A track sampled every frame moves forward by a fraction of a key, so the
/// answer is almost always the key it was last frame or the one after. Both
/// are checked before falling back to the binary search, and `cursor` is left
/// on the answer. For timestamps in order - which is what the format stores
/// and what the binary search already relies on - the answer is the one
/// lowerKeyIndex gives: key h is the last key at or before the time exactly
/// when ts[h] <= t < ts[h + 1] (or h is the last key). The comparisons are made
/// on the same float conversions, so a time between two keys never rounds to
/// a different side.
inline size_t lowerKeyIndexFrom(const std::vector<uint32_t>& timestamps,
                                size_t keyCount, float timeMs, uint32_t& cursor) {
    keyCount = std::min(keyCount, timestamps.size());
    if (keyCount <= 1 || timeMs <= static_cast<float>(timestamps[0])) {
        cursor = 0;
        return 0;
    }
    const auto isLower = [&](size_t h) {
        return h < keyCount && !(timeMs < static_cast<float>(timestamps[h])) &&
               (h + 1 == keyCount || timeMs < static_cast<float>(timestamps[h + 1]));
    };
    const size_t h = cursor;
    if (isLower(h)) return h;
    if (isLower(h + 1)) {
        cursor = static_cast<uint32_t>(h + 1);
        return h + 1;
    }
    const size_t lower = lowerKeyIndex(timestamps, keyCount, timeMs);
    cursor = static_cast<uint32_t>(lower);
    return lower;
}

/// The search a sampler makes: from the cursor when there is one.
inline size_t findLowerKey(const std::vector<uint32_t>& timestamps, size_t keyCount,
                           float timeMs, uint32_t* cursor) {
    return cursor ? lowerKeyIndexFrom(timestamps, keyCount, timeMs, *cursor)
                  : lowerKeyIndex(timestamps, keyCount, timeMs);
}

inline float interpolationFraction(const pipeline::M2AnimationTrack& track,
                                   const std::vector<uint32_t>& timestamps,
                                   size_t lower, size_t keyCount, float timeMs) {
    // Type 0 holds the lower key. Every other type is linear: the stock
    // evaluators for bone, colour, alpha, texture-transform, light, ribbon
    // and particle tracks (0x82af40, 0x82b0a0, 0x82b340, 0x82ad50) have no
    // Hermite or Bezier case, and these tracks store plain values with no
    // tangents. Only camera tracks are spline keys; see splineSegment.
    // Past the last key the last value holds; before the first (no stored
    // multi-key track starts after 0) the first value holds.
    if (track.interpolationType == 0 || lower + 1 >= keyCount) return 0.0f;
    const float t0 = static_cast<float>(timestamps[lower]);
    const float t1 = static_cast<float>(timestamps[lower + 1]);
    return t1 > t0 ? glm::clamp((timeMs - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
}

/// Linear interpolation of two rotation keys as the stock client does it
/// (0x982630): a component-wise lerp, then normalised. There is no turn
/// to the shorter arc, so two keys on opposite hemispheres go the long way
/// round, as authored; the data never relies on a flip being made for it.
inline glm::quat nlerpKeys(const glm::quat& a, const glm::quat& b, float t) {
    const glm::quat q(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                      a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    const float len2 = glm::dot(q, q);
    return len2 > 1e-12f ? q * (1.0f / std::sqrt(len2)) : a;
}

/// One segment of a camera spline track (M2SplineKey: value, inTan, outTan),
/// as the stock client evaluates it (0x82b460 vec3, 0x82b8a0 float). Type 1
/// is linear; type 2 is a cubic Bezier whose control points are the lower
/// key's outTan and the upper key's inTan; type 3 is Hermite with those
/// tangents taken as they are (not scaled by the segment length).
template <class T>
inline T splineSegment(int interpolationType, const T& p0, const T& outTan0,
                       const T& inTan1, const T& p1, float t) {
    if (interpolationType == 0) return p0;
    if (interpolationType == 2) {
        const float t2 = t * t, t3 = t2 * t;
        return p0 * (3.0f * t2 - t3 - 3.0f * t + 1.0f) + inTan1 * (3.0f * t2 - 3.0f * t3) +
               outTan0 * (3.0f * t3 - 6.0f * t2 + 3.0f * t) + p1 * t3;
    }
    if (interpolationType == 3) {
        const float t2 = t * t, t3 = t2 * t;
        return p1 * (3.0f * t2 - 2.0f * t3) + p0 * (2.0f * t3 - 3.0f * t2 + 1.0f) +
               inTan1 * (t3 - t2) + outTan0 * (t3 - 2.0f * t2 + t);
    }
    return p0 + (p1 - p0) * t;
}

/// The weight of the incoming sequence while two are blended, from the
/// fraction of the blend time gone by. The stock client eases it
/// (0x82f0f0: the outgoing weight is 3x^2 - 2x^3 of the fraction left), so
/// the incoming one is the same smoothstep of the fraction elapsed.
inline float sequenceBlendWeight(float elapsedFraction) {
    const float x = glm::clamp(elapsedFraction, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/// Whether a float track can give more than one value: some key, or the
/// default an empty sequence gives, differs from the rest. A track that
/// cannot is a constant and is baked once; one that can is played per frame,
/// as the stock client plays every track of every model (0x82f0f0).
inline bool trackVaries(const pipeline::M2AnimationTrack& track, float defaultValue) {
    bool have = false;
    float first = 0.0f;
    const auto see = [&](float v) {
        if (!have) { first = v; have = true; return false; }
        return v != first;
    };
    for (const auto& keys : track.sequences) {
        const size_t n = std::min(keys.timestamps.size(), keys.floatValues.size());
        if (n == 0) {
            if (see(defaultValue)) return true;
            continue;
        }
        for (size_t k = 0; k < n; ++k)
            if (see(keys.floatValues[k])) return true;
    }
    return false;
}

/// The one value of a track trackVaries says is constant: its key, or the
/// default when it has none.
inline float trackConstant(const pipeline::M2AnimationTrack& track, float defaultValue) {
    for (const auto& keys : track.sequences) {
        if (!keys.timestamps.empty() && !keys.floatValues.empty()) return keys.floatValues[0];
    }
    return defaultValue;
}

inline float sampleFloat(const pipeline::M2AnimationTrack& track,
                         int animationSequenceIndex, float animationTimeMs,
                         float globalTimeMs,
                         const std::vector<uint32_t>& globalSequenceDurations,
                         float defaultValue) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    const auto* keysPtr = keysFor(track, sampleTime);
    if (!keysPtr) return defaultValue;
    const auto& keys = *keysPtr;
    const size_t count = std::min(keys.timestamps.size(), keys.floatValues.size());
    if (count == 0) return defaultValue;
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? glm::mix(keys.floatValues[lower], keys.floatValues[lower + 1], fraction)
        : keys.floatValues[lower];
}

inline glm::vec3 sampleVec3(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations,
                            const glm::vec3& defaultValue, uint32_t* cursor = nullptr) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    const auto* keysPtr = keysFor(track, sampleTime);
    if (!keysPtr) return defaultValue;
    const auto& keys = *keysPtr;
    const size_t count = std::min(keys.timestamps.size(), keys.vec3Values.size());
    if (count == 0) return defaultValue;
    const auto safe = [&](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            ? value : defaultValue;
    };
    const size_t lower = findLowerKey(keys.timestamps, count, sampleTime.timeMs, cursor);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? safe(glm::mix(safe(keys.vec3Values[lower]), safe(keys.vec3Values[lower + 1]), fraction))
        : safe(keys.vec3Values[lower]);
}

inline glm::quat sampleQuat(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations,
                            uint32_t* cursor = nullptr) {
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    const auto* keysPtr = keysFor(track, sampleTime);
    if (!keysPtr) return identity;
    const auto& keys = *keysPtr;
    const size_t count = std::min(keys.timestamps.size(), keys.quatValues.size());
    if (count == 0) return identity;
    const auto safe = [&](const glm::quat& value) {
        const float lengthSquared = glm::dot(value, value);
        return std::isfinite(lengthSquared) && lengthSquared >= 0.000001f
            ? glm::normalize(value) : identity;
    };
    const size_t lower = findLowerKey(keys.timestamps, count, sampleTime.timeMs, cursor);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? nlerpKeys(safe(keys.quatValues[lower]), safe(keys.quatValues[lower + 1]), fraction)
        : safe(keys.quatValues[lower]);
}

} // namespace wowee::rendering::m2_track
