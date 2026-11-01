#include "audio/combat_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/sound_data.hpp"
#include "audio/sound_variation.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <random>
#include <glm/geometric.hpp>

namespace wowee {
namespace audio {

namespace {
    std::random_device rd;
    std::mt19937 gen(rd());

    double nowMs() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr uint32_t eventTag(const char (&s)[5]) {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
    }
    constexpr uint32_t kTagSwoosh = eventTag("$CSS");
    constexpr uint32_t kTagHit = eventTag("$CAH");

    // Item.dbc class 2 subclasses.
    constexpr uint32_t kSubclassFist = 13;

    // WeaponSwingSounds2.SwingType: 0 light, 1 medium, 2 heavy. Daggers,
    // fists, wands and thrown weapons swing light; one-handers medium;
    // two-handers (and the long weapons held in both hands) heavy. An empty
    // hand or a creature's claws swing medium.
    uint32_t swingTypeFor(const CombatItem* weapon) {
        if (!weapon) return 1;
        switch (weapon->subclass) {
            case 13: case 14: case 15: case 16: case 19: return 0;
            case 1: case 5: case 6: case 8: case 10: case 12: case 17: case 20: return 2;
            default: return 1;
        }
    }

    // WeaponImpactSounds.ParrySoundType: 1 for a metal weapon, 0 for wood
    // (a wooden mace, a staff). Cloth and leather (fist wraps) count as wood.
    bool isMetal(uint32_t material) { return material != 2 && material != 7 && material != 8; }
}

bool CombatSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("CombatSoundManager: AssetManager is null");
        return false;
    }

    LOG_INFO("CombatSoundManager: Initializing...");

    // Load weapon swing sounds
    swingSmallSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall1.wav", swingSmallSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall2.wav", swingSmallSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall3.wav", swingSmallSounds_[2], assets);

    swingMediumSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium1.wav", swingMediumSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium2.wav", swingMediumSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium3.wav", swingMediumSounds_[2], assets);

    swingLargeSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav", swingLargeSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge2.wav", swingLargeSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge3.wav", swingLargeSounds_[2], assets);

    swingSmallCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmallCrit.wav", swingSmallCritSounds_[0], assets);

    swingMediumCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMediumCrit.wav", swingMediumCritSounds_[0], assets);

    swingLargeCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLargeCrit.wav", swingLargeCritSounds_[0], assets);

    missWhoosh1HSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\MissSwings\\MissWhoosh1Handed.wav", missWhoosh1HSounds_[0], assets);

    missWhoosh2HSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\MissSwings\\MissWhoosh2Handed.wav", missWhoosh2HSounds_[0], assets);

    // Load impact sounds (using 1H axe as base)
    hitFleshSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1a.wav", hitFleshSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1b.wav", hitFleshSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1c.wav", hitFleshSounds_[2], assets);

    hitChainSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1a.wav", hitChainSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1b.wav", hitChainSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1c.wav", hitChainSounds_[2], assets);

    hitPlateSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1a.wav", hitPlateSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1b.wav", hitPlateSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1c.wav", hitPlateSounds_[2], assets);

    hitShieldSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1a.wav", hitShieldSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1b.wav", hitShieldSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1c.wav", hitShieldSounds_[2], assets);

    hitMetalWeaponSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalWeapon1a.wav", hitMetalWeaponSounds_[0], assets);

    hitWoodSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1A.wav", hitWoodSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1B.wav", hitWoodSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1C.wav", hitWoodSounds_[2], assets);

    hitStoneSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1A.wav", hitStoneSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1B.wav", hitStoneSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1C.wav", hitStoneSounds_[2], assets);

    // Load crit impact sounds
    hitFleshCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFleshCrit.wav", hitFleshCritSounds_[0], assets);

    hitChainCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChainCrit.wav", hitChainCritSounds_[0], assets);

    hitPlateCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlateCrit.wav", hitPlateCritSounds_[0], assets);

    hitShieldCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShieldCrit.wav", hitShieldCritSounds_[0], assets);

    initialized_ = true;
    LOG_INFO("CombatSoundManager: Initialization complete");
    return true;
}

void CombatSoundManager::shutdown() {
    initialized_ = false;
    pendingBlows_.clear();
    lastInjuryMs_.clear();
}

bool CombatSoundManager::loadSound(const std::string& path, CombatSample& sample, pipeline::AssetManager* assets) {
    // Quietly: not every sound in this bank ships with every install.
    return loadSampleFile(path, sample, assets, nullptr);
}

void CombatSoundManager::playSound(const std::vector<CombatSample>& library, float volumeMultiplier) {
    if (!initialized_ || library.empty() || !library[0].loaded) return;

    float volume = 0.8f * volumeScale_ * volumeMultiplier;
    AudioEngine::instance().playSound2D(library[0].data, volume, 1.0f);
}

void CombatSoundManager::playRandomSound(const std::vector<CombatSample>& library, float volumeMultiplier) {
    if (!initialized_) return;
    // Among the ones that loaded - see pickLoadedSample. The base volume is
    // this bank's own and stays here.
    const CombatSample* chosen = pickLoadedSample(library, gen);
    if (!chosen) return;
    const float volume = 0.8f * volumeScale_ * volumeMultiplier;
    AudioEngine::instance().playSound2D(chosen->data, volume, 1.0f);
}

void CombatSoundManager::playVariedImpact(const std::vector<CombatSample>& library,
                                          float volumeMultiplier) {
    if (!initialized_) return;
    std::vector<size_t> loaded;
    loaded.reserve(library.size());
    for (size_t i = 0; i < library.size(); ++i) {
        if (library[i].loaded) loaded.push_back(i);
    }
    if (loaded.empty()) return;
    // The last pick is held as an index into this bank's loaded list; a
    // different bank starts afresh.
    const size_t last = (lastImpactLibrary_ == &library) ? lastImpactIndex_ : SIZE_MAX;
    const size_t pick = pickAvoidingRepeat(loaded.size(), last, gen);
    lastImpactLibrary_ = &library;
    lastImpactIndex_ = pick;
    const SoundJitter jitter = soundJitter(gen);
    const float volume = std::min(1.0f, 0.8f * volumeScale_ * volumeMultiplier * jitter.gain);
    AudioEngine::instance().playSound2D(library[loaded[pick]].data, volume, jitter.pitch);
}

void CombatSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

void CombatSoundManager::playWeaponSwing(WeaponSize size, bool isCrit) {
    if (isCrit) {
        switch (size) {
            case WeaponSize::SMALL:
                playSound(swingSmallCritSounds_);
                break;
            case WeaponSize::MEDIUM:
                playSound(swingMediumCritSounds_);
                break;
            case WeaponSize::LARGE:
                playSound(swingLargeCritSounds_);
                break;
        }
    } else {
        switch (size) {
            case WeaponSize::SMALL:
                playRandomSound(swingSmallSounds_);
                break;
            case WeaponSize::MEDIUM:
                playRandomSound(swingMediumSounds_);
                break;
            case WeaponSize::LARGE:
                playRandomSound(swingLargeSounds_);
                break;
        }
    }
}

void CombatSoundManager::playWeaponMiss(bool twoHanded) {
    if (twoHanded) {
        playSound(missWhoosh2HSounds_);
    } else {
        playSound(missWhoosh1HSounds_);
    }
}

void CombatSoundManager::playImpact([[maybe_unused]] WeaponSize weaponSize, ImpactType impactType, bool isCrit) {
    // Select appropriate impact sound library
    const std::vector<CombatSample>* normalLibrary = nullptr;
    const std::vector<CombatSample>* critLibrary = nullptr;

    switch (impactType) {
        case ImpactType::FLESH:
            normalLibrary = &hitFleshSounds_;
            critLibrary = &hitFleshCritSounds_;
            break;
        case ImpactType::CHAIN:
            normalLibrary = &hitChainSounds_;
            critLibrary = &hitChainCritSounds_;
            break;
        case ImpactType::PLATE:
            normalLibrary = &hitPlateSounds_;
            critLibrary = &hitPlateCritSounds_;
            break;
        case ImpactType::SHIELD:
            normalLibrary = &hitShieldSounds_;
            critLibrary = &hitShieldCritSounds_;
            break;
        case ImpactType::METAL_WEAPON:
            normalLibrary = &hitMetalWeaponSounds_;
            break;
        case ImpactType::WOOD:
            normalLibrary = &hitWoodSounds_;
            break;
        case ImpactType::STONE:
            normalLibrary = &hitStoneSounds_;
            break;
    }

    if (!normalLibrary) return;

    // Play crit version if available, otherwise use normal
    // Each with a small turn of pitch and level and never the same clip
    // twice running: impacts come in runs, and a run of one sample is the
    // most obviously canned sound in a fight.
    if (isCrit && critLibrary && !critLibrary->empty()) {
        playVariedImpact(*critLibrary, 1.2f);  // Crits slightly louder
    } else {
        playVariedImpact(*normalLibrary);
    }
}

// ---------------------------------------------------------------------------
// Table-driven melee and body sounds
// ---------------------------------------------------------------------------

bool CombatSoundManager::playEntry3D(uint32_t soundId, const glm::vec3& position, float volumeMul,
                                     bool vary) {
    if (!initialized_ || !soundData_ || soundId == 0) return false;
    const SoundEntry* entry = soundData_->sound(soundId);
    if (!entry || entry->files.empty()) return false;
    const float maxDistance = entry->distanceCutoff > 0.0f ? entry->distanceCutoff : 40.0f;
    auto& engine = AudioEngine::instance();
    if (!engine.isInitialized()) return false;
    // Out of earshot: not started at all. Every unit in view now has steps,
    // fidgets and blows, and most of them are too far to hear.
    if (glm::distance(engine.getListenerPosition(), position) > maxDistance) return false;
    if (vary) {
        const auto lastIt = lastVariedFile_.try_emplace(soundId, SIZE_MAX).first;
        const size_t file = pickAvoidingRepeat(entry->files.size(), lastIt->second, gen);
        lastIt->second = file;
        const SoundJitter jitter = soundJitter(gen);
        return engine.playSound3D(entry->files[file], position,
                                  std::min(1.0f, entry->volume * volumeScale_ * volumeMul * jitter.gain),
                                  jitter.pitch, maxDistance);
    }
    std::uniform_int_distribution<size_t> pick(0, entry->files.size() - 1);
    return engine.playSound3D(entry->files[pick(gen)], position,
                              entry->volume * volumeScale_ * volumeMul, 1.0f, maxDistance);
}

uint32_t CombatSoundManager::pickInjury(uint64_t victimGuid, uint32_t displayId, bool crit) {
    const CreatureSounds* cs = soundData_ ? soundData_->displaySounds(displayId) : nullptr;
    if (!cs) return 0;
    const double now = nowMs();
    if (lastInjuryMs_.size() > 512) {
        for (auto it = lastInjuryMs_.begin(); it != lastInjuryMs_.end();)
            it = (now - it->second > 10000.0) ? lastInjuryMs_.erase(it) : std::next(it);
    }
    double& last = lastInjuryMs_[victimGuid];
    // A crit always cries out (InjuryCritical, or the plain injury for a
    // model without one). An ordinary hit does about one time in three and
    // never twice inside a second, so a flurry of blows is not a stream of
    // grunts.
    if (crit) {
        if (now - last < 300.0) return 0;
        last = now;
        return cs->injuryCritical != 0 ? cs->injuryCritical : cs->injury;
    }
    if (now - last < 1000.0) return 0;
    if (std::uniform_int_distribution<int>(0, 2)(gen) != 0) return 0;
    last = now;
    return cs->injury;
}

void CombatSoundManager::playBlowSwing(PendingBlow& blow) {
    if (blow.swingPlayed) return;
    blow.swingPlayed = true;
    playEntry3D(blow.swingSound, blow.attackerPos);
}

void CombatSoundManager::playBlowImpact(PendingBlow& blow) {
    if (blow.impactPlayed) return;
    blow.impactPlayed = true;
    playEntry3D(blow.impactSound, blow.victimPos, 1.0f, /*vary=*/true);
    playEntry3D(blow.injurySound, blow.victimPos);
}

void CombatSoundManager::onMelee(uint64_t attackerGuid, uint64_t victimGuid, MeleeOutcome outcome,
                                 bool offHand, bool animated, bool injureVictim, bool ourCombat,
                                 bool withSwoosh) {
    if (!initialized_ || outcome == MeleeOutcome::NONE) return;
    CombatUnitInfo attacker, victim;
    const bool haveAttacker = unitResolver_ && attackerGuid != 0 && unitResolver_(attackerGuid, attacker);
    const bool haveVictim = unitResolver_ && victimGuid != 0 && unitResolver_(victimGuid, victim);

    if (!soundData_ || !soundData_->loaded() || !haveAttacker) {
        // No tables, or nobody to place it on: the fixed bank, in the
        // listener's head, for the local character's own fights.
        if (!ourCombat) return;
        const auto size = WeaponSize::MEDIUM;
        switch (outcome) {
            case MeleeOutcome::MISS: case MeleeOutcome::DODGE: playWeaponMiss(false); break;
            case MeleeOutcome::PARRY: playWeaponSwing(size, false); playImpact(size, ImpactType::METAL_WEAPON, false); break;
            case MeleeOutcome::BLOCK: playWeaponSwing(size, false); playImpact(size, ImpactType::SHIELD, false); break;
            default:
                playWeaponSwing(size, outcome == MeleeOutcome::CRIT);
                playImpact(size, ImpactType::FLESH, outcome == MeleeOutcome::CRIT);
                break;
        }
        return;
    }

    const CombatItem& weapon = offHand ? attacker.offHand : attacker.mainHand;
    const bool armed = weapon.isWeapon();
    // An empty hand, a creature's claws and fist weapons all use the Fist
    // row (Unarmed_Generic and the unarmed parries).
    const uint32_t subclass = armed ? weapon.subclass : kSubclassFist;
    const bool metal = armed && isMetal(weapon.material);
    const bool crit = outcome == MeleeOutcome::CRIT;

    PendingBlow blow;
    blow.attackerPos = attacker.position;
    blow.victimPos = haveVictim ? victim.position : attacker.position;
    blow.swingSound = withSwoosh ? soundData_->weaponSwingSound(swingTypeFor(armed ? &weapon : nullptr), crit) : 0;

    if (outcome == MeleeOutcome::HIT || outcome == MeleeOutcome::CRIT ||
        outcome == MeleeOutcome::PARRY || outcome == MeleeOutcome::BLOCK) {
        ImpactSlot slot = ImpactSlot::FLESH;
        if (outcome == MeleeOutcome::PARRY) {
            // The weapon that turned the blow aside: the main hand's, or the
            // off hand's when only that is a weapon.
            const CombatItem* parrier = victim.mainHand.isWeapon() ? &victim.mainHand
                                      : (victim.offHand.isWeapon() ? &victim.offHand : nullptr);
            slot = (parrier && !isMetal(parrier->material)) ? ImpactSlot::PARRY_WOOD : ImpactSlot::PARRY_METAL;
        } else if (outcome == MeleeOutcome::BLOCK) {
            const CombatItem* shield = victim.offHand.isShield() ? &victim.offHand : nullptr;
            slot = (shield && shield->material == 2) ? ImpactSlot::SHIELD_WOOD : ImpactSlot::SHIELD_METAL;
        } else if (haveVictim) {
            // The body that was struck: a player's worn mail or plate, else
            // the model's CreatureImpactType (1 on the stone golem and the
            // infernal: stone), else flesh.
            if (victim.isPlayer && victim.chestArmorSubclass == 3) slot = ImpactSlot::CHAIN;
            else if (victim.isPlayer && victim.chestArmorSubclass == 4) slot = ImpactSlot::PLATE;
            else if (const CreatureSounds* cs = soundData_->displaySounds(victim.displayId);
                     cs && cs->impactType == 1) slot = ImpactSlot::STONE;
        }
        blow.impactSound = soundData_->weaponImpactSound(subclass, metal, slot, crit);
        if ((outcome == MeleeOutcome::HIT || outcome == MeleeOutcome::CRIT) && injureVictim && haveVictim &&
            !victim.isLocalPlayer) {
            blow.injurySound = pickInjury(victimGuid, victim.displayId, crit);
        }
    }

    // Synced to the attack animation when it has the events: the swoosh at
    // $CSS, the blow at $CAH. A blow still waiting for the same attacker's
    // last swing is played out first.
    if (animated && attacker.instanceId != 0 && (attacker.animHasSwingEvent || attacker.animHasHitEvent)) {
        if (auto old = pendingBlows_.find(attacker.instanceId); old != pendingBlows_.end()) {
            playBlowSwing(old->second);
            playBlowImpact(old->second);
            pendingBlows_.erase(old);
        }
        if (!attacker.animHasSwingEvent) playBlowSwing(blow);
        blow.impactWithSwing = !attacker.animHasHitEvent;
        // $CAH comes 300-530 ms into the stock attack animations
        // (HumanMale Attack1H 300, Attack2H 400; Wolf Attack1H 334).
        blow.deadlineMs = nowMs() + 1000.0;
        pendingBlows_[attacker.instanceId] = blow;
        return;
    }
    playBlowSwing(blow);
    playBlowImpact(blow);
}

void CombatSoundManager::onWounded(uint64_t victimGuid, bool crit) {
    if (!initialized_ || !soundData_ || !unitResolver_) return;
    CombatUnitInfo victim;
    if (!unitResolver_(victimGuid, victim) || victim.isLocalPlayer) return;
    playEntry3D(pickInjury(victimGuid, victim.displayId, crit), victim.position);
}

void CombatSoundManager::onMeleeAnimEvent(uint32_t instanceId, uint32_t tag) {
    if (tag != kTagSwoosh && tag != kTagHit) return;
    auto it = pendingBlows_.find(instanceId);
    if (it == pendingBlows_.end()) return;
    PendingBlow& blow = it->second;
    if (tag == kTagSwoosh) {
        playBlowSwing(blow);
        if (blow.impactWithSwing) playBlowImpact(blow);
    } else {
        playBlowSwing(blow);
        playBlowImpact(blow);
    }
    if (blow.swingPlayed && blow.impactPlayed) pendingBlows_.erase(it);
}

void CombatSoundManager::update() {
    if (pendingBlows_.empty()) return;
    const double now = nowMs();
    for (auto it = pendingBlows_.begin(); it != pendingBlows_.end();) {
        if (now >= it->second.deadlineMs) {
            playBlowSwing(it->second);
            playBlowImpact(it->second);
            it = pendingBlows_.erase(it);
        } else {
            ++it;
        }
    }
}

void CombatSoundManager::playBodySound(uint32_t displayId, BodySound which, uint32_t index,
                                       const glm::vec3& position) {
    const CreatureSounds* cs = soundData_ ? soundData_->displaySounds(displayId) : nullptr;
    if (!cs) return;
    uint32_t soundId = 0;
    switch (which) {
        case BodySound::DEATH:         soundId = cs->death; break;
        case BodySound::WING_FLAP:     soundId = cs->wingFlap; break;
        case BodySound::FIDGET:        soundId = index < 5 ? cs->fidget[index] : 0; break;
        case BodySound::CUSTOM_ATTACK: soundId = index < 4 ? cs->customAttack[index] : 0; break;
        case BodySound::INJURY:        soundId = cs->injury; break;
        case BodySound::INJURY_CRIT:   soundId = cs->injuryCritical; break;
    }
    playEntry3D(soundId, position);
}

void CombatSoundManager::playFootstep(uint32_t displayId, uint32_t terrainSoundId, bool inWater,
                                      const glm::vec3& position) {
    const CreatureSounds* cs = soundData_ ? soundData_->displaySounds(displayId) : nullptr;
    if (!cs || cs->footstep == 0) return;
    playEntry3D(soundData_->footstepSound(cs->footstep, terrainSoundId, inWater), position);
}
} // namespace audio
} // namespace wowee
