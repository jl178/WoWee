// Settings saved before the rename to the enhanced naming load unchanged.
//
// core/legacy_settings_migration.hpp maps the old settings.cfg keys, the old
// setting names and the old light table file to the new ones. A file written
// before the rename has to come back with every enhancement row as it was
// saved, and the old names must not be in use for anything else.
#include <catch_amalgamated.hpp>

#include "core/legacy_settings_migration.hpp"
#include "rendering/enhanced_lighting_table.hpp"
#include "rendering/enhancements.hpp"
#include "ui/enhancement_rows.hpp"

#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using wowee::rendering::Enhancements;
using namespace wowee::ui;
namespace core = wowee::core;

namespace {

using Lines = std::vector<std::pair<std::string, std::string>>;

Lines parse(const std::string& file) {
    Lines out;
    std::istringstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

/// GameScreen::loadSettings' enhancement half, line for line: the file
/// migrated, then each line that names a row by its saved key set onto it.
Enhancements load(const std::string& file) {
    Enhancements e;
    for (const auto& [key, val] : core::migrateSettingsLines(parse(file))) {
        if (const EnhancementRow* row = findEnhancementRowBySaveKey(key)) {
            setEnhancementValue(e, *row,
                                row->asBool ? (std::stoi(val) != 0 ? 1.0f : 0.0f)
                                            : std::stof(val));
        }
    }
    return e;
}

/// The key a row was saved under before the rename.
std::string oldSaveKey(const EnhancementRow& r) {
    for (const auto& m : core::kLegacySettingsKeys) {
        if (std::strcmp(m.newName, r.saveKey) == 0) return m.oldName;
    }
    return r.saveKey;
}

/// A value for every row that is not its stock one, so a row the load
/// missed shows up as stock.
float notStock(const EnhancementRow& r) {
    if (r.kind == SettingKind::Bool) return r.stockValue != 0.0f ? 0.0f : 1.0f;
    return r.stockValue == r.maxValue ? r.minValue : r.maxValue;
}

std::string file(bool oldKeys, float (*value)(const EnhancementRow&)) {
    std::ostringstream out;
    out << "ui_opacity=80\ngraphics_preset=6\nshadows=1\n";
    for (const auto& r : kEnhancementRows) {
        out << (oldKeys ? oldSaveKey(r) : std::string(r.saveKey)) << "=" << value(r) << "\n";
    }
    out << "view_distance=1900\n";
    return out.str();
}

}  // namespace

TEST_CASE("legacy settings: every old key maps to a row, and no row still uses one",
          "[settings][legacy]") {
    for (const auto& m : core::kLegacySettingsKeys) {
        INFO(m.oldName << " -> " << m.newName);
        CHECK(findEnhancementRowBySaveKey(m.newName) != nullptr);
        CHECK(findEnhancementRowBySaveKey(m.oldName) == nullptr);
        CHECK(std::string(core::migratedSettingsKey(m.oldName)) == m.newName);
    }
    for (const auto& m : core::kLegacySettingNames) {
        INFO(m.oldName << " -> " << m.newName);
        CHECK(findEnhancementRow(m.newName) != nullptr);
        CHECK(findEnhancementRow(m.oldName) == nullptr);
        CHECK(std::string(core::migratedSettingName(m.oldName)) == m.newName);
    }
    CHECK(core::migratedSettingsKey("enhanced_daylight") == nullptr);
    CHECK(core::migratedSettingsKey("shadows") == nullptr);
    CHECK(core::migratedSettingName("skyshade") == nullptr);
}

TEST_CASE("legacy settings: an old-style file loads with every row in the same state",
          "[settings][legacy]") {
    // Twice: every row off its stock value, and every row on it, so a renamed
    // key that loaded as nothing fails one way or the other.
    float (*const values[])(const EnhancementRow&) = {
        notStock, [](const EnhancementRow& r) { return r.stockValue; }};
    for (auto value : values) {
        const Enhancements fromOld = load(file(true, value));
        const Enhancements fromNew = load(file(false, value));
        for (const auto& r : kEnhancementRows) {
            INFO(r.key << " (saved as " << oldSaveKey(r) << ")");
            Enhancements want;
            setEnhancementValue(want, r, value(r));
            CHECK(enhancementValue(fromOld, r) == enhancementValue(want, r));
            CHECK(enhancementValue(fromOld, r) == enhancementValue(fromNew, r));
        }
    }
}

TEST_CASE("legacy settings: the rest of the file passes through as it was",
          "[settings][legacy]") {
    const Lines before = parse(file(true, notStock));
    const Lines after = core::migrateSettingsLines(before);
    REQUIRE(after.size() == before.size());
    std::size_t renamed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(after[i].second == before[i].second);
        if (after[i].first != before[i].first) {
            ++renamed;
            CHECK(std::string(core::migratedSettingsKey(before[i].first)) == after[i].first);
        }
    }
    CHECK(renamed == std::size(core::kLegacySettingsKeys));
}

TEST_CASE("legacy settings: a new key beside an old one wins", "[settings][legacy]") {
    // The login screen rewrites settings.cfg keeping the keys it does not
    // know, so an old line can sit beside the new one the game saved later.
    const auto& m = core::kLegacySettingsKeys[0];
    const EnhancementRow* row = findEnhancementRowBySaveKey(m.newName);
    REQUIRE(row != nullptr);
    REQUIRE(row->asBool != nullptr);
    for (const bool newFirst : {true, false}) {
        const std::string a = std::string(m.newName) + "=0\n";
        const std::string b = std::string(m.oldName) + "=1\n";
        const Enhancements e = load(newFirst ? a + b : b + a);
        CHECK_FALSE(e.*(row->asBool));
    }
}

TEST_CASE("legacy settings: the light table is looked for under its new name first",
          "[settings][legacy]") {
    namespace table = wowee::rendering::enhanced_lighting_table;
    CHECK(std::string(table::kFileName) == "wowee\\enhanced_lighting_table.bin");
    CHECK(std::string(core::kLegacyLightTableFile) != table::kFileName);
    CHECK(std::string(core::kLegacyLightTableFile).rfind("wowee\\", 0) == 0);
}
