/// What a content pack may name and what a sync must do about it. No
/// network: the parsing and the plan are the part that decides what lands
/// under Data/.

#include <catch_amalgamated.hpp>

#include <map>
#include <string>

#include "core/wowpack_index.hpp"

using namespace wowee::core::wowpack;

namespace {

const std::string kSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string indexJson(const std::string& files) {
    return R"({"format":1,"name":"isle","version":"1","expansion":"wotlk","build":12340,"files":[)" + files + "]}";
}

std::string file(const std::string& path, std::uint64_t size = 3, const std::string& sha = kSha) {
    return R"({"path":")" + path + R"(","size":)" + std::to_string(size) + R"(,"sha256":")" + sha + "\"}";
}

}  // namespace

TEST_CASE("safe paths", "[wowpack]") {
    CHECK(isSafePackPath("dbfilesclient/map.dbc"));
    CHECK(isSafePackPath("world/maps/tolbarad/tolbarad_33_35.adt"));
    CHECK(isSafePackPath("interface/addons/myzone/myzone.toc"));
    CHECK(isSafePackPath("textures/minimap/md5translate.trs"));
}

TEST_CASE("unsafe paths are refused", "[wowpack]") {
    for (const char* bad : {
             "", "map.dbc", "dbfilesclient", "/dbfilesclient/map.dbc", "dbfilesclient//map.dbc",
             "dbfilesclient/../wowee.exe", "dbfilesclient/./map.dbc", "DBFilesClient/Map.dbc",
             "dbfilesclient\\map.dbc", "dbfilesclient/.hidden", "dbfilesclient/-flag",
             "dbfilesclient/map.", "dbfilesclient/con.dbc", "dbfilesclient/map dbc",
             "secret/key", ".wowpack/installed.json", "a/b/c/d/e/f/g/h/i.x",
             "dbfilesclient/m:ap.dbc", "dbfilesclient/map.dbc?x", "dbfilesclient/ma..p.dbc",
         }) {
        INFO(bad);
        CHECK_FALSE(isSafePackPath(bad));
    }
    CHECK_FALSE(isSafePackPath(std::string("world/") + std::string(300, 'a')));
}

TEST_CASE("a good index parses", "[wowpack]") {
    const Index i = parseIndex(indexJson(file("dbfilesclient/map.dbc") + "," + file("world/maps/x/x.wdt", 10)));
    REQUIRE(i.ok);
    CHECK(i.name == "isle");
    CHECK(i.expansion == "wotlk");
    REQUIRE(i.files.size() == 2);
    CHECK(i.files[1].size == 10);
}

TEST_CASE("bad indexes are refused whole", "[wowpack]") {
    CHECK_FALSE(parseIndex("not json").ok);
    CHECK_FALSE(parseIndex("[]").ok);
    CHECK_FALSE(parseIndex(R"({"format":2,"name":"a","version":"1","expansion":"wotlk","files":[]})").ok);
    CHECK_FALSE(parseIndex(R"({"format":1,"version":"1","expansion":"wotlk","files":[]})").ok);
    CHECK_FALSE(parseIndex(indexJson(file("../x"))).ok);
    CHECK_FALSE(parseIndex(indexJson(file("dbfilesclient/map.dbc", 3, "abc"))).ok);
    CHECK_FALSE(parseIndex(indexJson(file("dbfilesclient/map.dbc") + "," + file("dbfilesclient/map.dbc"))).ok);
    CHECK_FALSE(parseIndex(indexJson(file("dbfilesclient/map.dbc", kMaxFileBytes + 1))).ok);
    CHECK_FALSE(parseIndex(indexJson(R"({"path":"dbfilesclient/map.dbc"})")).ok);
}

TEST_CASE("installed record round trips", "[wowpack]") {
    Installed in;
    in.url = "https://h/p/wowpack.json";
    in.name = "isle";
    in.version = "2";
    in.files = {{"dbfilesclient/map.dbc", 3, kSha}};
    Installed out;
    REQUIRE(parseInstalled(serializeInstalled(in), &out));
    CHECK(out.url == in.url);
    CHECK(out.version == "2");
    REQUIRE(out.files.size() == 1);
    CHECK(out.files[0].path == "dbfilesclient/map.dbc");
    CHECK_FALSE(parseInstalled("", &out));
    CHECK_FALSE(parseInstalled(R"({"files":[{"path":"../x","size":1,"sha256":")" + kSha + "\"}]}", &out));
}

TEST_CASE("the plan downloads what changed and removes what left", "[wowpack]") {
    const Index idx = parseIndex(indexJson(
        file("dbfilesclient/map.dbc", 3) + "," + file("world/maps/x/x.wdt", 10) + "," +
        file("world/maps/x/x_1_1.adt", 20, std::string(64, 'b'))));
    REQUIRE(idx.ok);
    Installed have;
    have.files = {
        {"dbfilesclient/map.dbc", 3, kSha},                 // unchanged, on disk
        {"world/maps/x/x.wdt", 10, kSha},                   // unchanged but missing on disk
        {"world/maps/x/x_1_1.adt", 20, std::string(64, 'a')}, // hash changed
        {"world/maps/x/x_2_2.adt", 20, kSha},               // no longer listed
    };
    std::map<std::string, std::int64_t> disk = {{"dbfilesclient/map.dbc", 3}, {"world/maps/x/x_1_1.adt", 20}};
    const Plan p = planSync(idx, &have, [&](const std::string& path) {
        const auto it = disk.find(path);
        return it == disk.end() ? -1 : it->second;
    });
    REQUIRE(p.keep.size() == 1);
    CHECK(p.keep[0].path == "dbfilesclient/map.dbc");
    REQUIRE(p.download.size() == 2);
    CHECK(p.downloadBytes == 30);
    REQUIRE(p.remove.size() == 1);
    CHECK(p.remove[0] == "world/maps/x/x_2_2.adt");

    const Plan fresh = planSync(idx, nullptr, [](const std::string&) { return -1; });
    CHECK(fresh.download.size() == 3);
    CHECK(fresh.remove.empty());
}

TEST_CASE("private hosts", "[wowpack]") {
    CHECK(isPrivateHost("localhost"));
    CHECK(isPrivateHost("127.0.0.1"));
    CHECK(isPrivateHost("10.2.0.5"));
    CHECK(isPrivateHost("192.168.1.121"));
    CHECK(isPrivateHost("172.16.0.1"));
    CHECK(isPrivateHost("172.31.255.255"));
    CHECK_FALSE(isPrivateHost("172.32.0.1"));
    CHECK_FALSE(isPrivateHost("8.8.8.8"));
    CHECK_FALSE(isPrivateHost("realm.example"));
    CHECK_FALSE(isPrivateHost("192.168.1"));
}
