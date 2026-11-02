// SoundData reads the client's sound tables. These tests feed it synthetic
// tables in the 3.3.5 layout and check the lookups and the parent chain.
#include <catch_amalgamated.hpp>
#include "audio/player_voice_manager.hpp"
#include "audio/sound_data.hpp"
#include "audio/world_fx.hpp"
#include "pipeline/dbc_loader.hpp"
#include "synthetic_dbc.hpp"

#include <cmath>
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

TEST_CASE("SoundData picks a character's error line from VocalUISounds", "[sound_data]") {
    Fixture f;
    Strings se;
    const uint32_t maleDir = se.add("Sound\\Character\\Human\\MaleErrorMessages");
    const uint32_t femaleDir = se.add("Sound\\Character\\Human\\FemaleErrorMessages");
    f.tables["SoundEntries.dbc"] = table(30, {
        row(30, {{0, 1906}, {2, se.add("HumanMale_CantCastOutofRange")}, {3, se.add("HumanMale_err_outofrange02.wav")},
                 {4, se.add("HumanMale_err_outofrange04.wav")}, {5, se.add("HumanMale_err_outofrange05.wav")},
                 {23, maleDir}, {24, bits(0.69f)}}),
        row(30, {{0, 2030}, {2, se.add("HumanFemale_CantCastOutOfRange")}, {3, se.add("HumanFemale_err_outofrange02.wav")},
                 {23, femaleDir}, {24, bits(0.69f)}}),
        row(30, {{0, 1880}, {2, se.add("HumanMale_InventoryFull")}, {3, se.add("HumanMale_err_inventoryfull01.wav")},
                 {23, maleDir}, {24, bits(0.69f)}}),
        row(30, {{0, 8000}, {2, se.add("HumanMale_InventoryFullPissed")}, {3, se.add("HumanMale_err_inventoryfull02.wav")},
                 {23, maleDir}, {24, bits(0.5f)}}),
    }, se.block);
    // ID, VocalUIEnum, RaceID, NormalSoundID[male, female], PissedSoundID[male, female].
    // The stock file marks an unused column 0 or 0xFFFFFFFF; 626 stands for
    // the stock pissed ids that name no SoundEntries row.
    f.tables["VocalUISounds.dbc"] = table(7, {
        row(7, {{0, 363}, {1, 46}, {2, 1}, {3, 1906}, {4, 2030}, {5, 0xFFFFFFFFu}, {6, 0}}),
        row(7, {{0, 9}, {1, 0}, {2, 1}, {3, 1880}, {4, 0xFFFFFFFFu}, {5, 8000}, {6, 626}}),
    }, std::string(1, '\0'));
    SoundData data;
    REQUIRE(data.load(f.loader()));
    REQUIRE(data.hasVocalUiSounds());

    using wowee::audio::PlayerErrorSpeech;
    using wowee::audio::vocalUiEnum;
    REQUIRE(vocalUiEnum(PlayerErrorSpeech::OUT_OF_RANGE) == 46);
    REQUIRE(vocalUiEnum(PlayerErrorSpeech::INVENTORY_FULL) == 0);
    REQUIRE(vocalUiEnum(PlayerErrorSpeech::NO_MANA) == 15);
    REQUIRE(vocalUiEnum(PlayerErrorSpeech::CANT_CREATE_HERE) == 18);

    const auto* male = data.vocalUiSound(vocalUiEnum(PlayerErrorSpeech::OUT_OF_RANGE), 1, false, false);
    REQUIRE(male != nullptr);
    REQUIRE(male->files.size() == 3);
    REQUIRE(male->files[0] == "Sound\\Character\\Human\\MaleErrorMessages\\HumanMale_err_outofrange02.wav");
    REQUIRE(male->volume == Catch::Approx(0.69f));
    REQUIRE(data.vocalUiSound(46, 1, true, false)->id == 2030);
    REQUIRE(data.vocalUiSound(46, 1, false, true) == nullptr);  // 0xFFFFFFFF
    REQUIRE(data.vocalUiSound(46, 1, true, true) == nullptr);   // 0
    REQUIRE(data.vocalUiSound(46, 2, false, false) == nullptr); // no row for the race
    REQUIRE(data.vocalUiSound(15, 1, false, false) == nullptr); // no row for the line

    REQUIRE(data.vocalUiSound(0, 1, false, true)->id == 8000);
    REQUIRE(data.vocalUiSound(0, 1, true, false) == nullptr);
    REQUIRE(data.vocalUiSound(0, 1, true, true) == nullptr);    // names no SoundEntries row

    f.tables.erase("VocalUISounds.dbc");
    SoundData without;
    REQUIRE(without.load(f.loader()));
    REQUIRE_FALSE(without.hasVocalUiSounds());
    REQUIRE(without.vocalUiSound(46, 1, false, false) == nullptr);
}

TEST_CASE("SoundData finds the loot coin and an item's pickup and put-down rows", "[sound_data]") {
    Fixture f;
    Strings se;
    const uint32_t uiDir = se.add("Sound\\Interface");
    const uint32_t pickupDir = se.add("Sound\\interface\\PickUp");
    // The stock file carries the coin row twice, 120 and 895, identical.
    f.tables["SoundEntries.dbc"] = table(30, {
        row(30, {{0, 120}, {2, se.add("LOOTWINDOWCOINSOUND")}, {3, se.add("LootCoinSmall.wav")},
                 {23, uiDir}, {24, bits(0.7f)}}),
        row(30, {{0, 895}, {2, se.add("LOOTWINDOWCOINSOUND")}, {3, se.add("LootCoinSmall.wav")},
                 {23, uiDir}, {24, bits(0.7f)}}),
        row(30, {{0, 1185}, {2, se.add("PickUpCloth_Leather")}, {3, se.add("PickUpCloth_Leather01.wav")},
                 {23, pickupDir}, {24, bits(0.69f)}}),
        row(30, {{0, 1202}, {2, se.add("PutDownClothLeather")}, {3, se.add("PutDownCloth_Leather01.wav")},
                 {23, pickupDir}, {24, bits(0.69f)}}),
        row(30, {{0, 7000}, {2, se.add("NoFiles")}}),
    }, se.block);
    // ItemGroupSounds: ID, Sound[4]. Group 7 is cloth and leather; group 30
    // names a row with no files and group 31 a row that does not exist.
    f.tables["ItemGroupSounds.dbc"] = table(5, {
        row(5, {{0, 7}, {1, 1185}, {2, 1202}}),
        row(5, {{0, 30}, {1, 7000}, {2, 7000}}),
        row(5, {{0, 31}, {1, 4242}, {2, 4242}}),
    }, std::string(1, '\0'));
    // ItemDisplayInfo: GroupSoundIndex is column 12.
    f.tables["ItemDisplayInfo.dbc"] = table(25, {
        row(25, {{0, 2453}, {12, 7}}),
        row(25, {{0, 2454}, {12, 30}}),
        row(25, {{0, 2455}, {12, 31}}),
        row(25, {{0, 2456}}),
    }, std::string(1, '\0'));
    SoundData data;
    REQUIRE(data.load(f.loader()));

    const auto* coin = data.coinSound();
    REQUIRE(coin != nullptr);
    REQUIRE((coin->id == 120 || coin->id == 895));
    REQUIRE(coin->files == std::vector<std::string>{"Sound\\Interface\\LootCoinSmall.wav"});
    REQUIRE(coin->volume == Catch::Approx(0.7f));

    const auto* pickup = data.itemFoleyEntry(2453, true);
    REQUIRE(pickup != nullptr);
    REQUIRE(pickup->id == 1185);
    REQUIRE(pickup->files[0] == "Sound\\interface\\PickUp\\PickUpCloth_Leather01.wav");
    REQUIRE(data.itemFoleyEntry(2453, false)->id == 1202);
    REQUIRE(data.itemFoleyEntry(2454, true) == nullptr);   // row has no files
    REQUIRE(data.itemFoleyEntry(2455, false) == nullptr);  // names no row
    REQUIRE(data.itemFoleyEntry(2456, true) == nullptr);   // display has no group
    REQUIRE(data.itemFoleyEntry(9999, true) == nullptr);   // unknown display
    REQUIRE(data.itemFoleyEntry(0, false) == nullptr);

    // The default fixture's SoundEntries has no coin row.
    Fixture plain;
    SoundData without;
    REQUIRE(without.load(plain.loader()));
    REQUIRE(without.coinSound() == nullptr);
    REQUIRE(without.itemFoleyEntry(2453, true) == nullptr);
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

TEST_CASE("SoundData finds an area's and a room's reverb", "[sound_data]") {
    Fixture f;
    // AreaTable columns 5 and 6: SoundProviderPref, and the underwater one.
    // Northshire (9) names neither and takes Elwynn's underwater row.
    Strings at;
    f.tables["AreaTable.dbc"] = table(36, {
        row(36, {{0, 12}, {6, 11}, {11, at.add("Elwynn Forest")}}),
        row(36, {{0, 9}, {2, 12}, {11, at.add("Northshire Valley")}}),
        row(36, {{0, 1519}, {5, 83}, {11, at.add("Stormwind City")}}),
    }, at.block);
    // A mine whose building row names a cave and whose group row names only
    // its music, and a group of its own with -1 (none).
    Strings wa;
    f.tables["WMOAreaTable.dbc"] = table(28, {
        row(28, {{0, 1}, {1, 77}, {2, 0}, {3, 0xFFFFFFFFu}, {4, 75}, {5, 11}, {11, wa.add("Fargodeep Mine")}}),
        row(28, {{0, 2}, {1, 77}, {2, 0}, {3, 4}, {7, 9}}),
        row(28, {{0, 3}, {1, 78}, {2, 0}, {3, 1}, {4, 0xFFFFFFFFu}}),
    }, wa.block);
    Strings sp;
    f.tables["SoundProviderPreferences.dbc"] = table(24, {
        row(24, {{0, 11}, {1, sp.add("Underwater")}, {4, bits(1.0f)}, {7, 0xFFFFFC18u /* -1000 */},
                 {8, 0xFFFFD8F0u /* -10000 */}, {9, bits(0.1f)}, {12, 1700}}),
        row(24, {{0, 75}, {1, sp.add("PRESET_CAVE")}, {4, bits(2.91f)}, {7, 0xFFFFFC18u},
                 {8, 0xFFFFFE0Cu /* -500 */}, {9, bits(1.3f)}, {11, bits(0.015f)},
                 {12, 0xFFFFFE6Eu /* -402 */}, {13, bits(0.022f)}}),
        row(24, {{0, 66}, {1, sp.add("PRESET_OFF")}, {4, bits(1.0f)}, {7, 0xFFFFD8F0u}}),
    }, sp.block);
    SoundData data;
    REQUIRE(data.load(f.loader()));

    REQUIRE(data.areaReverb(12, false) == 0);   // the open forest is dry
    REQUIRE(data.areaReverb(9, true) == 11);    // through the parent
    REQUIRE(data.areaReverb(1519, false) == 83);
    REQUIRE(data.wmoReverb(77, 0, 4, false) == 75);  // the building's row
    REQUIRE(data.wmoReverb(77, 0, 4, true) == 11);
    REQUIRE(data.wmoReverb(78, 0, 1, false) == 0);   // -1 is none

    const auto* cave = data.providerPref(75);
    REQUIRE(cave != nullptr);
    REQUIRE(cave->name == "PRESET_CAVE");
    REQUIRE(cave->room == -1000);
    REQUIRE(cave->reverb == -402);
    REQUIRE(data.providerPrefByName("Underwater")->id == 11);
    REQUIRE(data.providerPref(0) == nullptr);
    REQUIRE(data.providerPref(-1) == nullptr);

    // The tail: a cave rings longer and quieter than the generic room's
    // -8 dB, PRESET_OFF is silent, and nothing passes the cap.
    const auto caveReverb = wowee::audio::reverbFromProvider(*cave);
    REQUIRE(caveReverb.wet > 0.05f);
    REQUIRE(caveReverb.wet < wowee::audio::reverbFromProvider(wowee::audio::SoundProviderPref{}).wet);
    REQUIRE(caveReverb.decayTime == Catch::Approx(2.91f));
    REQUIRE(wowee::audio::reverbFromProvider(*data.providerPref(66)).wet == 0.0f);
    REQUIRE(wowee::audio::reverbFromProvider(*data.providerPref(11)).wet <= 0.45f);
}

TEST_CASE("UI messages play the sound Wow.exe's message table names", "[sound_data]") {
    using wowee::audio::uiMessageSoundName;
    auto named = [](std::string_view key) {
        const char* s = uiMessageSoundName(key);
        return s ? std::string(s) : std::string();
    };
    // First, last and a spread of rows between, so a misordered row (which
    // the binary search would miss) shows up.
    REQUIRE(named("ERR_ARENA_TEAM_CREATE_S") == "LEVELUP");
    REQUIRE(named("ERR_TRADE_REQUEST_S") == "LEVELUP");
    REQUIRE(named("ERR_DUEL_REQUESTED") == "LEVELUP");
    REQUIRE(named("ERR_INVITED_TO_GUILD_SSS") == "LEVELUP");
    REQUIRE(named("ERR_DECLINE_GROUP_S") == "igPlayerInviteDecline");
    REQUIRE(named("ERR_FRIEND_ONLINE_SS") == "FRIENDJOINGAME");
    REQUIRE(named("ERR_NEWTAXIPATH") == "TaxiNodeDiscovered");
    REQUIRE(named("ERR_MEETING_STONE_IN_QUEUE_S") == "PVPENTERQUEUE");
    REQUIRE(named("ERR_PETITION_SIGNED") == "igPlayerInviteAccept");
    REQUIRE(named("ERR_PETITION_SIGNED_S") == "igPlayerInviteAccept");
    REQUIRE(named("ERR_PETITION_DECLINED_S") == "igPlayerInviteDecline");
    REQUIRE(named("ERR_PETITION_NOT_ENOUGH_SIGNATURES") == "igPlayerInviteDecline");
    REQUIRE(named("ERR_QUEST_ACCEPTED_S") == "QUESTADDED");
    REQUIRE(named("ERR_QUEST_FAILED_S") == "igQuestFailed");
    REQUIRE(named("ERR_QUEST_LOG_FULL") == "igQuestFailed");
    REQUIRE(named("ERR_QUEST_MUST_CHOOSE") == "igQuestFailed");
    REQUIRE(named("ERR_RECEIVE_ITEM_S") == "ITEMGENERICSOUND");

    // Rows that are silent or speak a vocal line instead, a message that is
    // not in the table, and keys that are near misses.
    REQUIRE(uiMessageSoundName("ERR_INV_FULL") == nullptr);
    REQUIRE(uiMessageSoundName("ERR_OUT_OF_RANGE") == nullptr);
    REQUIRE(uiMessageSoundName("ERR_JOINED_GROUP_S") == nullptr);
    REQUIRE(uiMessageSoundName("ERR_LFG_JOINED_QUEUE") == nullptr);
    REQUIRE(uiMessageSoundName("err_duel_requested") == nullptr);
    REQUIRE(uiMessageSoundName("ERR_QUEST_FAILED") == nullptr);
    REQUIRE(uiMessageSoundName("") == nullptr);
}

TEST_CASE("WorldFx is transparent when dry and muffles under water", "[sound_data]") {
    wowee::audio::WorldFx fx;
    fx.prepare(48000, 2);
    std::vector<float> in(960), out(960);
    // A 5 kHz tone: passes untouched dry, mostly gone under water.
    auto run = [&](int blocks) {
        double sumIn = 0.0, sumOut = 0.0;
        long t = 0;
        for (int b = 0; b < blocks; ++b) {
            for (size_t i = 0; i < 480; ++i, ++t) {
                const float v = std::sin(6.2831853f * 5000.0f * static_cast<float>(t) / 48000.0f);
                in[2 * i] = in[2 * i + 1] = v;
            }
            fx.process(in.data(), out.data(), 480);
            if (b >= blocks / 2) {
                for (size_t i = 0; i < in.size(); ++i) {
                    sumIn += in[i] * in[i];
                    sumOut += out[i] * out[i];
                }
            }
        }
        return std::sqrt(sumOut / sumIn);
    };
    REQUIRE(run(20) == Catch::Approx(1.0).margin(1e-6));
    fx.setMuffle(1.0f);
    REQUIRE(run(200) < 0.1);
    fx.setMuffle(0.0f);
    REQUIRE(run(300) == Catch::Approx(1.0).margin(0.01));
}
