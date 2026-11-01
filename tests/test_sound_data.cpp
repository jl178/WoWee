// SoundData reads the client's sound tables. These tests feed it synthetic
// tables in the 3.3.5 layout and check the lookups and the parent chain.
#include <catch_amalgamated.hpp>
#include "audio/sound_data.hpp"
#include "pipeline/dbc_loader.hpp"
#include "synthetic_dbc.hpp"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using wowee::audio::SoundData;
using wowee::pipeline::DBCFile;

namespace {

using synthetic_dbc::Strings;
using synthetic_dbc::table;
using synthetic_dbc::row;
using synthetic_dbc::bits;

// The fixture: Elwynn (12) with music set 1 and ambience 35, Northshire (9)
// under it with neither of its own, Stormwind (1519) with its own set and an
// intro, and a human race whose model's sound row names an exertion sound.
struct Fixture {
    std::map<std::string, std::shared_ptr<DBCFile>> tables;

    Fixture() {
        Strings se;
        const uint32_t seDir = se.add("Sound\\Music\\ZoneMusic\\Forest");
        tables["SoundEntries.dbc"] = table(30, {
            row(30, {{0, 2523}, {2, se.add("Zone-Forest Day")}, {3, se.add("DayForest01.mp3")}, {4, se.add("DayForest02.mp3")},
                     {23, seDir}, {24, bits(0.4f)}}),
            row(30, {{0, 2532}, {2, se.add("Zone-Stormwind")}, {3, se.add("stormwind05-zone.mp3")},
                     {23, se.add("Sound\\Music\\CityMusic\\Stormwind")}, {24, bits(0.69f)}}),
            row(30, {{0, 2541}, {2, se.add("Moment-ValleyofHeros Intro")}, {3, se.add("stormwind_intro-moment.mp3")},
                     {23, se.add("Sound\\Music\\CityMusic\\Stormwind")}, {24, bits(0.69f)}}),
            row(30, {{0, 4183}, {2, se.add("ForestNormalDay")}, {3, se.add("ForestNormalDay.wav")},
                     {23, se.add("Sound\\Ambience\\ZoneAmbience")}, {24, bits(0.69f)}}),
            row(30, {{0, 4184}, {2, se.add("ForestNormalNight")}, {3, se.add("ForestNormalNight.wav")},
                     {23, se.add("Sound\\Ambience\\ZoneAmbience")}, {24, bits(0.69f)}}),
            row(30, {{0, 2941}, {2, se.add("HumanMalePlayerAttack")}, {3, se.add("HumanMaleMainAttackA.wav")},
                     {23, se.add("Sound\\Character\\PlayerExertions\\HumanMaleFinal")}, {24, bits(0.69f)}}),
            row(30, {{0, 3283}, {2, se.add("HumanmaleMainJump")}, {3, se.add("HumanMaleMainJump.wav")},
                     {23, se.add("Sound\\Character\\PlayerExertions\\HumanMaleFinal")}, {24, bits(0.69f)}}),
            row(30, {{0, 3281}, {2, se.add("HumanFemaleMainJump")}, {3, se.add("HumanFeamleMainJump.wav")},
                     {23, se.add("Sound\\Character\\PlayerExertions\\HumanFemaleFinal")}, {24, bits(0.69f)}}),
            row(30, {{0, 7000}, {2, se.add("NoFiles")}}),
        }, se.block);

        Strings zm;
        tables["ZoneMusic.dbc"] = table(8, {
            row(8, {{0, 1}, {1, zm.add("Zone-Forest")}, {2, 180000}, {3, 180000}, {4, 300000}, {5, 300000}, {6, 2523}, {7, 2523}}),
            row(8, {{0, 13}, {1, zm.add("Zone-Stormwind")}, {2, 1000}, {3, 1000}, {4, 2000}, {5, 2000}, {6, 2532}, {7, 0}}),
        }, zm.block);

        tables["SoundAmbience.dbc"] = table(3, {row(3, {{0, 35}, {1, 4183}, {2, 4184}})}, std::string(1, '\0'));
        tables["ZoneIntroMusicTable.dbc"] = table(5, {row(5, {{0, 61}, {2, 2541}, {3, 1}, {4, 60}})}, std::string(1, '\0'));

        Strings at;
        tables["AreaTable.dbc"] = table(36, {
            row(36, {{0, 12}, {7, 35}, {8, 1}, {11, at.add("Elwynn Forest")}}),
            row(36, {{0, 9}, {2, 12}, {11, at.add("Northshire Valley")}}),
            row(36, {{0, 1519}, {7, 31}, {8, 13}, {9, 61}, {11, at.add("Stormwind City")}}),
            row(36, {{0, 5000}, {2, 5000}, {11, at.add("Loop")}}),
        }, at.block);

        Strings cr;
        tables["ChrRaces.dbc"] = table(69, {row(69, {{0, 1}, {4, 49}, {5, 50}, {11, cr.add("Human")}})}, cr.block);
        tables["CreatureDisplayInfo.dbc"] = table(16, {row(16, {{0, 49}, {1, 49}}), row(16, {{0, 50}, {1, 50}, {2, 77}})}, std::string(1, '\0'));
        tables["CreatureModelData.dbc"] = table(28, {row(28, {{0, 49}, {13, 49}})}, std::string(1, '\0'));
        tables["CreatureSoundData.dbc"] = table(38, {row(38, {{0, 49}, {1, 2941}}), row(38, {{0, 77}, {1, 7000}})}, std::string(1, '\0'));
    }

    SoundData::Loader loader() const {
        return [this](const std::string& name) -> std::shared_ptr<DBCFile> {
            auto it = tables.find(name);
            return it == tables.end() ? nullptr : it->second;
        };
    }
};

} // namespace

TEST_CASE("SoundData needs SoundEntries and nothing else", "[sound_data]") {
    SoundData data;
    REQUIRE_FALSE(data.load([](const std::string&) { return nullptr; }));
    REQUIRE_FALSE(data.loaded());

    Fixture f;
    f.tables.erase("ZoneMusic.dbc");
    f.tables.erase("AreaTable.dbc");
    REQUIRE(data.load(f.loader()));
    REQUIRE(data.loaded());
    REQUIRE(data.areaMusic(12) == nullptr);
    REQUIRE(data.sound(2523) != nullptr);
}

TEST_CASE("SoundData skips a table from another build", "[sound_data]") {
    Fixture f;
    f.tables["ZoneMusic.dbc"] = table(6, {row(6, {{0, 1}})}, std::string(1, '\0'));
    SoundData data;
    REQUIRE(data.load(f.loader()));
    REQUIRE(data.zoneMusic(1) == nullptr);
}

TEST_CASE("SoundData joins directory and file names", "[sound_data]") {
    Fixture f;
    SoundData data;
    REQUIRE(data.load(f.loader()));
    const auto* e = data.sound(2523);
    REQUIRE(e != nullptr);
    REQUIRE(e->files == std::vector<std::string>{"Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3",
                                                 "Sound\\Music\\ZoneMusic\\Forest\\DayForest02.mp3"});
    REQUIRE(e->volume == Catch::Approx(0.4f));
    REQUIRE(data.sound(0) == nullptr);
    REQUIRE(data.sound(99999) == nullptr);
    REQUIRE(data.sound(7000)->files.empty());
    REQUIRE(data.soundByName("zone-forest DAY") == e);
}

TEST_CASE("SoundData area lookups follow the parent chain", "[sound_data]") {
    Fixture f;
    SoundData data;
    REQUIRE(data.load(f.loader()));

    const auto* elwynn = data.areaMusic(12);
    REQUIRE(elwynn != nullptr);
    REQUIRE(elwynn->id == 1);
    REQUIRE(elwynn->silenceMinMs[0] == 180000);
    REQUIRE(elwynn->silenceMaxMs[1] == 300000);
    REQUIRE(data.areaMusic(9) == elwynn);
    REQUIRE(data.areaMusic(1519)->id == 13);
    REQUIRE(data.areaMusic(4242) == nullptr);
    REQUIRE(data.areaMusic(5000) == nullptr);  // self-parented row ends the walk

    uint32_t day = 0, night = 0;
    REQUIRE(data.areaAmbience(9, day, night));
    REQUIRE(day == 4183);
    REQUIRE(night == 4184);
    REQUIRE_FALSE(data.areaAmbience(1519, day, night));  // ambience 31 has no row

    REQUIRE(data.areaIntro(1519) == 61);
    REQUIRE(data.areaIntro(12) == 0);
    REQUIRE(data.intro(61)->soundId == 2541);
    REQUIRE(data.intro(61)->minDelayMinutes == 60);
    REQUIRE(data.area(9)->name == "Northshire Valley");
}

TEST_CASE("SoundData resolves a player's voice through the model chain", "[sound_data]") {
    Fixture f;
    SoundData data;
    REQUIRE(data.load(f.loader()));

    // Male: display 49 names no sound row itself, so the model's row is used.
    const auto* male = data.playerSounds(1, false);
    REQUIRE(male != nullptr);
    REQUIRE(male->exertion == 2941);
    // Female: display 50 carries its own sound row.
    const auto* female = data.playerSounds(1, true);
    REQUIRE(female != nullptr);
    REQUIRE(female->exertion == 7000);
    REQUIRE(data.playerSounds(3, false) == nullptr);

    REQUIRE(data.raceClientName(1) == "Human");
    REQUIRE(data.playerJumpSound(1, false)->id == 3283);
    REQUIRE(data.playerJumpSound(1, true)->id == 3281);  // the "Feamle" spelling is not needed here
    REQUIRE(data.playerLandSound(1, false) == nullptr);
}

TEST_CASE("SoundData finds the misspelt female rows", "[sound_data]") {
    Fixture f;
    Strings se;
    f.tables["SoundEntries.dbc"] = table(30, {
        row(30, {{0, 3285}, {2, se.add("NightElfFeamleMainJump")}, {3, se.add("NightElfFemaleMainJump.wav")},
                 {23, se.add("Sound\\Character\\PlayerExertions\\NightElfFemaleFinal")}, {24, bits(0.69f)}}),
    }, se.block);
    Strings cr;
    f.tables["ChrRaces.dbc"] = table(69, {row(69, {{0, 4}, {4, 53}, {5, 54}, {11, cr.add("NightElf")}})}, cr.block);
    SoundData data;
    REQUIRE(data.load(f.loader()));
    REQUIRE(data.playerJumpSound(4, true)->id == 3285);
    REQUIRE(data.playerJumpSound(4, false) == nullptr);
}

TEST_CASE("SoundData picks the room's WMOAreaTable row over the building's", "[sound_data]") {
    Fixture f;
    Strings wa;
    f.tables["WMOAreaTable.dbc"] = table(28, {
        row(28, {{0, 20327}, {1, 53}, {2, 0}, {3, 0xFFFFFFFFu}, {6, 170}, {7, 14}, {11, wa.add("Lion's Pride Inn")}}),
        row(28, {{0, 20256}, {1, 53}, {2, 1}, {3, 893}, {6, 0}, {7, 15}, {10, 2104}}),
        row(28, {{0, 20255}, {1, 53}, {2, 1}, {3, 0xFFFFFFFFu}, {6, 171}, {7, 14}, {10, 2104}, {11, wa.add("Deepwater Tavern")}}),
    }, wa.block);
    SoundData data;
    REQUIRE(data.load(f.loader()));
    const auto* inn = data.wmoArea(53, 0, 5);
    REQUIRE(inn != nullptr);
    REQUIRE(inn->name == "Lion's Pride Inn");
    REQUIRE(inn->zoneMusicId == 14);
    REQUIRE(inn->ambienceId == 170);
    // Name set 1 is Menethil's copy of the same building.
    REQUIRE(data.wmoArea(53, 1, 5)->name == "Deepwater Tavern");
    REQUIRE(data.wmoArea(53, 1, 893)->zoneMusicId == 15);
    REQUIRE(data.wmoArea(99, 0, -1) == nullptr);
}
