// The pure parts of the M2 particle step (rendering/m2_particle_step.hpp).
//
// The classified emitter tracks must give what the sampler gives, bit for
// bit, at any time and sequence; the in-order step must leave the same
// particles, moved the same way, as the swap-and-pop step it replaces; the
// birth merge must keep a list grouped by emitter; the weight split must
// cover the list in order.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_step.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace wowee::rendering;
using wowee::pipeline::M2AnimationTrack;

namespace {

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

M2AnimationTrack::SequenceKeys keys(std::vector<uint32_t> ts, std::vector<float> vs) {
    M2AnimationTrack::SequenceKeys k;
    k.timestamps = std::move(ts);
    k.floatValues = std::move(vs);
    return k;
}

void checkAgainstSampler(const M2AnimationTrack& track, const std::vector<uint32_t>& gsd) {
    const m2_particle::TrackConst c = m2_particle::classifyTrack(track, gsd);
    const float times[] = {-50.0f, 0.0f, 1.0f, 99.5f, 100.0f, 250.0f, 1000.0f, 12345.6f,
                           std::numeric_limits<float>::quiet_NaN()};
    for (int seq = -2; seq < 6; ++seq) {
        for (float anim : times) {
            for (float global : times) {
                const float want = m2_track::sampleFloat(track, seq, anim, global, gsd, 0.0f);
                const float got = m2_particle::sampleTrack(c, track, seq, anim, global, gsd);
                INFO("seq " << seq << " anim " << anim << " global " << global);
                REQUIRE(sameBits(want, got));
            }
        }
    }
}

struct P {
    glm::vec3 position{0.0f};
    float life = 0.0f;
    glm::vec3 velocity{0.0f};
    float maxLife = 1.0f;
    glm::vec3 localPos{0.0f};
    uint16_t emitterIndex = 0;
    uint16_t tileIndex = 0;
};

struct E {
    glm::mat4 xf{1.0f};
    float gravity = 0.0f;
    float dragFactor = 1.0f;
    bool noTrail = false;
};

bool sameParticle(const P& a, const P& b) {
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
           std::memcmp(&a.velocity, &b.velocity, sizeof(a.velocity)) == 0 &&
           std::memcmp(&a.localPos, &b.localPos, sizeof(a.localPos)) == 0 &&
           sameBits(a.life, b.life) && sameBits(a.maxLife, b.maxLife) &&
           a.emitterIndex == b.emitterIndex && a.tileIndex == b.tileIndex;
}

/// The step as it was: swap-and-pop removal, each particle moved in place.
size_t legacyStep(P* ps, size_t n, const E* ems, size_t numEm, float dt) {
    for (size_t i = 0; i < n;) {
        P& p = ps[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            p = ps[n - 1];
            --n;
            continue;
        }
        const size_t e = p.emitterIndex;
        if (e < numEm) {
            const E& s = ems[e];
            if (s.noTrail) {
                p.velocity.z -= s.gravity * dt;
                p.velocity *= s.dragFactor;
                p.localPos += p.velocity * dt;
                p.position = glm::vec3(s.xf * glm::vec4(p.localPos, 1.0f));
                ++i;
                continue;
            }
            p.velocity.z -= s.gravity * dt;
            p.velocity *= s.dragFactor;
        }
        p.position += p.velocity * dt;
        ++i;
    }
    return n;
}

} // namespace

TEST_CASE("classified particle tracks sample as the sampler does", "[m2_particle]") {
    const std::vector<uint32_t> gsd{0, 400, 1000};

    SECTION("no sequences") {
        M2AnimationTrack t;
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Always);
        checkAgainstSampler(t, gsd);
    }
    SECTION("empty sequences") {
        M2AnimationTrack t;
        t.sequences = {keys({}, {}), keys({0}, {}), keys({}, {3.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Always);
        checkAgainstSampler(t, gsd);
    }
    SECTION("one key of one value per sequence") {
        M2AnimationTrack t;
        t.sequences = {keys({0}, {2.5f}), keys({100}, {2.5f}), keys({0, 50}, {2.5f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind ==
              m2_particle::TrackConst::Kind::PerSequence);
        checkAgainstSampler(t, gsd);
    }
    SECTION("one key per sequence, values differ") {
        M2AnimationTrack t;
        t.sequences = {keys({0}, {2.5f}), keys({0}, {4.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Sampled);
        checkAgainstSampler(t, gsd);
    }
    SECTION("a sequence without keys among keyed ones") {
        M2AnimationTrack t;
        t.sequences = {keys({0}, {2.5f}), keys({}, {})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Sampled);
        checkAgainstSampler(t, gsd);
    }
    SECTION("animated keys") {
        M2AnimationTrack t;
        t.interpolationType = 1;
        t.sequences = {keys({0, 100, 250}, {1.0f, 3.0f, -2.0f}), keys({0}, {1.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Sampled);
        checkAgainstSampler(t, gsd);
    }
    SECTION("equal animated keys are still sampled") {
        // mix(v, v, f) need not be v to the last bit, so this is not constant.
        M2AnimationTrack t;
        t.interpolationType = 1;
        t.sequences = {keys({0, 100}, {0.1f, 0.1f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Sampled);
        checkAgainstSampler(t, gsd);
    }
    SECTION("global sequence with one key") {
        M2AnimationTrack t;
        t.globalSequence = 1;
        t.sequences = {keys({0}, {7.0f}), keys({0}, {9.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Always);
        checkAgainstSampler(t, gsd);
    }
    SECTION("global sequence of zero length") {
        M2AnimationTrack t;
        t.globalSequence = 0;
        t.sequences = {keys({0}, {7.0f})};
        checkAgainstSampler(t, gsd);
    }
    SECTION("global sequence animated") {
        M2AnimationTrack t;
        t.interpolationType = 1;
        t.globalSequence = 2;
        t.sequences = {keys({0, 500}, {0.0f, 10.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind == m2_particle::TrackConst::Kind::Sampled);
        checkAgainstSampler(t, gsd);
    }
    SECTION("global sequence index past the table reads the animation") {
        M2AnimationTrack t;
        t.globalSequence = 9;
        t.sequences = {keys({0}, {7.0f}), keys({0}, {7.0f})};
        CHECK(m2_particle::classifyTrack(t, gsd).kind ==
              m2_particle::TrackConst::Kind::PerSequence);
        checkAgainstSampler(t, gsd);
    }
    SECTION("global sequence without keys") {
        M2AnimationTrack t;
        t.globalSequence = 1;
        checkAgainstSampler(t, gsd);
    }
}

TEST_CASE("the in-order particle step matches the swap-and-pop step", "[m2_particle]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-3.0f, 3.0f);
    std::vector<E> ems(3);
    ems[0].gravity = 9.8f;
    ems[0].dragFactor = 0.97f;
    ems[1].noTrail = true;
    ems[1].gravity = 1.5f;
    ems[1].xf = glm::mat4(0.0f, 1.0f, 0.0f, 0.0f,
                          -1.0f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, 1.0f, 0.0f,
                          5.0f, 6.0f, 7.0f, 1.0f);
    ems[2].dragFactor = 0.5f;

    std::vector<P> ps(500);
    for (size_t i = 0; i < ps.size(); ++i) {
        P& p = ps[i];
        // Emitter 3 is past the table: moved without gravity or drag.
        p.emitterIndex = static_cast<uint16_t>(i % 4);
        p.position = {u(rng), u(rng), u(rng)};
        p.velocity = {u(rng), u(rng), u(rng)};
        p.localPos = {u(rng), u(rng), u(rng)};
        p.maxLife = 1.0f + std::abs(u(rng));
        p.life = std::abs(u(rng));
        p.tileIndex = static_cast<uint16_t>(i);  // an id, for the order check
    }

    for (int frame = 0; frame < 30; ++frame) {
        const float dt = 0.016f + 0.01f * static_cast<float>(frame % 3);
        std::vector<P> a = ps, b = ps;
        const size_t na = m2_particle::stepAndCompact(a.data(), a.size(), ems.data(), ems.size(), dt);
        const size_t nb = legacyStep(b.data(), b.size(), ems.data(), ems.size(), dt);
        REQUIRE(na == nb);
        a.resize(na);
        b.resize(nb);

        // The same survivors, moved identically: each in-order survivor is
        // found once among the swap-and-pop ones.
        std::vector<bool> used(nb, false);
        for (const P& p : a) {
            bool found = false;
            for (size_t k = 0; k < nb && !found; ++k) {
                if (!used[k] && sameParticle(p, b[k])) used[k] = found = true;
            }
            REQUIRE(found);
        }

        // In order: survivors keep their relative order from before the step.
        size_t from = 0;
        for (const P& p : a) {
            while (from < ps.size() && ps[from].tileIndex != p.tileIndex) ++from;
            REQUIRE(from < ps.size());
            ++from;
        }
        ps = a;
    }
}

TEST_CASE("the in-order particle step keeps emitters grouped", "[m2_particle]") {
    std::vector<E> ems(3);
    std::vector<P> ps;
    for (uint16_t e = 0; e < 3; ++e) {
        for (int i = 0; i < 10; ++i) {
            P p;
            p.emitterIndex = e;
            p.maxLife = 1.0f;
            p.life = (i % 3 == 0) ? 0.995f : 0.1f * static_cast<float>(i % 3);
            ps.push_back(p);
        }
    }
    const size_t n = m2_particle::stepAndCompact(ps.data(), ps.size(), ems.data(), ems.size(), 0.01f);
    ps.resize(n);
    CHECK(n == 18);
    CHECK(std::is_sorted(ps.begin(), ps.end(),
                         [](const P& x, const P& y) { return x.emitterIndex < y.emitterIndex; }));
}

TEST_CASE("births merge in after their own emitter", "[m2_particle]") {
    const auto make = [](std::vector<uint16_t> emitters, uint16_t tag) {
        std::vector<P> out;
        for (size_t i = 0; i < emitters.size(); ++i) {
            P p;
            p.emitterIndex = emitters[i];
            p.tileIndex = static_cast<uint16_t>(tag + i);
            out.push_back(p);
        }
        return out;
    };
    const auto emittersOf = [](const std::vector<P>& v) {
        std::vector<uint16_t> out;
        for (const P& p : v) out.push_back(p.emitterIndex);
        return out;
    };
    const auto tagsOf = [](const std::vector<P>& v) {
        std::vector<uint16_t> out;
        for (const P& p : v) out.push_back(p.tileIndex);
        return out;
    };

    SECTION("into the middle and the ends") {
        auto ps = make({0, 0, 1, 1, 1, 3}, 0);
        const auto births = make({0, 1, 2, 3, 4}, 100);
        m2_particle::mergeByEmitter(ps, births);
        CHECK(emittersOf(ps) == std::vector<uint16_t>{0, 0, 0, 1, 1, 1, 1, 2, 3, 3, 4});
        // Stable: the old ones of an emitter first, then its births.
        CHECK(tagsOf(ps) == std::vector<uint16_t>{0, 1, 100, 2, 3, 4, 101, 102, 5, 103, 104});
    }
    SECTION("into an empty list") {
        std::vector<P> ps;
        m2_particle::mergeByEmitter(ps, make({1, 1, 2}, 10));
        CHECK(tagsOf(ps) == std::vector<uint16_t>{10, 11, 12});
    }
    SECTION("no births leaves the list alone") {
        auto ps = make({0, 2}, 0);
        m2_particle::mergeByEmitter(ps, {});
        CHECK(tagsOf(ps) == std::vector<uint16_t>{0, 1});
    }
    SECTION("births before every old particle") {
        auto ps = make({2, 2}, 0);
        m2_particle::mergeByEmitter(ps, make({0, 1}, 10));
        CHECK(tagsOf(ps) == std::vector<uint16_t>{10, 11, 0, 1});
    }
    SECTION("many frames of step and merge stay grouped") {
        std::mt19937 rng(3);
        std::vector<E> ems(4);
        std::vector<P> ps;
        for (int frame = 0; frame < 200; ++frame) {
            const size_t n = m2_particle::stepAndCompact(ps.data(), ps.size(), ems.data(),
                                                         ems.size(), 0.05f);
            ps.resize(n);
            std::vector<P> births;
            for (uint16_t e = 0; e < 4; ++e) {
                const int count = static_cast<int>(rng() % 4);
                for (int i = 0; i < count; ++i) {
                    P p;
                    p.emitterIndex = e;
                    p.maxLife = 0.2f + 0.1f * static_cast<float>(rng() % 20);
                    births.push_back(p);
                }
            }
            m2_particle::mergeByEmitter(ps, births);
            REQUIRE(std::is_sorted(ps.begin(), ps.end(), [](const P& x, const P& y) {
                return x.emitterIndex < y.emitterIndex;
            }));
        }
    }
}

TEST_CASE("work splits by weight, in order", "[m2_particle]") {
    std::vector<size_t> bounds;
    const auto check = [&](const std::vector<uint32_t>& w, size_t parts) {
        m2_particle::splitByWeight(w, parts, bounds);
        REQUIRE(bounds.size() == parts + 1);
        CHECK(bounds.front() == 0);
        CHECK(bounds.back() == w.size());
        CHECK(std::is_sorted(bounds.begin(), bounds.end()));
    };

    SECTION("even weights split evenly") {
        std::vector<uint32_t> w(10, 5);
        check(w, 2);
        CHECK(bounds[1] == 5);
    }
    SECTION("one heavy item at the front") {
        std::vector<uint32_t> w{1000, 1, 1, 1, 1};
        check(w, 2);
        CHECK(bounds[1] == 1);
    }
    SECTION("one heavy item at the back") {
        // Split before it: nearer half than everything in one part.
        std::vector<uint32_t> w{1, 1, 1, 1, 1000};
        check(w, 2);
        CHECK(bounds[1] == 4);
    }
    SECTION("more parts than items") {
        check({3, 4}, 4);
    }
    SECTION("empty and zero weights") {
        check({}, 2);
        check({0, 0, 0}, 3);
    }
    SECTION("one part takes everything") {
        check({1, 2, 3}, 1);
    }
}
