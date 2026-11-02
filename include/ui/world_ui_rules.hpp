#pragma once

// The stock 3.3.5a (build 12340) rules for what the engine itself draws in
// the world: nameplates, names over heads, the damage numbers over a target
// and chat bubbles. docs/re/world-ui.md has the addresses each one is read
// from. Everything here is a plain function of its inputs so the rules can be
// tested without a game running.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wowee {
namespace ui {
namespace stockui {

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

/// The engine's frame coordinates ("DDC") make the screen diagonal 1.0
/// (0x47bf90: height 1/sqrt(a^2+1), width a/sqrt(a^2+1)). A size the engine
/// gives in DDC is therefore that fraction of the diagonal, in pixels.
inline float ddcPixels(float ddc, float screenW, float screenH) {
    return ddc * std::sqrt(screenW * screenW + screenH * screenH);
}

/// The 768-tall interface units FrameXML lays out in, in pixels, for a frame
/// with no scale of its own (a WorldFrame child).
inline float uiUnitPixels(float units, float screenH) { return units * screenH / 768.0f; }

struct Rgb {
    uint8_t r = 255, g = 255, b = 255;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

// ---------------------------------------------------------------------------
// Nameplates (CGNamePlateFrame, NamePlateFrame.cpp, 0x98e000..0x990000)
// ---------------------------------------------------------------------------

/// The plate is 0.1 x 0.025 DDC (0xb2da74 / 0xb2da70). The layout below is
/// in the 128 x 32 units of Nameplate-Border.blp, which is that plate at
/// 1280 units to the DDC; multiply by plateUnitPixels() for pixels.
inline float plateUnitPixels(float screenW, float screenH) {
    return ddcPixels(1.0f / 1280.0f, screenW, screenH);
}

/// Range of a plate, from the active player (not the camera): 41 yd
/// (0x72b060, dist^2 < 1681 at 0xadaa7c).
constexpr float kPlateRange = 41.0f;
/// The plate's TOP hangs from the unit's name position plus 2/3 yd
/// (0x715720, 0x9fc830), the point the overlap mode anchors to (0x725890).
constexpr float kPlateLiftYards = 2.0f / 3.0f;

/// Health bar colour (0x98ee30). `reaction` is UnitReaction's 1..8 (1 hated,
/// 2 hostile, 3 unfriendly, 4 neutral, 5 friendly ...). Hated and hostile are
/// red; a player otherwise blue; an NPC green from friendly up and yellow for
/// neutral and unfriendly. With ShowClassColorInNameplate, a hostile player
/// takes its class colour instead. There is no grey for a tapped unit.
enum class PlateBar { Red, Yellow, Green, Blue, ClassColour };
inline PlateBar nameplateBar(int reaction, bool isPlayer, bool classColourCVar) {
    if (reaction <= 2) {
        return (isPlayer && classColourCVar) ? PlateBar::ClassColour : PlateBar::Red;
    }
    if (isPlayer) return PlateBar::Blue;
    return reaction >= 5 ? PlateBar::Green : PlateBar::Yellow;
}
inline Rgb nameplateBarRgb(PlateBar bar) {
    switch (bar) {
        case PlateBar::Red:    return {255, 0, 0};     // 0xb2da18
        case PlateBar::Yellow: return {255, 255, 0};   // 0xb2da14
        case PlateBar::Green:  return {0, 255, 0};     // 0xb2da10
        case PlateBar::Blue:   return {0, 0, 255};     // 0xb2da0c
        default:               return {255, 255, 255};
    }
}

/// Level text colour (0x98e5f0): its own table, not GetQuestDifficultyColor.
/// Grey once the unit is further below than the green range for the player's
/// level (0xaa34b8, indexed by level / 5).
inline Rgb nameplateLevelRgb(int level, int playerLevel) {
    static constexpr int kGreenRange[20] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                            8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    const int d = level - playerLevel;
    if (d > 4) return {255, 25, 25};
    if (d > 2) return {255, 127, 63};
    if (d > -3) return {255, 255, 0};
    const int band = playerLevel / 5;
    const int range = (band >= 0 && band < 20) ? kGreenRange[band] : 8;
    return (-d > range) ? Rgb{127, 127, 127} : Rgb{63, 178, 63};
}

/// The threat glow (UI-TargetingFrame-Flash) shows for a threat status of 1
/// and up, in GetThreatStatusColor's colours (0xad2d70 + 4 * (status + 1)).
inline bool nameplateThreatShown(int status) { return status >= 1 && status <= 3; }
inline Rgb nameplateThreatRgb(int status) {
    switch (status) {
        case 1:  return {255, 255, 119};
        case 2:  return {255, 153, 0};
        case 3:  return {255, 0, 0};
        default: return {176, 176, 176};
    }
}

/// The name on a plate turns red for 5 s when the unit takes damage
/// (0x98e5b0, from the damage log handlers) and yellow while the plate is the
/// highlighted one (0x98e910); white otherwise.
constexpr float kPlateNameDamageSeconds = 5.0f;
inline Rgb nameplateNameRgb(bool damagedRecently, bool highlighted) {
    if (damagedRecently) return {255, 0, 0};
    if (highlighted) return {255, 255, 0};
    return {255, 255, 255};
}

/// Alpha of a plate (0x98e9f0): full for the target, or for every plate when
/// there is no target; half (0x7f) for the rest.
inline uint8_t nameplateAlpha(bool hasTarget, bool isTarget) {
    return (!hasTarget || isTarget) ? 255 : 127;
}

// ---------------------------------------------------------------------------
// Names over heads (PlayerName.cpp, 0x7e4000..0x7e7000)
// ---------------------------------------------------------------------------

/// The UnitName* CVar bits (0x7e6150) and their defaults.
enum UnitNameBit : uint32_t {
    kNameOwn                 = 0x0001,  // UnitNameOwn 0
    kNameNpc                 = 0x0002,  // UnitNameNPC 0
    kNamePlayerGuild         = 0x0004,  // UnitNamePlayerGuild 1
    kNamePlayerPvpTitle      = 0x0008,  // UnitNamePlayerPVPTitle 1
    kNameEnemyPlayer         = 0x0010,  // UnitNameEnemyPlayerName 1
    kNameEnemyPet            = 0x0020,  // UnitNameEnemyPetName 1
    kNameEnemyTotem          = 0x0040,  // UnitNameEnemyTotemName 0
    kNameFriendlyPlayer      = 0x0080,  // UnitNameFriendlyPlayerName 1
    kNameFriendlyPet         = 0x0100,  // UnitNameFriendlyPetName 1
    kNameFriendlyTotem       = 0x0200,  // UnitNameFriendlyTotemName 0
    kNameNonCombatCreature   = 0x0400,  // UnitNameNonCombatCreatureName 0
    kNameEnemyGuardian       = 0x1000,  // UnitNameEnemyGuardianName 0
    kNameFriendlyGuardian    = 0x2000,  // UnitNameFriendlyGuardianName 0
};

/// What a unit is, as far as the name rule (0x729c70) cares.
enum class NameKind {
    Self,
    Npc,                // no player owner
    NonCombatCreature,  // creature type critter (8) or non-combat pet (12)
    PlayerTotem,        // type 11 with a player owner
    PlayerGuardian,     // a player's summon that is not its pet
    PlayerPet,
    Player,
};

/// Whether a unit with no plate shows its name (0x729c70). The target always
/// does; a unit with a plate never does (the plate carries it). Hostile
/// stealthed units show nothing. Note the stock quirk: a guardian answers to
/// the enemy-guardian bit whichever side it is on.
inline bool unitNameShown(NameKind kind, bool isTarget, bool friendly, bool stealthedHostile,
                          uint32_t mask) {
    if (kind == NameKind::Self) return (mask & kNameOwn) != 0;
    if (isTarget) return true;
    if (stealthedHostile) return false;
    switch (kind) {
        case NameKind::Npc:               return (mask & kNameNpc) != 0;
        case NameKind::NonCombatCreature: return (mask & kNameNonCombatCreature) != 0;
        case NameKind::PlayerTotem:
            return (mask & (friendly ? kNameFriendlyTotem : kNameEnemyTotem)) != 0;
        case NameKind::PlayerGuardian:    return (mask & kNameEnemyGuardian) != 0;
        case NameKind::PlayerPet:
            return (mask & (friendly ? kNameFriendlyPet : kNameEnemyPet)) != 0;
        case NameKind::Player:
            return (mask & (friendly ? kNameFriendlyPlayer : kNameEnemyPlayer)) != 0;
        default: return false;
    }
}

/// One line of a name, in yards: world text 0.2 yd tall, scaled up for a
/// model over 4 yd tall (0x7e5420: height * 0.25 * 1.5).
inline float unitNameLineYards(float modelHeight) {
    return 0.2f * (modelHeight > 4.0f ? modelHeight * 0.375f : 1.0f);
}

/// The raid mark over a head is a 1 yd square (0xaf46ac..) drawn without the
/// depth test, faded by the camera's distance (0x7e52a0): 34/255 within 4 yd,
/// rising 55.25 a yard to full at 8 yd.
constexpr float kHeadMarkYards = 1.0f;
inline uint8_t headMarkAlpha(float cameraDistance) {
    if (cameraDistance > 8.0f) return 255;
    if (cameraDistance > 4.0f)
        return static_cast<uint8_t>(std::lround((cameraDistance - 4.0f) * 55.25f + 34.0f));
    return 34;
}

/// Name colour (0x521bf0 with its "extended" argument, also the selection
/// circle's). `reaction` is 1..8.
inline Rgb reactionNameRgb(int reaction) {
    if (reaction <= 2) return {255, 0, 0};
    if (reaction == 3) return {255, 128, 0};
    if (reaction == 4) return {255, 255, 0};
    return {0, 255, 0};
}

// ---------------------------------------------------------------------------
// World text: the numbers over a unit (WorldText.cpp, table at 0xaf4748)
// ---------------------------------------------------------------------------

enum WorldTextType : int {
    kWtDamage = 0, kWtAbsorb = 1, kWtCritDamage = 2, kWtMiss = 3,
    kWtHeal = 6, kWtCritHeal = 7, kWtThreat = 10,
};

struct WorldTextTiming {
    float riseYards;
    int fadeInMs, fadeOutStartMs, durationMs;
    float heightStart, heightEnd;  // DDC
    bool critCurve;
};

inline WorldTextTiming worldTextTiming(int type) {
    constexpr float h = 0.018333f;
    switch (type) {
        case kWtAbsorb:     return {2.0f, 150, 90, 1500, h, h, false};
        case kWtCritDamage:
        case kWtCritHeal:   return {0.0f, 150, 1000, 1500, 0.0275f, 0.0275f, true};
        case kWtMiss:       return {2.0f, 150, 1000, 1500, h, h, false};
        default:            return {2.0f, 150, 760, 1500, h, h, false};  // 0, 6, 10
    }
}

/// Text height in DDC at `ms` since the text was made. A crit pops from a
/// tenth to twice its size over the first 150 ms and settles by 300 ms
/// (the curve at 0xa41618).
inline float worldTextHeight(int type, float ms) {
    const WorldTextTiming t = worldTextTiming(type);
    const float f = std::clamp(ms / static_cast<float>(t.durationMs), 0.0f, 1.0f);
    if (t.critCurve) {
        float k = 1.0f;
        if (f < 0.1f) k = 0.1f + (2.0f - 0.1f) * (f / 0.1f);
        else if (f < 0.2f) k = 2.0f - (f - 0.1f) / 0.1f;
        return t.heightEnd * k;
    }
    return std::max(0.001f, t.heightStart + (t.heightEnd - t.heightStart) * f);
}

/// Text alpha 0..1 (0x7e6850); the shadow is half of it.
inline float worldTextAlpha(int type, float ms) {
    const WorldTextTiming t = worldTextTiming(type);
    if (ms < static_cast<float>(t.fadeInMs)) return std::max(0.0f, ms / t.fadeInMs);
    if (ms >= static_cast<float>(t.fadeOutStartMs)) {
        const float span = static_cast<float>(t.durationMs - t.fadeOutStartMs);
        return std::clamp(1.0f - (ms - t.fadeOutStartMs) / span, 0.0f, 1.0f);
    }
    return 1.0f;
}

/// Yards risen at `ms`: linear over the whole life.
inline float worldTextRise(int type, float ms) {
    const WorldTextTiming t = worldTextTiming(type);
    return t.riseYards * std::clamp(ms / static_cast<float>(t.durationMs), 0.0f, 1.0f);
}

/// The numbers start 1/3 yd under the unit's name position (0x7e5c30), and a
/// unit carries at most four at once; a fifth is dropped (0x7e5c30).
constexpr float kWorldTextDropYards = 1.0f / 3.0f;
constexpr int kWorldTextPerUnit = 4;
/// Shadow: black at half the text's alpha, 0.002 DDC right and down.
constexpr float kWorldTextShadowDdc = 0.002f;

// ---------------------------------------------------------------------------
// Chat bubbles (ChatBubbleFrame.cpp, 0x56c000..0x56d200; 0x720010)
// ---------------------------------------------------------------------------

/// Which CVar a chat type's bubble answers to: 1 chatBubbles (say, yell,
/// monster say and yell), 2 chatBubblesParty (party, party leader), 0 none.
inline int chatBubbleGate(uint8_t chatType) {
    switch (chatType) {
        case 0x01: case 0x06: case 0x0C: case 0x0E: return 1;
        case 0x02: case 0x33:                       return 2;
        default:                                    return 0;
    }
}

/// A "break" is a run of white space after a non-space (0x56c1c0); a last
/// word with nothing after it does not count.
inline int chatBubbleBreaks(std::string_view text) {
    int breaks = 0;
    bool inWord = false;
    for (char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (space) {
            if (inWord) ++breaks;
            inWord = false;
        } else {
            inWord = true;
        }
    }
    return breaks;
}

/// How long the bubble stays at full strength, in ms.
inline int chatBubbleLifetimeMs(std::string_view text, bool ownMessage) {
    const int breaks = chatBubbleBreaks(text);
    return ownMessage ? 1500 + 500 * breaks : 2750 + 750 * breaks;
}

constexpr float kChatBubbleFadeMs = 250.0f;
constexpr float kChatBubbleRange = 25.0f;   // from the active player
constexpr float kChatBubbleLiftYards = 0.7f;  // over the name attachment
constexpr float kChatBubbleTextDdc = 0.01f;   // NAMEPLATE_FONT height
constexpr float kChatBubblePadDdc = 0.01f;
constexpr float kChatBubbleMaxTextDdc = 0.2f;
constexpr float kChatBubbleMinTextUnits = 32.0f;  // two edge widths
constexpr float kChatBubbleEdgeUnits = 16.0f;
constexpr float kChatBubbleTailUnits = 16.0f;

}  // namespace stockui
}  // namespace ui
}  // namespace wowee
