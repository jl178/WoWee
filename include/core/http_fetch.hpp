#pragma once

/// One HTTP or HTTPS GET, streamed to a sink as it arrives.
///
/// The update check needed a few kilobytes of JSON and got https_get for it.
/// A realm's content pack is hundreds of files, some of them tens of
/// megabytes, so this is the same transport with the body handed over as it
/// is read rather than held in a string: a size cap, a progress callback that
/// can cancel, one redirect, and a running SHA-256 so the caller can verify
/// what landed without reading it back.
///
/// It asks for gzip (`Accept-Encoding: gzip`) and inflates a gzip reply as it
/// streams. Everything a caller sees, the sink, the cap, the hash, `bytes`
/// and the progress count, is about the inflated body, so a pack index that
/// lists sizes and hashes of the files themselves checks the same way
/// whether or not the server compressed them.
///
/// Still not a general HTTP client: no keep-alive, no POST, no cookies, no
/// proxy. Call it off the render thread; it blocks on every socket operation.

#include <cstdint>
#include <functional>
#include <string>

namespace wowee::core {

/// The parts of a URL this client acts on.
struct HttpUrl {
    std::string scheme;   ///< "http" or "https"
    std::string host;     ///< without brackets for IPv6
    std::string port;     ///< as text; defaulted from the scheme
    std::string path;     ///< begins with '/', query included
};

/// Split a URL. Refuses anything that is not http or https, has no host, or
/// carries user information. `error` says which.
bool parseHttpUrl(const std::string& url, HttpUrl* out, std::string* error);

/// The directory part of a URL: everything through the last '/'. The index
/// of a pack names its files relative to where the index itself was fetched.
std::string httpUrlDirectory(const std::string& url);

/// Ceiling on an inflated gzip body when the caller set no cap of its own,
/// so a small compressed reply cannot expand into the disk or memory.
constexpr std::uint64_t kMaxInflatedBytes = 2ull * 1024 * 1024 * 1024;

struct FetchOptions {
    std::string userAgent = "WoWee";
    std::string accept = "*/*";
    /// Per socket operation, not for the whole exchange.
    int timeoutSeconds = 15;
    /// The most body bytes accepted, counted after inflating. Zero means no
    /// cap, except that an inflated gzip body is still held to
    /// kMaxInflatedBytes. Exceeding it is an error, not a truncated success.
    std::uint64_t maxBytes = 0;
    /// Send `Accept-Encoding: gzip`. A server that ignores it still works.
    bool acceptGzip = true;
    /// Plain http:// is refused unless the caller says it has decided the
    /// host can be trusted without TLS (a LAN address, a loopback).
    bool allowPlainHttp = false;
    /// Follow one 301/302/303/307/308 to the same or a stricter scheme.
    bool followRedirect = true;
    /// Hand a non-2xx body to the sink too. Off, the body of an error reply
    /// is read and dropped so a 404 page never lands in a file.
    bool bodyOnError = false;
};

struct FetchResult {
    /// False when the exchange never completed: resolve, connect, TLS, a
    /// read, the cap, or a cancel. `status` is then 0.
    bool ok = false;
    /// HTTP status of the final reply. 200 is the only success for a pack.
    int status = 0;
    std::string error;
    /// Body bytes delivered, after inflating.
    std::uint64_t bytes = 0;
    /// Body bytes as the server encoded them: the compressed size for gzip,
    /// the same as `bytes` otherwise. Transfer framing is not counted.
    std::uint64_t wireBytes = 0;
    /// "gzip" when the reply was inflated, empty when it came as is.
    std::string contentEncoding;
    /// Lower-case hex SHA-256 of the delivered body.
    std::string sha256;
};

/// Receives body bytes. Return false to cancel.
using FetchSink = std::function<bool(const char* data, std::size_t size)>;
/// Progress after each read: inflated bytes so far and the Content-Length,
/// or 0 when the server did not say or the body is compressed (the
/// Content-Length of a gzip reply is not the size the count runs to).
/// Return false to cancel.
using FetchProgress = std::function<bool(std::uint64_t got, std::uint64_t total)>;

FetchResult httpFetch(const std::string& url, const FetchOptions& opts,
                      const FetchSink& sink, const FetchProgress& progress = {});

/// Fetch into a string. `maxBytes` should be set; this holds the body in memory.
FetchResult httpFetchToString(const std::string& url, const FetchOptions& opts, std::string* body);

/// Fetch into a file, created or truncated. On any failure the file is
/// removed, so a partial download never looks like a finished one.
FetchResult httpFetchToFile(const std::string& url, const FetchOptions& opts,
                            const std::string& path, const FetchProgress& progress = {});

}  // namespace wowee::core
