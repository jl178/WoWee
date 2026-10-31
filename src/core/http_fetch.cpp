#include "core/http_fetch.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include "network/net_platform.hpp"

#ifdef _WIN32
#include <wincrypt.h>
#endif

namespace wowee::core {
namespace {

#ifdef _WIN32
/// OpenSSL has no trust store of its own on Windows, so the default verify
/// paths find nothing and every chain fails. Hand it the system's roots.
int loadWindowsRoots(SSL_CTX* ctx) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr || sys == nullptr) return 0;

    int added = 0;
    for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(sys, nullptr); cert != nullptr;
         cert = CertEnumCertificatesInStore(sys, cert)) {
        const unsigned char* der = cert->pbCertEncoded;
        X509* x = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded));
        if (x == nullptr) continue;
        if (X509_STORE_add_cert(store, x) == 1) ++added;
        X509_free(x);
    }
    CertCloseStore(sys, 0);
    ERR_clear_error();   // duplicate certificates queue an error that is not one
    return added;
}
#endif

std::string sslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) return "no detail";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Connect a plain TCP socket, with the timeout on the socket rather than on
/// connect(): the handshake and the reads that follow are where a wedged host
/// costs, and SO_RCVTIMEO covers those.
socket_t connectTcp(const std::string& host, const std::string& port, int timeoutSeconds,
                    std::string* error) {
    net::ensureInit();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        *error = "cannot resolve " + host;
        return INVALID_SOCK;
    }

    socket_t sock = INVALID_SOCK;
    for (addrinfo* at = found; at != nullptr; at = at->ai_next) {
        sock = ::socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (sock == INVALID_SOCK) continue;

#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeoutSeconds) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_sec = timeoutSeconds;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

        if (::connect(sock, at->ai_addr, static_cast<int>(at->ai_addrlen)) == 0) break;
        net::closeSocket(sock);
        sock = INVALID_SOCK;
    }
    freeaddrinfo(found);

    if (sock == INVALID_SOCK) *error = "cannot reach " + host;
    return sock;
}

/// A connection, plain or TLS, that closes itself.
class Connection {
public:
    ~Connection() {
        if (ssl_ != nullptr) SSL_free(ssl_);
        if (ctx_ != nullptr) SSL_CTX_free(ctx_);
        if (sock_ != INVALID_SOCK) net::closeSocket(sock_);
    }

    bool open(const HttpUrl& url, int timeoutSeconds, std::string* error) {
        sock_ = connectTcp(url.host, url.port, timeoutSeconds, error);
        if (sock_ == INVALID_SOCK) return false;
        if (url.scheme != "https") return true;

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) {
            *error = "no TLS context: " + sslError();
            return false;
        }
        // Verify the chain, and verify the name on it. Without the identity
        // check a valid certificate for any host would satisfy the first.
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            *error = "no trust store: " + sslError();
            return false;
        }
#ifdef _WIN32
        loadWindowsRoots(ctx_);
#endif
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) {
            *error = "no TLS session: " + sslError();
            return false;
        }
        SSL_set_tlsext_host_name(ssl_, url.host.c_str());   // SNI
        X509_VERIFY_PARAM* verify = SSL_get0_param(ssl_);
        std::array<unsigned char, 16> address{};
        int identitySet = 0;
        if (inet_pton(AF_INET, url.host.c_str(), address.data()) == 1) {
            identitySet = X509_VERIFY_PARAM_set1_ip(verify, address.data(), 4);
        } else if (inet_pton(AF_INET6, url.host.c_str(), address.data()) == 1) {
            identitySet = X509_VERIFY_PARAM_set1_ip(verify, address.data(), 16);
        } else {
            identitySet = X509_VERIFY_PARAM_set1_host(verify, url.host.c_str(), url.host.size());
        }
        if (identitySet != 1) {
            *error = "could not configure TLS peer identity: " + sslError();
            return false;
        }
        SSL_set_fd(ssl_, static_cast<int>(sock_));
        if (SSL_connect(ssl_) != 1) {
            *error = "TLS handshake failed: " + sslError();
            return false;
        }
        return true;
    }

    bool writeAll(const std::string& data) {
        std::size_t done = 0;
        while (done < data.size()) {
            int n = 0;
            if (ssl_ != nullptr) {
                n = SSL_write(ssl_, data.data() + done, static_cast<int>(data.size() - done));
            } else {
                n = static_cast<int>(::send(sock_, data.data() + done,
                                            static_cast<int>(data.size() - done), 0));
            }
            if (n <= 0) return false;
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Bytes read, 0 at end of stream, negative on error or timeout.
    int read(char* buf, int size) {
        if (ssl_ != nullptr) {
            const int n = SSL_read(ssl_, buf, size);
            if (n > 0) return n;
            const int why = SSL_get_error(ssl_, n);
            return (why == SSL_ERROR_ZERO_RETURN) ? 0 : -1;
        }
        return static_cast<int>(::recv(sock_, buf, size, 0));
    }

private:
    socket_t sock_ = INVALID_SOCK;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

std::string headerValue(const std::string& headers, const std::string& name) {
    const std::string lowerHeaders = lower(headers);
    const std::string key = "\r\n" + lower(name) + ":";
    std::size_t at = lowerHeaders.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    const std::size_t end = headers.find("\r\n", at);
    std::string value = headers.substr(at, end == std::string::npos ? end : end - at);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

/// Feeds body bytes through the hash, the cap, the sink and the progress.
class BodyWriter {
public:
    BodyWriter(const FetchOptions& opts, const FetchSink& sink, const FetchProgress& progress,
               std::uint64_t total, bool deliver)
        : opts_(opts), sink_(sink), progress_(progress), total_(total), deliver_(deliver) {
        md_ = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_, EVP_sha256(), nullptr);
    }
    ~BodyWriter() { EVP_MD_CTX_free(md_); }

    /// False when the caller cancelled or the cap was hit; `error` says which.
    bool write(const char* data, std::size_t size, std::string* error) {
        bytes_ += size;
        if (opts_.maxBytes != 0 && bytes_ > opts_.maxBytes) {
            *error = "reply is larger than " + std::to_string(opts_.maxBytes) + " bytes";
            return false;
        }
        if (!deliver_) return true;
        EVP_DigestUpdate(md_, data, size);
        if (sink_ && !sink_(data, size)) {
            *error = "cancelled";
            return false;
        }
        if (progress_ && !progress_(bytes_, total_)) {
            *error = "cancelled";
            return false;
        }
        return true;
    }

    std::uint64_t bytes() const { return bytes_; }

    std::string hexDigest() {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(md_, out, &len);
        std::string hex;
        char two[3];
        for (unsigned int i = 0; i < len; ++i) {
            std::snprintf(two, sizeof(two), "%02x", out[i]);
            hex += two;
        }
        return hex;
    }

private:
    const FetchOptions& opts_;
    const FetchSink& sink_;
    const FetchProgress& progress_;
    std::uint64_t total_;
    bool deliver_;
    std::uint64_t bytes_ = 0;
    EVP_MD_CTX* md_ = nullptr;
};

/// Transfer-Encoding: chunked, decoded as the bytes arrive.
class ChunkedDecoder {
public:
    /// Returns false on malformed input. Sets done when the last chunk ended.
    bool feed(const char* data, std::size_t size, BodyWriter& out, std::string* error, bool* done) {
        for (std::size_t i = 0; i < size;) {
            if (remaining_ == 0) {
                line_.push_back(data[i++]);
                if (line_.size() >= 2 && line_[line_.size() - 2] == '\r' && line_.back() == '\n') {
                    const std::string text = line_.substr(0, line_.size() - 2);
                    line_.clear();
                    if (text.empty()) {
                        // The CRLF that follows a chunk's data, or a trailer line.
                        if (afterData_) {
                            afterData_ = false;
                            continue;
                        }
                        if (finished_) {
                            *done = true;
                            return true;
                        }
                        continue;
                    }
                    if (finished_) continue;   // trailer, ignored
                    char* end = nullptr;
                    const unsigned long n = std::strtoul(text.c_str(), &end, 16);
                    if (end == text.c_str()) {
                        *error = "bad chunk size";
                        return false;
                    }
                    if (n == 0) {
                        finished_ = true;
                        continue;
                    }
                    remaining_ = n;
                }
                if (line_.size() > 1024) {
                    *error = "bad chunk header";
                    return false;
                }
                continue;
            }
            const std::size_t take = std::min<std::size_t>(remaining_, size - i);
            if (!out.write(data + i, take, error)) return false;
            remaining_ -= take;
            i += take;
            if (remaining_ == 0) afterData_ = true;
        }
        return true;
    }

    bool finished() const { return finished_; }

private:
    std::string line_;
    std::uint64_t remaining_ = 0;
    bool afterData_ = false;
    bool finished_ = false;
};

FetchResult fetchOnce(const std::string& url, const FetchOptions& opts, const FetchSink& sink,
                      const FetchProgress& progress, int redirectsLeft) {
    FetchResult result;
    HttpUrl parsed;
    if (!parseHttpUrl(url, &parsed, &result.error)) return result;
    if (parsed.scheme == "http" && !opts.allowPlainHttp) {
        result.error = "plain http is not allowed for " + parsed.host;
        return result;
    }

    Connection conn;
    if (!conn.open(parsed, opts.timeoutSeconds, &result.error)) return result;

    std::string hostHeader = parsed.host;
    if (hostHeader.find(':') != std::string::npos) hostHeader = "[" + hostHeader + "]";
    const bool defaultPort = (parsed.scheme == "https" && parsed.port == "443") ||
                             (parsed.scheme == "http" && parsed.port == "80");
    if (!defaultPort) hostHeader += ":" + parsed.port;

    const std::string request =
        "GET " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + hostHeader + "\r\n"
        "User-Agent: " + opts.userAgent + "\r\n"
        "Accept: " + opts.accept + "\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (!conn.writeAll(request)) {
        result.error = "could not send the request";
        return result;
    }

    // Headers first, byte by byte into a buffer until the blank line; what
    // follows it in the same read is the start of the body.
    std::string raw;
    char buffer[16384];
    std::size_t split = std::string::npos;
    while (split == std::string::npos) {
        const int got = conn.read(buffer, sizeof(buffer));
        if (got <= 0) {
            result.error = got == 0 ? "connection closed before the headers" : "read failed";
            return result;
        }
        raw.append(buffer, static_cast<std::size_t>(got));
        split = raw.find("\r\n\r\n");
        if (split == std::string::npos && raw.size() > 65536) {
            result.error = "headers too large";
            return result;
        }
    }
    const std::string headers = raw.substr(0, split);
    if (headers.rfind("HTTP/", 0) != 0) {
        result.error = "not an HTTP reply";
        return result;
    }
    const std::size_t firstSpace = headers.find(' ');
    if (firstSpace == std::string::npos) {
        result.error = "no status in the reply";
        return result;
    }
    result.status = std::atoi(headers.c_str() + firstSpace + 1);

    // One redirect, to the same scheme or from http to https. A pack index
    // that has moved is worth one hop; a chain is somebody else's problem.
    if (result.status == 301 || result.status == 302 || result.status == 303 ||
        result.status == 307 || result.status == 308) {
        std::string location = headerValue(headers, "Location");
        if (location.empty() || !opts.followRedirect || redirectsLeft <= 0) {
            result.error = "redirected (" + std::to_string(result.status) + ") and not followed";
            result.status = 0;
            return result;
        }
        if (location.front() == '/') {
            location = parsed.scheme + "://" + hostHeader + location;
        }
        HttpUrl target;
        std::string err;
        if (!parseHttpUrl(location, &target, &err)) {
            result.error = "redirect to an unusable location: " + err;
            result.status = 0;
            return result;
        }
        if (parsed.scheme == "https" && target.scheme == "http") {
            result.error = "redirect from https to http refused";
            result.status = 0;
            return result;
        }
        return fetchOnce(location, opts, sink, progress, redirectsLeft - 1);
    }

    const bool success = result.status >= 200 && result.status < 300;
    const bool deliver = success || opts.bodyOnError;
    const bool chunked = lower(headerValue(headers, "Transfer-Encoding")).find("chunked") != std::string::npos;
    std::uint64_t total = 0;
    const std::string lengthText = headerValue(headers, "Content-Length");
    if (!chunked && !lengthText.empty()) total = std::strtoull(lengthText.c_str(), nullptr, 10);

    BodyWriter body(opts, sink, progress, total, deliver);
    ChunkedDecoder chunks;
    bool done = false;

    auto consume = [&](const char* data, std::size_t size) -> bool {
        if (chunked) return chunks.feed(data, size, body, &result.error, &done);
        if (!body.write(data, size, &result.error)) return false;
        if (total != 0 && body.bytes() >= total) done = true;
        return true;
    };

    if (split + 4 < raw.size()) {
        if (!consume(raw.data() + split + 4, raw.size() - split - 4)) return result;
    }
    while (!done) {
        const int got = conn.read(buffer, sizeof(buffer));
        if (got < 0) {
            result.error = "read failed";
            return result;
        }
        if (got == 0) break;
        if (!consume(buffer, static_cast<std::size_t>(got))) return result;
    }
    if (chunked && !done) {
        result.error = "connection closed inside a chunked body";
        return result;
    }
    if (!chunked && total != 0 && body.bytes() < total) {
        result.error = "connection closed after " + std::to_string(body.bytes()) + " of " +
                       std::to_string(total) + " bytes";
        return result;
    }

    result.ok = true;
    result.bytes = body.bytes();
    result.sha256 = body.hexDigest();
    return result;
}

}  // namespace

bool parseHttpUrl(const std::string& url, HttpUrl* out, std::string* error) {
    HttpUrl u;
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos) {
        *error = "no scheme in " + url;
        return false;
    }
    u.scheme = lower(url.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https") {
        *error = "unsupported scheme " + u.scheme;
        return false;
    }
    std::size_t at = sep + 3;
    std::size_t pathStart = url.find_first_of("/?#", at);
    std::string authority = url.substr(at, pathStart == std::string::npos ? std::string::npos : pathStart - at);
    if (authority.find('@') != std::string::npos) {
        *error = "user information in a URL is not supported";
        return false;
    }
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) {
            *error = "unterminated IPv6 host";
            return false;
        }
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') u.port = authority.substr(close + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            u.host = authority;
        } else {
            u.host = authority.substr(0, colon);
            u.port = authority.substr(colon + 1);
        }
    }
    if (u.host.empty()) {
        *error = "no host in " + url;
        return false;
    }
    if (u.port.empty()) u.port = u.scheme == "https" ? "443" : "80";
    for (char c : u.port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            *error = "bad port in " + url;
            return false;
        }
    }
    if (pathStart == std::string::npos) {
        u.path = "/";
    } else {
        u.path = url.substr(pathStart);
        if (u.path.front() != '/') u.path = "/" + u.path;
        const std::size_t hash = u.path.find('#');
        if (hash != std::string::npos) u.path.erase(hash);
    }
    *out = std::move(u);
    return true;
}

std::string httpUrlDirectory(const std::string& url) {
    const std::size_t sep = url.find("://");
    const std::size_t firstSlash = url.find('/', sep == std::string::npos ? 0 : sep + 3);
    if (firstSlash == std::string::npos) return url + "/";
    const std::size_t query = url.find_first_of("?#", firstSlash);
    const std::string noQuery = url.substr(0, query);
    const std::size_t last = noQuery.rfind('/');
    return noQuery.substr(0, last + 1);
}

FetchResult httpFetch(const std::string& url, const FetchOptions& opts, const FetchSink& sink,
                      const FetchProgress& progress) {
    return fetchOnce(url, opts, sink, progress, opts.followRedirect ? 1 : 0);
}

FetchResult httpFetchToString(const std::string& url, const FetchOptions& opts, std::string* body) {
    body->clear();
    return httpFetch(url, opts, [body](const char* data, std::size_t size) {
        body->append(data, size);
        return true;
    });
}

FetchResult httpFetchToFile(const std::string& url, const FetchOptions& opts,
                            const std::string& path, const FetchProgress& progress) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        FetchResult r;
        r.error = "cannot write " + path;
        return r;
    }
    FetchResult r = httpFetch(url, opts, [&out](const char* data, std::size_t size) {
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    }, progress);
    out.close();
    if (!r.ok || r.status < 200 || r.status >= 300) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    }
    return r;
}

}  // namespace wowee::core
