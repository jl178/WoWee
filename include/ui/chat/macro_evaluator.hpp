// MacroEvaluator - the macro option parser, SecureCmdOptionParse.
//
// One parser for every caller: the Lua SecureCmdOptionParse that FrameXML's
// secure slash commands and state drivers run (lua_macro_api.cpp), and this
// client's own /cast, /use, /castsequence, /target, /focus, /stopmacro and the
// rest (chat/commands). The rules are the stock client's, read from Wow.exe
// 12340 (0x564AE0 -> 0x5F0DF0, docs/re/macros-bindings.md):
//
//   "[c1,c2] [c3] action; [c4] other; fallback"
//
// - Clauses split on ';'. A clause with no brackets always matches.
// - Several brackets in one clause are alternatives (OR); the conditions in a
//   bracket, split on ',', must all hold (AND).
// - "no" (lower case, followed by at least one more letter) negates a
//   condition. Names are case-sensitive and must be in the stock table of 33;
//   any other name reports ERR_UNKNOWN_MACRO_OPTION_S once and counts as true.
// - "name:a/b/c" gives arguments, alternatives split on '/'.
// - "target=unit" or "@unit" in a bracket names the unit the conditions in it
//   test (the current target otherwise) and is handed back with the action.
// - The action is the text after the last bracket, trimmed of spaces.
//   Nothing matching hands back nothing at all (nil in Lua); a matching clause
//   with no text hands back "".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wowee {
namespace game { class GameHandler; }
namespace ui {

class IGameState;
class IModifierState;

/// What SecureCmdOptionParse returns: the action and, when the matching
/// bracket named one, the unit.
struct MacroOptions {
    bool matched = false;   ///< false: no clause matched (Lua: no values)
    std::string action;     ///< the text of the matching clause, trimmed
    std::string target;     ///< the target=/@ unit of the matching bracket
    bool hasTarget = false; ///< false: none given (Lua: nil)
};

class MacroEvaluator {
public:
    MacroEvaluator(const IGameState& gameState, const IModifierState& modState);

    /// SecureCmdOptionParse(options).
    [[nodiscard]] MacroOptions parse(std::string_view options) const;

    /// One bracket's contents, without the brackets: whether it holds, and the
    /// unit it named. Exposed for the tests and for [target=] callers.
    [[nodiscard]] bool evaluateBracket(std::string_view contents, std::string& unit,
                                       bool& hasUnit) const;

    /// The parsed brackets are kept, as the stock client keeps them
    /// (0x617C50); this empties that cache so a test sees a first parse again.
    static void clearCache();

private:
    const IGameState& gameState_;
    const IModifierState& modState_;
};

/// SecureCmdOptionParse with this client's game state behind it, for the
/// chat commands. The unit is resolved here: targetGuid is the target= unit's
/// guid (0 when it names nobody), or the current target when none was given.
struct MacroOptionsResult : MacroOptions {
    uint64_t targetGuid = 0;
};
MacroOptionsResult evaluateMacroOptions(const std::string& options, game::GameHandler& gameHandler);

/// A unit token ("focus", "party1target") or the name of a unit nearby or in
/// the group, as TargetUnit and the other unit verbs take it; 0 for nobody.
uint64_t resolveMacroUnit(const std::string& unit, game::GameHandler& gameHandler);

/// The older shape of the same call, kept for the callers that only want the
/// action: "" when nothing matched. targetOverride is the target= unit's guid,
/// or UINT64_MAX when the matching bracket named no unit.
std::string evaluateMacroConditionals(const std::string& rawArg,
                                      game::GameHandler& gameHandler,
                                      uint64_t& targetOverride);

} // namespace ui
} // namespace wowee
