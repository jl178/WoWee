#include <span>

#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/weapon_anim_select.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/anim_capability_probe.hpp"
#include "rendering/animation/mount_fsm.hpp"
#include "rendering/animation/anim_selection.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/inventory.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/sound_data.hpp"
#include "audio/music_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/swim_effects.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <set>
#include <random>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// ── AnimationController implementation ───────────────────────────────────────

AnimationController::AnimationController() = default;
AnimationController::~AnimationController() = default;

void AnimationController::initialize(Renderer* renderer) {
    renderer_ = renderer;
}

void AnimationController::probeCapabilities() {
    if (!renderer_) return;
    uint32_t instanceId = renderer_->getCharacterInstanceId();
    if (instanceId == 0) return;
    auto caps = AnimCapabilityProbe::probe(renderer_, instanceId);
    characterAnimator_.setCapabilities(caps);
    capabilitiesProbed_ = true;
}

void AnimationController::onCharacterFollow(uint32_t /*instanceId*/) {
    // Reset animation state when follow target changes
    capabilitiesProbed_ = false;
}

// ── Emote support ────────────────────────────────────────────────────────────

void AnimationController::playEmote(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    auto result = registry.findEmote(emoteName);
    if (!result) return;

    uint32_t animId = result->animId;
    bool loop = result->loop;

    // What an emote resolved to, and whether the model can play it. An emote
    // that does nothing is either a name that resolved to no animation or an
    // animation the model does not carry, and from outside those look the same.
    {
        auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
        uint32_t ci = renderer_ ? renderer_->getCharacterInstanceId() : 0;
        const bool has = (cr && ci > 0) ? cr->hasAnimation(ci, animId) : false;
        LOG_WARNING("Emote '", emoteName, "' -> animation ", animId,
                    (loop ? " (looping)" : " (one shot)"),
                    " model has it: ", (has ? "yes" : "NO"));
    }

    // On the move a one-shot emote plays on the upper body over the run (an
    // emote is an upper-body animation, 0x71D800, and 0x723E30 splits it off
    // a moving unit's legs): a wave while running. Standing, it takes the body.
    if (!loop && playSwingOnUpperBody(animId)) return;

    // Forward to CharacterAnimator (ActivityFSM handles emote state)
    characterAnimator_.playEmote(animId, loop);

    // Immediately play the emote animation on the renderer
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (characterRenderer && characterInstanceId > 0) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }
}

AnimationController::SheathReachTiming
AnimationController::playWeaponSheathAnimation(SheathSpot mainHand, SheathSpot offHand) {
    SheathReachTiming timing;
    if (!renderer_) return timing;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    const uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return timing;

    // One animation both ways: 3.3.5 has Sheath and HipSheath and no unsheathe,
    // and plays the reach for drawing as well as for putting away. Each arm
    // reaches for its own hand's item and an empty hand follows the body.
    const SheathSpot body = mainHand != SheathSpot::NONE ? mainHand : offHand;
    if (body == SheathSpot::NONE) return timing;
    uint32_t animId = anim::sheathReachAnim(body);
    if (!characterRenderer->hasAnimation(characterInstanceId, animId)) {
        animId = body == SheathSpot::HIP ? anim::SHEATHE : anim::HIP_SHEATHE;
        if (!characterRenderer->hasAnimation(characterInstanceId, animId)) return timing;
    }

    // On the upper body, over whatever the legs are doing: the stock client
    // sheathes on the run without stopping. Played as a full-body emote it
    // held the character still, and locomotion cancelled it the moment the
    // player moved - which is how it came to show no reach at all.
    if (!characterRenderer->playUpperBodyAnimation(characterInstanceId, animId,
                                                   anim::sheathReachAnim(offHand),
                                                   anim::sheathReachAnim(mainHand))) {
        return timing;
    }
    timing.durationMs = characterRenderer->getAnimationDurationMs(characterInstanceId, animId);
    timing.swapMs = characterRenderer->getSheathEventTime(characterInstanceId, animId);
    if (timing.swapMs == UINT32_MAX || timing.swapMs > timing.durationMs)
        timing.swapMs = timing.durationMs / 2;
    return timing;
}

void AnimationController::cancelEmote() {
    characterAnimator_.cancelEmote();
}

std::string AnimationController::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textFor(emoteName, targetName);
}

uint32_t AnimationController::getEmoteDbcId(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.dbcIdFor(emoteName);
}

std::string AnimationController::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                                      const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textByDbcId(dbcId, senderName, targetName);
}
uint32_t AnimationController::getEmoteAnimByEmotesId(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.animByEmotesId(emoteId);
}

bool AnimationController::isStateEmoteById(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.isStateEmote(emoteId);
}

// ── Spell casting ────────────────────────────────────────────────────────────

void AnimationController::startSpellCast(uint32_t precastAnimId, uint32_t castAnimId, bool castLoop,
                                         uint32_t finalizeAnimId) {
    characterAnimator_.startSpellCast(precastAnimId, castAnimId, castLoop, finalizeAnimId);
}

void AnimationController::stopSpellCast() {
    characterAnimator_.stopSpellCast();
}

void AnimationController::setSeatedLoopAnimation(uint32_t animationId) {
    characterAnimator_.getActivity().setSeatedLoopAnimation(animationId);
}

// ── Loot animation ───────────────────────────────────────────────────────────

void AnimationController::startLooting() {
    characterAnimator_.startLooting();
}

void AnimationController::stopLooting() {
    characterAnimator_.stopLooting();
}

// ── Hit reactions ────────────────────────────────────────────────────────────

void AnimationController::triggerHitReaction(uint32_t animId) {
    if (animId != 0) {
        // On the move the flinch, parry or dodge goes on the upper body over
        // the run, as the stock client plays it (Wow.exe 0x736640 picks the
        // upper-body track unless the unit stands idle or in its stance).
        // The combat state machine dropped it altogether while moving.
        if (playSwingOnUpperBody(animId)) return;
        // Not over a swing of our own: the swing state would take the body
        // straight back and start the swing again from its first frame.
        if (meleeSwingTimer_ > 0.0f) return;
    }
    characterAnimator_.triggerHitReaction(animId);
}

// ── Crowd control ────────────────────────────────────────────────────────────

void AnimationController::setStunned(bool stunned) {
    stunned_ = stunned;
    characterAnimator_.setStunned(stunned);
}

// ── Health-based idle ────────────────────────────────────────────────────────

void AnimationController::setLowHealth(bool low) {
    characterAnimator_.setLowHealth(low);
}

// ── Stand state ──────────────────────────────────────────────────────────────

void AnimationController::setStandState(uint8_t state) {
    characterAnimator_.setStandState(state);
}

// ── Stealth ──────────────────────────────────────────────────────────────────

void AnimationController::setStealthed(bool stealth) {
    characterAnimator_.setStealthed(stealth);
}

// ── Sprint aura ──────────────────────────────────────────────────────────────

void AnimationController::setSprintAuraActive(bool active) {
    // Straight through. The animator keeps the copy the locomotion state
    // machine reads; this class kept one of its own and read it nowhere.
    characterAnimator_.setSprintAuraActive(active);
}

// ── Targeting / combat ───────────────────────────────────────────────────────

void AnimationController::setTargetPosition(const glm::vec3* pos) {
    targetPosition_ = pos;
}

void AnimationController::setInCombat(bool combat) {
    inCombat_ = combat;
}

void AnimationController::setAttacking(bool attacking) {
    characterAnimator_.setAttacking(attacking);
}

void AnimationController::setAutoRepeatShot(bool on) {
    characterAnimator_.setAutoRepeatShot(on);
}

void AnimationController::resetCombatVisualState() {
    inCombat_ = false;
    targetPosition_ = nullptr;
    meleeSwingTimer_ = 0.0f;
    meleeSwingCooldown_ = 0.0f;
    specialAttackAnimId_ = 0;
    rangedShootTimer_ = 0.0f;
    rangedAnimId_ = 0;
    restoreWeaponAfterRangedShot_ = false;
    characterAnimator_.setRangedWeaponActive(false);
    stunned_ = false;
    charging_ = false;

    // Reset all CharacterAnimator combat state
    characterAnimator_.setAttacking(false);
    characterAnimator_.setAutoRepeatShot(false);
    characterAnimator_.setStunned(false);
    characterAnimator_.setCharging(false);
    characterAnimator_.setLowHealth(false);
    characterAnimator_.stopSpellCast();
    characterAnimator_.triggerHitReaction(0);  // Clear hit reaction

    if (auto* svs = renderer_->getSpellVisualSystem()) svs->reset();
}

bool AnimationController::isMoving() const {
    auto* cameraController = renderer_->getCameraController();
    return cameraController && cameraController->isMoving();
}

// ── Melee combat ─────────────────────────────────────────────────────────────

void AnimationController::triggerMeleeSwing(bool offHand) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown_ > 0.0f) return;
    if (characterAnimator_.getActivity().isEmoteActive()) {
        characterAnimator_.cancelEmote();
    }
    specialAttackAnimId_ = 0;  // Clear any special attack override
    // The hand the server says swung (HITINFO_OFFHAND), not a toggle: the
    // two hands' swing timers differ, so blind alternation drifts off them.
    meleeOffHandTurn_ = offHand && weaponLoadout_.hasOffHand;
    const uint32_t swingAnim = resolveMeleeAnimId();
    if (swingAnim != 0 && playSwingOnUpperBody(swingAnim)) {
        // The whole-body swing state is not entered: the legs stay with the
        // locomotion, and stopping mid-swing does not start it over.
        meleeSwingTimer_ = 0.0f;
        specialAttackAnimId_ = 0;
        meleeSwingCooldown_ = 0.1f;
        return;
    }
    // CombatFSM alternates hands on its own; a dual wielder's swing is
    // handed to it explicitly so it plays the hand named here.
    if (weaponLoadout_.hasOffHand && swingAnim != 0) specialAttackAnimId_ = swingAnim;
    meleeSwingCooldown_ = 0.1f;
    float durationSec = meleeAnimDurationMs_ > 0.0f ? meleeAnimDurationMs_ / 1000.0f : 0.6f;
    if (durationSec < 0.25f) durationSec = 0.25f;
    if (durationSec > 1.0f) durationSec = 1.0f;
    meleeSwingTimer_ = durationSec;

    // No sound here: the swing's swoosh and blow come from the combat
    // sounds at the animation's $CSS and $CAH, as for every other unit.
    // A fixed whoosh as the swing started doubled the swoosh, and sounded
    // for misses the stock client gives only "Combat Miss".
}

void AnimationController::setEquippedWeaponType(uint32_t inventoryType, bool is2HLoose,
                                                 bool isFist, bool isDagger,
                                                 bool hasOffHand, bool hasShield,
                                                 bool offHandIsFist,
                                                 bool offHandIsDagger) {
    weaponLoadout_.inventoryType = inventoryType;
    weaponLoadout_.is2HLoose = is2HLoose;
    weaponLoadout_.isFist = isFist;
    weaponLoadout_.isDagger = isDagger;
    weaponLoadout_.hasOffHand = hasOffHand;
    weaponLoadout_.offHandIsFist = offHandIsFist;
    weaponLoadout_.offHandIsDagger = offHandIsDagger;
    weaponLoadout_.hasShield = hasShield;
    meleeAnimId_ = 0;  // Force re-resolve on next swing
    characterAnimator_.setEquippedWeaponType(weaponLoadout_);
}

void AnimationController::triggerAbilityAnimation(uint32_t kitAnimId) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    // A kit with no animation of its own: the normal next swing.
    if (kitAnimId == 0) {
        triggerMeleeSwing(false);
        return;
    }
    // An ability is its own swing: it replaces an auto-attack swing that
    // started a moment ago rather than being dropped by the swing cooldown.
    if (characterAnimator_.getActivity().isEmoteActive()) {
        characterAnimator_.cancelEmote();
    }

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // The kit's animation, with the weapon-generic attacks swapped for the
    // held weapon's. This used to ignore the spell altogether and pick by
    // loadout - a shield made every ability Shield Bash, which is how
    // Heroic Strike (kit AnimID 57, Special1H) played ShieldBash (59).
    const uint32_t specAnim = anim::firstPlayable(
        has, anim::abilityAnimCandidates(kitAnimId, weaponLoadout_));

    if (specAnim == 0) {
        triggerMeleeSwing(false);
        return;
    }
    if (playSwingOnUpperBody(specAnim)) {
        meleeSwingTimer_ = 0.0f;
        specialAttackAnimId_ = 0;
        meleeSwingCooldown_ = 0.1f;
        return;
    }

    specialAttackAnimId_ = specAnim;
    meleeSwingCooldown_ = 0.1f;
    // Query the special attack animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == specAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    // A roar or a stomp runs its full length; a swing is capped as before.
    if (dur > (anim::isMeleeAttackAnim(specAnim) ? 1.0f : 3.0f))
        dur = anim::isMeleeAttackAnim(specAnim) ? 1.0f : 3.0f;
    meleeSwingTimer_ = dur;
}

void AnimationController::slowMeleeSwing(float speed) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    const uint32_t instanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || instanceId == 0 || meleeSwingTimer_ <= 0.0f || speed <= 0.0f) return;
    uint32_t cur = 0;
    float t = 0.0f, d = 0.0f;
    if (!characterRenderer->getAnimationState(instanceId, cur, t, d) || d <= t) return;
    characterRenderer->setAnimationSpeed(instanceId, speed);
    // The swing state ends with the animation, which now takes longer; left
    // as it was, the stance would cut the slowed follow-through short.
    meleeSwingTimer_ = std::max(meleeSwingTimer_, (d - t) / 1000.0f / speed);
}

bool AnimationController::playSwingOnUpperBody(uint32_t animId) {
    // The stock client splits an animation between the legs and the upper
    // body when the unit is moving (Wow.exe 0x7385C0 via 0x723E30: movement
    // flags 0x2E000FF - a direction, a turn, swimming, flying) and the
    // animation is an upper-body one (0x71D800). Otherwise it takes the whole
    // body, which on a running character slid it along the ground in its
    // attack pose for the length of the swing.
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    const uint32_t instanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || !cameraController || instanceId == 0) return false;
    const bool legsBusy = cameraController->isMoving() || cameraController->isSwimming() ||
                          !cameraController->isGrounded() || cameraController->isJumping();
    if (!legsBusy || !anim::playsOnUpperBody(animId)) return false;
    return characterRenderer->playUpperBodyAnimation(instanceId, animId, animId, animId);
}

// ── Ranged combat ────────────────────────────────────────────────────────────

void AnimationController::setEquippedRangedType(RangedWeaponType type) {
    weaponLoadout_.rangedType = type;
    rangedAnimId_ = 0;
    characterAnimator_.setEquippedRangedType(type);
}

void AnimationController::setRangedWeaponActive(bool active) {
    characterAnimator_.setRangedWeaponActive(active);
}

void AnimationController::setCharging(bool c) {
    charging_ = c;
    characterAnimator_.setCharging(c);
}

void AnimationController::triggerRangedShot() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (rangedShootTimer_ > 0.0f) return;
    if (characterAnimator_.getActivity().isEmoteActive()) characterAnimator_.cancelEmote();

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Resolve ranged attack animation based on weapon type
    uint32_t shootAnim = 0;
    switch (weaponLoadout_.rangedType) {
        case RangedWeaponType::BOW:
            if (has(anim::FIRE_BOW))        shootAnim = anim::FIRE_BOW;
            else if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::GUN:
            if (has(anim::ATTACK_RIFLE))    shootAnim = anim::ATTACK_RIFLE;
            break;
        case RangedWeaponType::CROSSBOW:
            if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::THROWN:
            if (has(anim::ATTACK_THROWN))    shootAnim = anim::ATTACK_THROWN;
            break;
        case RangedWeaponType::WAND: {
            // WOWEE_WAND_ANIM=<id> overrides the shot animation, for comparing
            // candidates on screen: 32 cast, 49 rifle, 53 directed, 54 omni.
            static const uint32_t kForced = []() -> uint32_t {
                const char* v = std::getenv("WOWEE_WAND_ANIM");
                return v ? static_cast<uint32_t>(std::atoi(v)) : 0;
            }();
            if (kForced != 0 && has(kForced))        shootAnim = kForced;
            else if (has(anim::SPELL_CAST_DIRECTED)) shootAnim = anim::SPELL_CAST_DIRECTED;
            else if (has(anim::ATTACK_RIFLE))        shootAnim = anim::ATTACK_RIFLE;
            break;
        }
        default: break;
    }
    if (shootAnim == 0) return;  // Model has no ranged animation

    rangedAnimId_ = shootAnim;

    // Query animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == shootAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    if (dur > 1.5f) dur = 1.5f;
    rangedShootTimer_ = dur;
    restoreWeaponAfterRangedShot_ =
        (weaponLoadout_.rangedType == RangedWeaponType::THROWN);
}

uint32_t AnimationController::resolveMeleeAnimId() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    // When dual-wielding, bypass the cache: the hand changes swing to swing.
    if (!weaponLoadout_.hasOffHand && meleeAnimId_ != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId_)) {
        return meleeAnimId_;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    auto findDuration = [&](uint32_t id) -> float {
        for (const auto& seq : sequences) {
            if (seq.id == id && seq.duration > 0) {
                return static_cast<float>(seq.duration);
            }
        }
        return 0.0f;
    };

    std::span<const uint32_t> chain;
    // The chains live in melee_anim_chains.hpp, which the capability probe
    // reads too - it used to keep its own copies under a comment saying they
    // matched these.
    // The hand was set by triggerMeleeSwing from the swing's hit flags; the
    // rules that turn hand + weapon into a chain are shared with every other
    // unit (weapon_anim_select.hpp).
    const bool useOffHand = weaponLoadout_.hasOffHand && meleeOffHandTurn_;
    chain = anim::meleeAnimChain(anim::swingChain(weaponLoadout_, useOffHand));
    for (uint32_t id : chain) {
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId_ = id;
            meleeAnimDurationMs_ = findDuration(id);
            return meleeAnimId_;
        }
    }

    const uint32_t avoidIds[] = {anim::STAND, anim::DEATH, anim::WALK, anim::RUN, anim::SHUFFLE_LEFT, anim::SHUFFLE_RIGHT, anim::WALK_BACKWARDS, anim::JUMP_START, anim::JUMP, anim::JUMP_END, anim::SWIM_IDLE, anim::SWIM, anim::SITTING};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId_ = bestId;
    meleeAnimDurationMs_ = static_cast<float>(bestDuration);
    return meleeAnimId_;
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position) {
    auto* levelUpEffect = renderer_->getLevelUpEffect();
    if (!levelUpEffect) return;

    if (!levelUpEffect->isModelLoaded()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (!assetManager) {
                LOG_WARNING("LevelUpEffect: no asset manager available");
            } else {
                auto m2Data = assetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
                auto skinData = assetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
                LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
                if (!m2Data.empty()) {
                    levelUpEffect->loadModel(m2Renderer, m2Data, skinData);
                } else {
                    LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
                }
            }
        }
    }

    levelUpEffect->trigger(position);
}

// ── Mount ────────────────────────────────────────────────────────────────────

void AnimationController::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    (void)modelPath;

    mountInstanceId_ = mountInstId;
    mountDisplayId_ = mountDisplayId;
    mountHeightOffset_ = heightOffset;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    lastMountAnimRequest_ = UINT32_MAX;

    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    // The mount model plays the unit's movement animation (stock 0x7385C0
    // resolves it against the mount, 0x7176F0): what it has of the
    // locomotion set, nothing guessed from durations or ids.
    AnimCapabilitySet caps;
    if (characterRenderer && mountInstId > 0)
        caps = AnimCapabilityProbe::probeLocomotion(renderer_, mountInstId);
    characterAnimator_.configureMountFSM(caps);

    // The rider rides the mount's attachment 0 at its own size, the seat
    // bone's whole transform driving it (0x73D5D0, 0x4E26F0), as other
    // players' riders already do.
    const uint32_t riderInst = renderer_->getCharacterInstanceId();
    if (characterRenderer && riderInst != 0 && mountInstId != 0)
        characterRenderer->setInstanceMountedOn(riderInst, mountInstId);

    LOG_INFO("Mount configured: displayId=", mountDisplayId, " taxiFlight=", taxiFlight_,
             " swim=", caps.resolvedSwim, " sprint=", caps.resolvedSprint,
             " jumpStart=", caps.resolvedJumpStart);
}

void AnimationController::clearMount() {
    LOG_INFO("Mount cleared: was taxiFlight=", taxiFlight_, " mountInstanceId=", mountInstanceId_);
    if (auto* cr = renderer_->getCharacterRenderer()) {
        if (uint32_t riderInst = renderer_->getCharacterInstanceId())
            cr->setInstanceMountedOn(riderInst, 0);
    }
    mountInstanceId_ = 0;
    mountDisplayId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    riderAnim_ = anim::MOUNT;
    lastMountAnimRequest_ = UINT32_MAX;

    characterAnimator_.clearMountFSM();

    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }
}

void AnimationController::playMountCreatureSound(bool jumpEnd) {
    auto* coord = renderer_->getAudioCoordinator();
    if (!coord || mountDisplayId_ == 0) return;
    const auto* data = coord->getSoundData();
    auto* csm = coord->getCombatSoundManager();
    if (!data || !csm) return;
    const auto* cs = data->displaySounds(mountDisplayId_);
    if (!cs) return;
    const uint32_t id = jumpEnd ? cs->jumpEnd : cs->jumpStart;
    if (id != 0) csm->playEntry3D(id, renderer_->getCharacterPosition());
}

void AnimationController::playMountSpecial() {
    characterAnimator_.getMountFSM().requestSpecial();
}

// ── Query helpers ────────────────────────────────────────────────────────────

bool AnimationController::isFootstepAnimationState() const {
    auto state = characterAnimator_.getLocomotion().getState();
    return state == LocomotionFSM::State::WALK || state == LocomotionFSM::State::RUN;
}

// ── Melee timers ─────────────────────────────────────────────────────────────

void AnimationController::updateMeleeTimers(float deltaTime) {
    // Starting to move cuts a whole-body swing short: the stock client picks
    // the run the moment movement starts and it takes the body from the
    // attack (0x73AC30 -> 0x7385C0). Held on, the swing kept the character
    // gliding in its attack pose until the swing timer ran out.
    {
        auto* cc = renderer_->getCameraController();
        const bool moving = cc && cc->isMoving();
        if (moving && !movingLastFrame_ && meleeSwingTimer_ > 0.0f &&
            (specialAttackAnimId_ == 0 || anim::playsOnUpperBody(specialAttackAnimId_))) {
            meleeSwingTimer_ = 0.0f;
            specialAttackAnimId_ = 0;
        }
        movingLastFrame_ = moving;
    }
    if (meleeSwingCooldown_ > 0.0f) {
        meleeSwingCooldown_ = std::max(0.0f, meleeSwingCooldown_ - deltaTime);
    }
    if (meleeSwingTimer_ > 0.0f) {
        meleeSwingTimer_ = std::max(0.0f, meleeSwingTimer_ - deltaTime);
        if (meleeSwingTimer_ <= 0.0f) specialAttackAnimId_ = 0;
    }
    // Ranged shot timer (same pattern as melee)
    if (rangedShootTimer_ > 0.0f) {
        rangedShootTimer_ = std::max(0.0f, rangedShootTimer_ - deltaTime);
        if (rangedShootTimer_ <= 0.0f && restoreWeaponAfterRangedShot_) {
            restoreWeaponAfterRangedShot_ = false;
            if (rangedShotCompleteCallback_) rangedShotCompleteCallback_();
        }
    }
}

// ── Mount positioning ────────────────────────────────────────────────────────

void AnimationController::applyMountPositioning(float characterYaw) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0 || mountInstanceId_ == 0) return;

    // The mount stands where the unit is, turned to its facing; in the air
    // it takes the movement pitch, on a taxi the spline's bank as well. The
    // rider's matrix is the seat's (CharacterRenderer, mountedOn); its
    // position is kept on the seat for whatever asks where the player is.
    const float yawRad = glm::radians(characterYaw);
    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
    characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);
    characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll_, yawRad));

    glm::mat4 seat(1.0f);
    glm::vec3 riderPos = characterPosition + glm::vec3(0.0f, 0.0f, mountHeightOffset_);
    if (characterRenderer->getAttachmentTransform(mountInstanceId_, 0, seat))
        riderPos = glm::vec3(seat[3]);
    characterRenderer->setInstancePosition(characterInstanceId, riderPos);
    characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll_, yawRad));
}

// ── Mounted animation update (MountFSM) ──────────────────────────────────────

void AnimationController::updateMountedAnimation(float deltaTime) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    auto& mountFSM = characterAnimator_.getMountFSM();
    if (!characterRenderer || !cameraController) return;

    MountFSM::Input in;
    in.moving = cameraController->isMoving();
    in.movingBackward = cameraController->isMovingBackward();
    in.strafeLeft = cameraController->isStrafingLeft();
    in.strafeRight = cameraController->isStrafingRight();
    in.grounded = cameraController->isGrounded();
    in.jumping = cameraController->isJumping();
    in.swimming = cameraController->isSwimming();
    // In the air on a flying mount: the swim set. On the ground it runs.
    in.flying = cameraController->isFlightAirborne();
    in.flyingSpline = taxiFlight_;
    in.speed = cameraController->currentMoveSpeed();
    in.walkSpeed = cameraController->currentWalkSpeed();
    in.deltaTime = deltaTime;
    {
        const bool still = !in.moving && in.grounded && !in.swimming && !in.flying && !taxiFlight_;
        const float step = renderer_->getBodyFacingStep();
        constexpr float kStepEps = 1.0e-5f;
        in.turnLeft = still && (cameraController->isTurningLeft() || step > kStepEps);
        in.turnRight = still && !in.turnLeft && (cameraController->isTurningRight() || step < -kStepEps);
    }
    in.haveAnimState = characterRenderer->getAnimationState(
        mountInstanceId_, in.currentAnimId, in.currentAnimTime, in.currentAnimDuration);

    // A mounted unit's creature sounds are the mount display's (0x73D5D0 sets
    // CGUnit +0x978 from the mount's CreatureSoundData; 0x73ED10 plays kind
    // 11, JumpStart, on a jump and kind 12, JumpEnd, on landing, both at
    // 100%, 0xADBA74).
    {
        const bool airborne = !in.grounded && !in.swimming && !in.flying && !taxiFlight_;
        if (in.jumping && !mountJumpSounded_) {
            mountJumpSounded_ = true;
            playMountCreatureSound(/*jumpEnd=*/false);
        }
        if (mountWasAirborne_ && !airborne && mountJumpSounded_) {
            playMountCreatureSound(/*jumpEnd=*/true);
        }
        if (!airborne) mountJumpSounded_ = false;
        mountWasAirborne_ = airborne;
    }

    const MountFSM::Output out = mountFSM.evaluate(in);
    if (out.anim.valid) {
        const bool changed = out.anim.animId != lastMountAnimRequest_;
        // Something else played on the mount (a model swap, a stray call):
        // put the loop back, unless the model lacks it and plays a fallback.
        const bool drifted = in.haveAnimState && out.anim.loop && in.currentAnimId != out.anim.animId &&
                             characterRenderer->hasAnimation(mountInstanceId_, out.anim.animId);
        if (changed || drifted) {
            characterRenderer->playAnimation(mountInstanceId_, out.anim.animId, out.anim.loop, out.returnAnim);
            if (changed)
                LOG_INFO("Mount anim: ", out.anim.animId, " speed=", in.speed, " flying=", in.flying,
                         " swim=", in.swimming, " taxi=", in.flyingSpline);
            lastMountAnimRequest_ = out.anim.animId;
        }
    }

    // A moving unit's animation plays at its speed over the sequence's
    // MoveSpeed (0x7385C0). Ground only here: the speed above is the ground
    // speed the keys select.
    {
        uint32_t cur = 0;
        float t = 0.0f, d = 0.0f;
        const bool ground = !in.swimming && !in.flying && !taxiFlight_;
        if (characterRenderer->getAnimationState(mountInstanceId_, cur, t, d)) {
            // The sequence actually playing: the rolled variation, or the
            // fallback a mount without the requested id plays.
            pipeline::M2Sequence playing{};
            const float moveSpeed =
                characterRenderer->getPlayingSequence(mountInstanceId_, playing) ? playing.movingSpeed : 0.0f;
            float rate = 1.0f;
            if (ground && in.moving && in.speed > 0.0f && moveSpeed != 0.0f)
                rate = in.speed / std::abs(moveSpeed);
            characterRenderer->setAnimationSpeed(mountInstanceId_, rate);
        }
    }

    // The rider sits: Mount (91), or the mount aura's state-kit animation.
    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
    const bool haveState = characterRenderer->getAnimationState(
        characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    if (!haveState || currentAnimId != riderAnim_) {
        characterRenderer->playAnimation(characterInstanceId, riderAnim_, true);
        lastPlayerAnimRequest_ = riderAnim_;
        lastPlayerAnimLoopRequest_ = true;
    }

    applyMountPositioning(renderer_->getCharacterVisualYaw());
}

// ── Character animation state machine (delegates to CharacterAnimator) ──────────

void AnimationController::updateCharacterAnimation() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();

    // Lazy probe: populate capability set once per model.
    // Re-probe if melee capabilities are missing (model may not have been fully
    // loaded on the first probe attempt).
    if (characterRenderer && characterInstanceId != 0) {
        if (!capabilitiesProbed_) {
            probeCapabilities();
        } else if (meleeSwingTimer_ > 0.0f && !characterAnimator_.getCapabilities().hasMelee) {
            capabilitiesProbed_ = false;
            probeCapabilities();
        }
    }

    // In a vehicle's seat the seat plays the character (its ride loop).
    if (seatedInVehicle_ && !isMounted()) {
        lastPlayerAnimRequest_ = UINT32_MAX;
        return;
    }

    // When mounted, delegate to MountFSM and handle positioning
    if (isMounted()) {
        updateMountedAnimation(lastDeltaTime_);
        return;
    }

    // Build FrameInput for CharacterAnimator from camera/renderer state
    CharacterAnimator::FrameInput fi;
    fi.moving = cameraController->isMoving();
    fi.sprinting = cameraController->isSprinting();
    fi.movingForward = cameraController->isMovingForward();
    fi.movingBackward = cameraController->isMovingBackward();
    fi.strafeLeft = cameraController->isStrafingLeft();
    fi.strafeRight = cameraController->isStrafingRight();
    // See setM2TransportRiding() comment: real physics correctly reports "not grounded"
    // over open track under a moving platform, but the animation FSM needs "grounded"
    // to avoid playing a falling animation for the whole ride.
    fi.grounded = m2TransportRiding_ ? true : cameraController->isGrounded();
    fi.jumping = cameraController->isJumping();
    // In the air with flight allowed (".gm fly", a flight form without its
    // model) the character plays the swim set, as a flying mount does: the
    // stock idle 0x71E340 and move 0x724500 pick SwimIdle 41 / Swim 42 and
    // the swim strafes and backstroke for SWIMMING or FLYING (0x2200000).
    // Without it the on-foot path had no flight at all and a flying
    // character hung in the jump's fall pose.
    fi.swimming = cameraController->isSwimming() || cameraController->isFlightAirborne();
    fi.sitting = cameraController->isSitting();
    fi.ascending = cameraController->isAscending();
    fi.descending = cameraController->isDescending();
    fi.jumpKeyPressed = cameraController->isJumpKeyPressed();
    fi.characterYaw = renderer_->getCharacterYaw();
    // The pace comes from the speed the movement flags select (0x717050).
    fi.speed = fi.moving ? cameraController->currentMoveSpeed() : 0.0f;
    fi.walkSpeed = cameraController->currentWalkSpeed();
    // Turning in place, as CGUnit_C's update does it (0x0073E2C0): only with
    // none of the flags 0x2E0100F (moving, falling, swimming, ascending,
    // descending, flying); then the turn keys (0x10/0x20) or the drawn body
    // catching up with the facing (the body-facing step) pick ShuffleLeft or
    // ShuffleRight, so the feet step while the body comes round.
    {
        const bool still = !fi.moving && fi.grounded && !fi.swimming && !fi.ascending &&
                           !fi.descending && !cameraController->isFlyingActive();
        const float step = renderer_->getBodyFacingStep();
        constexpr float kStepEps = 1.0e-5f;  // 0x009EA558 / 0x00A34EEC
        fi.turnLeft = still && (cameraController->isTurningLeft() || step > kStepEps);
        fi.turnRight = still && !fi.turnLeft &&
                       (cameraController->isTurningRight() || step < -kStepEps);
    }
    // Melee/ranged timers
    fi.meleeSwingTimer = meleeSwingTimer_;
    fi.rangedShootTimer = rangedShootTimer_;
    fi.specialAttackAnimId = specialAttackAnimId_;
    fi.rangedAnimId = rangedAnimId_;
    // Animation state query for one-shot completion detection
    if (characterRenderer && characterInstanceId > 0) {
        fi.haveAnimState = characterRenderer->getAnimationState(
            characterInstanceId, fi.currentAnimId, fi.currentAnimTime, fi.currentAnimDuration);
    }

    // Inject FrameInput and resolve animation via CharacterAnimator
    characterAnimator_.setFrameInput(fi);
    characterAnimator_.update(lastDeltaTime_);

    // Read the resolved animation output
    AnimOutput output = characterAnimator_.getLastOutput();

    // STAY policy: if CharacterAnimator returns invalid, keep current animation
    if (!output.valid) return;

    uint32_t animId = output.animId;
    bool loop = output.loop;

    // Apply animation to the character renderer
    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f;
    float currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    const bool requestChanged = (lastPlayerAnimRequest_ != animId) || (lastPlayerAnimLoopRequest_ != loop);
    // Only re-assert looping animations if the renderer drifted (e.g., external
    // playAnimation call).  One-shot animations must NOT be re-asserted after the
    // renderer auto-resets them to STAND on completion - the FSM detects the ID
    // change via oneShotComplete and transitions to the next state in the same frame.
    const bool drifted = haveState && currentAnimId != animId && loop;
    const bool shouldPlay = requestChanged || drifted;

    // Debug: log animation decisions (only when animation changes or replays)
    static uint32_t dbgLastAnim = UINT32_MAX;
    if (shouldPlay || animId != dbgLastAnim) {
        LOG_DEBUG("[AnimDbg] FSM→", animId, " loop=", loop,
                  " cur=", currentAnimId, " t=", currentAnimTimeMs, "/", currentAnimDurationMs,
                  " haveState=", haveState,
                  " reqChanged=", requestChanged, " drifted=", drifted, " shouldPlay=", shouldPlay,
                  " lastReq=", lastPlayerAnimRequest_,
                  " locoState=", static_cast<int>(characterAnimator_.getLocomotion().getState()),
                  " actState=", static_cast<int>(characterAnimator_.getActivity().getState()),
                  " fwd=", fi.movingForward, " back=", fi.movingBackward,
                  " strafeL=", fi.strafeLeft, " strafeR=", fi.strafeRight,
                  " moving=", fi.moving, " sprinting=", fi.sprinting);
        dbgLastAnim = animId;
    }

    if (shouldPlay) {
        // The renderer has already flowed into this loop (a one-shot's return
        // animation): recording the request is enough, restarting it is a hitch.
        const bool alreadyThere = requestChanged && loop && haveState && currentAnimId == animId;
        if (!alreadyThere) {
            // A finished one-shot returns to Stand unless told otherwise, which
            // put the arms down for a frame between JumpStart and the jump loop.
            uint32_t returnAnim = 0;
            if (animId == anim::JUMP_START && characterRenderer->hasAnimation(characterInstanceId, anim::JUMP)) {
                returnAnim = anim::JUMP;
            }
            characterRenderer->playAnimation(characterInstanceId, animId, loop, returnAnim);
        }
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }

    // A moving animation plays at the speed over its sequence's MoveSpeed
    // (0x7385C0), the local character's as everyone else's: a walk at 2.5,
    // a backpedal at 4.5, a run slowed or hastened, a swim stroke.
    syncLocalMoveRate(fi.moving, fi.speed);
}

void AnimationController::syncLocalMoveRate(bool moving, float speed) {
    auto* cr = renderer_->getCharacterRenderer();
    const uint32_t inst = renderer_->getCharacterInstanceId();
    if (!cr || inst == 0) return;
    uint32_t cur = 0;
    float t = 0.0f, d = 0.0f;
    if (!cr->getAnimationState(inst, cur, t, d) || !anim::isMovingAnim(cur)) return;
    pipeline::M2Sequence playing{};
    if (!cr->getPlayingSequence(inst, playing)) return;
    const float rate = moving ? anim::moveSpeedRate(speed, playing.movingSpeed) : 1.0f;
    cr->setAnimationSpeed(inst, rate);
}

// ── Footstep update (delegated to FootstepDriver) ───────────────────────────

void AnimationController::updateFootsteps(float deltaTime) {
    footstepDriver_.update(deltaTime, renderer_, isMounted(), mountInstanceId_,
                            taxiFlight_, isFootstepAnimationState());
}

// ── Activity SFX state tracking ──────────────────────────────────────────────

void AnimationController::updateSfxState(float deltaTime) {
    sfxStateDriver_.update(deltaTime, renderer_, isMounted(), taxiFlight_,
                           footstepDriver_);
}

} // namespace rendering
} // namespace wowee
