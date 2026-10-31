#include "core/wowpack_index.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string_view>
#include <unordered_set>

namespace wowee::core::wowpack {
namespace {

bool isHexLower(const std::string& s, std::size_t expected) {
    if (s.size() != expected) return false;
    for (char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) return false;
    }
    return true;
}

/// The names Windows resolves as devices wherever they appear, with or
/// without an extension: con.lua opens the console, not a file.
bool isWindowsDeviceName(std::string_view stem) {
    static constexpr std::string_view kDevices[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    for (std::string_view d : kDevices) {
        if (stem == d) return true;
    }
    return false;
}

bool readFile(const nlohmann::json& item, PackFile* out, std::string* error) {
    if (!item.is_object()) {
        *error = "a file entry is not an object";
        return false;
    }
    const auto path = item.find("path");
    const auto size = item.find("size");
    const auto sha = item.find("sha256");
    if (path == item.end() || !path->is_string() || size == item.end() ||
        !size->is_number_integer() || sha == item.end() || !sha->is_string()) {
        *error = "a file entry lacks path, size or sha256";
        return false;
    }
    out->path = path->get<std::string>();
    if (!isSafePackPath(out->path)) {
        *error = "unsafe path " + out->path;
        return false;
    }
    const auto n = size->get<std::int64_t>();
    if (n < 0 || static_cast<std::uint64_t>(n) > kMaxFileBytes) {
        *error = out->path + " declares an unacceptable size";
        return false;
    }
    out->size = static_cast<std::uint64_t>(n);
    out->sha256 = sha->get<std::string>();
    if (!isHexLower(out->sha256, 64)) {
        *error = out->path + " has a malformed sha256";
        return false;
    }
    return true;
}

}  // namespace

const std::vector<std::string>& allowedTopLevelDirs() {
    static const std::vector<std::string> dirs = {
        "cameras", "character", "creature", "dbfilesclient", "dungeons", "environment",
        "fonts", "interface", "item", "particles", "shaders", "sound", "spells",
        "textures", "tileset", "world",
    };
    return dirs;
}

bool isSafePackPath(const std::string& path) {
    if (path.empty() || path.size() > 200) return false;
    for (char c : path) {
        const bool lowerAlpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lowerAlpha && !digit && c != '.' && c != '-' && c != '_' && c != '/') return false;
    }
    int segments = 0;
    std::size_t start = 0;
    bool first = true;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string_view seg(path.data() + start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.front() == '.' || seg.front() == '-' || seg.back() == '.') return false;
        if (seg.find("..") != std::string_view::npos) return false;
        const std::size_t dot = seg.find('.');
        if (isWindowsDeviceName(dot == std::string_view::npos ? seg : seg.substr(0, dot))) return false;
        if (first) {
            bool allowed = false;
            for (const std::string& d : allowedTopLevelDirs()) {
                if (seg == d) allowed = true;
            }
            if (!allowed) return false;
            first = false;
        }
        ++segments;
        if (end == path.size()) break;
        start = end + 1;
    }
    return segments >= 2 && segments <= 8;
}

Index parseIndex(const std::string& json) {
    Index index;
    if (json.size() > kMaxIndexBytes) {
        index.error = "index is larger than the cap";
        return index;
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        index.error = std::string("index is not JSON: ") + e.what();
        return index;
    }
    if (!doc.is_object()) {
        index.error = "index is not an object";
        return index;
    }
    const auto format = doc.find("format");
    if (format == doc.end() || !format->is_number_integer() || format->get<int>() != kIndexFormat) {
        index.error = "index format is not " + std::to_string(kIndexFormat);
        return index;
    }
    auto str = [&](const char* key, std::string* out, bool required) -> bool {
        const auto it = doc.find(key);
        if (it == doc.end()) return !required;
        if (!it->is_string()) return false;
        *out = it->get<std::string>();
        return true;
    };
    if (!str("name", &index.name, true) || index.name.empty() || index.name.size() > 64) {
        index.error = "index has no usable name";
        return index;
    }
    if (!str("version", &index.version, true) || index.version.empty() || index.version.size() > 64) {
        index.error = "index has no usable version";
        return index;
    }
    if (!str("expansion", &index.expansion, true) || index.expansion.empty()) {
        index.error = "index names no expansion";
        return index;
    }
    if (!str("min_client", &index.minClient, false)) {
        index.error = "min_client is not a string";
        return index;
    }
    if (const auto build = doc.find("build"); build != doc.end() && build->is_number_integer()) {
        index.build = build->get<int>();
    }
    const auto files = doc.find("files");
    if (files == doc.end() || !files->is_array()) {
        index.error = "index has no files list";
        return index;
    }
    if (files->size() > kMaxFiles) {
        index.error = "index lists more than " + std::to_string(kMaxFiles) + " files";
        return index;
    }
    std::unordered_set<std::string> seen;
    std::uint64_t total = 0;
    for (const auto& item : *files) {
        PackFile f;
        if (!readFile(item, &f, &index.error)) return index;
        if (!seen.insert(f.path).second) {
            index.error = "path listed twice: " + f.path;
            return index;
        }
        total += f.size;
        if (total > kMaxPackBytes) {
            index.error = "pack is larger than the cap";
            return index;
        }
        index.files.push_back(std::move(f));
    }
    index.ok = true;
    return index;
}

bool parseInstalled(const std::string& json, Installed* out) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (...) {
        return false;
    }
    if (!doc.is_object()) return false;
    Installed inst;
    if (const auto it = doc.find("url"); it != doc.end() && it->is_string()) inst.url = it->get<std::string>();
    if (const auto it = doc.find("name"); it != doc.end() && it->is_string()) inst.name = it->get<std::string>();
    if (const auto it = doc.find("version"); it != doc.end() && it->is_string()) inst.version = it->get<std::string>();
    const auto files = doc.find("files");
    if (files == doc.end() || !files->is_array()) return false;
    for (const auto& item : *files) {
        PackFile f;
        std::string error;
        if (!readFile(item, &f, &error)) return false;
        inst.files.push_back(std::move(f));
    }
    *out = std::move(inst);
    return true;
}

std::string serializeInstalled(const Installed& installed) {
    nlohmann::json doc;
    doc["format"] = kIndexFormat;
    doc["url"] = installed.url;
    doc["name"] = installed.name;
    doc["version"] = installed.version;
    nlohmann::json files = nlohmann::json::array();
    for (const PackFile& f : installed.files) {
        files.push_back({{"path", f.path}, {"size", f.size}, {"sha256", f.sha256}});
    }
    doc["files"] = std::move(files);
    return doc.dump(1);
}

Plan planSync(const Index& index, const Installed* installed,
              const std::function<std::int64_t(const std::string&)>& sizeOnDisk) {
    Plan plan;
    std::unordered_set<std::string> wanted;
    for (const PackFile& f : index.files) {
        wanted.insert(f.path);
        bool keep = false;
        if (installed != nullptr) {
            for (const PackFile& have : installed->files) {
                if (have.path == f.path && have.sha256 == f.sha256) {
                    keep = sizeOnDisk(f.path) == static_cast<std::int64_t>(f.size);
                    break;
                }
            }
        }
        if (keep) {
            plan.keep.push_back(f);
        } else {
            plan.download.push_back(f);
            plan.downloadBytes += f.size;
        }
    }
    if (installed != nullptr) {
        for (const PackFile& have : installed->files) {
            if (wanted.count(have.path) == 0) plan.remove.push_back(have.path);
        }
    }
    return plan;
}

bool isPrivateHost(const std::string& host) {
    if (host == "localhost" || host == "::1") return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 127) return true;
    if (a == 10) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    return false;
}

}  // namespace wowee::core::wowpack
