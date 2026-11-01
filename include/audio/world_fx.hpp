#pragma once

/// The effects the world's sounds pass through before the speakers: the
/// room's reverb and the muffling of being under water.
///
/// The stock client handed SoundProviderPreferences.dbc rows - EAX 2 room
/// descriptions - to its sound provider, and the provider did the rest. There
/// is no EAX here, so this is a small reverb of its own, driven by the same
/// rows: a Schroeder/Freeverb network of six damped combs and four allpasses
/// per side, a few hundred multiply-adds per frame of audio.
///
/// Interface sounds and music do not come through here; see AudioEngine's
/// world group. Pure arithmetic with no miniaudio in it, so a test can feed it
/// numbers and read them back.

#include "audio/sound_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace audio {

/// What the reverb is asked to sound like.
struct ReverbSettings {
    float wet = 0.0f;        ///< level of the tail against the dry sound, linear; 0 is off
    float decayTime = 1.0f;  ///< seconds for the tail to fall 60 dB
    float damping = 0.3f;    ///< 0 bright .. 0.85 dark: high frequencies lost on each pass
    float preDelay = 0.02f;  ///< seconds of silence before the tail starts
};

/// The reverb an EAX 2 room description asks for.
///
/// The tail's level is the room's master level plus its late-reverb level,
/// both in millibels: PRESET_GENERIC is -1000 + 200, so -8 dB; a cave -14 dB;
/// the open-air presets (forest, plain, mountains) -12 to -29 dB, a hint of
/// distance rather than a room; PRESET_OFF's -10000 is silence. That is then
/// scaled down and capped: EAX hardware played these against far drier mixes
/// than this client's, and at full level every cave turned into a cathedral.
/// With WorldFx's normalisation a steady sound in PRESET_GENERIC carries a
/// tail about 11 dB under it, a cave 17 dB, and a single footstep far less.
/// RoomHF and DecayHFRatio together say how dark the tail is.
inline ReverbSettings reverbFromProvider(const SoundProviderPref& p) {
    ReverbSettings r;
    const float levelMb = static_cast<float>(std::clamp(p.room + p.reverb, -10000, 2000));
    if (p.room <= -9000 || levelMb <= -6000.0f) return r;  // the room effect is off
    constexpr float kTaste = 0.7f;
    constexpr float kMaxWet = 0.45f;
    r.wet = std::min(kMaxWet, kTaste * std::pow(10.0f, levelMb / 2000.0f));
    r.decayTime = std::clamp(p.decayTime, 0.1f, 6.0f);
    const float hfLoss = std::clamp(-static_cast<float>(p.roomHF) / 3000.0f, 0.0f, 1.0f);
    const float hfDecay = std::clamp(1.0f - p.decayHFRatio, 0.0f, 1.0f);
    r.damping = std::clamp(0.15f + 0.5f * hfLoss + 0.25f * hfDecay, 0.0f, 0.85f);
    r.preDelay = std::clamp(p.reflectionsDelay + p.reverbDelay, 0.0f, 0.1f);
    return r;
}

class WorldFx {
public:
    /// Ready for a stream of this rate and channel count. Not real-time safe:
    /// call before the first process(), from any thread.
    void prepare(uint32_t sampleRate, uint32_t channels) {
        sampleRate_ = std::max<uint32_t>(sampleRate, 8000);
        channels_ = std::max<uint32_t>(channels, 1);
        const float scale = static_cast<float>(sampleRate_) / 44100.0f;
        for (uint32_t side = 0; side < 2; ++side) {
            const int spread = side == 0 ? 0 : kStereoSpread;
            for (size_t i = 0; i < kCombs; ++i) {
                combs_[side][i].init(static_cast<size_t>((kCombLengths[i] + spread) * scale));
            }
            for (size_t i = 0; i < kAllpasses; ++i) {
                allpasses_[side][i].init(static_cast<size_t>((kAllpassLengths[i] + spread) * scale));
            }
        }
        preDelay_.assign(static_cast<size_t>(0.11f * static_cast<float>(sampleRate_)) + 2, 0.0f);
        preDelayPos_ = 0;
        lowpass_.assign(channels_ * 2, 0.0f);
        // Blocks are short (10 ms or so), so the slow parameters are eased
        // once a block: about a second to settle on a new room, a third of a
        // second to go under or come up.
        reverbEase_ = 1.0f - std::exp(-1.0f / (0.8f * static_cast<float>(sampleRate_)));
        muffleEase_ = 1.0f - std::exp(-1.0f / (0.25f * static_cast<float>(sampleRate_)));
        lowpassCoef_ = 1.0f - std::exp(-6.2831853f * kUnderwaterCutoffHz / static_cast<float>(sampleRate_));
        reverbIdle_ = true;
        prepared_ = true;
    }

    /// The room to ease toward. Any thread.
    void setReverb(const ReverbSettings& r) {
        targetWet_.store(std::clamp(r.wet, 0.0f, 1.0f), std::memory_order_relaxed);
        targetDecay_.store(std::clamp(r.decayTime, 0.1f, 6.0f), std::memory_order_relaxed);
        targetDamping_.store(std::clamp(r.damping, 0.0f, 0.9f), std::memory_order_relaxed);
        targetPreDelay_.store(std::clamp(r.preDelay, 0.0f, 0.1f), std::memory_order_relaxed);
    }

    /// 1 under water, 0 in air; eased toward. Any thread.
    void setMuffle(float amount) {
        targetMuffle_.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    /// Interleaved float frames, in and out may not alias. Audio thread.
    void process(const float* in, float* out, uint32_t frames) {
        const size_t n = static_cast<size_t>(frames) * channels_;
        if (!prepared_) {
            std::copy(in, in + n, out);
            return;
        }
        const float wetTarget = targetWet_.load(std::memory_order_relaxed);
        const float muffleTarget = targetMuffle_.load(std::memory_order_relaxed);

        // The slow parameters, once a block.
        const float blockEase = 1.0f - std::pow(1.0f - reverbEase_, static_cast<float>(frames));
        decay_ += (targetDecay_.load(std::memory_order_relaxed) - decay_) * blockEase;
        damping_ += (targetDamping_.load(std::memory_order_relaxed) - damping_) * blockEase;
        preDelaySec_ += (targetPreDelay_.load(std::memory_order_relaxed) - preDelaySec_) * blockEase;

        const bool reverbOn = wet_ > 1e-4f || wetTarget > 1e-4f;
        if (reverbOn && reverbIdle_) {
            // Back from silence: whatever the lines held from the last room
            // is long stale.
            clearReverb();
            reverbIdle_ = false;
        } else if (!reverbOn) {
            reverbIdle_ = true;
            wet_ = 0.0f;
        }
        const bool muffleOn = muffle_ > 1e-4f || muffleTarget > 1e-4f;
        if (!muffleOn) muffle_ = 0.0f;

        if (!reverbOn && !muffleOn) {
            std::copy(in, in + n, out);
            return;
        }

        // Per-comb feedback for the eased decay: each comb loses 60 dB over
        // decayTime whatever its own length.
        std::array<float, kCombs> feedback[2];
        float normalise = 0.0f;
        for (uint32_t side = 0; side < 2; ++side) {
            for (size_t i = 0; i < kCombs; ++i) {
                const float len = static_cast<float>(combs_[side][i].buffer.size());
                feedback[side][i] = std::pow(10.0f, -3.0f * len / (decay_ * static_cast<float>(sampleRate_)));
            }
        }
        // Scaled so a steady sound's tail is about as loud as the sound times
        // wet, whatever the decay: a comb's gain on noise is 1/sqrt(1 - g^2),
        // and the four allpasses together add about 3x (measured on noise).
        {
            float energy = 0.0f;
            for (size_t i = 0; i < kCombs; ++i) energy += 1.0f / (1.0f - feedback[0][i] * feedback[0][i]);
            normalise = (1.0f / 3.0f) / std::sqrt(std::max(energy, 1.0f));
        }
        const float damp = damping_;
        const size_t preLen = preDelay_.size();
        const float preSamples = std::clamp(preDelaySec_ * static_cast<float>(sampleRate_),
                                            0.0f, static_cast<float>(preLen - 2));
        const float invChannels = 1.0f / static_cast<float>(channels_);

        for (uint32_t f = 0; f < frames; ++f) {
            const float* x = in + static_cast<size_t>(f) * channels_;
            float* y = out + static_cast<size_t>(f) * channels_;

            float tail[2] = {0.0f, 0.0f};
            if (reverbOn) {
                wet_ += (wetTarget - wet_) * reverbEase_;
                float mono = 0.0f;
                for (uint32_t c = 0; c < channels_; ++c) mono += x[c];
                mono *= invChannels;
                // The pre-delay, read between two samples so an eased change
                // of it slides rather than clicks.
                preDelay_[preDelayPos_] = mono;
                float readPos = static_cast<float>(preDelayPos_) - preSamples;
                if (readPos < 0.0f) readPos += static_cast<float>(preLen);
                const size_t r0 = static_cast<size_t>(readPos);
                const size_t r1 = (r0 + 1) % preLen;
                const float frac = readPos - static_cast<float>(r0);
                const float delayed = preDelay_[r0] + (preDelay_[r1] - preDelay_[r0]) * frac;
                preDelayPos_ = (preDelayPos_ + 1) % preLen;

                for (uint32_t side = 0; side < 2; ++side) {
                    float acc = 0.0f;
                    for (size_t i = 0; i < kCombs; ++i) acc += combs_[side][i].process(delayed, feedback[side][i], damp);
                    acc *= normalise;
                    for (size_t i = 0; i < kAllpasses; ++i) acc = allpasses_[side][i].process(acc);
                    tail[side] = acc * wet_;
                }
            }

            if (muffleOn) muffle_ += (muffleTarget - muffle_) * muffleEase_;

            for (uint32_t c = 0; c < channels_; ++c) {
                float s = x[c] + (channels_ == 1 ? 0.5f * (tail[0] + tail[1]) : tail[c & 1u]);
                if (muffleOn) {
                    // Two one-pole stages, 12 dB an octave above the cutoff,
                    // crossfaded in rather than swept: the filter's own state
                    // never jumps, only how much of it is heard.
                    float& s1 = lowpass_[c * 2];
                    float& s2 = lowpass_[c * 2 + 1];
                    s1 += (s - s1) * lowpassCoef_;
                    s2 += (s1 - s2) * lowpassCoef_;
                    s += (s2 * kUnderwaterLevel - s) * muffle_;
                } else {
                    lowpass_[c * 2] = lowpass_[c * 2 + 1] = s;
                }
                y[c] = s;
            }
        }
    }

private:
    static constexpr size_t kCombs = 6;
    static constexpr size_t kAllpasses = 4;
    // Freeverb's tunings at 44.1 kHz, mutually prime so the combs' echoes do
    // not line up into a pitch; the right side is the left plus a spread.
    static constexpr int kCombLengths[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491};
    static constexpr int kAllpassLengths[kAllpasses] = {556, 441, 341, 225};
    static constexpr int kStereoSpread = 23;
    static constexpr float kUnderwaterCutoffHz = 900.0f;
    // A little quieter as well as duller: water carries the lows and loses
    // the presence that made the sound seem close.
    static constexpr float kUnderwaterLevel = 0.8f;

    struct Comb {
        std::vector<float> buffer;
        size_t pos = 0;
        float store = 0.0f;
        void init(size_t len) {
            buffer.assign(std::max<size_t>(len, 1), 0.0f);
            pos = 0;
            store = 0.0f;
        }
        float process(float input, float feedback, float damp) {
            const float out = buffer[pos];
            store = out * (1.0f - damp) + store * damp;
            if (std::fabs(store) < 1e-20f) store = 0.0f;  // no denormals in the loop
            buffer[pos] = input + store * feedback;
            if (++pos >= buffer.size()) pos = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t pos = 0;
        void init(size_t len) {
            buffer.assign(std::max<size_t>(len, 1), 0.0f);
            pos = 0;
        }
        float process(float input) {
            const float delayed = buffer[pos];
            const float out = delayed - input;
            float next = input + delayed * 0.5f;
            if (std::fabs(next) < 1e-20f) next = 0.0f;
            buffer[pos] = next;
            if (++pos >= buffer.size()) pos = 0;
            return out;
        }
    };

    void clearReverb() {
        for (auto& side : combs_) {
            for (auto& c : side) {
                std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
                c.store = 0.0f;
            }
        }
        for (auto& side : allpasses_) {
            for (auto& a : side) std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
        }
        std::fill(preDelay_.begin(), preDelay_.end(), 0.0f);
    }

    bool prepared_ = false;
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;

    Comb combs_[2][kCombs];
    Allpass allpasses_[2][kAllpasses];
    std::vector<float> preDelay_;
    size_t preDelayPos_ = 0;
    std::vector<float> lowpass_;  // two stages per channel

    std::atomic<float> targetWet_{0.0f};
    std::atomic<float> targetDecay_{1.0f};
    std::atomic<float> targetDamping_{0.3f};
    std::atomic<float> targetPreDelay_{0.02f};
    std::atomic<float> targetMuffle_{0.0f};

    // Audio thread only.
    float wet_ = 0.0f;
    float decay_ = 1.0f;
    float damping_ = 0.3f;
    float preDelaySec_ = 0.02f;
    float muffle_ = 0.0f;
    float reverbEase_ = 0.0f;
    float muffleEase_ = 0.0f;
    float lowpassCoef_ = 1.0f;
    bool reverbIdle_ = true;
};

} // namespace audio
} // namespace wowee
