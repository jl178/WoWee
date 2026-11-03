#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

/// A GlobalStrings format filled in the way the client's own printf does it
/// for these strings: %s, %d, %i and %u take the next argument in turn, and
/// %N$s / %N$d take argument N (CHAT_INVITE_NOTICE's "%2$s ... '%1$s'").
/// %% is a percent sign. Missing arguments become empty.
inline std::string formatGlobalString(const std::string& format, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(format.size() + 16);
    size_t next = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if (c != '%' || i + 1 >= format.size()) { out += c; continue; }
        if (format[i + 1] == '%') { out += '%'; ++i; continue; }
        size_t j = i + 1;
        size_t index = next;
        // %N$x: an explicit argument number.
        size_t k = j;
        while (k < format.size() && format[k] >= '0' && format[k] <= '9') ++k;
        bool positional = false;
        if (k > j && k < format.size() && format[k] == '$') {
            index = static_cast<size_t>(std::stoul(format.substr(j, k - j))) - 1;
            j = k + 1;
            positional = true;
        }
        // Flags, width and precision are carried by a few strings ("%.1f");
        // they are skipped, the argument arrives already written out.
        while (j < format.size() && std::string_view("-+ #0123456789.").find(format[j]) != std::string_view::npos) ++j;
        if (j >= format.size()) { out += format.substr(i); break; }
        const char conv = format[j];
        if (std::string_view("sdiufgx").find(conv) == std::string_view::npos) {
            out += format.substr(i, j - i + 1);
            i = j;
            continue;
        }
        if (index < args.size()) out += args[index];
        if (!positional) ++next;
        i = j;
    }
    return out;
}


} // namespace game
} // namespace wowee
