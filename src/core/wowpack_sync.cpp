#include "core/wowpack_sync.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/http_fetch.hpp"
#include "core/logger.hpp"
#include "core/update_check.hpp"
#include "core/version.hpp"
#include "core/wowpack_index.hpp"

namespace wowee::core {
namespace {

namespace fs = std::filesystem;

constexpr const char* kStateDir = ".wowpack";
constexpr const char* kInstalledFile = "installed.json";
constexpr const char* kTmpDir = "tmp";

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAtomically(const fs::path& target, const std::string& content) {
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << content;
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    return !ec;
}

/// Remove a file and then every directory above it that is now empty, up to
/// but not including the override root.
void removeAndPrune(const fs::path& root, const fs::path& target) {
    std::error_code ec;
    fs::remove(target, ec);
    fs::path dir = target.parent_path();
    while (dir != root && dir.string().size() > root.string().size()) {
        if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) break;
        fs::remove(dir, ec);
        dir = dir.parent_path();
    }
}

}  // namespace

WowpackSync::~WowpackSync() {
    cancel();
}

void WowpackSync::cancel() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
    cancel_.store(false);
}

std::string WowpackSync::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string WowpackSync::packName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packName_;
}

std::string WowpackSync::packVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packVersion_;
}

std::string WowpackSync::url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

void WowpackSync::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = why;
    }
    LOG_WARNING("Content pack: ", why);
    state_.store(State::Failed);
}

void WowpackSync::start(const std::string& url, const std::string& overrideDir,
                        const std::string& expansionId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The same pack again, in progress or already synced this session,
        // is nothing to do. An empty url is never "done": it means remove.
        if (!url.empty() && url == url_ &&
            (state_.load() == State::Running || url == lastGoodUrl_)) {
            return;
        }
    }
    cancel();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        error_.clear();
        packName_.clear();
        packVersion_.clear();
    }
    filesDone_.store(0);
    filesTotal_.store(0);
    bytesDone_.store(0);
    bytesTotal_.store(0);

    if (url.empty()) {
        std::error_code ec;
        if (!fs::exists(fs::path(overrideDir) / kStateDir / kInstalledFile, ec)) {
            state_.store(State::Idle);
            return;
        }
    }
    state_.store(State::Running);
    thread_ = std::thread(&WowpackSync::run, this, url, overrideDir, expansionId);
}

void WowpackSync::uninstall(const std::string& overrideDir) {
    const fs::path root(overrideDir);
    wowpack::Installed installed;
    if (!wowpack::parseInstalled(readAll(root / kStateDir / kInstalledFile), &installed)) {
        state_.store(State::Done);
        return;
    }
    filesTotal_.store(installed.files.size());
    for (const wowpack::PackFile& f : installed.files) {
        removeAndPrune(root, root / f.path);
        filesDone_.fetch_add(1);
    }
    std::error_code ec;
    fs::remove_all(root / kStateDir, ec);
    LOG_INFO("Content pack: removed ", installed.files.size(), " file(s) of ", installed.name);
    changed_.store(!installed.files.empty());
    state_.store(State::Done);
}

void WowpackSync::run(std::string url, std::string overrideDir, std::string expansionId) {
    if (url.empty()) {
        uninstall(overrideDir);
        return;
    }

    HttpUrl parsed;
    std::string error;
    if (!parseHttpUrl(url, &parsed, &error)) {
        fail(error);
        return;
    }
    // The pack carries Lua the client executes and DBCs that steer loading.
    // A hash in an index fetched over plain http guarantees nothing, so http
    // is only accepted from an address that cannot be anything but local.
    const bool allowPlain = wowpack::isPrivateHost(parsed.host);
    if (parsed.scheme == "http" && !allowPlain) {
        fail("content pack URL must use https (plain http is only allowed for LAN addresses)");
        return;
    }

    FetchOptions opts;
    opts.userAgent = std::string("WoWee/") + kVersion;
    opts.allowPlainHttp = allowPlain;
    opts.maxBytes = wowpack::kMaxIndexBytes;
    std::string indexText;
    FetchResult r = httpFetchToString(url, opts, &indexText);
    if (!r.ok) {
        fail("could not fetch the index: " + r.error);
        return;
    }
    if (r.status != 200) {
        fail("index request answered HTTP " + std::to_string(r.status));
        return;
    }
    const wowpack::Index index = wowpack::parseIndex(indexText);
    if (!index.ok) {
        fail(index.error);
        return;
    }
    if (!expansionId.empty() && index.expansion != expansionId) {
        fail("pack is for " + index.expansion + ", this server uses " + expansionId);
        return;
    }
    if (!index.minClient.empty() && isNewerVersion(index.minClient, kVersion)) {
        fail("pack needs WoWee " + index.minClient + " or newer");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packName_ = index.name;
        packVersion_ = index.version;
    }

    const fs::path root(overrideDir);
    const fs::path stateDir = root / kStateDir;
    const fs::path tmpDir = stateDir / kTmpDir;
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    if (ec) {
        fail("cannot create " + tmpDir.string());
        return;
    }
    // Whatever a crashed run left behind.
    for (const auto& entry : fs::directory_iterator(tmpDir, ec)) fs::remove(entry.path(), ec);

    wowpack::Installed installed;
    const bool haveInstalled = wowpack::parseInstalled(readAll(stateDir / kInstalledFile), &installed);
    // Switching to another pack: what the old one put there goes first, so a
    // file the new pack does not list cannot linger as the old realm's.
    if (haveInstalled && installed.url != url) {
        for (const wowpack::PackFile& f : installed.files) removeAndPrune(root, root / f.path);
        installed = wowpack::Installed{};
    }

    const wowpack::Plan plan = wowpack::planSync(index, haveInstalled ? &installed : nullptr,
        [&](const std::string& path) -> std::int64_t {
            std::error_code e;
            const auto size = fs::file_size(root / path, e);
            return e ? -1 : static_cast<std::int64_t>(size);
        });
    filesTotal_.store(plan.download.size());
    bytesTotal_.store(plan.downloadBytes);
    LOG_INFO("Content pack ", index.name, " ", index.version, ": ", plan.download.size(),
             " to download, ", plan.keep.size(), " kept, ", plan.remove.size(), " to remove");

    const std::string base = httpUrlDirectory(url);
    wowpack::Installed result;
    result.url = url;
    result.name = index.name;
    result.version = index.version;
    result.files = plan.keep;

    std::uint64_t doneBefore = 0;
    // What crossed the network for the files, compressed where the server
    // gzipped them; doneBefore counts the same files as they land.
    std::uint64_t wireBytes = 0;
    std::size_t gzipped = 0;
    for (const wowpack::PackFile& f : plan.download) {
        if (cancel_.load()) {
            fail("cancelled");
            return;
        }
        FetchOptions fileOpts = opts;
        fileOpts.maxBytes = f.size;
        const fs::path part = tmpDir / (f.sha256 + ".part");
        FetchResult fr;
        for (int attempt = 0; attempt < 2; ++attempt) {
            fr = httpFetchToFile(base + f.path, fileOpts, part.string(),
                [&](std::uint64_t got, std::uint64_t) {
                    bytesDone_.store(doneBefore + got);
                    return !cancel_.load();
                });
            if (fr.ok && fr.status == 200 && fr.bytes == f.size && fr.sha256 == f.sha256) break;
            fs::remove(part, ec);
            if (cancel_.load()) {
                fail("cancelled");
                return;
            }
        }
        if (!fr.ok) {
            fail(f.path + ": " + fr.error);
            return;
        }
        if (fr.status != 200) {
            fail(f.path + ": HTTP " + std::to_string(fr.status));
            return;
        }
        if (fr.bytes != f.size || fr.sha256 != f.sha256) {
            fail(f.path + " did not match its size or hash");
            return;
        }
        const fs::path target = root / f.path;
        fs::create_directories(target.parent_path(), ec);
        fs::rename(part, target, ec);
        if (ec) {
            fs::remove(part, ec);
            fail("cannot place " + f.path + ": " + ec.message());
            return;
        }
        result.files.push_back(f);
        wireBytes += fr.wireBytes;
        if (fr.contentEncoding == "gzip") ++gzipped;
        doneBefore += f.size;
        bytesDone_.store(doneBefore);
        filesDone_.fetch_add(1);
    }
    for (const std::string& path : plan.remove) removeAndPrune(root, root / path);

    if (!writeAtomically(stateDir / kInstalledFile, wowpack::serializeInstalled(result))) {
        fail("cannot write " + (stateDir / kInstalledFile).string());
        return;
    }
    if (!plan.download.empty()) {
        LOG_INFO("Content pack ", index.name, " ", index.version, ": fetched ", plan.download.size(),
                 " file(s), ", doneBefore, " bytes as ", wireBytes, " bytes transferred (",
                 gzipped, " gzip)");
    }
    LOG_INFO("Content pack ", index.name, " ", index.version, " is up to date");
    changed_.store(!plan.download.empty() || !plan.remove.empty());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastGoodUrl_ = url;
    }
    state_.store(State::Done);
}

}  // namespace wowee::core
