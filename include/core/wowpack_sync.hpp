#pragma once

/// Download a realm's content pack into the override directory before login.
///
/// One pack is active at a time, keyed by its URL. Selecting a server with
/// a different pack re-syncs; selecting one with no pack removes what the
/// last one installed. Per-realm directories would have cost a probe on
/// every asset lookup and still not let two realms with different DBCs
/// coexist, since the asset manager holds one table per name.
///
/// Runs on a thread of its own, like the update check. The login screen
/// polls the progress every frame and calls Application::reloadExpansionData
/// once a run that changed files has finished.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace wowee::core {

class WowpackSync {
public:
    enum class State { Idle, Running, Done, Failed };

    WowpackSync() = default;
    ~WowpackSync();
    WowpackSync(const WowpackSync&) = delete;
    WowpackSync& operator=(const WowpackSync&) = delete;

    /// Sync `url` into `overrideDir`. An empty url removes the installed pack
    /// if there is one. A url already synced successfully this session is
    /// not fetched again; a different url cancels a run in progress.
    void start(const std::string& url, const std::string& overrideDir, const std::string& expansionId);

    /// Stop a run as soon as the current read returns. Blocks until the
    /// thread has left; nothing half-written remains outside .wowpack/tmp.
    void cancel();

    [[nodiscard]] State state() const { return state_.load(); }
    [[nodiscard]] bool running() const { return state_.load() == State::Running; }
    [[nodiscard]] bool finished() const {
        const State s = state_.load();
        return s == State::Done || s == State::Failed;
    }

    [[nodiscard]] std::uint64_t filesDone() const { return filesDone_.load(); }
    [[nodiscard]] std::uint64_t filesTotal() const { return filesTotal_.load(); }
    [[nodiscard]] std::uint64_t bytesDone() const { return bytesDone_.load(); }
    [[nodiscard]] std::uint64_t bytesTotal() const { return bytesTotal_.load(); }

    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::string packName() const;
    [[nodiscard]] std::string packVersion() const;
    [[nodiscard]] std::string url() const;

    /// True once, after a run that added, replaced or removed a file. The
    /// caller then reloads what the asset manager has cached.
    bool takeChanged() { return changed_.exchange(false); }

private:
    void run(std::string url, std::string overrideDir, std::string expansionId);
    void uninstall(const std::string& overrideDir);
    void fail(const std::string& why);

    mutable std::mutex mutex_;
    std::string url_;
    std::string error_;
    std::string packName_;
    std::string packVersion_;
    std::string lastGoodUrl_;

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> changed_{false};
    std::atomic<std::uint64_t> filesDone_{0};
    std::atomic<std::uint64_t> filesTotal_{0};
    std::atomic<std::uint64_t> bytesDone_{0};
    std::atomic<std::uint64_t> bytesTotal_{0};
};

}  // namespace wowee::core
