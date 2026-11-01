// The zone music controller is the stock client's music schedule: a track on
// arrival, silence between tracks, day and night pools, a city intro once an
// hour. It is driven here with a scripted clock and a fake music channel.
#include <catch_amalgamated.hpp>
#include "audio/zone_music_controller.hpp"
#include "pipeline/dbc_loader.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

using wowee::audio::SoundData;
using wowee::audio::ZoneMusicAction;
using wowee::audio::ZoneMusicController;
using wowee::audio::ZoneMusicInput;
using wowee::pipeline::DBCFile;

namespace {

struct Strings {
    std::string block{std::string(1, '\0')};
    uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        const auto off = static_cast<uint32_t>(block.size());
        block += s;
        block.push_back('\0');
        return off;
    }
};

std::shared_ptr<DBCFile> table(uint32_t fields, const std::vector<std::vector<uint32_t>>& rows, const std::string& strings) {
    std::vector<uint8_t> bytes;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    bytes.insert(bytes.end(), {'W', 'D', 'B', 'C'});
    u32(static_cast<uint32_t>(rows.size()));
    u32(fields);
    u32(fields * 4);
    u32(static_cast<uint32_t>(strings.size()));
    for (const auto& row : rows) {
        REQUIRE(row.size() == fields);
        for (uint32_t v : row) u32(v);
    }
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(bytes));
    return dbc;
}

std::vector<uint32_t> row(uint32_t fields, std::map<uint32_t, uint32_t> set) {
    std::vector<uint32_t> r(fields, 0);
    for (auto [i, v] : set) r[i] = v;
    return r;
}

// Forest (area 12, set 1): one day track, one night track, 10-20 s silence.
// Northshire (9) inherits it. Stormwind (1519, set 13): one track, an intro
// with a 60 minute delay, 1-2 s silence. Area 300 has no music at all.
struct World {
    SoundData data;
    World() {
        std::map<std::string, std::shared_ptr<DBCFile>> t;
        Strings se;
        t["SoundEntries.dbc"] = table(30, {
            row(30, {{0, 10}, {2, se.add("Forest Day")}, {3, se.add("Day.mp3")}, {23, se.add("Forest")}, {24, 0x3ECCCCCD}}),   // 0.4
            row(30, {{0, 11}, {2, se.add("Forest Night")}, {3, se.add("Night.mp3")}, {23, se.add("Forest")}, {24, 0x3ECCCCCD}}),
            row(30, {{0, 20}, {2, se.add("Stormwind")}, {3, se.add("City.mp3")}, {23, se.add("City")}, {24, 0x3F000000}}),      // 0.5
            row(30, {{0, 21}, {2, se.add("Stormwind Intro")}, {3, se.add("Intro.mp3")}, {23, se.add("City")}, {24, 0x3F000000}}),
        }, se.block);
        Strings zm;
        t["ZoneMusic.dbc"] = table(8, {
            row(8, {{0, 1}, {1, zm.add("Forest")}, {2, 10000}, {3, 10000}, {4, 20000}, {5, 20000}, {6, 10}, {7, 11}}),
            row(8, {{0, 13}, {1, zm.add("Stormwind")}, {2, 1000}, {3, 1000}, {4, 2000}, {5, 2000}, {6, 20}, {7, 0}}),
        }, zm.block);
        t["ZoneIntroMusicTable.dbc"] = table(5, {row(5, {{0, 61}, {2, 21}, {3, 1}, {4, 60}})}, std::string(1, '\0'));
        Strings at;
        t["AreaTable.dbc"] = table(36, {
            row(36, {{0, 12}, {8, 1}, {11, at.add("Elwynn")}}),
            row(36, {{0, 9}, {2, 12}, {11, at.add("Northshire")}}),
            row(36, {{0, 1519}, {8, 13}, {9, 61}, {11, at.add("Stormwind")}}),
            row(36, {{0, 300}, {11, at.add("Quiet")}}),
        }, at.block);
        REQUIRE(data.load([t](const std::string& n) -> std::shared_ptr<DBCFile> {
            auto it = t.find(n);
            return it == t.end() ? nullptr : it->second;
        }));
    }
};

// A music channel that plays whatever it is told for `length` seconds.
struct Channel {
    bool playing = false;
    float left = 0.0f;
    std::string track;
    int starts = 0;
    int stops = 0;
    void apply(const ZoneMusicAction& a, float length) {
        if (a.kind == ZoneMusicAction::Kind::Play) {
            playing = true;
            left = length;
            track = a.path;
            ++starts;
        } else if (a.kind == ZoneMusicAction::Kind::Stop) {
            playing = false;
            ++stops;
        }
    }
    void tick(float dt) {
        if (!playing) return;
        left -= dt;
        if (left <= 0.0f) playing = false;
    }
};

ZoneMusicInput at(uint32_t area, bool day, const Channel& ch, float dt = 0.1f) {
    ZoneMusicInput in;
    in.areaId = area;
    in.isDay = day;
    in.trackPlaying = ch.playing;
    in.deltaTime = dt;
    return in;
}

// Run `seconds` of frames in one area. Returns how many tracks started.
int run(ZoneMusicController& c, Channel& ch, uint32_t area, bool day, float seconds, float trackLength = 5.0f) {
    const int before = ch.starts;
    for (float t = 0.0f; t < seconds; t += 0.1f) {
        ch.apply(c.update(at(area, day, ch)), trackLength);
        ch.tick(0.1f);
    }
    return ch.starts - before;
}

} // namespace

TEST_CASE("Zone music starts on arrival and rests between tracks", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    auto first = c.update(at(12, true, ch));
    REQUIRE(first.kind == ZoneMusicAction::Kind::Play);
    REQUIRE(first.path == "Forest\\Day.mp3");
    REQUIRE(first.volume == Catch::Approx(0.4f));
    ch.apply(first, 5.0f);

    // The track runs 5 s, then the set's 10-20 s silence, then the next one.
    REQUIRE(run(c, ch, 12, true, 5.0f) == 0);
    REQUIRE_FALSE(ch.playing);
    REQUIRE(run(c, ch, 12, true, 9.5f) == 0);
    REQUIRE(c.silenceRemaining() > 0.0f);
    REQUIRE(run(c, ch, 12, true, 11.0f) == 1);
}

TEST_CASE("Zone music picks the night pool after dark", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    ch.apply(c.update(at(12, false, ch)), 5.0f);
    REQUIRE(ch.track == "Forest\\Night.mp3");
    // Day comes while the track plays; the next pick is from the day pool.
    run(c, ch, 12, true, 30.0f);
    REQUIRE(ch.track == "Forest\\Day.mp3");
}

TEST_CASE("Zone music keeps playing across areas that share a set", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    ch.apply(c.update(at(9, true, ch)), 100.0f);
    REQUIRE(ch.starts == 1);
    REQUIRE(run(c, ch, 12, true, 2.0f) == 0);  // Northshire -> Elwynn: same set
    REQUIRE(ch.playing);
    REQUIRE(c.currentSetId() == 1);
}

TEST_CASE("Zone music changes at once when the set changes and stops where there is none", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    ch.apply(c.update(at(12, true, ch)), 100.0f);
    auto city = c.update(at(1519, true, ch));
    REQUIRE(city.kind == ZoneMusicAction::Kind::Play);
    REQUIRE(city.path == "City\\Intro.mp3");  // the intro plays first
    ch.apply(city, 5.0f);
    REQUIRE(run(c, ch, 1519, true, 8.0f) == 1);
    REQUIRE(ch.track == "City\\City.mp3");

    auto quiet = c.update(at(300, true, ch));
    REQUIRE(quiet.kind == ZoneMusicAction::Kind::Stop);
    ch.apply(quiet, 0.0f);
    REQUIRE(run(c, ch, 300, true, 30.0f) == 0);
    REQUIRE(c.currentSetId() == 0);
}

TEST_CASE("Zone music plays a city intro once per its delay", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    ch.apply(c.update(at(1519, true, ch)), 5.0f);
    REQUIRE(ch.track == "City\\Intro.mp3");
    run(c, ch, 1519, true, 10.0f);
    // Leave and come straight back: no second intro.
    ch.apply(c.update(at(300, true, ch)), 0.0f);
    auto back = c.update(at(1519, true, ch));
    REQUIRE(back.kind == ZoneMusicAction::Kind::Play);
    REQUIRE(back.path == "City\\City.mp3");
    ch.apply(back, 5.0f);
    // An hour later the intro is due again.
    run(c, ch, 1519, true, 3601.0f, 5.0f);
    ch.apply(c.update(at(300, true, ch)), 0.0f);
    REQUIRE(c.update(at(1519, true, ch)).path == "City\\Intro.mp3");
}

TEST_CASE("Zone music mixes extra tracks into the pool and waits for a slow start", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 7);
    Channel ch;
    bool sawExtra = false;
    // Each cycle is a second of track and ten to twenty of silence.
    for (int i = 0; i < 20000 && !sawExtra; ++i) {
        ZoneMusicInput in = at(12, true, ch, 0.1f);
        in.extraTracks = {"file:/tmp/own.mp3"};
        auto a = c.update(in);
        if (a.kind == ZoneMusicAction::Kind::Play) {
            sawExtra = a.path == "file:/tmp/own.mp3";
            // A started track is not reported playing for a while. The
            // controller waits rather than starting another.
            for (int g = 0; g < 10; ++g) REQUIRE(c.update(in).kind == ZoneMusicAction::Kind::None);
            ch.apply(a, 1.0f);
            run(c, ch, 12, true, 25.0f, 1.0f);
        }
    }
    REQUIRE(sawExtra);
}

TEST_CASE("Zone music ignores an unknown area", "[zone_music]") {
    World w;
    ZoneMusicController c(w.data, 1);
    Channel ch;
    REQUIRE(c.update(at(0, true, ch)).kind == ZoneMusicAction::Kind::None);
    ch.apply(c.update(at(12, true, ch)), 100.0f);
    REQUIRE(c.update(at(0, true, ch)).kind == ZoneMusicAction::Kind::None);  // chunk not resident: keep playing
    REQUIRE(ch.playing);
    c.reset();
    REQUIRE(c.currentSetId() == 0);
}
