// Off is stock: the enhancement table, the Classic preset and the defaults.
//
// docs/enhancements-plan.md section 3.3. Enhancements{} is the stock client.
// The Classic preset has to be exactly that, every row of the table has to say
// so, and a member added to the struct without a row - which is how Classic
// lost four keys when they were lists - has to fail here rather than in game.
#include <catch_amalgamated.hpp>

#include "rendering/enhancements.hpp"
#include "ui/enhancement_rows.hpp"
#include "ui/graphics_presets.hpp"
#include "ui/settings_schema.hpp"

#include <set>
#include <string>

using wowee::rendering::Enhancements;
using namespace wowee::ui;

namespace {

/// How many members an aggregate has: the most arguments a brace
/// initialiser takes. A member added to Enhancements without a row in
/// kEnhancementRows makes this disagree with the row count.
struct AnyArg {
    template <class T>
    constexpr operator T() const { return T{}; }
};

template <class T, class... A>
constexpr std::size_t memberCount() {
    if constexpr (requires { T{A{}..., AnyArg{}}; }) {
        return memberCount<T, A..., AnyArg>();
    } else {
        return sizeof...(A);
    }
}

}  // namespace

TEST_CASE("every row has exactly one member, and every member a row", "[enhancements]") {
    std::set<std::string> keys, saveKeys;
    for (const auto& r : kEnhancementRows) {
        INFO(r.key);
        const int pointers = (r.asBool ? 1 : 0) + (r.asInt ? 1 : 0) + (r.asFloat ? 1 : 0);
        CHECK(pointers == 1);
        CHECK(keys.insert(r.key).second);
        CHECK(saveKeys.insert(r.saveKey).second);
        CHECK(std::string(r.controls).size() > 0);
        CHECK(std::string(r.label).size() > 0);
        CHECK(std::string(r.tooltip).size() > 0);
        if (r.kind == SettingKind::Bool) CHECK(r.asBool != nullptr);
        if (r.kind == SettingKind::Enum) CHECK(r.asInt != nullptr);
    }
    // The guard: a member with no row is a member Classic does not reset.
    INFO("Enhancements has " << memberCount<Enhancements>() << " members and kEnhancementRows "
         << kEnhancementRowCount << " rows - a member was added without a row");
    CHECK(memberCount<Enhancements>() == kEnhancementRowCount);
}

TEST_CASE("every row's stock value is the member's default", "[enhancements]") {
    const Enhancements stock{};
    for (const auto& r : kEnhancementRows) {
        INFO(r.key << ": the table says stock is " << r.stockValue
                   << " and Enhancements{} holds " << enhancementValue(stock, r));
        CHECK(enhancementValue(stock, r) == r.stockValue);
    }
}

TEST_CASE("the all-off state is the Classic preset", "[enhancements]") {
    const GraphicsPresetValues& classic = kGraphicsPresets[kClassicPresetIndex];
    CHECK(classic.enh == Enhancements{});

    // And setting every row to its stock value gets there from anywhere.
    Enhancements e = recommendedEnhancements();
    for (const auto& r : kEnhancementRows) setEnhancementValue(e, r, r.stockValue);
    CHECK(e == Enhancements{});
}

TEST_CASE("only Classic is the stock picture", "[enhancements]") {
    // Every other preset turns something on; a preset that equals Classic in
    // its enhancements would make the two indistinguishable to the matcher.
    for (int i = 0; i < kClassicPresetIndex; ++i) {
        INFO("preset " << i);
        CHECK_FALSE(kGraphicsPresets[i].enh == Enhancements{});
    }
}

TEST_CASE("the schema offers every row, with its recommended value as default", "[enhancements]") {
    std::size_t count = 0;
    const SettingDesc* schema = clientSettingsSchema(count);
    REQUIRE(count > 0);
    for (const auto& r : kEnhancementRows) {
        const SettingDesc* found = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            if (std::string(schema[i].key) == r.key) { found = &schema[i]; break; }
        }
        INFO(r.key << " is in kEnhancementRows and not in the settings schema");
        REQUIRE(found != nullptr);
        CHECK(found->defaultValue == r.recommendedValue);
        CHECK(found->kind == r.kind);
        CHECK(std::string(found->category) == r.category);
        float lo = 0, hi = 0;
        CHECK(settingRange(r.key, lo, hi));
        CHECK(r.stockValue >= r.minValue - 1e-6f);
        if (r.kind != SettingKind::Bool) CHECK(r.stockValue <= r.maxValue + 1e-6f);
    }
    // And no key is offered twice: a row left behind in the hand-written
    // schema would be a second control for the same value.
    std::set<std::string> seen;
    for (std::size_t i = 0; i < count; ++i) {
        INFO(schema[i].key << " appears twice in the settings schema");
        CHECK(seen.insert(schema[i].key).second);
    }
}

TEST_CASE("each enhancement page keeps its rows together", "[enhancements]") {
    // The options panels lay a category out as it is first met; a category
    // that reappears later starts a second page with the same name.
    std::size_t count = 0;
    const SettingDesc* schema = clientSettingsSchema(count);
    std::set<std::string> closed;
    std::string current;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string c = schema[i].category;
        if (c == current) continue;
        INFO("category " << c << " resumes at " << schema[i].key);
        CHECK(closed.count(c) == 0);
        if (!current.empty()) closed.insert(current);
        current = c;
    }
}
