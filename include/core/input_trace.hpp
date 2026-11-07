#pragma once

// WOWEE_INPUT_TRACE=1: how long a key press or click takes to become a packet,
// a server reply and the interface's feedback, in milliseconds and frames.
//
// Off by default and costs one cached bool test where it is wired in. Each
// line is at warning so the default log keeps it:
//
//   [input] key 1 down          frame 4012 (+0.31 ms in frame), event +0.40 ms ago
//   [input] TX CMSG_CAST_SPELL  +0.52 ms after key 1 down, +0 frames, 0.43 ms into frame
//   [input] RX SMSG_SPELL_START +18.1 ms after key 1 down, +1 frames, 0.20 ms into frame
//
// "after" is measured from the event's own timestamp - when the window system
// queued it - not from when the pump got to it, so the time a press waits for
// the next frame is counted. See docs/re/input-timing.md for the numbers this
// produced and what stock 3.3.5a does at each step.

#include "core/env_flag.hpp"
#include "core/logger.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace wowee::core::input_trace {

using Clock = std::chrono::steady_clock;

inline bool enabled() {
    static const bool on = envFlagEnabled("WOWEE_INPUT_TRACE", false);
    return on;
}

struct State {
    uint64_t frame = 0;
    Clock::time_point frameStart{};
    Clock::time_point lastInputAt{};
    uint64_t lastInputFrame = 0;
    std::string lastInput;
};

inline State& state() {
    static State s;
    return s;
}

inline double msBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

/// The top of the main loop.
inline void beginFrame() {
    if (!enabled()) return;
    auto& s = state();
    ++s.frame;
    s.frameStart = Clock::now();
}

inline uint64_t frame() { return state().frame; }

/// A press, release or click the pump has just taken out of the queue.
/// `eventAgeMs` is how long ago the window system stamped it.
inline void noteInput(const std::string& what, double eventAgeMs) {
    if (!enabled()) return;
    auto& s = state();
    const auto now = Clock::now();
    s.lastInputAt = now - std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double, std::milli>(eventAgeMs));
    s.lastInputFrame = s.frame;
    s.lastInput = what;
    LOG_WARNING("[input] ", what, " frame ", s.frame, " (+", msBetween(s.frameStart, now),
                " ms in frame), event +", eventAgeMs, " ms ago");
}

/// Something that followed the last input: a packet sent or dispatched, an
/// interface event fired.
inline void note(const std::string& what) {
    if (!enabled()) return;
    auto& s = state();
    const auto now = Clock::now();
    if (s.lastInput.empty()) {
        LOG_WARNING("[input] ", what, " frame ", s.frame, " (", msBetween(s.frameStart, now),
                    " ms into frame), no input yet");
        return;
    }
    LOG_WARNING("[input] ", what, " +", msBetween(s.lastInputAt, now), " ms after ", s.lastInput,
                ", +", s.frame - s.lastInputFrame, " frames, ", msBetween(s.frameStart, now),
                " ms into frame");
}

/// The opcodes worth a line: casting, targeting, melee, movement starts and
/// stops, and the replies that drive the cast bar, cooldowns and swings.
/// Wire values for 3.3.5a.
inline const char* tracedOpcodeName(uint16_t op, bool outbound) {
    if (outbound) {
        switch (op) {
            case 0x12E: return "CMSG_CAST_SPELL";
            case 0x12F: return "CMSG_CANCEL_CAST";
            case 0x13D: return "CMSG_SET_SELECTION";
            case 0x141: return "CMSG_ATTACKSWING";
            case 0x142: return "CMSG_ATTACKSTOP";
            case 0x0AB: return "CMSG_USE_ITEM";
            case 0x26D: return "CMSG_CANCEL_AUTO_REPEAT_SPELL";
            case 0x0B5: return "MSG_MOVE_START_FORWARD";
            case 0x0B6: return "MSG_MOVE_START_BACKWARD";
            case 0x0B7: return "MSG_MOVE_STOP";
            case 0x0B8: return "MSG_MOVE_START_STRAFE_LEFT";
            case 0x0B9: return "MSG_MOVE_START_STRAFE_RIGHT";
            case 0x0BA: return "MSG_MOVE_STOP_STRAFE";
            case 0x0BB: return "MSG_MOVE_JUMP";
            case 0x0BC: return "MSG_MOVE_START_TURN_LEFT";
            case 0x0BD: return "MSG_MOVE_START_TURN_RIGHT";
            case 0x0BE: return "MSG_MOVE_STOP_TURN";
            case 0x0DA: return "MSG_MOVE_SET_FACING";
            case 0x0EE: return "MSG_MOVE_HEARTBEAT";
            case 0x0C9: return "MSG_MOVE_FALL_LAND";
            case 0x0F0: return "CMSG_MOVE_KNOCK_BACK_ACK";
            case 0x2C9: return "CMSG_MOVE_SPLINE_DONE";
            default: return nullptr;
        }
    }
    switch (op) {
        case 0x130: return "SMSG_CAST_FAILED";
        case 0x131: return "SMSG_SPELL_START";
        case 0x132: return "SMSG_SPELL_GO";
        case 0x133: return "SMSG_SPELL_FAILURE";
        case 0x134: return "SMSG_SPELL_COOLDOWN";
        case 0x143: return "SMSG_ATTACKSTART";
        case 0x144: return "SMSG_ATTACKSTOP";
        case 0x14A: return "SMSG_ATTACKERSTATEUPDATE";
        case 0x29C: return "SMSG_CANCEL_AUTO_REPEAT";
        case 0x0DD: return "SMSG_MONSTER_MOVE";
        case 0x0EF: return "SMSG_MOVE_KNOCK_BACK";
        default: return nullptr;
    }
}

/// The interface events that are the client's visible answer to a press.
inline bool tracedEvent(const std::string& name) {
    return name == "UNIT_SPELLCAST_SENT" || name == "UNIT_SPELLCAST_START" ||
           name == "UNIT_SPELLCAST_SUCCEEDED" || name == "UNIT_SPELLCAST_FAILED" ||
           name == "ACTIONBAR_UPDATE_COOLDOWN" || name == "SPELL_UPDATE_COOLDOWN" ||
           name == "PLAYER_TARGET_CHANGED" || name == "UI_ERROR_MESSAGE" ||
           name == "START_AUTOREPEAT_SPELL" || name == "STOP_AUTOREPEAT_SPELL" ||
           name == "PLAYER_ENTER_COMBAT" || name == "ACTIONBAR_UPDATE_STATE";
}

} // namespace wowee::core::input_trace
