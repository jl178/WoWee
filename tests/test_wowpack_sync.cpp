/// The installer: a pack is fetched from a loopback server into a temporary
/// override directory, then changed, then removed.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <openssl/sha.h>

#include "core/wowpack_sync.hpp"
#include "local_http_server.hpp"

using namespace wowee::core;
namespace fs = std::filesystem;

namespace {

std::string sha256Hex(const std::string& data) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
    std::string hex;
    char two[3];
    for (unsigned char b : out) {
        std::snprintf(two, sizeof(two), "%02x", b);
        hex += two;
    }
    return hex;
}

struct Pack {
    LocalHttpServer& server;
    std::string version;
    std::vector<std::pair<std::string, std::string>> files;   // path, content
    bool gzip = false;   // serve the files gzipped to a client that asks

    void publish() const {
        std::string list;
        for (const auto& [path, content] : files) {
            if (!list.empty()) list += ",";
            list += R"({"path":")" + path + R"(","size":)" + std::to_string(content.size()) +
                    R"(,"sha256":")" + sha256Hex(content) + "\"}";
            server.set("/pack/" + path, {.body = content, .gzip = gzip});
        }
        server.set("/pack/wowpack.json",
                   {.body = R"({"format":1,"name":"isle","version":")" + version +
                            R"(","expansion":"wotlk","build":12340,"files":[)" + list + "]}"});
    }
};

void waitDone(WowpackSync& sync) {
    for (int i = 0; i < 400 && sync.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE_FALSE(sync.running());
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / ("wowee_sync_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

}  // namespace

TEST_CASE("install, update and remove a pack", "[wowpack]") {
    LocalHttpServer server;
    TempDir dir;
    const fs::path override = dir.path / "override";
    fs::create_directories(override);
    // A file of the player's own, which no sync may touch.
    fs::create_directories(override / "textures");
    std::ofstream(override / "textures" / "mine.blp") << "mine";

    Pack pack{server, "1", {{"dbfilesclient/map.dbc", "MAP1"}, {"world/maps/isle/isle.wdt", std::string(5000, 'w')}}};
    pack.publish();

    WowpackSync sync;
    sync.start(server.url("/pack/wowpack.json"), override.string(), "wotlk");
    waitDone(sync);
    INFO(sync.error());
    REQUIRE(sync.state() == WowpackSync::State::Done);
    CHECK(sync.takeChanged());
    CHECK(readFile(override / "dbfilesclient" / "map.dbc") == "MAP1");
    CHECK(fs::file_size(override / "world" / "maps" / "isle" / "isle.wdt") == 5000);
    CHECK(fs::exists(override / ".wowpack" / "installed.json"));
    CHECK(sync.packName() == "isle");
    const int firstRequests = server.requests();

    // Same URL again this session: nothing is fetched.
    sync.start(server.url("/pack/wowpack.json"), override.string(), "wotlk");
    waitDone(sync);
    CHECK(server.requests() == firstRequests);

    // A new version changes one file and drops another.
    WowpackSync second;
    pack.version = "2";
    pack.files = {{"dbfilesclient/map.dbc", "MAP2"}};
    pack.publish();
    second.start(server.url("/pack/wowpack.json"), override.string(), "wotlk");
    waitDone(second);
    INFO(second.error());
    REQUIRE(second.state() == WowpackSync::State::Done);
    CHECK(second.takeChanged());
    CHECK(readFile(override / "dbfilesclient" / "map.dbc") == "MAP2");
    CHECK_FALSE(fs::exists(override / "world" / "maps" / "isle" / "isle.wdt"));
    CHECK_FALSE(fs::exists(override / "world"));   // emptied directories are pruned
    CHECK(fs::exists(override / "textures" / "mine.blp"));

    // No URL: the pack goes, the player's file stays.
    WowpackSync third;
    third.start("", override.string(), "wotlk");
    waitDone(third);
    REQUIRE(third.state() == WowpackSync::State::Done);
    CHECK(third.takeChanged());
    CHECK_FALSE(fs::exists(override / "dbfilesclient"));
    CHECK_FALSE(fs::exists(override / ".wowpack"));
    CHECK(readFile(override / "textures" / "mine.blp") == "mine");
}

TEST_CASE("a gzip-serving pack installs with the sizes and hashes of the plain files", "[wowpack]") {
    LocalHttpServer server;
    TempDir dir;
    const fs::path override = dir.path / "override";
    fs::create_directories(override);
    std::string wdt(200000, 'w');
    for (std::size_t i = 0; i < wdt.size(); i += 13) wdt[i] = static_cast<char>('a' + i % 26);
    Pack pack{server, "1", {{"dbfilesclient/map.dbc", "MAP1"}, {"world/maps/isle/isle.wdt", wdt}}, true};
    pack.publish();

    WowpackSync sync;
    sync.start(server.url("/pack/wowpack.json"), override.string(), "wotlk");
    waitDone(sync);
    INFO(sync.error());
    REQUIRE(sync.state() == WowpackSync::State::Done);
    CHECK(server.lastRequest().find("Accept-Encoding: gzip") != std::string::npos);
    CHECK(readFile(override / "world" / "maps" / "isle" / "isle.wdt") == wdt);
    CHECK(readFile(override / "dbfilesclient" / "map.dbc") == "MAP1");
    CHECK(sync.bytesDone() == wdt.size() + 4);
    CHECK(sync.bytesDone() == sync.bytesTotal());
}

TEST_CASE("a hash mismatch leaves nothing behind", "[wowpack]") {
    LocalHttpServer server;
    TempDir dir;
    server.set("/pack/wowpack.json", {.body = R"({"format":1,"name":"isle","version":"1","expansion":"wotlk","files":[{"path":"dbfilesclient/map.dbc","size":4,"sha256":")" + std::string(64, '0') + "\"}]}"});
    server.set("/pack/dbfilesclient/map.dbc", {.body = "MAP1"});
    WowpackSync sync;
    sync.start(server.url("/pack/wowpack.json"), dir.path.string(), "wotlk");
    waitDone(sync);
    CHECK(sync.state() == WowpackSync::State::Failed);
    CHECK(sync.error().find("did not match") != std::string::npos);
    CHECK_FALSE(fs::exists(dir.path / "dbfilesclient" / "map.dbc"));
    CHECK_FALSE(sync.takeChanged());
}

TEST_CASE("the wrong expansion and a bad index are refused", "[wowpack]") {
    LocalHttpServer server;
    TempDir dir;
    server.set("/pack/wowpack.json", {.body = R"({"format":1,"name":"isle","version":"1","expansion":"tbc","files":[]})"});
    WowpackSync sync;
    sync.start(server.url("/pack/wowpack.json"), dir.path.string(), "wotlk");
    waitDone(sync);
    CHECK(sync.state() == WowpackSync::State::Failed);
    CHECK(sync.error().find("tbc") != std::string::npos);

    WowpackSync other;
    other.start(server.url("/nowhere/wowpack.json"), dir.path.string(), "wotlk");
    waitDone(other);
    CHECK(other.state() == WowpackSync::State::Failed);
    CHECK(other.error().find("404") != std::string::npos);
}

TEST_CASE("plain http to a public host is refused before any request", "[wowpack]") {
    TempDir dir;
    WowpackSync sync;
    sync.start("http://example.com/wowpack.json", dir.path.string(), "wotlk");
    waitDone(sync);
    CHECK(sync.state() == WowpackSync::State::Failed);
    CHECK(sync.error().find("https") != std::string::npos);
}
