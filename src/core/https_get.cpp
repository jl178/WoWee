#include "core/https_get.hpp"

#include "core/http_fetch.hpp"

namespace wowee::core {

// The small-document face of http_fetch, kept so the update check and its
// test need not change. Same behaviour as before: port 443, GitHub's media
// type, no redirects, a 4 MB cap, and the body of an error reply returned
// so the caller can read what the API said.
HttpsResponse httpsGet(const std::string& host, const std::string& path,
                       const std::string& userAgent, int timeoutSeconds) {
    FetchOptions opts;
    opts.userAgent = userAgent;
    opts.accept = "application/vnd.github+json";
    opts.timeoutSeconds = timeoutSeconds;
    opts.maxBytes = 4u * 1024u * 1024u;
    opts.followRedirect = false;
    opts.bodyOnError = true;

    HttpsResponse response;
    const FetchResult r = httpFetchToString("https://" + host + path, opts, &response.body);
    response.ok = r.ok;
    response.status = r.status;
    response.error = r.error;
    if (!r.ok) response.body.clear();
    return response;
}

}  // namespace wowee::core
