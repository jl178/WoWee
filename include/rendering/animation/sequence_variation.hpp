#pragma once

// Stock 3.3.5a sequence variations: which of an animation's variations plays
// next, and how many times it repeats.
//
// An M2 animation id can have several sequences (variations). Each carries a
// `frequency` weight; across one id they sum to 0x7FFF (HumanMale Stand:
// 9830, 2184, 2184, 18569). Each time the playing variation finishes a lap
// the client rolls a new one by those weights - that is where the idle
// fidgets come from: the low-frequency Stand variations. A variation with a
// `replay` range (HumanMale Stand variation 1: 2-7) plays that many laps
// before the next roll. A sequence with flag 0x40 is an alias: it has no
// keyframes of its own and plays the sequence its `aliasNext` names. Without
// flag 0x20 the keyframes live in an .anim file that may not have been
// loaded, and such a variation is passed over unless its data is there.

#include "pipeline/m2_loader.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace wowee::rendering::anim {

constexpr uint32_t kSeqFlagEmbedded = 0x20;  ///< keyframes are in the .m2
constexpr uint32_t kSeqFlagAlias = 0x40;     ///< plays the sequence aliasNext names

inline std::mt19937& sequenceVariationRng() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return rng;
}

/// The sequence whose keyframes play for `idx`: an alias (flag 0x40) is
/// followed through aliasNext to the sequence that holds the data.
inline int resolveSequenceAlias(const std::vector<pipeline::M2Sequence>& seqs, int idx) {
    for (int guard = 0; guard < 8; ++guard) {
        if (idx < 0 || idx >= static_cast<int>(seqs.size())) return idx;
        const auto& s = seqs[idx];
        if ((s.flags & kSeqFlagAlias) == 0) return idx;
        const int next = static_cast<int>(s.aliasNext);
        if (next == idx || next >= static_cast<int>(seqs.size())) return idx;
        idx = next;
    }
    return idx;
}

/// Whether any bone has keyframes for sequence `idx` (the .anim of a sequence
/// without flag 0x20 is loaded only for some models and animations).
inline bool sequenceHasBoneData(const std::vector<pipeline::M2Bone>& bones, int idx) {
    if (idx < 0) return false;
    const size_t i = static_cast<size_t>(idx);
    for (const auto& b : bones) {
        if (i < b.rotation.sequences.size() && !b.rotation.sequences[i].timestamps.empty()) return true;
        if (i < b.translation.sequences.size() && !b.translation.sequences[i].timestamps.empty()) return true;
    }
    return false;
}

/// Whether sequence `idx` can be played: embedded, or its .anim was loaded.
/// A model with no bone data at all has nothing to check and plays anything.
inline bool sequencePlayable(const std::vector<pipeline::M2Sequence>& seqs,
                             const std::vector<pipeline::M2Bone>& bones, int idx) {
    idx = resolveSequenceAlias(seqs, idx);
    if (idx < 0 || idx >= static_cast<int>(seqs.size())) return false;
    if (bones.empty() || (seqs[idx].flags & kSeqFlagEmbedded) != 0) return true;
    return sequenceHasBoneData(bones, idx);
}

/// Roll a variation of `animId` by the sequences' frequency weights. Returns
/// the sequence index as listed (aliases unresolved: the caller resolves it
/// for playback), or -1 when the model has no sequence of that id. Only
/// playable variations take part; if all the weights are zero the first
/// (variation 0) plays.
inline int pickSequenceVariation(const std::vector<pipeline::M2Sequence>& seqs,
                                 const std::vector<pipeline::M2Bone>& bones, uint32_t animId) {
    int first = -1;
    int primary = -1;
    int count = 0;
    uint32_t total = 0;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animId) continue;
        if (first < 0) first = i;
        if (!sequencePlayable(seqs, bones, i)) continue;
        if (primary < 0 || seqs[i].variationIndex == 0) {
            if (primary < 0 || seqs[primary].variationIndex != 0) primary = i;
        }
        ++count;
        total += static_cast<uint32_t>(seqs[i].frequency > 0 ? seqs[i].frequency : 0);
    }
    if (count == 0) return first;
    if (count == 1 || total == 0) return primary;
    std::uniform_int_distribution<uint32_t> dist(0, total - 1);
    uint32_t roll = dist(sequenceVariationRng());
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animId || !sequencePlayable(seqs, bones, i)) continue;
        const uint32_t w = static_cast<uint32_t>(seqs[i].frequency > 0 ? seqs[i].frequency : 0);
        if (roll < w) return i;
        roll -= w;
    }
    return primary;
}

/// Extra laps the variation plays after this one, from its replay range
/// (the range counts laps in all; 0-0 is one lap).
inline uint32_t pickSequenceExtraLaps(const pipeline::M2Sequence& s) {
    if (s.replayMax == 0 || s.replayMax < s.replayMin) return 0;
    std::uniform_int_distribution<uint32_t> dist(s.replayMin, s.replayMax);
    const uint32_t laps = dist(sequenceVariationRng());
    return laps > 0 ? laps - 1 : 0;
}

/// Whether `animId` has more than one variation on this model.
inline bool hasSequenceVariations(const std::vector<pipeline::M2Sequence>& seqs, uint32_t animId) {
    int n = 0;
    for (const auto& s : seqs) {
        if (s.id == animId && ++n > 1) return true;
    }
    return false;
}

}  // namespace wowee::rendering::anim
