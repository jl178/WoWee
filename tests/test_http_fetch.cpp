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

namespace {

/// Bytes that compress, but not to nothing: a pack file is closer to this
/// than to a run of zeros.
std::string patterned(std::size_t n) {
    std::string s(n, '\0');
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        s[i] = "ABCDEFGH"[(x >> 16) & 7];
    }
    return s;
}

}  // namespace

TEST_CASE("gzip is asked for and a gzip body is inflated with the hash of the plain bytes", "[http][gzip]") {
    LocalHttpServer server;
    const std::string plain = patterned(300000);
    server.set("/g", {.body = plain, .gzip = true});
    server.set("/plain", {.body = plain});
    std::string body;
    std::uint64_t lastGot = 0;
    std::uint64_t lastTotal = 1;
    const FetchResult r = httpFetch(server.url("/g"), lanOptions(),
        [&](const char* d, std::size_t n) { body.append(d, n); return true; },
        [&](std::uint64_t got, std::uint64_t total) { lastGot = got; lastTotal = total; return true; });
    REQUIRE(r.ok);
    CHECK(server.lastRequest().find("\r\nAccept-Encoding: gzip\r\n") != std::string::npos);
    CHECK(r.contentEncoding == "gzip");
    CHECK(body == plain);
    CHECK(r.bytes == plain.size());
    CHECK(r.wireBytes == LocalHttpServer::gzipBytes(plain).size());
    CHECK(r.wireBytes < plain.size() / 2);
    // Progress counts inflated bytes, and the compressed Content-Length is
    // not offered as their total.
    CHECK(lastGot == plain.size());
    CHECK(lastTotal == 0);

    std::string plainBody;
    const FetchResult p = httpFetchToString(server.url("/plain"), lanOptions(), &plainBody);
    REQUIRE(p.ok);
    CHECK(p.sha256 == r.sha256);
}

TEST_CASE("gzip inside a chunked body is decoded in both layers", "[http][gzip]") {
    LocalHttpServer server;
    const std::string plain = patterned(200000);
    server.set("/gc", {.body = plain, .chunked = true, .gzip = true});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/gc"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(r.contentEncoding == "gzip");
    CHECK(body == plain);
    CHECK(r.bytes == plain.size());
    CHECK(r.wireBytes == LocalHttpServer::gzipBytes(plain).size());
}

TEST_CASE("concatenated gzip members are one body", "[http][gzip]") {
    LocalHttpServer server;
    server.set("/two", {.body = LocalHttpServer::gzipBytes("first,") + LocalHttpServer::gzipBytes("second"),
                        .headers = "Content-Encoding: gzip\r\n"});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/two"), lanOptions(), &body);
    REQUIRE(r.ok);
    CHECK(body == "first,second");
}

TEST_CASE("a corrupt or cut gzip body is an error and leaves no file", "[http][gzip]") {
    LocalHttpServer server;
    const std::string plain = patterned(100000);
    const std::string good = LocalHttpServer::gzipBytes(plain);
    std::string bad = good;
    for (std::size_t i = bad.size() / 2; i < bad.size() / 2 + 64; ++i) bad[i] = static_cast<char>(bad[i] ^ 0x5a);
    server.set("/bad", {.body = bad, .headers = "Content-Encoding: gzip\r\n"});
    std::string crc = good;
    crc[crc.size() - 6] = static_cast<char>(crc[crc.size() - 6] ^ 1);   // inside the CRC-32
    server.set("/crc", {.body = crc, .headers = "Content-Encoding: gzip\r\n"});
    // Framing that agrees with itself around a gzip stream with no trailer.
    server.set("/cut", {.body = good.substr(0, good.size() - 10), .headers = "Content-Encoding: gzip\r\n"});
    server.set("/garbage", {.body = "this is not gzip at all", .headers = "Content-Encoding: gzip\r\n"});

    const std::filesystem::path out = std::filesystem::temp_directory_path() / "wowee_fetch_badgz.bin";
    for (const char* path : {"/bad", "/crc", "/cut", "/garbage"}) {
        INFO(path);
        const FetchResult r = httpFetchToFile(server.url(path), lanOptions(), out.string());
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("gzip") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(out));
    }
}

TEST_CASE("the cap counts inflated bytes and stops a gzip bomb early", "[http][gzip]") {
    LocalHttpServer server;
    // 64 MB of zeros is about 64 KB compressed.
    const std::string bomb = LocalHttpServer::gzipBytes(std::string(64u * 1024u * 1024u, '\0'));
    REQUIRE(bomb.size() < 200000);
    server.set("/bomb", {.body = bomb, .headers = "Content-Encoding: gzip\r\n"});
    FetchOptions o = lanOptions();
    o.maxBytes = 1000000;
    std::uint64_t delivered = 0;
    const FetchResult r = httpFetch(server.url("/bomb"), o,
        [&](const char*, std::size_t n) { delivered += n; return true; });
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("inflated reply is larger") != std::string::npos);
    CHECK(delivered <= o.maxBytes);

    // A body that inflates to exactly the cap gets through; one byte less
    // of cap and it does not.
    const std::string plain = patterned(50000);
    server.set("/fit", {.body = plain, .gzip = true});
    o.maxBytes = plain.size();
    std::string body;
    CHECK(httpFetchToString(server.url("/fit"), o, &body).ok);
    o.maxBytes = plain.size() - 1;
    CHECK_FALSE(httpFetchToString(server.url("/fit"), o, &body).ok);
}

TEST_CASE("a server that ignores Accept-Encoding is read as is", "[http][gzip]") {
    LocalHttpServer server;
    const std::string plain = patterned(80000);
    server.set("/ignore", {.body = plain});
    std::string body;
    std::uint64_t lastTotal = 0;
    const FetchResult r = httpFetch(server.url("/ignore"), lanOptions(),
        [&](const char* d, std::size_t n) { body.append(d, n); return true; },
        [&](std::uint64_t, std::uint64_t total) { lastTotal = total; return true; });
    REQUIRE(r.ok);
    CHECK(server.lastRequest().find("Accept-Encoding: gzip") != std::string::npos);
    CHECK(r.contentEncoding.empty());
    CHECK(body == plain);
    CHECK(r.wireBytes == plain.size());
    CHECK(lastTotal == plain.size());

    // A client that does not ask gets the plain body from a server that
    // would have compressed it.
    server.set("/would", {.body = plain, .gzip = true});
    FetchOptions o = lanOptions();
    o.acceptGzip = false;
    const FetchResult n = httpFetchToString(server.url("/would"), o, &body);
    REQUIRE(n.ok);
    CHECK(server.lastRequest().find("Accept-Encoding") == std::string::npos);
    CHECK(n.contentEncoding.empty());
    CHECK(body == plain);
}

TEST_CASE("an encoding that was not asked for is refused by name", "[http][gzip]") {
    LocalHttpServer server;
    server.set("/br", {.body = "xyz", .headers = "Content-Encoding: br\r\n"});
    std::string body;
    const FetchResult r = httpFetchToString(server.url("/br"), lanOptions(), &body);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("br") != std::string::npos);
    // An error page that is dropped unread is not decoded, so its encoding
    // does not matter.
    server.set("/gone", {.status = 410, .body = "zz", .headers = "Content-Encoding: br\r\n"});
    const FetchResult g = httpFetchToString(server.url("/gone"), lanOptions(), &body);
    CHECK(g.ok);
    CHECK(g.status == 410);
}
