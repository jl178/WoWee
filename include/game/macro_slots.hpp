#pragma once

#include <cstdint>
#include <string>

namespace wowee {
namespace game {

/// How 3.3.5 numbers macros. blizzard_macroui.lua: MAX_ACCOUNT_MACROS = 36,
/// MAX_CHARACTER_MACROS = 18, and the character tab sets macroBase to 36 -
/// so 1-36 are the account's and 37-54 the character's. The two halves are
/// separate lists: deleting from one never shifts the other.
constexpr uint32_t kAccountMacroFirst   = 1;
constexpr uint32_t kAccountMacroLast    = 36;
constexpr uint32_t kCharacterMacroFirst = 37;
constexpr uint32_t kCharacterMacroLast  = 54;

inline bool isAccountMacroId(uint32_t id) {
    return id >= kAccountMacroFirst && id <= kAccountMacroLast;
}
inline bool isCharacterMacroId(uint32_t id) {
    return id >= kCharacterMacroFirst && id <= kCharacterMacroLast;
}

/// What an action-bar MACRO slot holding `slotId` must hold after macro
/// `deletedId` is deleted and the rest of its half shifts down one.
/// 0 means the slot pointed at the deleted macro and is cleared; a slot in the
/// other half, or below the deleted one, keeps its id.
inline uint32_t macroIdAfterDelete(uint32_t slotId, uint32_t deletedId) {
    if (slotId == deletedId) return 0;
    const bool sameHalf = (isAccountMacroId(slotId) && isAccountMacroId(deletedId)) ||
                          (isCharacterMacroId(slotId) && isCharacterMacroId(deletedId));
    if (sameHalf && slotId > deletedId) return slotId - 1;
    return slotId;
}

/// A macro body on one config line: newlines as "\n", backslashes doubled,
/// carriage returns dropped.
inline std::string escapeMacroBody(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        if (c == '\n') escaped += "\\n";
        else if (c == '\r') { /* skip CR */ }
        else if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }
    return escaped;
}

inline std::string unescapeMacroBody(const std::string& val) {
    std::string unescaped;
    unescaped.reserve(val.size());
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '\\' && i + 1 < val.size()) {
            if (val[i + 1] == 'n') { unescaped += '\n'; ++i; }
            else if (val[i + 1] == '\\') { unescaped += '\\'; ++i; }
            else unescaped += val[i];
        } else {
            unescaped += val[i];
        }
    }
    return unescaped;
}

}  // namespace game
}  // namespace wowee
