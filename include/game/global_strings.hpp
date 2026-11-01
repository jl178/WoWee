#pragma once

#include <cstdint>
#include <string>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

/// The GlobalStrings.lua name for a SpellCastResult (AzerothCore 3.3.5a
/// SharedDefines order), e.g. 79 -> "SPELL_FAILED_NO_DUELING". Empty when
/// the value has no name.
const char* spellCastResultName(uint8_t result);

/// A FrameXML GlobalStrings.lua value by name ("Dueling isn't allowed here."),
/// read once from the client data. Empty when unknown.
std::string globalString(pipeline::AssetManager* assets, const std::string& key);

} // namespace game
} // namespace wowee
