#pragma once

/// The old names, and only here: what the enhanced look's settings and files
/// were called before they were renamed to the enhanced naming.
///
/// The rename changed three settings.cfg keys, three setting names (the keys
/// WoweeSetSetting and the options pages use) and the local light table's
/// file name. A settings file written before it still holds the old keys, and
/// a data folder still holds the table under its old name, so each is read
/// through this table: an old key loads as the new one, and the table is
/// looked for under the new name first and the old one after. Nothing writes
/// the old names; the next save writes the new keys.
///
/// The graphics preset is saved by its index (graphics_preset=6), which the
/// rename did not move, and no CVar was renamed, so neither needs a row.
/// This is the one file in the tree allowed to spell the old names.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee {
namespace core {

struct LegacyName {
    const char* oldName;
    const char* newName;
};

/// settings.cfg keys (EnhancementRow::saveKey).
inline constexpr LegacyName kLegacySettingsKeys[] = {
    {"enhanced_daylight", "enhanced_daylight"},
    {"enhanced_shadows",  "enhanced_shadows"},
    {"enhanced_water",    "enhanced_water"},
};

/// Setting names (EnhancementRow::key): the schema, the options pages and
/// WoweeSetSetting / the client-setting hooks Lua reaches.
inline constexpr LegacyName kLegacySettingNames[] = {
    {"enhanceddaylight", "enhanceddaylight"},
    {"enhancedshadows",  "enhancedshadows"},
    {"enhancedwater",    "enhancedwater"},
};

/// The local light table (enhanced_lighting_table.hpp's kFileName) under the
/// name tools wrote it with before the rename: read when the new name is
/// absent, until the table is regenerated.
inline constexpr const char* kLegacyLightTableFile = "wowee\\enhanced_lights.bin";

/// The new name for an old one in `table`, or nullptr when `name` is not old.
template <std::size_t N>
inline const char* legacyRename(const LegacyName (&table)[N], std::string_view name) {
    for (const auto& row : table) {
        if (name == row.oldName) return row.newName;
    }
    return nullptr;
}

inline const char* migratedSettingsKey(std::string_view key) {
    return legacyRename(kLegacySettingsKeys, key);
}

inline const char* migratedSettingName(std::string_view key) {
    return legacyRename(kLegacySettingNames, key);
}

/// settings.cfg's key=value lines, in file order, with each old key renamed.
/// A file that holds both an old key and its new one keeps the new line and
/// drops the old: the new one was written later (the login screen rewrites the
/// file keeping keys it does not know, so an old line can outlive the save
/// that replaced it).
inline std::vector<std::pair<std::string, std::string>>
migrateSettingsLines(std::vector<std::pair<std::string, std::string>> lines) {
    std::unordered_set<std::string> present;
    for (const auto& kv : lines) present.insert(kv.first);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(lines.size());
    for (auto& kv : lines) {
        if (const char* renamed = migratedSettingsKey(kv.first)) {
            if (present.count(renamed)) continue;
            kv.first = renamed;
        }
        out.push_back(std::move(kv));
    }
    return out;
}

}  // namespace core
}  // namespace wowee
