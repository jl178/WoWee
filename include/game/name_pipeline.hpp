#pragma once

// How a unit's name is found and kept, after the stock 3.3.5a client (build
// 12340). docs/re/names-and-caches.md has the addresses.
//
// - A creature's name is its creature cache record (DBCache 0xC5D690,
//   creaturecache.wdb), asked for when the unit is created or its
//   OBJECT_FIELD_ENTRY changes (0x72D940, 0x72CEB0).
// - A pet's (UNIT_FIELD_PETNUMBER set) is the pet name cache, keyed by the pet
//   number and good while UNIT_FIELD_PET_NAME_TIMESTAMP matches (0x72A000).
// - A player's is the name cache (DBCache 0xC5D938), asked for on first use.
// - Until the answer is in, GetUnitName gives UNKNOWNOBJECT, "Unknown"; the
//   answer fires UNIT_NAME_UPDATE for every token naming the unit (0x512B00).
//
// The creature and game object caches are kept on disk between sessions as
// stock keeps its WDB files; the name cache is not (stock creates it with its
// persist flags clear and there is no namecache.wdb).
//
// Kept apart from the handlers so the rules and the file format can be tested.

#include "game/world_packets.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::game::name_pipeline {

/// Where a unit's name comes from.
enum class NameSource { Player, Pet, Creature };

/// 0x72A000's order: a player by guid; a unit carrying a pet number from the
/// pet name cache; anything else from its creature record.
[[nodiscard]] constexpr NameSource nameSourceFor(bool isPlayer, uint32_t petNumber) {
    if (isPlayer) return NameSource::Player;
    if (petNumber != 0) return NameSource::Pet;
    return NameSource::Creature;
}

/// A query still unanswered after this long is asked again. Stock never asks
/// twice (a record stays pending until its answer); a reply lost on the way
/// would leave a unit "Unknown" for the session, so Wowee re-asks.
inline constexpr int64_t kQueryResendMs = 10000;

[[nodiscard]] constexpr bool queryDueForResend(int64_t sentMs, int64_t nowMs) {
    return nowMs - sentMs >= kQueryResendMs;
}

/// A pet name cache record answers for a pet only while the pet's name stamp
/// matches the one the answer carried (0x72A000 compares record +0x58 with
/// UNIT_FIELD_PET_NAME_TIMESTAMP; a rename moves the stamp).
struct PetName {
    std::string name;
    uint32_t timestamp = 0;
};
[[nodiscard]] inline bool petNameCurrent(const PetName& cached, uint32_t unitTimestamp) {
    return !cached.name.empty() && cached.timestamp == unitTimestamp;
}

// ---------------------------------------------------------------------------
// The caches on disk
// ---------------------------------------------------------------------------

/// The per-user data directory every other per-user file lives in.
inline std::filesystem::path userDataDir() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) return std::filesystem::path(appData) / "wowee";
#else
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".wowee";
#endif
    return std::filesystem::path(".wowee");
}

/// A directory name for one realm. Stock keeps one WDB set per locale
/// (Cache/WDB/enUS); Wowee keeps one per realm, because the servers it talks
/// to are private ones whose templates differ from each other, and a record
/// from one would name another's creatures wrongly.
inline std::string realmCacheKey(const std::string& host, uint16_t port, uint32_t realmId) {
    std::string key;
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-';
        key += ok ? c : '_';
    }
    if (key.empty()) key = "realm";
    key += "_" + std::to_string(port) + "_" + std::to_string(realmId);
    return key;
}

/// The file's first line: format, build and kind. A file of another build or
/// another format is not read (stock rejects a WDB whose header build differs).
inline std::string cacheHeader(const char* kind, uint32_t build) {
    return std::string("wowee-cache 1 ") + kind + " " + std::to_string(build);
}

inline std::string escapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::string unescapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char n = s[++i];
        out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    }
    return out;
}

inline std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line) {
        if (c == '\t') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

inline bool parseU32(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

inline void writeCreatureCache(std::ostream& os, uint32_t build,
                               const std::unordered_map<uint32_t, CreatureQueryResponseData>& cache) {
    os << cacheHeader("creature", build) << '\n';
    for (const auto& [entry, d] : cache) {
        if (!d.isValid()) continue;
        os << entry << '\t' << escapeField(d.name) << '\t' << escapeField(d.subName) << '\t'
           << escapeField(d.iconName) << '\t' << d.typeFlags << '\t' << d.creatureType << '\t'
           << d.family << '\t' << d.rank;
        for (uint32_t id : d.displayId) os << '\t' << id;
        os << '\n';
    }
}

/// Reads what writeCreatureCache wrote. False, and nothing added, for a file
/// of another build or format; a damaged line is skipped.
inline bool readCreatureCache(std::istream& is, uint32_t build,
                              std::unordered_map<uint32_t, CreatureQueryResponseData>& out) {
    std::string line;
    if (!std::getline(is, line) || line != cacheHeader("creature", build)) return false;
    while (std::getline(is, line)) {
        const auto f = splitTabs(line);
        if (f.size() != 12) continue;
        CreatureQueryResponseData d;
        uint32_t num[9] = {};
        if (!parseU32(f[0], d.entry)) continue;
        bool ok = true;
        for (int i = 0; i < 8 && ok; ++i) ok = parseU32(f[4 + i], num[i]);
        if (!ok) continue;
        d.name = unescapeField(f[1]);
        d.subName = unescapeField(f[2]);
        d.iconName = unescapeField(f[3]);
        d.typeFlags = num[0];
        d.creatureType = num[1];
        d.family = num[2];
        d.rank = num[3];
        for (int i = 0; i < 4; ++i) d.displayId[i] = num[4 + i];
        if (d.isValid()) out[d.entry] = d;
    }
    return true;
}

inline void writeGameObjectCache(std::ostream& os, uint32_t build,
                                 const std::unordered_map<uint32_t, GameObjectQueryResponseData>& cache) {
    os << cacheHeader("gameobject", build) << '\n';
    for (const auto& [entry, d] : cache) {
        if (!d.isValid()) continue;
        os << entry << '\t' << escapeField(d.name) << '\t' << d.type << '\t' << d.displayId << '\t'
           << escapeField(d.iconName) << '\t' << (d.hasData ? 1 : 0);
        for (uint32_t v : d.data) os << '\t' << v;
        os << '\n';
    }
}

inline bool readGameObjectCache(std::istream& is, uint32_t build,
                                std::unordered_map<uint32_t, GameObjectQueryResponseData>& out) {
    std::string line;
    if (!std::getline(is, line) || line != cacheHeader("gameobject", build)) return false;
    while (std::getline(is, line)) {
        const auto f = splitTabs(line);
        if (f.size() != 6 + 24) continue;
        GameObjectQueryResponseData d;
        uint32_t hasData = 0;
        if (!parseU32(f[0], d.entry) || !parseU32(f[2], d.type) || !parseU32(f[3], d.displayId) ||
            !parseU32(f[5], hasData)) {
            continue;
        }
        bool ok = true;
        for (int i = 0; i < 24 && ok; ++i) ok = parseU32(f[6 + i], d.data[i]);
        if (!ok) continue;
        d.name = unescapeField(f[1]);
        d.iconName = unescapeField(f[4]);
        d.hasData = hasData != 0;
        if (d.isValid()) out[d.entry] = d;
    }
    return true;
}

}  // namespace wowee::game::name_pipeline
