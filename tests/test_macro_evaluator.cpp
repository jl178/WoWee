// Tests for MacroEvaluator - the stock macro option parser (SecureCmdOptionParse).
//
// The rules and the condition table are the shipped client's (Wow.exe 12340,
// 0x564AE0 / 0x5F0DF0 / 0x5F0BB0 / 0x5F0040 and the handlers at 0xAD1560);
// docs/re/macros-bindings.md has the addresses for each case below.

#include <catch_amalgamated.hpp>
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/i_game_state.hpp"
#include "ui/chat/i_modifier_state.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wowee::ui;

namespace {

/// A unit as the mock knows it.
struct MockUnit {
    bool exists = true;
    bool assist = false;
    bool attack = false;
    bool party = false;
    bool raid = false;
    bool dead = false;
    bool vehicleUI = false;
};

class MockGameState : public IGameState {
public:
    // "" is the current target, as the parser passes it.
    std::map<std::string, MockUnit> units;
    bool combat = false, flyable = false, outdoors = true, swimming = false, flying = false;
    bool mounted = false, stealthed = false, party = false, raid = false, vehicleUI = false;
    int form = 0, page = 1, bonus = 0, spec = 1;
    bool pet = false;
    std::string pName, pFamily, channel, cursor, button;
    std::set<std::string> equipped;
    mutable std::vector<std::string> unknown;

    const MockUnit* find(const std::string& u) const {
        auto it = units.find(u);
        return it == units.end() ? nullptr : &it->second;
    }
    bool unitExists(const std::string& u) const override { auto* m = find(u); return m && m->exists; }
    bool unitCanAssist(const std::string& u) const override { auto* m = find(u); return m && m->assist; }
    bool unitCanAttack(const std::string& u) const override { auto* m = find(u); return m && m->attack; }
    bool unitInParty(const std::string& u) const override { auto* m = find(u); return m && m->party; }
    bool unitInRaid(const std::string& u) const override { auto* m = find(u); return m && m->raid; }
    bool unitIsDead(const std::string& u) const override { auto* m = find(u); return m && m->dead; }
    bool unitHasVehicleUI(const std::string& u) const override { auto* m = find(u); return m && m->vehicleUI; }
    bool inCombat() const override { return combat; }
    bool isFlyableArea() const override { return flyable; }
    bool isOutdoors() const override { return outdoors; }
    bool isSwimming() const override { return swimming; }
    bool isFlying() const override { return flying; }
    bool isMounted() const override { return mounted; }
    bool isStealthed() const override { return stealthed; }
    bool inParty() const override { return party; }
    bool inRaid() const override { return raid; }
    bool playerHasVehicleUI() const override { return vehicleUI; }
    int shapeshiftForm() const override { return form; }
    int actionBarPage() const override { return page; }
    int bonusBarOffset() const override { return bonus; }
    int activeTalentGroup() const override { return spec; }
    bool hasPet() const override { return pet; }
    std::string petName() const override { return pName; }
    std::string petFamily() const override { return pFamily; }
    bool isEquippedItemType(const std::string& t) const override { return equipped.count(t) > 0; }
    std::string channelingSpell() const override { return channel; }
    std::string cursorType() const override { return cursor; }
    std::string clickedButton() const override { return button; }
    void reportUnknownOption(const std::string& name) const override { unknown.push_back(name); }
};

class MockMods : public IModifierState {
public:
    std::set<Key> down;
    std::set<std::string> clicks;
    bool isKeyDown(Key k) const override { return down.count(k) > 0; }
    bool isModifiedClick(const std::string& a) const override { return clicks.count(a) > 0; }
};

struct Fixture {
    MockGameState gs;
    MockMods mods;
    Fixture() { MacroEvaluator::clearCache(); }
    MacroOptions parse(const std::string& s) { return MacroEvaluator(gs, mods).parse(s); }
    /// The action, or "<nil>" when nothing matched.
    std::string act(const std::string& s) {
        const auto r = parse(s);
        return r.matched ? r.action : "<nil>";
    }
};

using Key = IModifierState::Key;

} // namespace

// ── Clauses, brackets, the action text ──────────────────────

TEST_CASE("Plain text is the action, trimmed of spaces", "[macro_eval]") {
    Fixture f;
    auto r = f.parse("  Fireball  ");
    CHECK(r.matched);
    CHECK(r.action == "Fireball");
    CHECK_FALSE(r.hasTarget);
}

TEST_CASE("Empty options match with an empty action", "[macro_eval]") {
    Fixture f;
    auto r = f.parse("");
    CHECK(r.matched);
    CHECK(r.action.empty());
}

TEST_CASE("A clause with no brackets always matches, the first one wins", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("Frostbolt; Fireball") == "Frostbolt");
    CHECK(f.act("[combat] A; B") == "B");
    f.gs.combat = true;
    CHECK(f.act("[combat] A; B") == "A");
}

TEST_CASE("Nothing matching hands back nothing", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[combat] A") == "<nil>");
    CHECK(f.act("[combat] A; [mounted] B") == "<nil>");
}

TEST_CASE("A matching clause with no text hands back an empty action", "[macro_eval]") {
    Fixture f;
    f.gs.combat = true;
    auto r = f.parse("[combat]");
    CHECK(r.matched);
    CHECK(r.action.empty());
    // An empty clause in the middle is a clause with no brackets.
    CHECK(f.act("[mounted] A; ; B").empty());
}

TEST_CASE("Brackets in one clause are alternatives", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[combat] [mounted] A; B") == "B");
    f.gs.mounted = true;
    CHECK(f.act("[combat] [mounted] A; B") == "A");
    CHECK(f.act("[combat][mounted]A;B") == "A");
}

TEST_CASE("Conditions in one bracket must all hold", "[macro_eval]") {
    Fixture f;
    f.gs.combat = true;
    CHECK(f.act("[combat,mounted] A; B") == "B");
    f.gs.mounted = true;
    CHECK(f.act("[combat,mounted] A; B") == "A");
    CHECK(f.act("[ combat , mounted ] A; B") == "A");
}

TEST_CASE("Text before a bracket is not part of the action", "[macro_eval]") {
    Fixture f;
    f.gs.combat = true;
    CHECK(f.act("Junk [combat] Real") == "Real");
}

TEST_CASE("A bracket left open leaves no action", "[macro_eval]") {
    Fixture f;
    f.gs.combat = true;
    auto r = f.parse("[combat A");
    CHECK(r.matched);
    CHECK(r.action.empty());
}

TEST_CASE("A semicolon inside a bracket does not split the clause", "[macro_eval]") {
    Fixture f;
    // "[a;b]" is one bracket whose condition "a;b" is unknown, so true.
    CHECK(f.act("[a;b] A; B") == "A");
}

TEST_CASE("Empty brackets and empty conditions hold", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[] A; B") == "A");
    f.gs.combat = true;
    CHECK(f.act("[combat,] A; B") == "A");
}

// ── "no", names, arguments ──────────────────────────────────

TEST_CASE("The no prefix negates", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[nocombat] A; B") == "A");
    f.gs.combat = true;
    CHECK(f.act("[nocombat] A; B") == "B");
}

TEST_CASE("Names are case-sensitive; others report once and hold", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[Combat] A; B") == "A");   // unknown, so true
    REQUIRE(f.gs.unknown.size() == 1);
    CHECK(f.gs.unknown[0] == "Combat");
    // The bracket is cached: the second parse reports nothing.
    CHECK(f.act("[Combat] A; B") == "A");
    CHECK(f.gs.unknown.size() == 1);
    // A negated unknown is false.
    CHECK(f.act("[nofoo] A; B") == "B");
    CHECK(f.gs.unknown.back() == "foo");
}

TEST_CASE("Conditions this client once added are not stock", "[macro_eval]") {
    Fixture f;
    // buff, debuff, casting, vehicle, stealthed: not in the 3.3.5a table.
    for (const char* n : {"buff:Rejuvenation", "casting", "vehicle", "stealthed"}) {
        f.gs.unknown.clear();
        (void)f.act(std::string("[") + n + "] A");
        CHECK(f.gs.unknown.size() == 1);
    }
}

TEST_CASE("Yes/no conditions take a number argument", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[combat:0] A; B") == "A");
    CHECK(f.act("[combat:1] A; B") == "B");
    f.gs.combat = true;
    CHECK(f.act("[combat:1] A; B") == "A");
    CHECK(f.act("[combat:x] A; B") == "B");   // no digits reads as 0
}

TEST_CASE("Each stock name is known", "[macro_eval]") {
    Fixture f;
    for (const char* n : {"combat", "exists", "help", "harm", "party", "raid", "dead", "flyable",
                          "indoors", "outdoors", "swimming", "flying", "mounted", "stealth",
                          "group", "stance", "form", "pet", "modifier", "mod", "button", "btn",
                          "actionbar", "bar", "bonusbar", "equipped", "worn", "channeling", "spec",
                          "vehicleui", "unithasvehicleui", "cursor"}) {
        (void)f.act(std::string("[") + n + "] A");
    }
    CHECK(f.gs.unknown.empty());
}

// ── target= and @ ───────────────────────────────────────────

TEST_CASE("target= and @ name the unit and hand it back", "[macro_eval]") {
    Fixture f;
    auto r = f.parse("[target=focus] Heal");
    CHECK(r.action == "Heal");
    CHECK(r.hasTarget);
    CHECK(r.target == "focus");
    r = f.parse("[@party1] Heal");
    CHECK(r.target == "party1");
    r = f.parse("[@ mouseover ] Heal");
    CHECK(r.target == "mouseover");
}

TEST_CASE("The unit is the one the unit conditions test", "[macro_eval]") {
    Fixture f;
    f.gs.units[""] = {.exists = true, .attack = true};
    f.gs.units["focus"] = {.exists = true, .assist = true};
    CHECK(f.act("[harm] A; B") == "A");          // the target
    CHECK(f.act("[@focus,harm] A; B") == "B");
    CHECK(f.act("[@focus,help] A; B") == "A");
    CHECK(f.act("[target=focus,help] A; B") == "A");
    CHECK(f.act("[@mouseover,exists] A; B") == "B");
}

TEST_CASE("Only the matching bracket's unit comes back", "[macro_eval]") {
    Fixture f;
    f.gs.units["focus"] = {.exists = true};
    auto r = f.parse("[@mouseover,exists] [@focus,exists] Heal; Other");
    CHECK(r.action == "Heal");
    CHECK(r.target == "focus");
    r = f.parse("[@mouseover,exists] A; B");
    CHECK(r.action == "B");
    CHECK_FALSE(r.hasTarget);
}

TEST_CASE("An empty @ is no unit", "[macro_eval]") {
    Fixture f;
    auto r = f.parse("[@] A");
    CHECK(r.matched);
    CHECK_FALSE(r.hasTarget);
}

TEST_CASE("target= is lower case only", "[macro_eval]") {
    Fixture f;
    (void)f.act("[Target=focus] A");
    REQUIRE(f.gs.unknown.size() == 1);
    CHECK(f.gs.unknown[0] == "Target=focus");
}

// ── The conditions ──────────────────────────────────────────

TEST_CASE("Unit conditions", "[macro_eval]") {
    Fixture f;
    f.gs.units[""] = {.exists = true, .party = true, .dead = true};
    CHECK(f.act("[exists] A; B") == "A");
    CHECK(f.act("[party] A; B") == "A");
    CHECK(f.act("[raid] A; B") == "A");        // party counts for raid (0x5EEE20)
    CHECK(f.act("[dead] A; B") == "A");
    CHECK(f.act("[unithasvehicleui] A; B") == "B");
    CHECK(f.act("[@nobody,exists] A; B") == "B");
}

TEST_CASE("Player state conditions", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[outdoors] A; B") == "A");
    CHECK(f.act("[indoors] A; B") == "B");
    f.gs.outdoors = false;
    CHECK(f.act("[indoors] A; B") == "A");
    f.gs.swimming = f.gs.flying = f.gs.mounted = f.gs.stealthed = f.gs.flyable = true;
    CHECK(f.act("[swimming,flying,mounted,stealth,flyable] A; B") == "A");
    CHECK(f.act("[vehicleui] A; B") == "B");
    f.gs.vehicleUI = true;
    CHECK(f.act("[vehicleui] A; B") == "A");
}

TEST_CASE("group with and without an argument", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[group] A; B") == "B");
    f.gs.party = true;
    CHECK(f.act("[group] A; B") == "A");
    CHECK(f.act("[group:party] A; B") == "A");
    CHECK(f.act("[group:raid] A; B") == "B");
    CHECK(f.act("[group:party/raid] A; B") == "B");   // the last argument decides
    CHECK(f.act("[group:Party] A; B") == "B");        // exact words only
    f.gs.raid = true;
    CHECK(f.act("[group:raid] A; B") == "A");
}

TEST_CASE("stance and form", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[stance] A; B") == "B");
    CHECK(f.act("[nostance] A; B") == "A");
    CHECK(f.act("[stance:0] A; B") == "A");
    f.gs.form = 3;
    CHECK(f.act("[form] A; B") == "A");
    CHECK(f.act("[stance:1/2] A; B") == "B");
    CHECK(f.act("[stance:1/3] A; B") == "A");
    CHECK(f.act("[noform:3] A; B") == "B");
}

TEST_CASE("pet, by name or family", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[pet] A; B") == "B");
    f.gs.pet = true;
    f.gs.pName = "Fluffy";
    f.gs.pFamily = "Wolf";
    CHECK(f.act("[pet] A; B") == "A");
    CHECK(f.act("[pet:wolf] A; B") == "A");
    CHECK(f.act("[pet:FLUFFY] A; B") == "A");
    CHECK(f.act("[pet:Imp/Voidwalker] A; B") == "B");
}

TEST_CASE("modifier names, sides and runs", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[mod] A; B") == "B");
    CHECK(f.act("[nomod] A; B") == "A");
    f.mods.down = {Key::RightShift};
    CHECK(f.act("[mod] A; B") == "A");
    CHECK(f.act("[mod:shift] A; B") == "A");
    CHECK(f.act("[modifier:SHIFT] A; B") == "A");
    CHECK(f.act("[mod:lshift] A; B") == "B");
    CHECK(f.act("[mod:rshift] A; B") == "A");
    CHECK(f.act("[mod:ctrl] A; B") == "B");
    CHECK(f.act("[mod:ctrl/shift] A; B") == "A");
    // A run needs every key in it; another key held does not spoil it.
    CHECK(f.act("[mod:shiftctrl] A; B") == "B");
    f.mods.down.insert(Key::LeftCtrl);
    CHECK(f.act("[mod:shiftctrl] A; B") == "A");
    CHECK(f.act("[mod:ctrl-shift] A; B") == "A");
    CHECK(f.act("[mod:shift] A; B") == "A");
    // Not a key: a modified click.
    f.mods.down.clear();
    CHECK(f.act("[mod:SELFCAST] A; B") == "B");
    f.mods.clicks = {"SELFCAST"};
    CHECK(f.act("[mod:SELFCAST] A; B") == "A");
}

TEST_CASE("button by number or name, LeftButton when no click runs", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[button] A; B") == "B");          // no argument: false
    CHECK(f.act("[button:1] A; B") == "A");
    CHECK(f.act("[btn:2] A; B") == "B");
    f.gs.button = "RightButton";
    CHECK(f.act("[button:2] A; B") == "A");
    CHECK(f.act("[button:1/2] A; B") == "A");
    CHECK(f.act("[button:rightbutton] A; B") == "A");
    f.gs.button = "Button4";
    CHECK(f.act("[button:4] A; B") == "A");
    f.gs.button = "MiddleButton";
    CHECK(f.act("[button:3] A; B") == "A");
}

TEST_CASE("actionbar, bonusbar and spec compare numbers", "[macro_eval]") {
    Fixture f;
    f.gs.page = 2;
    f.gs.bonus = 5;
    f.gs.spec = 2;
    CHECK(f.act("[bar:2] A; B") == "A");
    CHECK(f.act("[actionbar:1/3] A; B") == "B");
    CHECK(f.act("[bar] A; B") == "B");             // no argument: false
    CHECK(f.act("[bonusbar:5] A; B") == "A");
    CHECK(f.act("[bonusbar] A; B") == "B");
    CHECK(f.act("[spec:2] A; B") == "A");
    CHECK(f.act("[spec:1] A; B") == "B");
    CHECK(f.act("[spec] A; B") == "B");
}

TEST_CASE("equipped and worn", "[macro_eval]") {
    Fixture f;
    f.gs.equipped = {"Shields"};
    CHECK(f.act("[equipped:Shields] A; B") == "A");
    CHECK(f.act("[worn:Daggers/Shields] A; B") == "A");
    CHECK(f.act("[equipped:Daggers] A; B") == "B");
    CHECK(f.act("[equipped] A; B") == "B");
    CHECK(f.act("[noequipped:Shields] A; B") == "B");
}

TEST_CASE("channeling, any or by name", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[channeling] A; B") == "B");
    CHECK(f.act("[nochanneling] A; B") == "A");
    f.gs.channel = "Mind Flay";
    CHECK(f.act("[channeling] A; B") == "A");
    CHECK(f.act("[channeling:mind flay] A; B") == "A");
    CHECK(f.act("[channeling:Drain Life] A; B") == "B");
    CHECK(f.act("[nochanneling:Mind Flay] A; B") == "B");
    CHECK(f.act("[nochanneling:Drain Life] A; B") == "A");
}

TEST_CASE("cursor, any or by kind", "[macro_eval]") {
    Fixture f;
    CHECK(f.act("[cursor] A; B") == "B");
    f.gs.cursor = "item";
    CHECK(f.act("[cursor] A; B") == "A");
    CHECK(f.act("[cursor:spell/item] A; B") == "A");
    CHECK(f.act("[cursor:money] A; B") == "B");
}

// ── Macros as people write them ─────────────────────────────

TEST_CASE("A mouseover heal macro", "[macro_eval]") {
    Fixture f;
    f.gs.units["mouseover"] = {.exists = true, .assist = true};
    auto r = f.parse("[@mouseover,help,nodead][help,nodead][@player] Flash Heal");
    CHECK(r.action == "Flash Heal");
    CHECK(r.target == "mouseover");
    f.gs.units["mouseover"].dead = true;
    r = f.parse("[@mouseover,help,nodead][help,nodead][@player] Flash Heal");
    CHECK(r.target == "player");
}

TEST_CASE("A stance dance macro", "[macro_eval]") {
    Fixture f;
    f.gs.form = 1;
    CHECK(f.act("[stance:1] Charge; [stance:3] Intercept; Battle Stance") == "Charge");
    f.gs.form = 3;
    CHECK(f.act("[stance:1] Charge; [stance:3] Intercept; Battle Stance") == "Intercept");
    f.gs.form = 2;
    CHECK(f.act("[stance:1] Charge; [stance:3] Intercept; Battle Stance") == "Battle Stance");
}

TEST_CASE("A state driver string", "[macro_eval]") {
    Fixture f;
    // ActionBarController-style paging as securestatedriver.lua evaluates it.
    const std::string paging = "[bonusbar:5] 11; [bar:2] 2; [bonusbar:1] 7; 1";
    CHECK(f.act(paging) == "1");
    f.gs.page = 2;
    CHECK(f.act(paging) == "2");
    f.gs.bonus = 5;
    CHECK(f.act(paging) == "11");
    // "[@pet,exists] show; hide", the pet frame's driver.
    CHECK(f.act("[@pet,exists] show; hide") == "hide");
    f.gs.units["pet"] = {.exists = true};
    CHECK(f.act("[@pet,exists] show; hide") == "show");
}

TEST_CASE("castsequence keeps reset= in the action", "[macro_eval]") {
    Fixture f;
    f.gs.combat = true;
    CHECK(f.act("[combat] reset=5/target Rend, Hamstring; Charge") == "reset=5/target Rend, Hamstring");
}
