/// http_fetch against a server on the loopback: the transport, the body
/// framing and the cap are what decide whether a pack lands whole.

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/http_fetch.hpp"
#include "local_http_server.hpp"

using namespace wowee::core;

namespace {

FetchOptions lanOptions() {
    FetchOptions o;
    o.allowPlainHttp = true;
    o.timeoutSeconds = 5;
    return o;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("url parsing", "[http]") {
    HttpUrl u;
    std::string err;
    REQUIRE(parseHttpUrl("https://realm.example/pack/wowpack.json", &u, &err));
    CHECK(u.scheme == "https");
    CHECK(u.host == "realm.example");
    CHECK(u.port == "443");
    CHECK(u.path == "/pack/wowpack.json");
    REQUIRE(parseHttpUrl("http://192.168.1.5:8090", &u, &err));
    CHECK(u.port == "8090");
    CHECK(u.path == "/");
    REQUIRE(parseHttpUrl("http://[::1]:81/x?y=1#frag", &u, &err));
    CHECK(u.host == "::1");
    CHECK(u.path == "/x?y=1");
    CHECK_FALSE(parseHttpUrl("ftp://a/b", &u, &err));
    CHECK_FALSE(parseHttpUrl("https://user@host/", &u, &err));
    CHECK_FALSE(parseHttpUrl("nohost", &u, &err));
    CHECK_FALSE(parseHttpUrl("https://host:abc/", &u, &err));
    CHECK(httpUrlDirectory("https://h/a/b/wowpack.json?x") == "https://h/a/b/");
    CHECK(httpUrlDirectory("https://h") == "https://h/");
}

TEST_CASE("a content-length body arrives whole with its hash", "[http]") {
    LocalHttpServer server;
    server.set("/abc", {.body = "abc"});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/abc"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(r.status == 200);
    CHECK(body == "abc");
    CHECK(r.bytes == 3);
    CHECK(r.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("a chunked body is decoded across chunk boundaries", "[http]") {
    LocalHttpServer server;
    std::string big(100000, 'x');
    for (std::size_t i = 0; i < big.size(); i += 7) big[i] = 'y';
    server.set("/chunked", {.body = big, .chunked = true});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/chunked"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(body == big);
    CHECK(r.bytes == big.size());
}

TEST_CASE("a 404 is reported and its body is not delivered", "[http]") {
    LocalHttpServer server;
    std::string body = "untouched";
    const FetchResult r = httpFetchToString(server.url("/missing"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(r.status == 404);
    CHECK(body.empty());
}

TEST_CASE("the cap stops a reply that is too large", "[http]") {
    LocalHttpServer server;
    server.set("/big", {.body = std::string(5000, 'a')});
    FetchOptions o = lanOptions();
    o.maxBytes = 1000;
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/big"), o, &body);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("larger") != std::string::npos);
}

TEST_CASE("a body shorter than its content-length is an error", "[http]") {
    LocalHttpServer server;
    server.set("/short", {.body = std::string(3000, 'b'), .truncate = 500});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/short"), lanOptions(), &body);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("closed after") != std::string::npos);
}

TEST_CASE("one redirect is followed, a second is not", "[http]") {
    LocalHttpServer server;
    server.set("/moved", {.status = 302, .headers = "Location: " + server.url("/final") + "\r\n"});
    server.set("/final", {.body = "landed"});
    server.set("/twice", {.status = 301, .headers = "Location: " + server.url("/moved") + "\r\n"});
    std::string body;
    FetchResult r = httpFetchToString(server.url("/moved"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(body == "landed");
    r = httpFetchToString(server.url("/twice"), lanOptions(), &body);
    CHECK_FALSE(r.ok);
    FetchOptions noFollow = lanOptions();
    noFollow.followRedirect = false;
    r = httpFetchToString(server.url("/moved"), noFollow, &body);
    CHECK_FALSE(r.ok);
}

TEST_CASE("plain http is refused unless allowed", "[http]") {
    LocalHttpServer server;
    server.set("/x", {.body = "x"});
    std::string body;
    FetchOptions strict;
    const FetchResult r = httpFetchToString(server.url("/x"), strict, &body);
    CHECK_FALSE(r.ok);
    CHECK(server.requests() == 0);
}

TEST_CASE("progress can cancel and a cancelled file is removed", "[http]") {
    LocalHttpServer server;
    server.set("/file", {.body = std::string(200000, 'z')});
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "wowee_fetch_cancel.bin";
    const FetchResult r = httpFetchToFile(server.url("/file"), lanOptions(), out.string(),
                                          [](std::uint64_t, std::uint64_t) { return false; });
    CHECK_FALSE(r.ok);
    CHECK_FALSE(std::filesystem::exists(out));
}

TEST_CASE("a file download lands byte for byte", "[http]") {
    LocalHttpServer server;
    std::string data(70000, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31);
    server.set("/data.bin", {.body = data});
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "wowee_fetch_ok.bin";
    std::uint64_t seenTotal = 0;
    const FetchResult r = httpFetchToFile(server.url("/data.bin"), lanOptions(), out.string(),
                                          [&](std::uint64_t, std::uint64_t total) { seenTotal = total; return true; });
    REQUIRE(r.ok);
    CHECK(readFile(out) == data);
    CHECK(seenTotal == data.size());
    std::filesystem::remove(out);
}
