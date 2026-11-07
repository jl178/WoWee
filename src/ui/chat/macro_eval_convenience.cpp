// evaluateMacroOptions - SecureCmdOptionParse for this client's chat commands.
//
// The same parser the interface's SecureCmdOptionParse runs, over the same
// answers: with an interface loaded the conditions are asked of its Lua API
// (addons::LuaMacroState), so /cast typed into this client's chat and /cast in
// FrameXML's agree. Without one, GameStateAdapter answers from the game.
// Separate TU so the macro_evaluator unit test links without the client.
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/game_state_adapter.hpp"
#include "ui/chat/input_modifier_adapter.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_macro_state.hpp"
#include "game/game_handler.hpp"
#include "core/application.hpp"
#include "rendering/renderer.hpp"

namespace wowee { namespace ui {

MacroOptionsResult evaluateMacroOptions(const std::string& options, game::GameHandler& gameHandler) {
    MacroOptionsResult out;
    auto& app = core::Application::getInstance();
    auto* am = app.getAddonManager();
    lua_State* L = (am && am->isInitialized()) ? am->getLuaEngine()->getState() : nullptr;
    if (L) {
        addons::LuaMacroState state(L);
        addons::LuaModifierState mods(L);
        static_cast<MacroOptions&>(out) = MacroEvaluator(state, mods).parse(options);
    } else {
        GameStateAdapter state(gameHandler, app.getRenderer());
        InputModifierAdapter mods;
        static_cast<MacroOptions&>(out) = MacroEvaluator(state, mods).parse(options);
    }
    if (out.matched) {
        out.targetGuid = out.hasTarget
            ? addons::resolveUnitOrName(&gameHandler, out.target)
            : gameHandler.getTargetGuid();
    }
    return out;
}

uint64_t resolveMacroUnit(const std::string& unit, game::GameHandler& gameHandler) {
    return addons::resolveUnitOrName(&gameHandler, unit);
}

std::string evaluateMacroConditionals(const std::string& rawArg,
                                      game::GameHandler& gameHandler,
                                      uint64_t& targetOverride) {
    const MacroOptionsResult r = evaluateMacroOptions(rawArg, gameHandler);
    targetOverride = (r.matched && r.hasTarget) ? r.targetGuid : static_cast<uint64_t>(-1);
    return r.matched ? r.action : std::string();
}

} // namespace ui
} // namespace wowee
