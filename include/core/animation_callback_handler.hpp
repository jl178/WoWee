#pragma once

#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace core { class AppearanceComposer; class EntitySpawner; }

namespace core {

/// Handles animation callbacks: death, respawn, swing, hit reaction, spell cast, emote,
/// stun, stealth, health, ghost, stand state, loot, sprint, vehicle, charge.
/// Owns charge rush state (interpolated in update).
class AnimationCallbackHandler {
public:
    AnimationCallbackHandler(EntitySpawner& entitySpawner,
                             rendering::Renderer& renderer,
                             game::GameHandler& gameHandler,
                             AppearanceComposer& appearanceComposer);

    void setupCallbacks();

    /// Called each frame from Application::update() to drive charge interpolation.
    /// Returns true if charge is active (player is externally driven).
    bool updateCharge(float deltaTime);

    // Charge state queries (used by Application::update for externallyDrivenMotion)
    [[nodiscard]] bool isCharging() const { return chargeActive_; }

    // Reset charge state (logout/disconnect)
    void resetChargeState();

    /// A unit's animation reached $CPP (the victim's defence) or $CAH (the
    /// blow): the part of the reaction of the victim it struck, held since
    /// the swing's packet, that belongs to that moment plays now.
    void onMeleeAnimEvent(uint32_t attackerInstanceId, uint32_t eventTag);
    /// Each frame: reactions whose blow never came play out late.
    void updatePendingReactions();

private:
    /// The AnimID of a spell's cast kit (SpellVisual -> CastKit ->
    /// SpellVisualKit.AnimID); 0 when it names none.
    uint32_t kitCastAnim(uint32_t spellId);
    /// The victim's dodge, parry, block or flinch, played now.
    /// `reaction` is a GameHandler::HitReaction.
    void playHitReaction(uint64_t victimGuid, uint8_t reaction);
    /// The rest of the attacker's swing at half speed, after a miss, a dodge
    /// or an evade (Wow.exe 0x756240 at $CAH -> 0x735CC0(0.5)).
    void slowAttackerSwing(uint32_t attackerInstanceId);
    static double nowMs();

    /// A melee blow's reaction waiting for the attacker's $CPP and $CAH, by
    /// the attacker's render instance (one per attacker, as the stock client
    /// keeps one swing per unit).
    struct PendingReaction {
        uint64_t victimGuid = 0;
        uint8_t reaction = 0;
        double deadlineMs = 0.0;
        /// The defence already played at $CPP. The stock client then clears
        /// HITINFO_AFFECTS_VICTIM on the pending blow, so $CAH plays no flinch.
        bool defended = false;
    };
    /// What a pending blow does when it is played out without its events
    /// (the fallback timer, or the same attacker swinging again).
    void flushPendingReaction(const PendingReaction& p);
    std::unordered_map<uint32_t, PendingReaction> pendingReactions_;
    /// $CAH comes 300-530 ms into the stock attack animations (HumanMale
    /// AttackUnarmed 300, Attack1H 400; Wolf 334). A swing whose events never
    /// fire reacts at about that point instead.
    static constexpr double kBlowFallbackMs = 450.0;

    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    game::GameHandler& gameHandler_;
    AppearanceComposer& appearanceComposer_;

    // Charge rush state (moved from Application)
    bool chargeActive_ = false;
    float chargeTimer_ = 0.0f;
    float chargeDuration_ = 0.0f;
    glm::vec3 chargeStartPos_{0.0f};  // Render coordinates
    glm::vec3 chargeEndPos_{0.0f};    // Render coordinates
    uint64_t chargeTargetGuid_ = 0;
};

} // namespace core
} // namespace wowee
