#pragma once

// GuildRoster()'s throttle, as Wow.exe keeps it (0x5CA9B0): a request goes
// out only once the previous one is ten seconds old, and a guild command
// result that makes the roster stale (a note changed, SMSG_GUILD_COMMAND_RESULT
// 0x13/0x14, 0x5CA0F0) clears it so the interface's next ask goes at once.

#include <chrono>

namespace wowee {
namespace game {

inline std::chrono::steady_clock::time_point& guildRosterNextAllowed() {
    static std::chrono::steady_clock::time_point next{};
    return next;
}

/// True, and the throttle started, when a GuildRoster() may be sent now.
inline bool takeGuildRosterSlot() {
    const auto now = std::chrono::steady_clock::now();
    auto& next = guildRosterNextAllowed();
    if (next != std::chrono::steady_clock::time_point{} && now < next) return false;
    next = now + std::chrono::seconds(10);
    return true;
}

inline void clearGuildRosterThrottle() { guildRosterNextAllowed() = {}; }

}  // namespace game
}  // namespace wowee
