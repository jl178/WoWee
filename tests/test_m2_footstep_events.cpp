#include <catch_amalgamated.hpp>

#include "test_support.hpp"

#include "pipeline/m2_loader.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// M2 models embed $FSD (footfall) animation events - the keyframes the game
// client uses to sync footstep sounds to feet striking the ground. The loader
// collects them into M2Model::footstepEventTimes, one sorted ms-list per
// sequence index. WotLK (v264) stores per-sequence timestamp arrays; vanilla
// (v256) stores one flat global-timeline array that must be bucketed by each
// sequence's [start, end) window. Expected values below were extracted from
// the raw files with an independent Python parser.

using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;

namespace {


// Sequence index of the first sequence with the given animation ID and
// variation 0, or -1.
int findSequenceIndex(const M2Model& model, uint32_t animId) {
    for (size_t i = 0; i < model.sequences.size(); i++) {
        if (model.sequences[i].id == animId && model.sequences[i].variationIndex == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("WotLK riding horse has 4-beat gallop footfall events", "[m2][footstep]") {
    auto data = wowee::test::readFile("Data/creature/ridinghorse/ridinghorse.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version >= 264);
    REQUIRE(model.footstepEventTimes.size() == model.sequences.size());

    const int walkIdx = findSequenceIndex(model, 4);
    REQUIRE(walkIdx >= 0);
    CHECK(model.footstepEventTimes[walkIdx] == std::vector<uint32_t>{67, 333, 467, 733});

    const int runIdx = findSequenceIndex(model, 5);
    REQUIRE(runIdx >= 0);
    // Gallop: four hoofbeats clustered in the front of the 800ms stride,
    // then airborne suspension.
    CHECK(model.footstepEventTimes[runIdx] == std::vector<uint32_t>{134, 234, 334, 500});
}

TEST_CASE("WotLK human male has alternating biped footfall events", "[m2][footstep]") {
    auto data = wowee::test::readFile("Data/character/human/male/humanmale.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version >= 264);

    const int runIdx = findSequenceIndex(model, 5);
    REQUIRE(runIdx >= 0);
    CHECK(model.footstepEventTimes[runIdx] == std::vector<uint32_t>{267, 600});
}

// $CSD events carry a SoundEntries id: 6923 is HumanMaleEmoteLaugh. EmoteLaugh
// (anim 70) keeps its keyframes in humanmale0070-00.anim, and the event's
// timestamps with them, so the M2 alone yields none for that sequence.
TEST_CASE("WotLK human male laugh voice is a $CSD event in its .anim", "[m2][sound_event]") {
    auto data = wowee::test::readFile("Data/character/human/male/humanmale.m2");
    auto anim = wowee::test::readFile("Data/character/human/male/humanmale0070-00.anim");
    if (data.empty() || anim.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version >= 264);
    const int laughIdx = findSequenceIndex(model, 70);
    REQUIRE(laughIdx >= 0);
    REQUIRE((model.sequences[laughIdx].flags & 0x20) == 0);
    REQUIRE(model.soundEvents.size() == model.sequences.size());
    CHECK(model.soundEvents[laughIdx].empty());

    M2Loader::loadAnimFile(data, anim, static_cast<uint32_t>(laughIdx), model);
    REQUIRE(model.soundEvents[laughIdx].size() == 1);
    CHECK(model.soundEvents[laughIdx][0].timeMs == 0);
    CHECK(model.soundEvents[laughIdx][0].soundEntryId == 6923);

    // Loading the same .anim again does not duplicate the event.
    M2Loader::loadAnimFile(data, anim, static_cast<uint32_t>(laughIdx), model);
    CHECK(model.soundEvents[laughIdx].size() == 1);
}

TEST_CASE("Vanilla model buckets global-timeline footfall events per sequence", "[m2][footstep]") {
    auto data = wowee::test::readFile("Data/expansions/turtle/overlay/creature/orcmalewarriorlight/OrcMaleWarriorLight.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version < 264);
    REQUIRE(model.footstepEventTimes.size() == model.sequences.size());

    const int walkIdx = findSequenceIndex(model, 4);
    REQUIRE(walkIdx >= 0);
    CHECK(model.footstepEventTimes[walkIdx] == std::vector<uint32_t>{266, 800});

    const int runIdx = findSequenceIndex(model, 5);
    REQUIRE(runIdx >= 0);
    CHECK(model.footstepEventTimes[runIdx] == std::vector<uint32_t>{233, 567});
}

// $CSL / $CSR / $CST: the frame of a cast animation where the spell leaves the
// hand. The stock client holds a missile until one of them fires (Wow.exe
// 0x734390). HumanMale's SpellCastDirected (53) lets go from the left hand
// ($CSL) 200 ms in, SpellCastOmni (54) from the chest ($CST) at 300 ms, and
// the ReadySpell hold (51) has none.
TEST_CASE("WotLK human male cast release events", "[m2][cast_release]") {
    auto data = wowee::test::readFile("Data/character/human/male/humanmale.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version >= 264);
    REQUIRE(model.castReleaseEvents.size() == model.sequences.size());

    const int directedIdx = findSequenceIndex(model, 53);
    REQUIRE(directedIdx >= 0);
    CHECK(model.castReleaseEvents[directedIdx].timeMs == 200);
    // The left hand's side of the model (+Y), about hand height.
    CHECK(model.castReleaseEvents[directedIdx].position.y > 0.3f);

    const int omniIdx = findSequenceIndex(model, 54);
    REQUIRE(omniIdx >= 0);
    CHECK(model.castReleaseEvents[omniIdx].timeMs == 300);

    const int readyIdx = findSequenceIndex(model, 51);
    REQUIRE(readyIdx >= 0);
    CHECK(model.castReleaseEvents[readyIdx].timeMs == UINT32_MAX);
}

// $CPP is the frame of an attack where the victim's dodge, parry or block
// plays (Wow.exe 0x756240), well before the blow lands at $CAH. It makes no
// sound but is kept with the other tagged events. HumanMale Attack1H (17)
// raises the defence 67 ms in, lands the blow at 400 ms.
TEST_CASE("M2 $CPP defence events are kept with the tagged events", "[m2][sound_event]") {
    using wowee::pipeline::isM2SoundEventTag;
    using wowee::pipeline::m2EventTag;
    STATIC_REQUIRE(isM2SoundEventTag(m2EventTag("$CPP")));
    STATIC_REQUIRE(isM2SoundEventTag(m2EventTag("$CAH")));
    STATIC_REQUIRE_FALSE(isM2SoundEventTag(m2EventTag("$CST")));

    auto data = wowee::test::readFile("Data/character/human/male/humanmale.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    const int attackIdx = findSequenceIndex(model, 17);
    REQUIRE(attackIdx >= 0);
    REQUIRE(static_cast<size_t>(attackIdx) < model.soundEvents.size());
    uint32_t cpp = UINT32_MAX, cah = UINT32_MAX;
    for (const auto& ev : model.soundEvents[attackIdx]) {
        if (ev.tag == m2EventTag("$CPP")) cpp = ev.timeMs;
        if (ev.tag == m2EventTag("$CAH")) cah = ev.timeMs;
    }
    CHECK(cpp == 67);
    CHECK(cah == 400);
}
