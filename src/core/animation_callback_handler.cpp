#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/weapon_anim_select.hpp"
#include "core/animation_callback_handler.hpp"
#include "core/appearance_composer.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "game/game_handler.hpp"
#include "game/spell_classification.hpp"
#include "game/spell_handler.hpp"
#include "game/world_packets.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>

namespace wowee { namespace core {

AnimationCallbackHandler::AnimationCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    game::GameHandler& gameHandler,
    AppearanceComposer& appearanceComposer)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , appearanceComposer_(appearanceComposer)
{
}

uint32_t AnimationCallbackHandler::kitCastAnim(uint32_t spellId) {
    auto* svs = renderer_.getSpellVisualSystem();
    if (spellId == 0 || !svs) return 0;
    gameHandler_.loadSpellNameCache();
    auto it = gameHandler_.spellNameCacheRef().find(spellId);
    if (it == gameHandler_.spellNameCacheRef().end() || it->second.spellVisualId == 0) return 0;
    return svs->getKitAnimations(it->second.spellVisualId).cast;
}

void AnimationCallbackHandler::setupCallbacks() {
    // Sprint aura callback - use SPRINT(143) animation when sprint-type buff is active
    gameHandler_.setSprintAuraCallback([this](bool active) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setSprintAuraActive(active);
    });

    // A passenger is seen in its seat (stock hides one only on a seat with
    // flag 0x200): the seat placement is Application::syncVehiclePassengers'.

    // GUID → CharacterRenderer instance for bone-attached spell visuals:
    // local player uses the dedicated character instance, other units the
    // spawner's creature/player instances (0 = not rendered, no attachment).
    gameHandler_.setUnitRenderInstanceResolver([this](uint64_t guid) -> uint32_t {
        if (guid == gameHandler_.getPlayerGuid()) return renderer_.getCharacterInstanceId();
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        return instanceId;
    });

    // Death and coming back, for every unit: creatures, other players and the
    // local character (health reaching 0, UNIT_DYNFLAG_DEAD, a corpse object).
    // Death plays once and holds; the renderer then refuses every other
    // animation for that instance until the respawn callback lifts the lock,
    // so no idle, Ready stance, swing, flinch or emote clear can stand a
    // corpse back up. A unit whose render instance is still queued is marked
    // dead and spawns already lying down (EntitySpawner's spawn pose).
    //
    // world_loader registered its own pair on every world load, after this
    // one, and so replaced it; it no longer does.
    gameHandler_.setNpcDeathCallback([this](uint64_t guid) {
        const bool isLocal = guid == gameHandler_.getPlayerGuid();
        // The caster's own death ends its cast pose and build-up.
        if (auto* sh = gameHandler_.getSpellHandler()) sh->onCasterDied(guid);
        if (!isLocal) entitySpawner_.markCreatureDead(guid);
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = 0;
        if (isLocal) {
            instanceId = renderer_.getCharacterInstanceId();
        } else {
            instanceId = entitySpawner_.getCreatureInstanceId(guid);
            if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        }
        if (instanceId != 0) cr->playDeath(instanceId, /*alreadyDead=*/false);
    });

    gameHandler_.setNpcRespawnCallback([this](uint64_t guid) {
        const bool isLocal = guid == gameHandler_.getPlayerGuid();
        if (!isLocal) entitySpawner_.unmarkCreatureDead(guid);
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        if (isLocal) {
            // Resurrected, or released as a ghost (health 0 -> 1). Stand; the
            // animation controller takes over from there.
            const uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId == 0) return;
            cr->clearDeath(charInstId);
            cr->playAnimation(charInstId, rendering::anim::STAND, true);
            return;
        }
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;
        const bool wasDown = cr->isDeathLocked(instanceId);
        cr->clearDeath(instanceId);
        // Getting up off the ground plays Rise (a one-shot that returns to
        // Stand); a unit that was not lying there just stands.
        if (wasDown && cr->hasAnimation(instanceId, rendering::anim::RISE))
            cr->playAnimation(instanceId, rendering::anim::RISE, false);
        else
            cr->playAnimation(instanceId, rendering::anim::STAND, true);
    });

    // Swing callback for every unit but the local player, whoever it is
    // fighting. The swing comes from the unit's own weapons and the hand the
    // server named (HITINFO_OFFHAND); an instant ability plays its cast kit's
    // animation with the weapon variant swapped in. Both return to the unit's
    // idle - its Ready stance while in combat - rather than to Stand.
    // Before, every unit swung ATTACK_1H whatever it held, and the hand, the
    // ability and the stance were all lost.
    gameHandler_.setNpcSwingCallback([this](uint64_t guid, uint32_t hitInfo, uint32_t spellId) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;
        using namespace rendering::anim;
        uint32_t cur = 0; float t = 0.0f, d = 0.0f;
        if (cr->getAnimationState(instanceId, cur, t, d) && cur == DEATH) return;

        const rendering::WeaponLoadout weapons = entitySpawner_.unitWeaponLoadout(guid);
        auto has = [&](uint32_t id) { return cr->hasAnimation(instanceId, id); };
        uint32_t anim = 0;
        if (spellId != 0) anim = firstPlayable(has, abilityAnimCandidates(kitCastAnim(spellId), weapons));
        if (anim == 0) {
            constexpr uint32_t kHitInfoOffHand = 0x00000004;
            const auto chain = meleeAnimChain(swingChain(weapons, (hitInfo & kHitInfoOffHand) != 0));
            for (uint32_t id : chain) if (has(id)) { anim = id; break; }
        }
        if (anim == 0) return;  // a model with no attack at all stays as it is
        // A unit on the move swings with its upper body and keeps running
        // (Wow.exe 0x7385C0 / 0x723E30): a creature chasing a fleeing
        // player used to stop dead in its attack pose and slide.
        if (isLocomotionAnim(cur) && playsOnUpperBody(anim)) {
            cr->playUpperBodyAnimation(instanceId, anim, anim, anim);
            return;
        }
        cr->playAnimation(instanceId, anim, false, entitySpawner_.unitIdleAnim(guid, instanceId));
    });

    // The victim's reaction to a melee blow waits for the blow. The stock
    // client keeps the swing's SMSG_ATTACKERSTATEUPDATE on the attacker
    // (CGUnit_C +0xB98, Wow.exe 0x756800) and acts it out when the attacker's
    // attack animation reaches its events (0x756240, see onMeleeAnimEvent):
    // a dodge, parry or block as the swing begins ($CPP), the flinch, the
    // blood and the blow's sound as the weapon lands ($CAH), not when the
    // packet does, half a swing early. A blow still waiting when the
    // same attacker swings again is played out first (0x756180), and one
    // whose event never comes (a model without it, a swing cut short) after
    // kBlowFallbackMs. A spell's damage has no attacker swing and reacts at
    // once.
    gameHandler_.setHitReactionCallback([this](uint64_t victimGuid, game::GameHandler::HitReaction reaction,
                                               uint64_t attackerGuid) {
        uint32_t attackerInstance = 0;
        if (attackerGuid != 0) {
            if (attackerGuid == gameHandler_.getPlayerGuid()) {
                attackerInstance = renderer_.getCharacterInstanceId();
            } else {
                attackerInstance = entitySpawner_.getCreatureInstanceId(attackerGuid);
                if (attackerInstance == 0) attackerInstance = entitySpawner_.getPlayerInstanceId(attackerGuid);
            }
        }
        if (attackerInstance == 0) {
            if (reaction != game::GameHandler::HitReaction::MISS)
                playHitReaction(victimGuid, static_cast<uint8_t>(reaction));
            return;
        }
        if (auto old = pendingReactions_.find(attackerInstance); old != pendingReactions_.end()) {
            const PendingReaction p = old->second;
            pendingReactions_.erase(old);
            flushPendingReaction(p);
        }
        pendingReactions_[attackerInstance] =
            PendingReaction{victimGuid, static_cast<uint8_t>(reaction), nowMs() + kBlowFallbackMs};
    });

    // Stun state callback - enters/exits STUNNED animation on local player
    gameHandler_.setStunStateCallback([this](bool stunned) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStunned(stunned);
    });

    // Stealth state callback - switches to stealth animation variants
    gameHandler_.setStealthStateCallback([this](bool stealthed) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStealthed(stealthed);
    });

    // Player health callback - switches to wounded idle when HP < 20%
    gameHandler_.setPlayerHealthCallback([this](uint32_t health, uint32_t maxHealth) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        bool lowHp = (maxHealth > 0) && (health > 0) && (health * 5 <= maxHealth);
        ac->setLowHealth(lowHp);
    });

    // Unit animation hint callback - MSG_MOVE_JUMP from another player or NPC
    // (animId 38). A unit on its feet hands the jump to its LocomotionFSM
    // (animation/unit_locomotion.hpp), which plays JumpStart, the Jump loop
    // while FALLING lasts, and JumpEnd or JumpLandRun on landing, as the
    // local player's does. A rider's jump belongs to the mount; anything
    // else (in flight) still just loops the hinted animation.
    gameHandler_.setUnitAnimHintCallback([this](uint64_t guid, uint32_t animId) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) return;
        if (entitySpawner_.getRemotePlayerMount(guid)) return;
        if (animId == rendering::anim::JUMP && !entitySpawner_.getCreatureFlyingState().count(guid)) {
            auto& loco = entitySpawner_.getUnitLocomotion()[guid];
            loco.jumpPending = true;
            loco.airborneSeen = false;
            loco.jumpAge = 0.0f;
            return;
        }
        // Don't override Death animation
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) && curAnim == rendering::anim::DEATH) return;
        cr->playAnimation(instanceId, animId, /*loop=*/true);
    });

    // Unit move-flags callback - updates swimming and walking state from every MSG_MOVE_* packet.
    // This is more reliable than opcode-based hints for cold joins and heartbeats:
    // a player already swimming when we join will have SWIMMING set on the first heartbeat.
    // Walking(4) vs Running(5) is also driven here from the WALKING flag.
    gameHandler_.setUnitMoveFlagsCallback([this](uint64_t guid, uint32_t moveFlags,
                                                 uint32_t mask) {
        // Only what this update speaks about. A full movement block passes a
        // mask of all ones; the spline opcodes each pass the one bit they are
        // named after, so starting to swim no longer says anything about
        // whether the creature is also flying.
        const auto apply = [&](game::MovementFlags flag, auto& state) {
            const uint32_t bit = static_cast<uint32_t>(flag);
            if ((mask & bit) == 0) return;
            if (moveFlags & bit) state[guid] = true;
            else                 state.erase(guid);
        };
        apply(game::MovementFlags::SWIMMING, entitySpawner_.getCreatureSwimmingState());
        apply(game::MovementFlags::WALKING,  entitySpawner_.getCreatureWalkingState());
        apply(game::MovementFlags::FLYING,   entitySpawner_.getCreatureFlyingState());
        // What holds the unit up off the floor. Without any of these the stock
        // client lets a unit settle onto the floor under its server position.
        {
            const uint32_t kGravityFreeBits =
                static_cast<uint32_t>(game::MovementFlags::LEVITATING) |
                static_cast<uint32_t>(game::MovementFlags::CAN_FLY) |
                static_cast<uint32_t>(game::MovementFlags::FLYING) |
                static_cast<uint32_t>(game::MovementFlags::HOVER);
            const uint32_t gMask = mask & kGravityFreeBits;
            if (gMask != 0) {
                auto& gFlags = entitySpawner_.getUnitGravityFreeFlags();
                const uint32_t g = (gFlags[guid] & ~gMask) | (moveFlags & gMask);
                if (g != 0) gFlags[guid] = g;
                else        gFlags.erase(guid);
            }
        }
        // Direction bits for the stock body yaw and the locomotion animation
        // (forward, backward, strafes, turns, falling, falling far): a
        // strafing player's model turns toward travel, a backpedal walks
        // backwards, a turn in place shuffles, a fall falls.
        constexpr uint32_t kDirBits = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x1000u | 0x2000u;
        const uint32_t dirMask = mask & kDirBits;
        if (dirMask != 0) {
            auto& dirFlags = entitySpawner_.getUnitMoveDirFlags();
            const uint32_t dir = (dirFlags[guid] & ~dirMask) | (moveFlags & dirMask);
            if (dir != 0) dirFlags[guid] = dir;
            else          dirFlags.erase(guid);
        }
    });

    // Emote animation callback - play server-driven emote animations on NPCs and other players.
    // isState marks persistent STATE_ emotes (UNIT_NPC_EMOTESTATE or state-type
    // SMSG_EMOTE): they loop until cleared and are retained across model
    // reloads. One-shots play once and resume the retained state loop.
    gameHandler_.setEmoteAnimCallback([this](uint64_t guid, uint32_t emoteAnim, bool isState) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        const bool isCreature = entity && entity->getType() == game::ObjectType::UNIT;
        auto& activeEmotes = entitySpawner_.getCreatureActiveEmotes();
        if (isCreature && isState) {
            // State emotes can arrive while the creature's model is still loading.
            // Retain the raw animation so spawnOnlineCreature can apply it once the
            // correct display is instantiated.
            if (emoteAnim == 0) activeEmotes.erase(guid);
            else                activeEmotes[guid] = emoteAnim;
        }
        // Look up creature instance first, then online players
        uint32_t emoteInstanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (emoteInstanceId != 0) {
            if (emoteAnim == 0) {
                // A state clear returns to idle. A one-shot cancel (SMSG_EMOTE 0)
                // must not stomp a persistent UNIT_NPC_EMOTESTATE work loop -
                // resume it instead (Westfall woodworkers hammer through these).
                if (isState) activeEmotes.erase(guid);
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = rendering::anim::STAND;
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, resumeAnim, true);
            } else if (isState) {
                // A state emote loops the animation its Emotes.dbc row names -
                // STATE_DANCE is 69, the dance itself. 3.3.5 has no separate
                // looping variants to look for.
                activeEmotes[guid] = emoteAnim;
                cr->playAnimation(emoteInstanceId, emoteAnim, true);
            } else {
                // One-shot: play once, then resume the retained state loop (or Stand).
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = 0;
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, emoteAnim, false, resumeAnim);
            }
            return;
        }
        emoteInstanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (emoteInstanceId != 0) {
            // Another player's state loop (a dance, a sit) is kept too, so a
            // one-shot over it - the SitGroundDown on the way into the sit -
            // returns to it rather than to Stand.
            auto& playerStates = playerStateEmotes_;
            if (emoteAnim == 0) {
                if (isState) playerStates.erase(guid);
                cr->playAnimation(emoteInstanceId, rendering::anim::STAND, true);
            } else if (isState) {
                // Looped, as the NPC branch above does.
                playerStates[guid] = emoteAnim;
                cr->playAnimation(emoteInstanceId, emoteAnim, true);
            } else {
                const auto retained = playerStates.find(guid);
                const uint32_t resume = retained != playerStates.end() ? retained->second : 0;
                cr->playAnimation(emoteInstanceId, emoteAnim, false, resume);
            }
        }
    });

    // Spell cast animation callback - play cast animation on caster (player or NPC/other player)
    // WoW-accurate 3-phase spell animation sequence:
    //   SPELL_PRECAST (31)              - one-shot wind-up
    //   READY_SPELL_DIRECTED/OMNI (51/52) - looping hold while cast bar fills
    //   SPELL_CAST_DIRECTED/OMNI/AREA (53/54/33) - one-shot release at completion
    // Channels use CHANNEL_CAST_DIRECTED/OMNI (124/125).
    // castType comes from the spell packet's targetGuid:
    //   DIRECTED - spell targets a specific unit  (Frostbolt, Heal)
    //   OMNI     - self-cast / no explicit target (Arcane Explosion, buffs)
    //   AREA     - ground-targeted AoE           (Blizzard, Rain of Fire)
    gameHandler_.setSpellCastAnimCallback([this](uint64_t guid, bool start, bool isChannel,
                                                  game::SpellCastType castType) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;

        // Determine if this is the local player
        bool isLocalPlayer = false;
        uint32_t instanceId = 0;
        {
            uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId != 0 && guid == gameHandler_.getPlayerGuid()) {
                instanceId = charInstId;
                isLocalPlayer = true;
            }
        }
        if (instanceId == 0) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;

        const bool isDirected = (castType == game::SpellCastType::DIRECTED);
        const bool isArea     = (castType == game::SpellCastType::AREA);

        // The animations the spell's own kits name (SpellVisualKit.AnimID):
        // Flash Heal holds ReadySpellOmni and releases SpellCastOmni even when
        // cast on someone, Arcane Missiles channels ChannelCastDirected. The
        // target-based guess below stays as the fallback. Only the spell
        // cast families are taken (a kit can name a melee swing).
        rendering::SpellVisualSystem::KitAnimations kitAnims;
        {
            uint32_t kitSpell = isLocalPlayer ? gameHandler_.getCurrentCastSpellId() : 0;
            // A cast just sent: the build-up starts before SMSG_SPELL_START,
            // when only the prediction knows the spell.
            if (kitSpell == 0 && isLocalPlayer) kitSpell = gameHandler_.getPredictedCastSpellId();
            if (kitSpell == 0)
                if (const auto* st = gameHandler_.getUnitCastState(guid)) kitSpell = st->spellId;
            auto* svs = renderer_.getSpellVisualSystem();
            if (kitSpell != 0 && svs) {
                gameHandler_.loadSpellNameCache();
                auto it = gameHandler_.spellNameCacheRef().find(kitSpell);
                if (it != gameHandler_.spellNameCacheRef().end() && it->second.spellVisualId)
                    kitAnims = svs->getKitAnimations(it->second.spellVisualId);
            }
            using namespace rendering::anim;
            auto keep = [&](uint32_t& a, std::initializer_list<uint32_t> family) {
                if (std::find(family.begin(), family.end(), a) == family.end() ||
                    !cr->hasAnimation(instanceId, a)) a = 0;
            };
            // The build-up holds whatever its kit names, as the stock client
            // plays a kit's AnimID as it is (Wow.exe 0x73B140): UseStandingLoop
            // for an item being used, ReadyThrown, ChannelCastOmni for a
            // precast, EmoteRoar. Not a wound or a death - those are a
            // flinch and the death path's, not a pose to hold.
            auto keepHold = [&](uint32_t& a) {
                const uint32_t b = dbcBehavior(a);
                if (a == 0 || b == DEATH || b == DEAD || b == STAND_WOUND ||
                    b == COMBAT_WOUND || b == COMBAT_CRITICAL || !cr->hasAnimation(instanceId, a))
                    a = 0;
            };
            keepHold(kitAnims.precast);
            keepHold(kitAnims.channel);
            keep(kitAnims.cast, {SPELL_CAST_DIRECTED, SPELL_CAST_OMNI, SPELL_CAST_AREA});
        }

        if (start) {
            uint32_t currentSpell = isLocalPlayer ? gameHandler_.getCurrentCastSpellId() : 0;
            bool isFishing = isChannel && game::spellclass::isFishingCast(currentSpell);

            // Helper: pick first animation the model supports from a list
            auto pickFirst = [&](std::initializer_list<uint32_t> ids) -> uint32_t {
                for (uint32_t id : ids)
                    if (cr->hasAnimation(instanceId, id)) return id;
                return 0;
            };

            bool isBandage = false;
            bool isMining = false;
            bool isGathering = false;
            game::spellclass::RestChannelKind restChannel =
                game::spellclass::RestChannelKind::NONE;
            if (isLocalPlayer && currentSpell != 0) {
                gameHandler_.loadSpellNameCache();
                auto it = gameHandler_.spellNameCacheRef().find(currentSpell);
                isBandage = (it != gameHandler_.spellNameCacheRef().end() &&
                             it->second.name.find("Bandage") != std::string::npos);
                if (it != gameHandler_.spellNameCacheRef().end()) {
                    restChannel = game::spellclass::classifyRestChannel(it->second.name);
                }
                if (!isBandage && !isFishing && it != gameHandler_.spellNameCacheRef().end()) {
                    const auto& nm = it->second.name;
                    isMining = (nm == "Mining" || nm == "Smelting");
                    if (!isMining) {
                        isGathering = gameHandler_.isProfessionSpell(currentSpell);
                        if (!isGathering)
                            isGathering = (nm == "Opening" || nm == "Open Lock");
                    }
                }
            }

            if (restChannel != game::spellclass::RestChannelKind::NONE) {
                if (auto* ac = renderer_.getAnimationController()) {
                    if (restChannel == game::spellclass::RestChannelKind::CANNIBALIZE) {
                        // Standing, and looping for the length of the channel.
                        //
                        // Not the food path: that seats the player and swaps
                        // the seated idle, and cannibalize does not sit - the
                        // character stays on their feet over the corpse. Not
                        // the potion path either, which is one swig and done,
                        // where this runs until the channel ends.
                        uint32_t eatAnim = pickFirst({rendering::anim::EMOTE_EAT,
                                                      rendering::anim::EATING_LOOP});
                        if (eatAnim != 0) {
                            ac->startSpellCast(0, eatAnim, /*castLoop=*/true, 0);
                        }
                    } else if (restChannel == game::spellclass::RestChannelKind::POTION ||
                        restChannel == game::spellclass::RestChannelKind::ALCOHOL) {
                        // Potions/alcohol are drunk standing: one EmoteEat swig.
                        uint32_t swigAnim = pickFirst({rendering::anim::EMOTE_EAT});
                        if (swigAnim != 0) {
                            ac->startSpellCast(0, swigAnim, false, 0);
                        }
                    } else {
                        // Weapons go straight to the back before the sit - the real
                        // client auto-sheathes on consume with no sheath animation
                        // (playing one here would fight the FSM's sit-down one-shot).
                        if (!appearanceComposer_.isWeaponsSheathed()) {
                            appearanceComposer_.setWeaponsSheathed(true, "eat/drink");
                            appearanceComposer_.loadEquippedWeapons();
                        }
                        // Food and water keep the player seated (UNIT_FIELD_BYTES_1)
                        // with the plain seated idle, exactly like the real client:
                        // player models ship NO seated eating animation - EmoteEat is
                        // authored standing (root bone stays at stand height) and
                        // looping it while seated pops the model upright. Only
                        // override the seated idle for models that truly have the
                        // dedicated EatingLoop sequence.
                        ac->setSeatedLoopAnimation(
                            pickFirst({rendering::anim::EATING_LOOP}));
                    }
                }
                // No swig sound here. A potion's or a drink's gulp is its cast
                // kit's: Healing Potion's visual 147 (kit 237) and the
                // alcohol/drink visual 101 (kit 438) both play "Drinking", and
                // the spell visual system plays cast kits on SPELL_GO. This
                // played the same file a second time.
            } else if (isMining) {
                appearanceComposer_.showMiningPick(true);
                uint32_t mineAnim = pickFirst({
                    rendering::anim::ATTACK_1H,
                    rendering::anim::EMOTE_WORK,
                    rendering::anim::EMOTE_USE_STANDING
                });
                if (auto* ac = renderer_.getAnimationController()) {
                    ac->startSpellCast(0, mineAnim ? mineAnim : rendering::anim::STAND, true, 0);
                }
            } else if (isBandage || isGathering) {
                uint32_t useStart = isChannel ? 0 : pickFirst({
                    rendering::anim::USE_STANDING_START,
                    rendering::anim::EMOTE_USE_STANDING_NO_SHEATHE,
                    rendering::anim::EMOTE_USE_STANDING
                });
                uint32_t useLoop = pickFirst({
                    rendering::anim::USE_STANDING_LOOP,
                    rendering::anim::EMOTE_USE_STANDING_NO_SHEATHE,
                    rendering::anim::EMOTE_USE_STANDING,
                    rendering::anim::CHANNEL_CAST_OMNI,
                    rendering::anim::READY_SPELL_OMNI,
                    rendering::anim::STAND
                });
                uint32_t useEnd = isChannel ? 0 : pickFirst({
                    rendering::anim::USE_STANDING_END,
                    rendering::anim::STAND
                });
                if (auto* ac = renderer_.getAnimationController()) {
                    ac->startSpellCast(useStart, useLoop ? useLoop : rendering::anim::STAND, true, useEnd);
                }
            } else if (isFishing && cr->hasAnimation(instanceId, rendering::anim::FISHING_LOOP)) {
                // Fishing is a one-shot pole cast followed by the channel idle. If the
                // player had weapons sheathed, move the equipped pole into their hand
                // before starting so the cast animation actually swings it.
                if (isLocalPlayer) {
                    if (appearanceComposer_.isWeaponsSheathed()) {
                        appearanceComposer_.setWeaponsSheathed(false, "fishing cast");
                        appearanceComposer_.loadEquippedWeapons();
                    }
                    appearanceComposer_.showFishingPole(true);
                    auto* ac = renderer_.getAnimationController();
                    if (ac) {
                        uint32_t castAnim = cr->hasAnimation(instanceId, rendering::anim::FISHING_CAST)
                            ? rendering::anim::FISHING_CAST : 0;
                        ac->startSpellCast(castAnim, rendering::anim::FISHING_LOOP, true, 0);
                    }
                } else {
                    entitySpawner_.setUnitCastHold(guid, rendering::anim::FISHING_LOOP);
                    cr->playAnimation(instanceId, rendering::anim::FISHING_LOOP, true);
                }
            } else {
            // Precast wind-up (one-shot, non-channels only)
            // From the precast kit's StartAnimID when the spell has a kit:
            // Frostbolt's and Fireball's name none and go straight to
            // ReadySpellDirected. SpellPrecast only for spells without kits.
            uint32_t precastAnim = 0;
            if (!isChannel) {
                precastAnim = kitAnims.hasPrecastKit
                    ? (kitAnims.precastStart && cr->hasAnimation(instanceId, kitAnims.precastStart)
                           ? kitAnims.precastStart : 0)
                    : pickFirst({rendering::anim::SPELL_PRECAST});
            }

            // Cast hold (looping while cast bar fills / channel active)
            uint32_t castAnim = 0;
            if (isChannel) {
                // Channel hold: prefer DIRECTED/OMNI based on spell target classification
                if (isDirected) {
                    castAnim = pickFirst({
                        rendering::anim::CHANNEL_CAST_DIRECTED,
                        rendering::anim::CHANNEL_CAST_OMNI,
                        rendering::anim::READY_SPELL_DIRECTED,
                        rendering::anim::SPELL
                    });
                } else {
                    // OMNI or AREA channels (Blizzard channel, Tranquility, etc.)
                    castAnim = pickFirst({
                        rendering::anim::CHANNEL_CAST_OMNI,
                        rendering::anim::CHANNEL_CAST_DIRECTED,
                        rendering::anim::READY_SPELL_OMNI,
                        rendering::anim::SPELL
                    });
                }
            } else {
                // Regular cast hold: READY_SPELL_DIRECTED/OMNI while cast bar fills
                if (isDirected) {
                    castAnim = pickFirst({
                        rendering::anim::READY_SPELL_DIRECTED,
                        rendering::anim::READY_SPELL_OMNI,
                        rendering::anim::SPELL_CAST_DIRECTED,
                        rendering::anim::SPELL_CAST,
                        rendering::anim::SPELL
                    });
                } else {
                    // OMNI (self-buff) or AREA (AoE targeting)
                    castAnim = pickFirst({
                        rendering::anim::READY_SPELL_OMNI,
                        rendering::anim::READY_SPELL_DIRECTED,
                        rendering::anim::SPELL_CAST_OMNI,
                        rendering::anim::SPELL_CAST,
                        rendering::anim::SPELL
                    });
                }
            }
            // TBC models may omit the legacy SPELL (2) sequence; idle is a
            // quieter fallback than asking the renderer to play a missing anim.
            if (castAnim == 0) castAnim = rendering::anim::STAND;

            // Finalization release (one-shot after cast completes)
            // Animation chosen by spell target type: AREA → SPELL_CAST_AREA,
            // DIRECTED → SPELL_CAST_DIRECTED, OMNI → SPELL_CAST_OMNI
            uint32_t finalizeAnim = 0;
            if (isLocalPlayer && !isChannel) {
                if (isArea) {
                    // Ground-targeted AoE: SPELL_CAST_AREA → SPELL_CAST_OMNI
                    finalizeAnim = pickFirst({
                        rendering::anim::SPELL_CAST_AREA,
                        rendering::anim::SPELL_CAST_OMNI,
                        rendering::anim::SPELL_CAST,
                        rendering::anim::SPELL
                    });
                } else if (isDirected) {
                    // Single-target: SPELL_CAST_DIRECTED → SPELL_CAST_OMNI
                    finalizeAnim = pickFirst({
                        rendering::anim::SPELL_CAST_DIRECTED,
                        rendering::anim::SPELL_CAST_OMNI,
                        rendering::anim::SPELL_CAST,
                        rendering::anim::SPELL
                    });
                } else {
                    // OMNI (self-buff, Arcane Explosion): SPELL_CAST_OMNI → SPELL_CAST_AREA
                    finalizeAnim = pickFirst({
                        rendering::anim::SPELL_CAST_OMNI,
                        rendering::anim::SPELL_CAST_AREA,
                        rendering::anim::SPELL_CAST,
                        rendering::anim::SPELL
                    });
                }
            }

            // The kits' own animations over the guesses above.
            if (isChannel ? kitAnims.channel : kitAnims.precast)
                castAnim = isChannel ? kitAnims.channel : kitAnims.precast;
            if (finalizeAnim && kitAnims.cast) finalizeAnim = kitAnims.cast;

            if (isLocalPlayer) {
                auto* ac = renderer_.getAnimationController();
                if (ac) ac->startSpellCast(precastAnim, castAnim, true, finalizeAnim);
            } else {
                // Held until the cast resolves: unitIdleAnim answers with it,
                // so a flinch or the Stand/Ready switch comes back to it.
                if (castAnim != rendering::anim::STAND) entitySpawner_.setUnitCastHold(guid, castAnim);
                cr->playAnimation(instanceId, castAnim, true);
            }
            } // end !isFishing
        } else {
            // Cast/channel ended - plays finalization anim completely then returns to idle
            if (isLocalPlayer) {
                appearanceComposer_.showMiningPick(false);
                appearanceComposer_.showFishingPole(false);
                auto* ac = renderer_.getAnimationController();
                if (ac) {
                    ac->setSeatedLoopAnimation(0);
                    // isChannel doubles as "hard stop", as for other units
                    // below: a cast that did not go off (failed, interrupted,
                    // cancelled), a channel's end, a death - the pose is put
                    // away (0x7FEC00) with no release. Only a cast that went
                    // off (SMSG_SPELL_GO) plays its release one-shot.
                    if (isChannel) ac->cancelSpellCast();
                    else ac->stopSpellCast();
                }
            } else {
                // Another unit leaving its cast hold. Only a cast animation is
                // replaced, so a unit that has since died, run or swung keeps
                // that. isChannel doubles as "hard stop" (failure, timeout,
                // channel end): straight to its idle. A finished cast plays
                // its one-shot release, then the idle - the weapon's Ready
                // stance in combat, as between casts in the stock client.
                // Before, only channels were stopped and every remote cast
                // looped its hold until something else overwrote it.
                using namespace rendering::anim;
                entitySpawner_.setUnitCastHold(guid, 0);
                uint32_t cur = 0; float t = 0.0f, dur = 0.0f;
                if (cr->getAnimationState(instanceId, cur, t, dur) &&
                    (cur == READY_SPELL_DIRECTED || cur == READY_SPELL_OMNI ||
                     cur == CHANNEL_CAST_DIRECTED || cur == CHANNEL_CAST_OMNI ||
                     cur == SPELL_PRECAST || cur == SPELL || cur == SPELL_CAST ||
                     cur == SPELL_CAST_DIRECTED || cur == SPELL_CAST_OMNI ||
                     cur == SPELL_CAST_AREA || cur == FISHING_LOOP)) {
                    const uint32_t idle = entitySpawner_.unitIdleAnim(guid, instanceId);
                    uint32_t release = 0;
                    if (!isChannel) {
                        const uint32_t pref = kitAnims.cast ? kitAnims.cast
                                            : isArea ? SPELL_CAST_AREA
                                            : isDirected ? SPELL_CAST_DIRECTED : SPELL_CAST_OMNI;
                        for (uint32_t id : {pref, static_cast<uint32_t>(SPELL_CAST_OMNI),
                                            static_cast<uint32_t>(SPELL_CAST)})
                            if (cr->hasAnimation(instanceId, id)) { release = id; break; }
                    }
                    if (release) cr->playAnimation(instanceId, release, /*loop=*/false, idle);
                    else         cr->playAnimation(instanceId, idle, true);
                }
            }
        }
    });

    // Ghost state callback - make player semi-transparent when in spirit form
    gameHandler_.setGhostStateCallback([this](bool isGhost) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t charInstId = renderer_.getCharacterInstanceId();
        if (charInstId == 0) return;
        cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
    });

    // Stand state animation callback - route through AnimationController state machine
    // for proper sit/sleep/kneel transition animations (down → loop → up)
    gameHandler_.setStandStateCallback([this](uint8_t standState) {
        using AC = rendering::AnimationController;

        // The camera controller's two flags, from the one number: movement is
        // blocked while sitting or kneeling, and grounding stands down while
        // the server has the character in a chair.
        //
        // Both used to be set, by two callbacks - and this is one slot, so the
        // one registered later replaced the other. This handler is built long
        // after the world is, so the registration that carried
        // applyServerStandState never ran again: seatedInChair_ stayed false,
        // the chair stayed a floor, and a character sat down in the right pose
        // on top of the furniture rather than in it.
        if (auto* cc = renderer_.getCameraController()) {
            cc->applyServerStandState(standState);
        }

        auto* ac = renderer_.getAnimationController();
        if (!ac) return;

        // Death is special - play directly, not through sit state machine
        if (standState == AC::STAND_STATE_DEAD) {
            auto* cr = renderer_.getCharacterRenderer();
            if (!cr) return;
            uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId == 0) return;
            cr->playDeath(charInstId, /*alreadyDead=*/false);
            return;
        }
        // Up from a stand-state death while alive (a scripted one): lift the
        // lock. A real death is lifted by the respawn callback instead.
        if (!gameHandler_.isPlayerDead() || gameHandler_.isPlayerGhost()) {
            if (auto* cr = renderer_.getCharacterRenderer()) {
                const uint32_t charInstId = renderer_.getCharacterInstanceId();
                if (charInstId != 0) cr->clearDeath(charInstId);
            }
        }

        ac->setStandState(standState);
        // Restoration food/water is an aura rather than MSG_CHANNEL_START on
        // supported realms. The aura update starts its loop; standing up ends it.
        // Only stop the cast presentation while restoring - the server also
        // stands a seated player when a real cast begins, and stopping there
        // would kill that cast's animation as it starts.
        if (standState == AC::STAND_STATE_STAND) {
            ac->setSeatedLoopAnimation(0);
            if (gameHandler_.isRestoring()) ac->stopSpellCast();
        }
    });

    // Loot window callback - play kneel/loot animation while looting
    gameHandler_.setLootWindowCallback([this](bool open) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        if (open) ac->startLooting();
        else      ac->stopLooting();
    });
}

double AnimationCallbackHandler::nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {
using HitReaction = game::GameHandler::HitReaction;
bool isDefence(HitReaction r) {
    return r == HitReaction::DODGE || r == HitReaction::DEFLECT || r == HitReaction::PARRY ||
           r == HitReaction::BLOCK || r == HitReaction::SHIELD_BLOCK;
}
} // namespace

// The pending blow acted out by the attacker's animation events, as the stock
// client's handler for them does (Wow.exe 0x756240):
//   $CPP  the victim's defence - parry, dodge (a deflect too) or, with a
//         blocked amount, ShieldBlock - and HITINFO_AFFECTS_VICTIM is cleared
//         from the blow, so the blow then makes no flinch;
//   $CAH  after a miss, a dodge or an evade the rest of the swing runs at half
//         speed; then the victim side (0x755E40), whose flinch (0x736640) is
//         for any blow still carrying HITINFO_AFFECTS_VICTIM. AzerothCore sets
//         that bit on every swing that is not a miss, so a defence whose $CPP
//         never came - an attack animation without the event - is a flinch.
void AnimationCallbackHandler::onMeleeAnimEvent(uint32_t attackerInstanceId, uint32_t eventTag) {
    auto it = pendingReactions_.find(attackerInstanceId);
    if (it == pendingReactions_.end()) return;
    PendingReaction& p = it->second;
    const auto reaction = static_cast<HitReaction>(p.reaction);
    if (eventTag == pipeline::m2EventTag("$CPP")) {
        if (isDefence(reaction) && !p.defended) {
            p.defended = true;
            playHitReaction(p.victimGuid, p.reaction);
        }
        return;
    }
    if (eventTag != pipeline::m2EventTag("$CAH")) return;
    const PendingReaction done = p;
    pendingReactions_.erase(it);
    if (reaction == HitReaction::MISS || reaction == HitReaction::DODGE)
        slowAttackerSwing(attackerInstanceId);
    if (reaction == HitReaction::MISS || done.defended) return;
    if (isDefence(reaction)) {
        playHitReaction(done.victimGuid, static_cast<uint8_t>(HitReaction::WOUND));
        return;
    }
    playHitReaction(done.victimGuid, done.reaction);
}

void AnimationCallbackHandler::flushPendingReaction(const PendingReaction& p) {
    // Not the stock client's: its blow without events shows only its combat
    // text (0x756180). Here an event can also be missed by a model swapped
    // or culled mid-swing, so the reaction still plays, late.
    if (p.defended || static_cast<HitReaction>(p.reaction) == HitReaction::MISS) return;
    playHitReaction(p.victimGuid, p.reaction);
}

void AnimationCallbackHandler::slowAttackerSwing(uint32_t attackerInstanceId) {
    auto* cr = renderer_.getCharacterRenderer();
    if (!cr) return;
    // 0x735CC0 slows the model's whole-body track. Only when that track is
    // the swing: a swing on the upper body of a running unit leaves the run
    // at its own pace.
    if (!cr->currentAnimHasEvent(attackerInstanceId, pipeline::m2EventTag("$CAH"))) return;
    if (attackerInstanceId == renderer_.getCharacterInstanceId()) {
        if (auto* ac = renderer_.getAnimationController()) ac->slowMeleeSwing(0.5f);
        return;
    }
    cr->setAnimationSpeed(attackerInstanceId, 0.5f);
}

void AnimationCallbackHandler::updatePendingReactions() {
    if (pendingReactions_.empty()) return;
    const double now = nowMs();
    for (auto it = pendingReactions_.begin(); it != pendingReactions_.end();) {
        if (now < it->second.deadlineMs) { ++it; continue; }
        const PendingReaction p = it->second;
        it = pendingReactions_.erase(it);
        flushPendingReaction(p);
    }
}

void AnimationCallbackHandler::playHitReaction(uint64_t victimGuid, uint8_t reactionCode) {
    auto* cr = renderer_.getCharacterRenderer();
    if (!cr) return;
    using namespace rendering::anim;
    using HR = game::GameHandler::HitReaction;
    const HR reaction = static_cast<HR>(reactionCode);
    const bool isLocalPlayer = (victimGuid == gameHandler_.getPlayerGuid());
    auto* ac = renderer_.getAnimationController();

    uint32_t instanceId = 0;
    if (isLocalPlayer) {
        instanceId = renderer_.getCharacterInstanceId();
    } else {
        instanceId = entitySpawner_.getCreatureInstanceId(victimGuid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(victimGuid);
    }
    if (instanceId == 0) return;

    // The parry and the block are the victim's weapons': Parry1H/2H/2HL/
    // Unarmed for what parried it, ShieldBlock for a shield (only a shield
    // blocks; Block for a model without ShieldBlock). A dodge (and a deflect)
    // is Dodge (Wow.exe 0x756240 at $CPP: victim state 2 or 8).
    const rendering::WeaponLoadout weapons = isLocalPlayer
        ? (ac ? ac->getWeaponLoadout() : rendering::WeaponLoadout{})
        : entitySpawner_.unitWeaponLoadout(victimGuid);
    // The flinch is the victim's own business too (0x736640): a crit's
    // CombatCritical; CombatWound while it is attacking something itself,
    // StandWound while it is not - a character hit while standing at ease
    // flinches at ease rather than in its combat stance.
    const game::Unit* victim = gameHandler_.getUnitByGuid(victimGuid);
    const bool victimAttacking = victim && victim->getAttackTarget() != 0;
    // Weapons put away parry bare-handed (0x73B050 reads UNIT_FIELD_BYTES_2
    // byte 0). The local character's byte is its own and always current;
    // for anyone else it is what is shown, since AzerothCore leaves the byte
    // at 0 for units that draw their weapons without saying so (playerbots).
    bool sheathed = false;
    if (reaction == HR::PARRY) {
        sheathed = isLocalPlayer
            ? entitySpawner_.unitSheathState(victimGuid) == 0
            : entitySpawner_.presentedSheathState(victimGuid, instanceId) == 0;
    }
    uint32_t want = COMBAT_WOUND;
    switch (reaction) {
        case HR::MISS:         return;
        case HR::DODGE:
        case HR::DEFLECT:      want = DODGE; break;
        case HR::PARRY:        want = sheathed ? PARRY_UNARMED : parryAnim(weapons); break;
        case HR::BLOCK:
        case HR::SHIELD_BLOCK: want = SHIELD_BLOCK; break;
        case HR::CRIT_WOUND:   want = woundAnim(true, victimAttacking); break;
        case HR::WOUND:        want = woundAnim(false, victimAttacking); break;
    }
    auto has = [&](uint32_t id) { return cr->hasAnimation(instanceId, id); };
    uint32_t animId = 0;
    if (want == SHIELD_BLOCK)       animId = firstPlayable(has, {SHIELD_BLOCK, BLOCK});
    else if (want == STAND_WOUND)   animId = firstPlayable(has, {STAND_WOUND, COMBAT_WOUND});
    else if (want == COMBAT_WOUND)  animId = firstPlayable(has, {COMBAT_WOUND, STAND_WOUND});
    else                            animId = firstPlayable(has, {want});
    if (animId == 0) return;

    // Which track (0x736640). Bound for the upper body: a unit fighting
    // something (CombatWound, a parry, a block), a crit, a moving or casting
    // unit - so a fighter's swing goes on under the flinch and the next swing
    // does not cut it off. But over Stand or a Ready stance the whole body
    // takes it (0x7366B6, see woundOnUpperBody): a mob standing in its stance
    // between swings flinches whole. Played on the upper body there, the
    // flinch went over a stance whose upper half is most of the model, and
    // over a model with no upper-body bones (no SpineLow key bone) did
    // nothing at all - Jered's sword hits made no flinch.
    uint32_t cur = 0; float t = 0.0f, d = 0.0f;
    const bool haveState = cr->getAnimationState(instanceId, cur, t, d);
    if (haveState && cur == DEATH) return;
    const bool upperTrack = woundOnUpperBody(reaction == HR::CRIT_WOUND, victimAttacking, false, false,
                                             haveState ? cur : STAND);

    // For local player: use AnimationController state
    if (isLocalPlayer) {
        if (ac) ac->triggerHitReaction(animId, upperTrack);
        return;
    }

    if (haveState && (upperTrack || isLocomotionAnim(cur) || isSpellCastFamily(cur))) {
        if (playsOnUpperBody(animId)) cr->playUpperBodyAnimation(instanceId, animId, animId, animId);
        return;
    }
    const bool busy = haveState &&
        ((cur >= ATTACK_UNARMED && cur <= ATTACK_2H_LOOSE) ||
         (cur >= SPECIAL_1H && cur <= SHIELD_BASH) ||
         (cur >= ATTACK_1H_PIERCE && cur <= ATTACK_OFF_PIERCE) ||
         cur == ATTACK_UNARMED_OFF || cur == SPECIAL_UNARMED || cur == WHIRLWIND ||
         cur == KICK || cur == MUTILATE || cur == DEATH ||
         isSpellCastFamily(cur));
    if (!busy)
        cr->playAnimation(instanceId, animId, false, entitySpawner_.unitIdleAnim(victimGuid, instanceId));
}

}} // namespace wowee::core
