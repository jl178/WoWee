#pragma once

// Stock 3.3.5a (build 12340) animation selection rules that are not tied to
// one state machine: how a requested id becomes a sequence of the model (the
// AnimationData fallback chain and its play modes), which variation plays,
// where a moving animation starts, how fast it plays, and the pace a moving
// unit picks. Reverse engineered in docs/re/animation.md; pure functions,
// unit tested in tests/test_anim_selection.cpp.

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/sequence_variation.hpp"
#include "pipeline/m2_loader.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering::anim {

// ── Fallback chain (CM2Model 0x826350, called by SetBoneSequence 0x832AB0) ──
//
// A requested id the model lacks walks AnimationData's Fallback column until
// a row the model has. AnimationData.Flags along the way change how that
// sequence plays: a row with 0x10 reverses the direction (SitGroundUp ->
// SitGroundDown backwards), a row with 0x20 freezes it (SitGround ->
// SitGroundDown held on its last frame). The modes (0x826B00): 0 forward,
// 1 backward from the end, 2 held on the first frame, 3 held on the last.
// When the chain finds nothing the model plays Stand, else Closed (147),
// else its first sequence.

constexpr uint32_t kAnimFlagReverseFallback = 0x10;
constexpr uint32_t kAnimFlagHoldFallback = 0x20;

enum class PlayMode : uint8_t { Forward = 0, Reverse = 1, HoldFirst = 2, HoldLast = 3 };

struct FallbackResult {
    uint32_t id = 0;
    PlayMode mode = PlayMode::Forward;
    bool found = false;  ///< the id or a fallback of it; false = the default
};

/// `has(id)` says whether the model carries a sequence of that id;
/// `firstId` is the id of its first sequence (the last resort).
template <class Has>
FallbackResult resolveFallback(Has&& has, uint32_t id, uint32_t firstId) {
    FallbackResult r;
    if (has(id)) {
        r.id = id;
        r.found = true;
        return r;
    }
    int dir = 1;
    int acc = 0;
    bool seen[ANIM_COUNT] = {};
    uint32_t cur = id;
    while (cur < ANIM_COUNT && !seen[cur]) {
        const uint32_t fb = dbcFallback(cur);
        if (fb == UINT32_MAX || fb == cur) break;
        seen[cur] = true;
        const uint32_t flags = dbcAnimFlags(cur);
        if (flags & kAnimFlagReverseFallback) {
            acc += dir;
            dir = -dir;
        }
        if (flags & kAnimFlagHoldFallback) {
            acc += dir;
            dir = 0;
        }
        cur = fb;
        if (has(cur)) {
            r.id = cur;
            r.found = true;
            if (dir < 0) r.mode = PlayMode::Reverse;
            else if (dir == 0) r.mode = acc > 0 ? PlayMode::HoldLast : PlayMode::HoldFirst;
            return r;
        }
    }
    r.id = has(STAND) ? STAND : has(CLOSED) ? CLOSED : firstId;
    return r;
}

/// The time to sample in a sequence `durationMs` long, `elapsedMs` after it
/// started, in the given mode.
inline float playModeSampleTime(PlayMode mode, float elapsedMs, float durationMs) {
    switch (mode) {
        case PlayMode::Reverse: return durationMs > elapsedMs ? durationMs - elapsedMs : 0.0f;
        case PlayMode::HoldFirst: return 0.0f;
        case PlayMode::HoldLast: return durationMs;
        default: return elapsedMs;
    }
}

// ── Variations (CM2Model 0x826E60) ──────────────────────────────────────────
//
// Every play rolls (variation -1; CGUnit_C::PlayAnimation 0x7385C0 passes -1
// for one-shots and loops alike): rand() in [0, 0x7FFF] walks the chain from
// variation 0 along variationNext, taking off each frequency; the first
// variation the roll falls inside plays, and a roll beyond them all keeps
// variation 0. Variations whose keyframes are not loaded are passed over
// (Wowee loads .anim files on demand, the stock client loads them first).

inline int primaryVariationIndex(const std::vector<pipeline::M2Sequence>& seqs, uint32_t animId) {
    int first = -1;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animId) continue;
        if (seqs[i].variationIndex == 0) return i;
        if (first < 0) first = i;
    }
    return first;
}

inline int rollVariationStock(const std::vector<pipeline::M2Sequence>& seqs,
                              const std::vector<pipeline::M2Bone>& bones, uint32_t animId,
                              uint32_t roll) {
    const int primary = primaryVariationIndex(seqs, animId);
    if (primary < 0) return -1;
    int idx = primary;
    for (int guard = 0; guard < 64 && idx >= 0 && idx < static_cast<int>(seqs.size()); ++guard) {
        const uint32_t freq = static_cast<uint16_t>(seqs[idx].frequency);
        if (roll < freq) {
            return sequencePlayable(seqs, bones, idx) ? idx : primary;
        }
        roll -= freq;
        const int next = seqs[idx].nextAnimation;
        if (next < 0 || next == idx) break;
        idx = next;
    }
    return primary;
}

inline uint32_t stockRand() {
    return std::uniform_int_distribution<uint32_t>(0, 0x7FFF)(sequenceVariationRng());
}

/// The laps a sequence plays before its end (0x826B00): replayMin plus a
/// rand() share of the range, at least one. Only a sequence with flag 0x01
/// stops at its end; any other loops, and its end comes every lap.
inline uint32_t stockReplayLaps(const pipeline::M2Sequence& s, uint32_t roll) {
    const int span = static_cast<int>(s.replayMax) - static_cast<int>(s.replayMin);
    int laps = static_cast<int>(s.replayMin) + ((static_cast<int>(roll) * span) >> 15);
    return laps < 1 ? 1u : static_cast<uint32_t>(laps);
}
constexpr uint32_t kSeqFlagNonLooping = 0x01;

// ── Moving animations (0x714E80) and their pace (0x7385C0) ──────────────────

/// The animations a moving unit plays (by BehaviorID): Walk, Run, the
/// shuffles, Walkbackwards, the jumps, the swim strokes, StealthWalk, Fly,
/// Sprint, JumpLandRun and StealthRun. Only these get the speed rate and keep
/// their phase when one replaces another.
inline bool isMovingAnimBehavior(uint32_t b) {
    switch (b) {
        case 4: case 5: case 11: case 12: case 13: case 37: case 38: case 39:
        case 42: case 43: case 44: case 45: case 119: case 135: case 143: case 187: case 223:
            return true;
        default:
            return false;
    }
}
inline bool isMovingAnim(uint32_t id) { return isMovingAnimBehavior(dbcBehavior(id)); }

/// Where a moving animation starts when it replaces another moving one: the
/// same fraction of its cycle (0x7385C0: `old * newLen / oldLen % newLen`).
inline float carriedPhaseMs(float oldTimeMs, float oldDurationMs, float newDurationMs) {
    if (oldDurationMs <= 0.0f || newDurationMs <= 0.0f || oldTimeMs <= 0.0f) return 0.0f;
    const float t = oldTimeMs * newDurationMs / oldDurationMs;
    return std::fmod(t, newDurationMs);
}

/// The playback rate of a moving animation: the unit's speed over the
/// sequence's MoveSpeed (0x7385C0); 1 for an animation without one.
inline float moveSpeedRate(float speed, float sequenceMoveSpeed) {
    if (sequenceMoveSpeed == 0.0f || speed <= 0.0f) return 1.0f;
    return speed / std::abs(sequenceMoveSpeed);
}

}  // namespace wowee::rendering::anim
