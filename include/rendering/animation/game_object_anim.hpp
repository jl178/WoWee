#pragma once

// A game object's animation, as the stock 3.3.5a client (build 12340) plays
// it: the handler every ordinary game object type gets (GameObject_C.cpp,
// vtable 0xa33b80) keeps one "kind" - a state pose, a motion between two
// states, a custom animation - and picks the model's sequence for it.
// Reverse engineered in docs/re/interactions.md, section 12; pure functions,
// unit tested in tests/test_game_object_anim.cpp.
//
//   kind   AnimationData (table 0xada938)
//   0      Spawn 145
//   1      Closed 147       a state loop (READY)
//   2      Open 148         motion READY -> ACTIVE
//   3      Opened 149       a state loop (ACTIVE)
//   4      Close 146        motion ACTIVE -> READY
//   5      Destroy 150      motion -> ACTIVE_ALTERNATIVE
//   6      Destroyed 151    a state loop (ACTIVE_ALTERNATIVE)
//   7      Rebuild 152      motion ACTIVE_ALTERNATIVE -> READY
//   8..11  Custom0..3 153-156 (SMSG_GAMEOBJECT_CUSTOM_ANIM n < 4, 0x70be90)
//   12     Despawn 157 (SMSG_GAMEOBJECT_DESPAWN_ANIM)

#include "rendering/animation/anim_selection.hpp"

#include <cstdint>
#include <optional>

namespace wowee::rendering::anim::goanim {

enum class Kind : uint8_t {
    Spawn = 0,
    Closed = 1,
    Open = 2,
    Opened = 3,
    Close = 4,
    Destroy = 5,
    Destroyed = 6,
    Rebuild = 7,
    Custom0 = 8,
    Custom1 = 9,
    Custom2 = 10,
    Custom3 = 11,
    Despawn = 12,
};

/// GOState, byte 0 of GAMEOBJECT_BYTES_1.
constexpr uint8_t kStateActive = 0;
constexpr uint8_t kStateReady = 1;
constexpr uint8_t kStateActiveAlternative = 2;

/// GAMEOBJECT_FLAGS 0x80 (GO_FLAG_FREEZE_ANIMATION): a motion stops where it
/// is (0x70d160, 0x70d1e0 set the model's pause, model+0x64).
constexpr uint32_t kFlagFreezeAnimation = 0x80;

/// The AnimationData id of a kind (0xada938).
constexpr uint32_t kindAnimation(Kind k) {
    constexpr uint32_t table[] = {SPAWN, CLOSED, OPEN, OPENED, CLOSE, DESTROY, DESTROYED,
                                  REBUILD, CUSTOM_0, CUSTOM_1, CUSTOM_2, CUSTOM_3, DESPAWN};
    return table[static_cast<uint8_t>(k)];
}

/// A state loop: played on and on, never ends into another kind (1, 3, 6).
constexpr bool isStateLoop(Kind k) {
    return k == Kind::Closed || k == Kind::Opened || k == Kind::Destroyed;
}

/// A motion between two states (2, 4, 5, 7): the descriptor's progress and
/// the freeze flag apply to these only (0x70d1e0).
constexpr bool isMotion(Kind k) {
    return k == Kind::Open || k == Kind::Close || k == Kind::Destroy || k == Kind::Rebuild;
}

/// The kind a state shows when nothing is moving it: on first sight
/// (0x70d600), and after a Spawn or a custom animation, which re-apply the
/// state to itself (0x70d7e0 -> 0x70d690(state, state)). `progress` is the
/// descriptor's high half of GAMEOBJECT_DYNAMIC; 0xFFFF (AzerothCore's for
/// everything but transports) means no motion is under way.
constexpr Kind kindForState(uint8_t state, uint16_t progress = 0xFFFF) {
    const bool still = progress == 0xFFFF;
    switch (state) {
        case kStateActive: return still ? Kind::Opened : Kind::Open;
        case kStateActiveAlternative: return still ? Kind::Destroyed : Kind::Destroy;
        default: return still ? Kind::Closed : Kind::Close;
    }
}

/// The kind a state change starts (0x70d690, old -> new).
constexpr Kind kindForStateChange(uint8_t oldState, uint8_t newState, uint16_t progress = 0xFFFF) {
    const bool still = progress == 0xFFFF;
    switch (newState) {
        case kStateActive:
            return (oldState != kStateReady && still) ? Kind::Opened : Kind::Open;
        case kStateActiveAlternative:
            return (oldState != kStateReady && still) ? Kind::Destroyed : Kind::Destroy;
        default: {
            const uint8_t from = still ? oldState : kStateActive;
            if (from == kStateActive) return Kind::Close;
            if (from == kStateActiveAlternative) return Kind::Rebuild;
            return Kind::Closed;
        }
    }
}

/// What follows a kind when its sequence ends (0x70d7e0): a motion its
/// state loop; a loop itself again. nullopt for Spawn and the customs: they
/// go back to the state's own kind (kindForState). Despawn: nothing.
constexpr std::optional<Kind> kindAfterEnd(Kind k) {
    switch (k) {
        case Kind::Open: return Kind::Opened;
        case Kind::Close:
        case Kind::Rebuild: return Kind::Closed;
        case Kind::Destroy: return Kind::Destroyed;
        case Kind::Closed:
        case Kind::Opened:
        case Kind::Destroyed:
        case Kind::Despawn: return k;
        default: return std::nullopt;
    }
}

/// Two motions that undo each other (0x70d510): one starting while the other
/// runs starts from where the other has got to, mirrored.
constexpr bool isReversal(Kind from, Kind to) {
    return (from == Kind::Open && to == Kind::Close) || (from == Kind::Close && to == Kind::Open) ||
           (from == Kind::Destroy && to == Kind::Rebuild) || (from == Kind::Rebuild && to == Kind::Destroy);
}

/// The sequence a kind plays on a model (0x70d1e0). The state poses have
/// their own substitutes before the AnimationData chain is walked (the
/// chain would hold the wrong end of a motion for them):
///  - Close missing and Open missing too: Closed;
///  - Closed missing: Close (the chain holds its last frame) if the model
///    has one, else Open stopped on its first frame, else Stand;
///  - Open missing and Close missing: Destroy if the model has it, else
///    Opened;
///  - Opened missing: Open (held on its last frame by the chain) if the
///    model has one, else Close stopped on its first frame, else Destroyed.
/// Then SetBoneSequence's AnimationData walk (resolveFallback). `has(id)`
/// says whether the model has a sequence of that id; `firstId` is the id
/// of its first sequence. `requested`, when given, receives the id asked of
/// SetBoneSequence before its AnimationData walk: what 0x70d1e0 compares with
/// the sequence already playing.
template <class Has>
FallbackResult resolveKind(Has&& has, Kind kind, uint32_t firstId, uint32_t* requested = nullptr) {
    uint32_t id = kindAnimation(kind);
    if (requested) *requested = id;
    if (!has(id)) {
        switch (id) {
            case CLOSE:
                if (!has(OPEN)) id = CLOSED;
                break;
            case CLOSED:
                if (!has(CLOSE)) {
                    if (has(OPEN)) {
                        if (requested) *requested = OPEN;
                        return {OPEN, PlayMode::HoldFirst, true};
                    }
                    id = STAND;
                }
                break;
            case OPEN:
                if (!has(CLOSE)) id = has(DESTROY) ? DESTROY : OPENED;
                break;
            case OPENED:
                if (!has(OPEN)) {
                    if (has(CLOSE)) {
                        if (requested) *requested = CLOSE;
                        return {CLOSE, PlayMode::HoldFirst, true};
                    }
                    id = DESTROYED;
                }
                break;
            default:
                break;
        }
    }
    if (requested) *requested = id;
    return resolveFallback(has, id, firstId);
}

}  // namespace wowee::rendering::anim::goanim
