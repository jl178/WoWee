#pragma once

/// Content packs: what a realm may ship, how its index is read, and what a
/// sync has to do about it. Pure functions, so the part that decides what
/// lands under Data/ is tested without a network.
///
/// A pack is loose files at client paths plus `wowpack.json`, an index with a
/// size and SHA-256 per file. The client downloads what the index lists into
/// its override directory, which the asset manager reads before the
/// extraction. A pack carries Lua the client runs and DBCs that steer what it
/// loads, so every path is checked before it is joined to anything, and a
/// name that fails is refused rather than repaired.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::core::wowpack {

/// The index format this client reads.
inline constexpr int kIndexFormat = 1;

/// Caps on what one pack may declare. The size decides how much is written
/// to the player's disk, and it arrives from the same place as everything else.
inline constexpr std::size_t kMaxFiles = 50000;
inline constexpr std::uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxPackBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxIndexBytes = 16ull * 1024 * 1024;

struct PackFile {
    /// Lower case, forward slashes, relative to the override directory.
    std::string path;
    std::uint64_t size = 0;
    /// Lower-case hex, 64 characters.
    std::string sha256;
};

struct Index {
    bool ok = false;
    std::string error;
    std::string name;
    std::string version;
    std::string expansion;   ///< "wotlk"
    int build = 0;
    std::string minClient;   ///< optional "vX.Y.Z"
    std::vector<PackFile> files;
};

/// Whether `path` may be joined to the override directory.
///
/// Letters, digits, dot, dash, underscore and slash only; two to eight
/// segments; no empty, "." or ".." segment; no segment that starts with a
/// dot or a dash or ends with a dot; no Windows device name; and a first
/// segment from the list of directories the client reads assets from.
[[nodiscard]] bool isSafePackPath(const std::string& path);

/// The directories a pack may write under.
[[nodiscard]] const std::vector<std::string>& allowedTopLevelDirs();

/// Parse wowpack.json. Refuses the whole index on any bad entry.
[[nodiscard]] Index parseIndex(const std::string& json);

/// What is installed from a pack, kept at <override>/.wowpack/installed.json.
struct Installed {
    std::string url;
    std::string name;
    std::string version;
    std::vector<PackFile> files;
};

/// Parse installed.json; ok false means there is nothing installed.
[[nodiscard]] bool parseInstalled(const std::string& json, Installed* out);
[[nodiscard]] std::string serializeInstalled(const Installed& installed);

/// The work a sync has to do.
struct Plan {
    std::vector<PackFile> download;
    std::vector<PackFile> keep;
    std::vector<std::string> remove;
    std::uint64_t downloadBytes = 0;
};

/// Decide per file. `sizeOnDisk` returns the size of a path under the
/// override directory, or -1 when it is absent. A file is kept when the
/// installed record has the same hash and the file is on disk at the declared
/// size; everything else in the index is downloaded, and every recorded path
/// the index no longer lists is removed.
[[nodiscard]] Plan planSync(const Index& index, const Installed* installed,
                            const std::function<std::int64_t(const std::string&)>& sizeOnDisk);

/// Whether a host may be fetched over plain http: loopback and RFC 1918
/// literals only. Names are not resolved; a name is not a decision.
[[nodiscard]] bool isPrivateHost(const std::string& host);

}  // namespace wowee::core::wowpack
