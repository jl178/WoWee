// MacroEvaluator - the stock macro option parser (SecureCmdOptionParse).
//
// Wow.exe 12340 addresses (docs/re/macros-bindings.md):
//   0x564AE0  Lua SecureCmdOptionParse -> 0x5F0DF0
//   0x5F0DF0  clause/bracket walk, the bracket cache (0x617C50)
//   0x5F0BB0  one bracket: split on ',', target= / @
//   0x5F0040  one condition: "no", name:arg/arg, the name table 0xAD14D8
//   0x5EF5C0  evaluate a bracket: every condition, negated where asked
//   0xAD1560  the handlers, one per name
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/i_game_state.hpp"
#include "ui/chat/i_modifier_state.hpp"
#include <array>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace wowee { namespace ui {

namespace {

/// The stock name table (0xAD14D8), in its order. Index 0 is the empty name,
/// which an empty condition ("[combat,]") matches and which always holds.
enum class Cond {
    Empty, Combat, Exists, Help, Harm, Party, Raid, Dead, Flyable, Indoors,
    Outdoors, Swimming, Flying, Mounted, Stealth, Group, Stance, Pet, Modifier,
    Button, ActionBar, BonusBar, Equipped, Channeling, Spec, VehicleUI,
    UnitHasVehicleUI, Cursor,
};

struct NameEntry { const char* name; Cond cond; };
constexpr std::array<NameEntry, 33> kNames{{
    {"",                 Cond::Empty},
    {"combat",           Cond::Combat},
    {"exists",           Cond::Exists},
    {"help",             Cond::Help},
    {"harm",             Cond::Harm},
    {"party",            Cond::Party},
    {"raid",             Cond::Raid},
    {"dead",             Cond::Dead},
    {"flyable",          Cond::Flyable},
    {"indoors",          Cond::Indoors},
    {"outdoors",         Cond::Outdoors},
    {"swimming",         Cond::Swimming},
    {"flying",           Cond::Flying},
    {"mounted",          Cond::Mounted},
    {"stealth",          Cond::Stealth},
    {"group",            Cond::Group},
    {"stance",           Cond::Stance},
    {"form",             Cond::Stance},
    {"pet",              Cond::Pet},
    {"modifier",         Cond::Modifier},
    {"mod",              Cond::Modifier},
    {"button",           Cond::Button},
    {"btn",              Cond::Button},
    {"actionbar",        Cond::ActionBar},
    {"bar",              Cond::ActionBar},
    {"bonusbar",         Cond::BonusBar},
    {"equipped",         Cond::Equipped},
    {"worn",             Cond::Equipped},
    {"channeling",       Cond::Channeling},
    {"spec",             Cond::Spec},
    {"vehicleui",        Cond::VehicleUI},
    {"unithasvehicleui", Cond::UnitHasVehicleUI},
    {"cursor",           Cond::Cursor},
}};

struct Condition {
    Cond cond = Cond::Empty;
    bool negate = false;
    std::vector<std::string> args;
};

struct Bracket {
    std::vector<Condition> conditions;
    std::string unit;
    bool hasUnit = false;
};

std::string_view trimSpaces(std::string_view s) {
    // Spaces only: the stock walk trims ' ' and nothing else.
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && equalsNoCase(s.substr(0, prefix.size()), prefix);
}

/// The leading digits as a number, 0 when there are none (0x5EEB00).
int leadingNumber(std::string_view s) {
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        n = n * 10 + (c - '0');
    }
    return n;
}

/// One condition (0x5F0040).
Condition parseCondition(std::string_view tok, const IGameState& state) {
    Condition c;
    // "no" in lower case and something after it (the length test is < 3).
    if (tok.size() >= 3 && tok.substr(0, 2) == "no") {
        c.negate = true;
        tok.remove_prefix(2);
    }
    std::string_view name = tok;
    std::string_view argText;
    bool hasColon = false;
    if (const size_t colon = tok.find(':'); colon != std::string_view::npos) {
        name = tok.substr(0, colon);
        argText = tok.substr(colon + 1);
        hasColon = true;
    }
    name = trimSpaces(name);
    bool found = false;
    for (const auto& e : kNames) {
        if (name == e.name) { c.cond = e.cond; found = true; break; }
    }
    if (!found) {
        state.reportUnknownOption(std::string(name));
        c.cond = Cond::Empty;
        return c;
    }
    // Arguments only when there is something after the colon.
    if (hasColon && !argText.empty()) {
        size_t start = 0;
        while (true) {
            const size_t slash = argText.find('/', start);
            const std::string_view part = argText.substr(
                start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            c.args.emplace_back(trimSpaces(part));
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
    }
    return c;
}

/// One bracket's contents (0x5F0BB0).
Bracket parseBracket(std::string_view contents, const IGameState& state) {
    Bracket b;
    if (contents.empty()) return b;
    size_t start = 0;
    while (start <= contents.size()) {
        const size_t comma = contents.find(',', start);
        const std::string_view tok = trimSpaces(contents.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        // "target=" exactly, then '@'; anything else is a condition.
        if (tok.size() >= 7 && tok.substr(0, 7) == "target=") {
            b.unit = std::string(trimSpaces(tok.substr(7)));
            b.hasUnit = true;
        } else if (!tok.empty() && tok.front() == '@') {
            b.unit = std::string(trimSpaces(tok.substr(1)));
            b.hasUnit = true;
        } else {
            b.conditions.push_back(parseCondition(tok, state));
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return b;
}

std::unordered_map<std::string, Bracket>& bracketCache() {
    static std::unordered_map<std::string, Bracket> cache;
    return cache;
}

/// The mouse button a [button:n] number names (0x5EEC20 -> 0x96F530).
std::string buttonName(int n) {
    switch (n) {
        case 1: return "LeftButton";
        case 2: return "RightButton";
        case 3: return "MiddleButton";
        default: return "Button" + std::to_string(n);
    }
}

/// [mod:x] with an argument (0x55F940): a run of modifier names, each
/// optionally followed by '-', all of which must be held; or else the name of
/// a modified click (SELFCAST), asked as IsModifiedClick would.
bool modifierArgHolds(std::string_view arg, const IModifierState& mods) {
    using Key = IModifierState::Key;
    unsigned mask = 0;
    std::string_view rest = arg;
    struct ModName { const char* name; unsigned bits; };
    static constexpr ModName kMods[] = {
        {"LSHIFT", 0x01}, {"RSHIFT", 0x02}, {"SHIFT", 0x03},
        {"LCTRL", 0x04},  {"RCTRL", 0x08},  {"CTRL", 0x0C},
        {"LALT", 0x10},   {"RALT", 0x20},   {"ALT", 0x30},
    };
    while (!rest.empty()) {
        bool matched = false;
        for (const auto& m : kMods) {
            const std::string_view n(m.name);
            if (startsWithNoCase(rest, n)) {
                mask |= m.bits;
                rest.remove_prefix(n.size());
                matched = true;
                break;
            }
        }
        if (!matched) break;
        if (!rest.empty() && rest.front() == '-') rest.remove_prefix(1);
    }
    if (mask == 0) return mods.isModifiedClick(std::string(arg));

    auto side = [&](unsigned bits, Key left, Key right) {
        switch (bits) {
            case 0: return true;
            case 1: return mods.isKeyDown(left);
            case 2: return mods.isKeyDown(right);
            default: return mods.isKeyDown(left) || mods.isKeyDown(right);
        }
    };
    return side(mask & 0x03, Key::LeftShift, Key::RightShift) &&
           side((mask >> 2) & 0x03, Key::LeftCtrl, Key::RightCtrl) &&
           side((mask >> 4) & 0x03, Key::LeftAlt, Key::RightAlt);
}

bool anyModifierDown(const IModifierState& mods) {
    using Key = IModifierState::Key;
    for (Key k : {Key::LeftShift, Key::RightShift, Key::LeftCtrl, Key::RightCtrl,
                  Key::LeftAlt, Key::RightAlt})
        if (mods.isKeyDown(k)) return true;
    return false;
}

/// One condition against the game (the handlers at 0xAD1560), before "no".
bool holds(const Condition& c, const std::string& unit, const IGameState& gs,
           const IModifierState& mods) {
    const auto& args = c.args;
    // The yes/no conditions take a number: [combat:0] is out of combat.
    auto flag = [&](bool state) {
        if (args.empty()) return state;
        return state == (leadingNumber(args.front()) != 0);
    };
    auto anyNumberIs = [&](int value) {
        for (const auto& a : args) if (leadingNumber(a) == value) return true;
        return false;
    };
    auto anyNameIs = [&](const std::string& value) {
        for (const auto& a : args) if (equalsNoCase(a, value)) return true;
        return false;
    };

    switch (c.cond) {
        case Cond::Empty:     return true;
        case Cond::Combat:    return flag(gs.inCombat());
        case Cond::Exists:    return flag(gs.unitExists(unit));
        case Cond::Help:      return flag(gs.unitCanAssist(unit));
        case Cond::Harm:      return flag(gs.unitCanAttack(unit));
        case Cond::Party:     return flag(gs.unitInParty(unit));
        case Cond::Raid:      return flag(gs.unitInParty(unit) || gs.unitInRaid(unit));
        case Cond::Dead:      return flag(gs.unitIsDead(unit));
        case Cond::Flyable:   return flag(gs.isFlyableArea());
        case Cond::Indoors:   return flag(!gs.isOutdoors());
        case Cond::Outdoors:  return flag(gs.isOutdoors());
        case Cond::Swimming:  return flag(gs.isSwimming());
        case Cond::Flying:    return flag(gs.isFlying());
        case Cond::Mounted:   return flag(gs.isMounted());
        case Cond::Stealth:   return flag(gs.isStealthed());
        case Cond::Group: {
            if (args.empty()) return gs.inParty() || gs.inRaid();
            // Last argument first, and only the two exact words.
            for (auto it = args.rbegin(); it != args.rend(); ++it) {
                if (*it == "party") return gs.inParty();
                if (*it == "raid") return gs.inRaid();
            }
            return false;
        }
        case Cond::Stance: {
            const int form = gs.shapeshiftForm();
            return args.empty() ? form != 0 : anyNumberIs(form);
        }
        case Cond::Pet: {
            if (!gs.hasPet()) return false;
            if (args.empty()) return true;
            return anyNameIs(gs.petName()) || anyNameIs(gs.petFamily());
        }
        case Cond::Modifier: {
            if (args.empty()) return anyModifierDown(mods);
            for (const auto& a : args) if (modifierArgHolds(a, mods)) return true;
            return false;
        }
        case Cond::Button: {
            std::string clicked = gs.clickedButton();
            if (clicked.empty()) clicked = "LeftButton";
            for (const auto& a : args) {
                const bool numeric = !a.empty() && a.front() >= '0' && a.front() <= '9';
                if (equalsNoCase(numeric ? buttonName(leadingNumber(a)) : a, clicked)) return true;
            }
            return false;
        }
        case Cond::ActionBar: return anyNumberIs(gs.actionBarPage());
        case Cond::BonusBar:  return anyNumberIs(gs.bonusBarOffset());
        case Cond::Equipped: {
            for (const auto& a : args) if (gs.isEquippedItemType(a)) return true;
            return false;
        }
        case Cond::Channeling: {
            const std::string spell = gs.channelingSpell();
            if (spell.empty()) return false;
            return args.empty() || anyNameIs(spell);
        }
        case Cond::Spec:      return anyNumberIs(gs.activeTalentGroup());
        case Cond::VehicleUI: return gs.playerHasVehicleUI();
        case Cond::UnitHasVehicleUI: return gs.unitHasVehicleUI(unit);
        case Cond::Cursor: {
            const std::string type = gs.cursorType();
            if (type.empty()) return false;
            return args.empty() || anyNameIs(type);
        }
    }
    return true;
}

} // namespace

MacroEvaluator::MacroEvaluator(const IGameState& gameState, const IModifierState& modState)
    : gameState_(gameState), modState_(modState) {}

void MacroEvaluator::clearCache() { bracketCache().clear(); }

bool MacroEvaluator::evaluateBracket(std::string_view contents, std::string& unit,
                                     bool& hasUnit) const {
    auto& cache = bracketCache();
    const std::string key(contents);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, parseBracket(contents, gameState_)).first;
    const Bracket& b = it->second;
    // The unit is resolved once per bracket and every unit condition asks
    // about it; with none given that is the current target.
    for (const auto& c : b.conditions) {
        bool v = holds(c, b.unit, gameState_, modState_);
        if (c.negate) v = !v;
        if (!v) return false;
    }
    unit = b.unit;
    hasUnit = b.hasUnit && !b.unit.empty();
    return true;
}

MacroOptions MacroEvaluator::parse(std::string_view s) const {
    MacroOptions out;
    size_t actionStart = 0;
    bool sawBracket = false;
    bool passed = false;
    std::string unit;
    bool hasUnit = false;
    size_t pos = 0;
    while (true) {
        if (pos >= s.size() || s[pos] == ';') {
            if (!sawBracket || passed) {
                out.matched = true;
                if (actionStart < pos && actionStart < s.size())
                    out.action = std::string(trimSpaces(s.substr(actionStart, pos - actionStart)));
                if (passed) {
                    out.target = unit;
                    out.hasTarget = hasUnit;
                }
                return out;
            }
            if (pos >= s.size()) return out;
            ++pos;
            actionStart = pos;
            sawBracket = false;
            continue;
        }
        if (s[pos] == '[') {
            size_t start = pos + 1;
            while (start < s.size() && s[start] == ' ') ++start;
            size_t close = start;
            while (close < s.size() && s[close] != ']') ++close;
            size_t end = close;
            while (end > start && s[end - 1] == ' ') --end;
            if (!passed) passed = evaluateBracket(s.substr(start, end - start), unit, hasUnit);
            sawBracket = true;
            // A bracket left open runs to the end and leaves no action.
            pos = close + 1;
            actionStart = pos;
            if (pos > s.size()) pos = s.size();
            continue;
        }
        ++pos;
    }
}

} // namespace ui
} // namespace wowee
