#include "game/combat_handler.hpp"
#include "game/global_strings.hpp"
#include "game/combat_log_event.hpp"
#include "game/stock_combat_log.hpp"
#include "game/group_defines.hpp"
#include "game/classic_combat_chat.hpp"
#include "audio/ui_sound_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/combat_text_filter.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/protocol_constants.hpp"
#include "game/tab_target.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <random>

namespace wowee {
namespace game {

namespace {
/// Who a unit is to the player, in the terms 1.12 splits its combat chat by.
/// Another player counts as friendly unless the client knows otherwise, as
/// the combat log event's flags take them.
CombatRelation combatRelationOf(GameHandler& gh, uint64_t guid) {
    if (guid == 0) return CombatRelation::Creature;
    if (guid == gh.getPlayerGuid()) return CombatRelation::Self;
    if (guid == gh.getPetGuid()) return CombatRelation::Pet;
    if (gh.isInGroup()) {
        for (const auto& m : gh.getPartyData().members) {
            if (m.guid == guid) return CombatRelation::Party;
        }
    }
    if ((guid >> 48) != 0) return CombatRelation::Creature;
    const Unit* unit = gh.getUnitByGuid(guid);
    return (unit && gh.unitReactionToPlayer(*unit) <= 3)
        ? CombatRelation::HostilePlayer : CombatRelation::FriendlyPlayer;
}

/// The unit's attack target, which holds it in its Ready stance with its
/// weapons drawn (CGUnit_C +0xA20 in Wow.exe). The local player's is set the
/// moment it asks to attack, as CGPlayer_C::StartAttack (0x6E2610) does,
/// rather than a round trip later on SMSG_ATTACKSTART.
void setUnitAttackTarget(GameHandler& gh, uint64_t unitGuid, uint64_t targetGuid) {
    if (unitGuid == 0) return;
    if (auto entity = gh.getEntityManager().getEntity(unitGuid); entity && entity->isUnit())
        std::static_pointer_cast<Unit>(entity)->setAttackTarget(targetGuid);
}
}  // namespace

CombatHandler::CombatHandler(GameHandler& owner)
    : owner_(owner) {}

/// The same rule the threat indicator uses, kept in step with it.
///
/// threatWarning is 0 never, 1 in instances, 2 in a party, 3 always - and
/// IsThreatWarningEnabled answers the interface from exactly that. Reading it
/// here rather than inventing a second rule is what stops the sound and the
/// picture disagreeing about when a warning is wanted.
bool CombatHandler::threatWarningWanted() const {
    int setting = 3;
    const std::string stored = addons::storedCVarValue("threatWarning", "3");
    try { setting = std::stoi(stored); } catch (const std::exception&) {}
    switch (setting) {
        case 1:  return owner_.isInInstance();
        case 2:  return !owner_.getPartyData().members.empty();
        case 3:  return true;
        default: return false;   // 0, and anything unrecognised
    }
}

void CombatHandler::registerOpcodes(DispatchTable& table) {
    // ---- Combat clearing ----
    table[Opcode::SMSG_ATTACKSWING_DEADTARGET] = [this](network::Packet& /*packet*/) {
        // The stock client stops the attack outright here (Wow.exe 0x756800
        // -> CGPlayer_C::StopAttack 0x6E1660): no retry, no Ready stance.
        setUnitAttackTarget(owner_, owner_.getPlayerGuid(), 0);
        autoAttacking_ = false;
        autoAttackRequested_ = false;
        autoAttackRetryPending_ = false;
        autoAttackTarget_ = 0;
    };
    // A list emptied or shortened changes the standing of whoever was on it,
    // and the unit frames' threat indicators only look again when told so by
    // name. Without this a glow lit during a fight stayed lit after it.
    auto fireSituation = [this](uint64_t mobGuid, const std::vector<uint64_t>& victims) {
        if (!owner_.addonEventCallbackRef()) return;
        auto fire = owner_.addonEventCallbackRef();
        const std::string mob = owner_.guidToUnitId(mobGuid);
        fire("UNIT_THREAT_LIST_UPDATE", {mob});
        for (uint64_t v : victims) {
            const std::string who = owner_.guidToUnitId(v);
            if (!who.empty()) fire("UNIT_THREAT_SITUATION_UPDATE", {who});
        }
        if (!mob.empty()) fire("UNIT_THREAT_SITUATION_UPDATE", {mob});
    };
    // One mob's list, by its packed guid - not every list there is.
    table[Opcode::SMSG_THREAT_CLEAR] = [this, fireSituation](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const uint64_t unitGuid = packet.readPackedGuid();
        std::vector<uint64_t> victims;
        if (auto it = threatLists_.find(unitGuid); it != threatLists_.end()) {
            for (const ThreatEntry& e : it->second) victims.push_back(e.victimGuid);
            threatLists_.erase(it);
        }
        playerTopThreatOn_.erase(unitGuid);
        playerOverTankOn_.erase(unitGuid);
        threatVictims_.erase(unitGuid);
        fireSituation(unitGuid, victims);
    };
    table[Opcode::SMSG_THREAT_REMOVE] = [this, fireSituation](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint64_t unitGuid   = packet.readPackedGuid();
        if (!packet.hasRemaining(1)) return;
        uint64_t victimGuid = packet.readPackedGuid();
        auto it = threatLists_.find(unitGuid);
        if (it != threatLists_.end()) {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                [victimGuid](const ThreatEntry& e){ return e.victimGuid == victimGuid; }),
                list.end());
            if (list.empty()) threatLists_.erase(it);
        }
        if (victimGuid == owner_.getPlayerGuid()) {
            playerTopThreatOn_.erase(unitGuid);
            playerOverTankOn_.erase(unitGuid);
        }
        fireSituation(unitGuid, {victimGuid});
    };
    table[Opcode::SMSG_CANCEL_COMBAT] = [this](network::Packet& /*packet*/) {
        setUnitAttackTarget(owner_, owner_.getPlayerGuid(), 0);
        autoAttacking_ = false;
        autoAttackTarget_ = 0;
        autoAttackRequested_ = false;
        autoAttackRetryPending_ = false;
        hostileAttackers_.clear();
    };

    // ---- Attack/combat delegates ----
    table[Opcode::SMSG_ATTACKSTART] = [this](network::Packet& packet) { handleAttackStart(packet); };
    table[Opcode::SMSG_ATTACKSTOP] = [this](network::Packet& packet) { handleAttackStop(packet); };
    table[Opcode::SMSG_ATTACKSWING_NOTINRANGE] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = true;
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError("Target is too far away.");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_BADFACING] = [this](network::Packet& /*packet*/) {
        // Reported, not corrected. This used to turn the character to face the
        // target, which is not something the server may do to a player: the
        // facing is the player's. Running away from something with auto-attack
        // still on had the server say "not facing" every swing timer and the
        // character spin round to face it each time, fighting the player for
        // control of their own heading.
        //
        // The real client says so and leaves the heading alone - the swing
        // simply does not land until the player turns back.
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError("You are facing the wrong way!");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_NOTSTANDING] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = false;
        autoAttackOutOfRangeTime_ = 0.0f;
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError("You need to stand up to fight.");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_CANT_ATTACK] = [this](network::Packet& /*packet*/) {
        stopAutoAttack();
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError("You can't attack that.");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKERSTATEUPDATE] = [this](network::Packet& packet) { handleAttackerStateUpdate(packet); };
    table[Opcode::SMSG_AI_REACTION] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) return;
        uint64_t guid = packet.readUInt64();
        uint32_t reaction = packet.readUInt32();
        if (reaction == 2 && owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(guid);
            if (entity)
                owner_.npcAggroCallbackRef()(guid, glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        }
    };
    table[Opcode::SMSG_SPELLNONMELEEDAMAGELOG] = [this](network::Packet& packet) { handleSpellDamageLog(packet); };
    table[Opcode::SMSG_SPELLHEALLOG] = [this](network::Packet& packet) { handleSpellHealLog(packet); };

    // ---- Environmental damage ----
    table[Opcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG] = [this](network::Packet& packet) {
        // uint64 victimGuid + uint8 envDmgType + uint32 damage + uint32 absorbed + uint32 resisted
        // envDmgType: 0=Exhausted(fatigue), 1=Drowning, 2=Fall, 3=Lava, 4=Slime, 5=Fire
        if (!packet.hasRemaining(21)) { packet.skipAll(); return; }
        uint64_t victimGuid  = packet.readUInt64();
        uint8_t envType      = packet.readUInt8();
        uint32_t dmg         = packet.readUInt32();
        uint32_t envAbs      = packet.readUInt32();
        uint32_t envRes      = packet.readUInt32();
        // 0x756800 -> 0x755270 -> 0x751150, for any unit the client has.
        if (stockCombatLogActive() && owner_.getEntityManager().getEntity(victimGuid)) {
            if (auto r = stock_combat_log::buildEnvironmental(
                    victimGuid, envType, static_cast<int32_t>(dmg), static_cast<int32_t>(envAbs),
                    static_cast<int32_t>(envRes)))
                logCombatEvent(*r);
        }
        if (victimGuid == owner_.getPlayerGuid()) {
            // Environmental damage: pass envType via powerType field for display differentiation
            if (dmg > 0)
                addCombatText(CombatTextEntry::ENVIRONMENTAL, static_cast<int32_t>(dmg), 0, false, envType, 0, victimGuid);
            if (envAbs > 0)
                addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(envAbs), 0, false, 0, 0, victimGuid);
            if (envRes > 0)
                addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(envRes), 0, false, 0, 0, victimGuid);
            // Lava, fire and slime are magical schools to the addon; falling,
            // drowning and fatigue are plain damage.
            const bool envMagical = envType == 3 || envType == 4 || envType == 5;
            owner_.fireCombatTextUpdate(victimGuid,
                combatTextUpdateForHit(envMagical, false, dmg, envRes, 0, envAbs));
            // Drowning damage → play DROWN one-shot on player
            if (envType == 1 && dmg > 0 && owner_.emoteAnimCallbackRef())
                owner_.emoteAnimCallbackRef()(victimGuid, 131, /*isState=*/false); // anim::DROWN
        }
        packet.skipAll();
    };

    // ---- Threat updates ----
    // The two do NOT share a format, though this read them as if they did.
    // Unit::SendThreatListUpdate writes the unit's packed guid and then the
    // count; SendChangeCurrentVictimOpcode writes the unit, *then the new
    // highest-threat target's packed guid*, and then the count. Reading the
    // second guid from both meant the plain update consumed its count as a
    // guid and then read the first victim's guid as the count, so the common
    // one of the two - the one sent on every threat change - produced a list
    // built from nothing.
    for (auto op : {Opcode::SMSG_HIGHEST_THREAT_UPDATE,
                    Opcode::SMSG_THREAT_UPDATE}) {
        const bool hasVictimGuid = (op == Opcode::SMSG_HIGHEST_THREAT_UPDATE);
        table[op] = [this, hasVictimGuid](network::Packet& packet) {
            if (!packet.hasRemaining(1)) return;
            uint64_t unitGuid = packet.readPackedGuid();
            if (hasVictimGuid) {
                if (!packet.hasRemaining(1)) return;
                // The new highest-threat target, which is the mob's victim:
                // AzerothCore sends this when the victim changes.
                threatVictims_[unitGuid] = packet.readPackedGuid();
            }
            if (!packet.hasRemaining(4)) return;
            uint32_t cnt = packet.readUInt32();
            if (cnt > 100) { packet.skipAll(); return; } // sanity
            std::vector<ThreatEntry> list;
            list.reserve(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                if (!packet.hasRemaining(1)) return;
                ThreatEntry entry;
                entry.victimGuid = packet.readPackedGuid();
                if (!packet.hasRemaining(4)) return;
                entry.threat = packet.readUInt32();
                list.push_back(entry);
            }
            // Sort descending by threat so highest is first
            std::sort(list.begin(), list.end(),
                [](const ThreatEntry& a, const ThreatEntry& b){ return a.threat > b.threat; });
            // Play Aggro Sounds, which the panel offers and nothing read -
            // here or in the interface. The threat indicator FrameXML draws is
            // silent by design; in the real client the warning noise is the
            // client's, which is why nothing in this tree ever made it.
            //
            // The sounds mark moments, not every update while they hold: the
            // list arrives on every threat change and a sound per packet would
            // be a siren. Both are Wow.exe's, by id, under threatPlaySounds.
            //
            // The tank is the mob's victim - the target SMSG_HIGHEST_THREAT_UPDATE
            // names, then the mob's own target field, then the head of the
            // list. Not simply the head of the list: a mob keeps its victim
            // until someone passes 110% (130% at range) of the victim's
            // threat, and the warning is for the stretch in between.
            const uint64_t me = owner_.getPlayerGuid();
            uint64_t tank = 0;
            if (auto vIt = threatVictims_.find(unitGuid); vIt != threatVictims_.end()) tank = vIt->second;
            if (tank == 0) tank = unitTargetGuid(owner_.getEntityManager().getEntity(unitGuid));
            if (tank == 0 && !list.empty()) tank = list.front().victimGuid;
            uint32_t myThreat = 0, tankThreat = 0;
            bool onList = false;
            for (const ThreatEntry& e : list) {
                if (e.victimGuid == me) { myThreat = e.threat; onList = true; }
                if (e.victimGuid == tank) tankThreat = e.threat;
            }
            const bool soundsWanted = threatWarningWanted() &&
                                      addons::storedCVarValue("threatPlaySounds", "0") != "0";

            // Aggro_Pulled_Aggro (15263): the player becomes the tank (Wow.exe's
            // threat update, 0x737620). Error.wav played here was no row's.
            const bool tankNow = onList && tank == me;
            const bool tankBefore = playerTopThreatOn_.count(unitGuid) != 0;
            if (tankNow != tankBefore) {
                if (tankNow) playerTopThreatOn_.insert(unitGuid);
                else         playerTopThreatOn_.erase(unitGuid);
                if (tankNow && soundsWanted) owner_.playUiSoundEntry(15263);
            }

            // Aggro_Enter_Warning_State (15262): the player, not tanking,
            // reaches 100% of the tank's threat from below (Wow.exe's threat
            // entry update, 0x71c3b0, on its status going from 1 to 2).
            const bool overNow = onList && !tankNow && tankThreat > 0 && myThreat >= tankThreat;
            const bool overBefore = playerOverTankOn_.count(unitGuid) != 0;
            if (overNow != overBefore) {
                if (overNow) playerOverTankOn_.insert(unitGuid);
                else         playerOverTankOn_.erase(unitGuid);
                if (overNow && soundsWanted) owner_.playUiSoundEntry(15262);
            }

            threatLists_[unitGuid] = std::move(list);
            if (!owner_.addonEventCallbackRef()) return;
            auto fire = owner_.addonEventCallbackRef();
            // The list changed, and separately: whether each unit on it is
            // tanking. They are different events and unit frames read the
            // second one - UnitFrameThreatIndicator_OnEvent is registered for
            // UNIT_THREAT_SITUATION_UPDATE alone, and it is what puts the aggro
            // border on a frame. It was never fired, so no frame ever showed
            // one, and the list it would have been read from was misparsed
            // anyway.
            fire("UNIT_THREAT_LIST_UPDATE", {owner_.guidToUnitId(unitGuid)});
            // Named per unit, because the indicator compares the argument
            // against its own frame's unit and ignores anything else. Every
            // unit on the list that the interface has a token for, plus the mob
            // itself - a target frame's indicator is fed by the player's
            // situation against the target, so both ends have to be told.
            for (const ThreatEntry& e : threatLists_[unitGuid]) {
                const std::string who = owner_.guidToUnitId(e.victimGuid);
                if (!who.empty()) fire("UNIT_THREAT_SITUATION_UPDATE", {who});
            }
            if (const std::string mob = owner_.guidToUnitId(unitGuid); !mob.empty())
                fire("UNIT_THREAT_SITUATION_UPDATE", {mob});
        };
    }

    // ---- Forced faction reactions ----
    table[Opcode::SMSG_SET_FORCED_REACTIONS] = [this](network::Packet& packet) { handleSetForcedReactions(packet); };

    // ---- Entity delta updates: health / power / combo / PvP / proc ----
    table[Opcode::SMSG_HEALTH_UPDATE] = [this](network::Packet& p) { handleHealthUpdate(p); };
    table[Opcode::SMSG_POWER_UPDATE] = [this](network::Packet& p) { handlePowerUpdate(p); };
    table[Opcode::SMSG_UPDATE_COMBO_POINTS] = [this](network::Packet& p) { handleUpdateComboPoints(p); };
    table[Opcode::SMSG_PVP_CREDIT] = [this](network::Packet& p) { handlePvpCredit(p); };
    table[Opcode::SMSG_PROCRESIST] = [this](network::Packet& p) { handleProcResist(p); };

    // SMSG_ENVIRONMENTALDAMAGELOG is an alias for SMSG_ENVIRONMENTAL_DAMAGE_LOG
    // (registered above at line 108 with envType forwarding). No separate handler needed.
    table[Opcode::SMSG_SPELLDAMAGESHIELD] = [this](network::Packet& p) { handleSpellDamageShield(p); };
    table[Opcode::SMSG_SPELLORDAMAGE_IMMUNE] = [this](network::Packet& p) { handleSpellOrDamageImmune(p); };
    table[Opcode::SMSG_RESISTLOG] = [this](network::Packet& p) { handleResistLog(p); };

    // ---- Pet feedback ----
    table[Opcode::SMSG_PET_TAME_FAILURE] = [this](network::Packet& p) { handlePetTameFailure(p); };
    table[Opcode::SMSG_PET_ACTION_FEEDBACK] = [this](network::Packet& p) { handlePetActionFeedback(p); };
    table[Opcode::SMSG_PET_CAST_FAILED] = [this](network::Packet& p) { handlePetCastFailed(p); };
    table[Opcode::SMSG_PET_BROKEN] = [this](network::Packet& p) { handlePetBroken(p); };
    table[Opcode::SMSG_PET_LEARNED_SPELL] = [this](network::Packet& p) { handlePetLearnedSpell(p); };
    table[Opcode::SMSG_PET_UNLEARNED_SPELL] = [this](network::Packet& p) { handlePetUnlearnedSpell(p); };
    table[Opcode::SMSG_PET_MODE] = [this](network::Packet& p) { handlePetMode(p); };

    // ---- Resurrect ----
    table[Opcode::SMSG_RESURRECT_FAILED] = [this](network::Packet& p) { handleResurrectFailed(p); };
}

// ============================================================
// Auto-attack
// ============================================================

void CombatHandler::startAutoAttack(uint64_t targetGuid) {
    // Can't attack yourself
    if (targetGuid == owner_.getPlayerGuid()) return;
    if (targetGuid == 0) return;

    // Assist Attack: swinging at a friendly unit attacks whatever they are
    // fighting instead. The panel offers this and nothing read it, so an
    // attack on an ally was simply sent and refused by the server - a tank
    // pressing attack with a healer selected got nothing and no reason for it.
    //
    // Only when the friend has a target of their own, and only when that
    // target is hostile: assisting somebody who is fighting nothing, or who
    // has a friendly unit selected, would turn one refusal into another.
    if (addons::storedCVarValue("assistAttack", "0") != "0") {
        auto entity = owner_.getEntityManager().getEntity(targetGuid);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            if (!unit->isHostile() && !isAggressiveTowardPlayer(targetGuid)) {
                const uint64_t theirTarget = unitTargetGuid(entity);
                auto their = owner_.getEntityManager().getEntity(theirTarget);
                auto* theirUnit = dynamic_cast<Unit*>(their.get());
                if (theirUnit && (theirUnit->isHostile() ||
                                  isAggressiveTowardPlayer(theirTarget))) {
                    targetGuid = theirTarget;
                } else {
                    return;   // nothing of theirs worth swinging at
                }
            }
        }
    }

    // No range check. The stock client sends CMSG_ATTACKSWING for a target at
    // any distance (CGPlayer_C::StartAttack, Wow.exe 0x6E2610) and draws the
    // weapons and takes the Ready stance at once; the server keeps the attack
    // on and answers SMSG_ATTACKSWING_NOTINRANGE until the player closes in,
    // when the first swing lands without another click. Refusing here beyond
    // 8 yards meant a right-click on a wolf across the clearing did nothing
    // but select it, and the player had to click again on arrival.

    // No dismount. The stock client asks to attack from the saddle as from
    // the ground (StartAttack 0x6E2610 and its caller 0x6E4950 never touch
    // the mount); the server refuses a mounted attacker (AzerothCore
    // Unit::Attack) and answers SMSG_ATTACKSTOP, which ends the attack. The
    // player gets off the mount to fight. Wowee used to dismount here.

    autoAttackRequested_ = true;
    autoAttackRetryPending_ = true;
    // Keep combat animation/state server-authoritative. We only flip autoAttacking
    // on SMSG_ATTACKSTART where attackerGuid == playerGuid.
    autoAttacking_ = false;
    autoAttackTarget_ = targetGuid;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    // Face the target, once, on the command to attack it. This is the only
    // place combat turns the player: it used to be re-aimed every fifth of a
    // second for as long as the attack lasted.
    //
    // Only with the Combat page's debug setting on. The original client does
    // not turn the player at all; attacking something behind them is refused
    // until they face it themselves.
    auto target = owner_.isAutoFaceTarget() ? owner_.getEntityManager().getEntity(targetGuid)
                                            : nullptr;
    if (target) {
        const float toTargetX = target->getLatestX() - owner_.movementInfoRef().x;
        const float toTargetY = target->getLatestY() - owner_.movementInfoRef().y;
        // Through faceCanonicalYaw: the character's own facing turns with it,
        // or the per-frame facing sync turns it back with a SET_FACING of its
        // own on the next frame.
        if ((std::abs(toTargetX) > 0.01f || std::abs(toTargetY) > 0.01f) &&
            owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
            owner_.faceCanonicalYaw(std::atan2(-toTargetY, toTargetX));
        }
    }
    setUnitAttackTarget(owner_, owner_.getPlayerGuid(), targetGuid);
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackSwingPacket::build(targetGuid);
        owner_.getSocket()->send(packet);
    }
    LOG_INFO("Starting auto-attack on 0x", std::hex, targetGuid, std::dec);
}

void CombatHandler::stopAutoAttack() {
    if (!autoAttacking_ && !autoAttackRequested_) return;
    autoAttackRequested_ = false;
    autoAttacking_ = false;
    autoAttackRetryPending_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    setUnitAttackTarget(owner_, owner_.getPlayerGuid(), 0);
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackStopPacket::build();
        owner_.getSocket()->send(packet);
    }
    LOG_INFO("Stopping auto-attack");
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("PLAYER_LEAVE_COMBAT", {});
}

// ============================================================
// Combat text
// ============================================================

void CombatHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                  uint64_t srcGuid, uint64_t dstGuid) {
    // The panel's filters, before anything is built or announced. The mapping
    // lives in combat_text_filter.hpp so it can be tested without a settings
    // store; only the lookup is here.
    // The meter's numbers for the expansions with no 3.3.5a combat log to feed
    // it (logCombatEvent does for the rest). Ahead of the filter: hiding a
    // number from the screen is not leaving it out of the meter.
    if (!stockCombatLogActive()) {
        using T = CombatTextEntry;
        MeterEvent e;
        bool known = true;
        switch (type) {
            case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::CRIT_DAMAGE:
            case T::PERIODIC_DAMAGE: case T::GLANCING: case T::CRUSHING:
                e.kind = MeterEvent::Damage; break;
            case T::ENVIRONMENTAL:
                e.kind = MeterEvent::Damage; break;
            case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
                e.kind = MeterEvent::Heal; break;
            default: known = false; break;
        }
        if (known) {
            e.src = (srcGuid != 0) ? srcGuid
                  : (isPlayerSource ? owner_.getPlayerGuid() : 0);
            e.dst = (dstGuid != 0) ? dstGuid
                  : (isPlayerSource ? owner_.getTargetGuid() : owner_.getPlayerGuid());
            e.srcCombatant = meterCombatantOf(e.src);
            e.dstCombatant = isMeterCombatant(e.dst) ? e.dst : 0;
            e.spellId = type == T::ENVIRONMENTAL ? kMeterEnvironment : spellId;
            e.amount = amount;
            e.crit = type == T::CRIT_DAMAGE || type == T::CRIT_HEAL;
            damageMeter_.record(meterClock(), e);
        }
    }

    const auto rule = combatTextFilterFor(type, isPlayerSource, srcGuid, dstGuid,
                                          owner_.getPetGuid(), owner_.getTargetGuid());
    if (rule.cvar && addons::storedCVarValue(rule.cvar, rule.fallback) == "0") return;

    CombatTextEntry entry;
    entry.type = type;
    entry.amount = amount;
    entry.spellId = spellId;
    entry.age = 0.0f;
    entry.isPlayerSource = isPlayerSource;
    entry.powerType = powerType;
    entry.srcGuid = srcGuid;
    entry.dstGuid = dstGuid;
    // Random horizontal stagger so simultaneous hits don't stack vertically
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    entry.xSeed = dist(rng);
    combatText_.push_back(entry);

    // UNIT_COMBAT - the same hit, for the frames that draw their own feedback
    // over a portrait. This client renders the floating numbers itself from the
    // list above; the interface's unit frames render theirs from this event and
    // were never told, so a pet or target frame showed nothing while the world
    // filled with numbers.
    //
    // The tokens are not invented: CombatFeedback_OnCombatEvent enumerates the
    // ones it acts on, and everything else falls through its final else to a
    // blank. Only a unit the interface has a token for is worth announcing -
    // the frame compares the first argument against its own unit.
    if (dstGuid != 0 && owner_.addonEventCallbackRef()) {
        const std::string unitId = owner_.guidToUnitId(dstGuid);
        if (!unitId.empty()) {
            const char* action = nullptr;
            const char* flags  = "";
            switch (type) {
                case CombatTextEntry::MELEE_DAMAGE:
                case CombatTextEntry::SPELL_DAMAGE:
                case CombatTextEntry::PERIODIC_DAMAGE: action = "WOUND"; break;
                case CombatTextEntry::CRIT_DAMAGE: action = "WOUND"; flags = "CRITICAL"; break;
                case CombatTextEntry::GLANCING:    action = "WOUND"; flags = "GLANCING"; break;
                case CombatTextEntry::CRUSHING:    action = "WOUND"; flags = "CRUSHING"; break;
                case CombatTextEntry::ABSORB:      action = "WOUND"; flags = "ABSORB";   break;
                case CombatTextEntry::MISS:   action = "MISS";   break;
                case CombatTextEntry::DODGE:  action = "DODGE";  break;
                case CombatTextEntry::PARRY:  action = "PARRY";  break;
                case CombatTextEntry::BLOCK:  action = "BLOCK";  break;
                case CombatTextEntry::EVADE:  action = "EVADE";  break;
                case CombatTextEntry::IMMUNE: action = "IMMUNE"; break;
                case CombatTextEntry::RESIST: action = "RESIST"; break;
                case CombatTextEntry::HEAL:
                case CombatTextEntry::PERIODIC_HEAL: action = "HEAL"; break;
                case CombatTextEntry::CRIT_HEAL: action = "HEAL"; flags = "CRITICAL"; break;
                default: break;  // energize, xp, honour - not combat feedback
            }
            if (action) {
                owner_.addonEventCallbackRef()(
                    "UNIT_COMBAT",
                    {unitId, action, flags, std::to_string(amount), "0"});
            }
        }
    }

    // Persistent combat log - use explicit GUIDs if provided, else fall back to
    // player/current-target (the old behaviour for events without specific participants).
    CombatLogEntry log;
    log.type     = type;
    log.amount   = amount;
    log.spellId  = spellId;
    log.isPlayerSource = isPlayerSource;
    log.powerType = powerType;
    log.timestamp = std::time(nullptr);
    // If the caller provided an explicit destination GUID but left source GUID as 0,
    // preserve "unknown/no source" (e.g. environmental damage) instead of
    // backfilling from current target.
    uint64_t effectiveSrc = (srcGuid != 0) ? srcGuid
                          : ((dstGuid != 0) ? 0 : (isPlayerSource ? owner_.getPlayerGuid() : owner_.getTargetGuid()));
    uint64_t effectiveDst = (dstGuid != 0) ? dstGuid
                          : (isPlayerSource ? owner_.getTargetGuid() : owner_.getPlayerGuid());
    log.sourceName = owner_.lookupName(effectiveSrc);
    log.targetName = (effectiveDst != 0) ? owner_.lookupName(effectiveDst) : std::string{};
    // Held for the addon event below. log is moved into combatLog_ here, which
    // leaves both name strings empty, so reading them afterwards sent every
    // COMBAT_LOG_EVENT_UNFILTERED to Lua with a blank source and target.
    const std::string sourceName = log.sourceName;
    const std::string targetName = log.targetName;
    if (combatLog_.size() >= MAX_COMBAT_LOG)
        combatLog_.pop_front();
    combatLog_.push_back(std::move(log));
    ++combatLogAppended_;

    // The combat log's own events, in the layout Blizzard_CombatLog parses.
    // The unfiltered one is what addons listen for; the filtered one is what the
    // log window itself is registered to, and nothing fired it, so the window
    // was empty however much fighting there was. No filter is applied here: the
    // window applies its own settings to what it is given.
    // 1.12 has no combat log event, so Classic and Turtle are not sent one.
    // 3.3.5a's are built from the packets themselves (logCombatEvent); this
    // older shape is TBC's alone.
    if (owner_.addonEventCallbackRef() && !isClassicLikeExpansion() && !stockCombatLogActive()) {
        CombatLogEventInput input;
        input.layout = isActiveExpansion("tbc") ? CombatLogLayout::Tbc : CombatLogLayout::Wotlk;
        input.type = type;
        input.amount = amount;
        input.spellId = spellId;
        input.powerType = powerType;
        input.srcGuid = effectiveSrc;
        input.dstGuid = effectiveDst;
        input.playerGuid = owner_.getPlayerGuid();
        input.petGuid = owner_.getPetGuid();
        input.srcName = sourceName;
        input.dstName = targetName;
        input.spellName = (spellId != 0) ? owner_.getSpellName(spellId) : std::string{};
        input.timestamp = static_cast<int64_t>(std::time(nullptr));
        const std::vector<std::string> args = buildCombatLogEvent(input);
        if (!args.empty()) {
            // COMBAT_LOG_EVENT is fired by LuaEngine, through the filters.
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", args);
        }
    }

    // 1.12's combat log is chat: a sentence on one of its CHAT_MSG_COMBAT_*
    // and CHAT_MSG_SPELL_* events, which the Combat Log tab and the damage
    // meters written for 1.12 both read.
    if (owner_.addonEventCallbackRef() && isClassicLikeExpansion()) {
        ClassicCombatInput in;
        in.type = type;
        in.amount = amount;
        in.powerType = powerType;
        in.schoolMask = (spellId != 0) ? owner_.getSpellSchoolMask(spellId) : 0;
        in.spellName = (spellId != 0) ? owner_.getSpellName(spellId) : std::string{};
        in.srcName = sourceName.empty() ? classic_combat::global("UNKNOWN") : sourceName;
        in.dstName = targetName.empty() ? classic_combat::global("UNKNOWN") : targetName;
        in.src = combatRelationOf(owner_, effectiveSrc);
        in.dst = combatRelationOf(owner_, effectiveDst);
        const ClassicCombatMessage line = buildClassicCombatMessage(in);
        if (!line.event.empty()) {
            std::vector<std::string> args{line.event, line.pattern};
            args.insert(args.end(), line.args.begin(), line.args.end());
            owner_.addonEventCallbackRef()(classic_combat::kLineEvent, args);
        }
    }
}

bool CombatHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    if (spellId == 0) return false;

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kRecentWindow = std::chrono::seconds(1);
    while (!recentSpellstealLogs_.empty() &&
           now - recentSpellstealLogs_.front().timestamp > kRecentWindow) {
        recentSpellstealLogs_.pop_front();
    }

    for (auto it = recentSpellstealLogs_.begin(); it != recentSpellstealLogs_.end(); ++it) {
        if (it->casterGuid == casterGuid &&
            it->victimGuid == victimGuid &&
            it->spellId == spellId) {
            recentSpellstealLogs_.erase(it);
            return false;
        }
    }

    if (recentSpellstealLogs_.size() >= MAX_RECENT_SPELLSTEAL_LOGS)
        recentSpellstealLogs_.pop_front();
    recentSpellstealLogs_.push_back({.casterGuid = casterGuid, .victimGuid = victimGuid, .spellId = spellId, .timestamp = now});
    return true;
}

void CombatHandler::updateCombatText(float deltaTime) {
    flushCombatLog();
    // The group's combat flags a few times a second rather than every frame:
    // a raid is forty entity lookups, and a fight's edge a fifth of a second
    // late changes nothing a player can see.
    meterCombatPoll_ -= deltaTime;
    if (meterCombatPoll_ <= 0.0f) {
        meterCombatPoll_ = 0.2f;
        meterGroupInCombat_ = groupInCombat();
    }
    damageMeter_.update(meterClock(), meterGroupInCombat_);
    for (auto& entry : combatText_) {
        entry.age += deltaTime;
    }
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
                       [](const CombatTextEntry& e) { return e.isExpired(); }),
        combatText_.end());
}

// ============================================================
// Packet handlers
// ============================================================

void CombatHandler::autoTargetAttacker(uint64_t attackerGuid) {
    if (attackerGuid == 0 || attackerGuid == owner_.getPlayerGuid()) return;
    if (owner_.getTargetGuid() != 0) return;
    if (!owner_.getEntityManager().hasEntity(attackerGuid)) return;
    owner_.setTarget(attackerGuid);
}

void CombatHandler::handleAttackStart(network::Packet& packet) {
    AttackStartData data;
    if (!AttackStartParser::parse(packet, data)) return;

    // CREEP is a client presentation flag, not an instruction to keep a unit
    // translucent after it has openly engaged. The stock client reveals both
    // combatants at ATTACKSTART.
    //
    // The reason recorded here used to be that "some legacy realms do not send
    // a matching UNIT_FIELD_BYTES_1 update when an NPC breaks stealth". That
    // was almost certainly this client's own fault: UNIT_FIELD_BYTES_1 was
    // being read from field 137 where the server writes 74, so the update
    // arrived and was taken from the wrong slot. Fixed 2026-08-05. This is
    // kept because the stock client does it anyway, but if a stealth-break
    // ever needs looking at again, start by checking whether the field now
    // arrives - the premise behind this workaround is gone. Clear the cached
    // presentation state here so the normal creature visual sync restores full
    // opacity instead of leaving an invisible monster in melee.
    auto revealCombatant = [this](uint64_t guid) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (!unit->hasCreepVisibility()) return;
        unit->clearCreepVisibility();
        if (guid == owner_.getPlayerGuid() && owner_.stealthStateCallbackRef()) {
            owner_.stealthStateCallbackRef()(false);
        }
    };
    revealCombatant(data.attackerGuid);
    revealCombatant(data.victimGuid);

    // The attacker's target, kept for every unit in view as the stock client
    // keeps it (Wow.exe 0x756800 -> 0x754FF0): it draws the attacker's
    // weapons for as long as the swinging lasts, whatever UNIT_FIELD_BYTES_2
    // says - which for a playerbot, or a creature whose addon sheathes it,
    // stays 0 through the whole fight.
    if (auto attacker = owner_.getEntityManager().getEntity(data.attackerGuid);
        attacker && attacker->isUnit()) {
        std::static_pointer_cast<Unit>(attacker)->setAttackTarget(data.victimGuid);
    }

    if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttackRequested_ = true;
        autoAttacking_ = true;
        autoAttackRetryPending_ = false;
        autoAttackTarget_ = data.victimGuid;
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PLAYER_ENTER_COMBAT", {});
    } else if (data.victimGuid == owner_.getPlayerGuid() && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);

        // Play aggro sound when NPC attacks player
        if (owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(data.attackerGuid);
            if (entity && entity->getType() == ObjectType::UNIT) {
                glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
                owner_.npcAggroCallbackRef()(data.attackerGuid, pos);
            }
        }
    }

    // Force both participants to face each other at combat start.
    // Uses atan2(-dy, dx): canonical orientation convention where the West/Y
    // component is negated (renderYaw = orientation + 90°, model-forward = render+X).
    auto attackerEnt = owner_.getEntityManager().getEntity(data.attackerGuid);
    auto victimEnt   = owner_.getEntityManager().getEntity(data.victimGuid);
    if (attackerEnt && victimEnt) {
        float dx = victimEnt->getX() - attackerEnt->getX();
        float dy = victimEnt->getY() - attackerEnt->getY();
        if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
            attackerEnt->setOrientation(std::atan2(-dy,  dx));   // attacker → victim
            victimEnt->setOrientation  (std::atan2( dy, -dx));   // victim   → attacker
        }
    }
}

void CombatHandler::handleAttackStop(network::Packet& packet) {
    AttackStopData data;
    if (!AttackStopParser::parse(packet, data)) return;

    // Whoever stopped, its weapons go back to what its sheath byte says
    // (Wow.exe 0x756770 clears the target whatever the victim).
    if (auto attacker = owner_.getEntityManager().getEntity(data.attackerGuid);
        attacker && attacker->isUnit()) {
        std::static_pointer_cast<Unit>(attacker)->setAttackTarget(0);
    }

    // Keep intent, but clear server-confirmed active state until ATTACKSTART resumes.
    // Not from the saddle: that stop is the server refusing a mounted
    // attacker, and asking again would only be refused again.
    if (data.attackerGuid == owner_.getPlayerGuid() && owner_.isMounted()) {
        autoAttacking_ = false;
        autoAttackRequested_ = false;
        autoAttackRetryPending_ = false;
    } else if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttacking_ = false;
        autoAttackRetryPending_ = autoAttackRequested_;
        autoAttackResendTimer_ = 0.0f;
        LOG_DEBUG("SMSG_ATTACKSTOP received (keeping auto-attack intent)");
    } else if (data.victimGuid == owner_.getPlayerGuid()) {
        hostileAttackers_.erase(data.attackerGuid);
    }
}

// VictimState, the same in every expansion's servers.
constexpr uint32_t VICTIMSTATE_DODGE = 2, VICTIMSTATE_PARRY = 3, VICTIMSTATE_BLOCKS = 5,
                   VICTIMSTATE_EVADES = 6, VICTIMSTATE_IS_IMMUNE = 7, VICTIMSTATE_DEFLECTS = 8;

void CombatHandler::handleAttackerStateUpdate(network::Packet& packet) {
    AttackerStateUpdateData data;
    if (!owner_.getPacketParsers()->parseAttackerStateUpdate(packet, data)) return;

    // The combat log, for every swing the client is told of (0x7512B0).
    if (stockCombatLogActive()) {
        stock_combat_log::SwingInput in;
        in.attacker = data.attackerGuid;
        in.victim = data.targetGuid;
        in.hitInfo = data.hitInfo;
        in.victimState = data.victimState;
        in.damage = data.totalDamage;
        in.overkill = data.overkill > 0 ? data.overkill : 0;
        in.blocked = static_cast<int32_t>(data.blocked);
        for (size_t i = 0; i < data.subDamages.size() && i < 2; ++i) {
            const auto& sub = data.subDamages[i];
            if (sub.schoolMask == 0) break;
            if (i == 0) in.firstSchool = sub.schoolMask;
            in.absorbed += static_cast<int32_t>(sub.absorbed);
            in.resisted += static_cast<int32_t>(sub.resisted);
        }
        if (auto r = stock_combat_log::buildSwing(in)) logCombatEvent(*r);
    }

    bool isPlayerAttacker = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    // Animations are for every fight in view, as in the stock client: two
    // other units trading blows swing and flinch too, and are heard. Only the
    // combat text, the local character's voice and auto-targeting below are
    // kept to the player's own fights.
    const bool ourCombat = isPlayerAttacker || isPlayerTarget;
    bool rangedShot = false;

    if (isPlayerAttacker) {
        lastMeleeSwingMs_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Skip melee animation if a ranged shot was just triggered from
        // SMSG_SPELL_GO (Auto Shot / Shoot / Throw).  The ranged animation
        // is already playing; firing the melee callback here would override it.
        if (owner_.consumeSuppressMeleeSwingAnim()) {
            rangedShot = true;
            LOG_DEBUG("Suppressed melee swing anim - ranged shot already triggered");
        } else if (!data.noAnimation()) {
            if (owner_.meleeSwingCallbackRef()) owner_.meleeSwingCallbackRef()(0, data.hitInfo);
        }
    }
    if (!isPlayerAttacker && !data.noAnimation() && owner_.npcSwingCallbackRef()) {
        owner_.npcSwingCallbackRef()(data.attackerGuid, data.hitInfo, 0);
    }

    // The victim's reaction: dodge, parry, block or flinch. Which parry and
    // which block is the victim's weapons' business, decided where they are
    // known; when it plays is the attacker's swing's (a defence at its $CPP,
    // a flinch at its $CAH). An ability's blow (HITINFO_NO_ANIMATION: the
    // attacker's animation is the spell's) still makes the victim react, as
    // in the stock client, whose victim side (Wow.exe 0x755E40) does not
    // look at that flag. A deflect is a dodge to look at (0x756240). A miss
    // or an evade moves no victim but is passed on all the same: the
    // attacker's swing finishes at half speed from its $CAH.
    if (owner_.hitReactionCallbackRef() && data.victimState != VICTIMSTATE_IS_IMMUNE) {
        using HR = GameHandler::HitReaction;
        HR reaction = HR::WOUND;
        if (data.isMiss() || data.victimState == VICTIMSTATE_EVADES) reaction = HR::MISS;
        else if (data.victimState == VICTIMSTATE_DODGE) reaction = HR::DODGE;
        else if (data.victimState == VICTIMSTATE_DEFLECTS) reaction = HR::DEFLECT;
        else if (data.victimState == VICTIMSTATE_PARRY) reaction = HR::PARRY;
        else if (data.victimState == VICTIMSTATE_BLOCKS || data.blocked > 0) reaction = HR::BLOCK;
        else if (data.isCrit()) reaction = HR::CRIT_WOUND;
        owner_.hitReactionCallbackRef()(data.targetGuid, reaction, data.attackerGuid);
    }

    // The swing and the blow, heard for every fight in view as in the stock
    // client: the swoosh by the attacker's weapon, the blow by that weapon
    // against what it struck - the victim's body or armour, its shield on a
    // block, its weapon on a parry; a dodge or a miss is the swoosh alone.
    // The victim's injury voice comes with some blows. Synced to the attack
    // animation's $CSS / $CAH events where it has them (CombatSoundManager).
    if (!rangedShot && data.victimState != VICTIMSTATE_EVADES && data.victimState != VICTIMSTATE_IS_IMMUNE) {
        if (auto* ac = owner_.services().audioCoordinator) {
            if (auto* csm = ac->getCombatSoundManager()) {
                using MO = audio::CombatSoundManager::MeleeOutcome;
                MO outcome = MO::HIT;
                if (data.victimState == VICTIMSTATE_DODGE) outcome = MO::DODGE;
                else if (data.victimState == VICTIMSTATE_PARRY) outcome = MO::PARRY;
                else if (data.isMiss()) outcome = MO::MISS;
                else if (data.victimState == VICTIMSTATE_BLOCKS || data.blocked > 0) outcome = MO::BLOCK;
                else if (data.isCrit()) outcome = MO::CRIT;
                constexpr uint32_t kHitInfoOffHand = 0x00000004;
                csm->onMelee(data.attackerGuid, data.targetGuid, outcome, (data.hitInfo & kHitInfoOffHand) != 0,
                             !data.noAnimation(), /*injureVictim=*/true, ourCombat);
            }
            // A creature's attack cry (CreatureSoundData exertion), which is
            // also all a claw or a fist has to say for itself.
            if (!isPlayerAttacker) {
                auto entity = owner_.getEntityManager().getEntity(data.attackerGuid);
                auto* voiceManager = ac->getNpcVoiceManager();
                if (entity && entity->getType() == ObjectType::UNIT && voiceManager) {
                    auto unit = std::static_pointer_cast<Unit>(entity);
                    const glm::vec3 canonical(unit->getLatestX(), unit->getLatestY(), unit->getLatestZ());
                    voiceManager->playCombatAttack(
                        data.attackerGuid, unit->getDisplayId(), core::coords::canonicalToRender(canonical));
                }
            }
        }
    }

    if (!ourCombat) return;  // Not our combat: no text, voice or targeting

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    // The local character's own voice
    if (auto* ac = owner_.services().audioCoordinator) {
        // Character vocalizations
        if (auto* asm_ = ac->getActivitySoundManager()) {
            if (isPlayerAttacker && !data.isMiss() && data.victimState != VICTIMSTATE_DODGE &&
                data.victimState != VICTIMSTATE_PARRY) {
                asm_->playAttackGrunt(data.isCrit());
            }
            if (isPlayerTarget && !data.isMiss() && data.victimState != VICTIMSTATE_DODGE &&
                data.victimState != VICTIMSTATE_PARRY) {
                asm_->playWound(data.isCrit());
            }
        }
    }

    // An evade comes with the miss flag set as well, so it is asked first.
    if (data.victimState == VICTIMSTATE_EVADES) {
        addCombatText(CombatTextEntry::EVADE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.isMiss()) {
        addCombatText(CombatTextEntry::MISS, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == VICTIMSTATE_DODGE) {
        addCombatText(CombatTextEntry::DODGE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == VICTIMSTATE_PARRY) {
        addCombatText(CombatTextEntry::PARRY, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == VICTIMSTATE_BLOCKS) {
        addCombatText(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == VICTIMSTATE_IS_IMMUNE) {
        addCombatText(CombatTextEntry::IMMUNE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == VICTIMSTATE_DEFLECTS) {
        addCombatText(CombatTextEntry::DEFLECT, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else {
        CombatTextEntry::Type type;
        if (data.isCrit())
            type = CombatTextEntry::CRIT_DAMAGE;
        else if (data.isCrushing())
            type = CombatTextEntry::CRUSHING;
        else if (data.isGlancing())
            type = CombatTextEntry::GLANCING;
        else
            type = CombatTextEntry::MELEE_DAMAGE;
        addCombatText(type, data.totalDamage, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        // Show partial absorb/resist from sub-damage entries
        uint32_t totalAbsorbed = 0, totalResisted = 0;
        for (const auto& sub : data.subDamages) {
            totalAbsorbed += sub.absorbed;
            totalResisted += sub.resisted;
        }
        if (totalAbsorbed > 0)
            addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(totalAbsorbed), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        if (totalResisted > 0)
            addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(totalResisted), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        // A partial block is a hit like any other, with the blocked part beside it.
        if (data.blocked > 0)
            addCombatText(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    }

    // The same swing as Blizzard_CombatText hears it, when it landed on us.
    if (isPlayerTarget) {
        std::vector<std::string> ctu;
        if (data.victimState == VICTIMSTATE_EVADES)          ctu = {"EVADE"};
        else if (data.isMiss())                               ctu = {"MISS"};
        else if (data.victimState == VICTIMSTATE_DODGE)       ctu = {"DODGE"};
        else if (data.victimState == VICTIMSTATE_PARRY)       ctu = {"PARRY"};
        else if (data.victimState == VICTIMSTATE_IS_IMMUNE)   ctu = {"IMMUNE"};
        else if (data.victimState == VICTIMSTATE_DEFLECTS)    ctu = {"DEFLECT"};
        else if (data.victimState == VICTIMSTATE_BLOCKS && data.totalDamage <= 0) ctu = {"BLOCK"};
        else {
            uint32_t absorbed = 0, resisted = 0;
            for (const auto& sub : data.subDamages) {
                absorbed += sub.absorbed;
                resisted += sub.resisted;
            }
            const uint32_t dealt = data.totalDamage > 0 ? static_cast<uint32_t>(data.totalDamage) : 0u;
            ctu = combatTextUpdateForHit(false, data.isCrit(), dealt, resisted, data.blocked, absorbed);
        }
        owner_.fireCombatTextUpdate(data.targetGuid, ctu);
    }
}

void CombatHandler::handleSpellDamageLog(network::Packet& packet) {
    SpellDamageLogData data;
    if (!owner_.getPacketParsers()->parseSpellDamageLog(packet, data)) return;

    // 0x6D3840 -> 0x751C40 / 0x752DE0. The source is the target when the
    // hit-type's 0x10 is set, as the client reads it.
    if (stockCombatLogActive()) {
        stock_combat_log::SpellDamageInput in;
        in.caster = (data.hitFlags & 0x10) ? data.targetGuid : data.attackerGuid;
        in.victim = data.targetGuid;
        in.spellId = data.spellId;
        in.damage = static_cast<int32_t>(data.damage);
        in.overkill = static_cast<int32_t>(data.overkill);
        in.school = data.schoolMask;
        in.absorbed = static_cast<int32_t>(data.absorbed);
        in.resisted = static_cast<int32_t>(data.resisted);
        in.blocked = static_cast<int32_t>(data.blocked);
        in.crit = data.isCrit;
        in.periodic = data.periodicLog;
        in.split = (data.hitFlags & 0x8) != 0;
        if (in.split) in.caster = data.attackerGuid;
        if (auto r = stock_combat_log::buildSpellDamage(in, combatLogSpellRules(data.spellId)))
            logCombatEvent(*r);
    }

    bool isPlayerSource = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    // Spell damage makes the victim flinch, as a melee hit does - any
    // victim in view, not only in the player's own fights. Not for a spell
    // that flies a missile: this log comes with its SPELL_GO, and the stock
    // client flinches the target as the missile lands (Wow.exe 0x702DB0),
    // which the spell visual system does.
    auto* spells = owner_.getSpellHandler();
    if (owner_.hitReactionCallbackRef() && data.damage > 0 &&
        !(spells && spells->spellFliesMissile(data.spellId))) {
        owner_.hitReactionCallbackRef()(data.targetGuid, data.isCrit
            ? GameHandler::HitReaction::CRIT_WOUND : GameHandler::HitReaction::WOUND, 0);
    }
    // And cries out as a melee blow can make it: its injury voice
    // (CreatureSoundData), the crit one on a crit. The local character's is
    // its activity sounds'.
    if (data.damage > 0) {
        if (auto* ac = owner_.services().audioCoordinator) {
            if (isPlayerTarget) {
                if (auto* asm_ = ac->getActivitySoundManager()) asm_->playWound(data.isCrit);
            } else if (auto* csm = ac->getCombatSoundManager()) {
                csm->onWounded(data.targetGuid, data.isCrit);
            }
        }
    }
    if (!isPlayerSource && !isPlayerTarget) return;  // Not our combat: no text or targeting

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    auto type = data.isCrit ? CombatTextEntry::CRIT_DAMAGE : CombatTextEntry::SPELL_DAMAGE;
    if (data.damage > 0)
        addCombatText(type, static_cast<int32_t>(data.damage), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
    if (data.absorbed > 0)
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(data.absorbed), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
    if (data.resisted > 0)
        addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(data.resisted), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
    // A physical ability - Mortal Strike, a mob's Cleave - is DAMAGE to the
    // addon rather than SPELL_DAMAGE, and drawn in the melee colour.
    if (isPlayerTarget) {
        const bool magical = data.schoolMask != 0 && data.schoolMask != 1;
        owner_.fireCombatTextUpdate(data.targetGuid,
            combatTextUpdateForHit(magical, data.isCrit, data.damage, data.resisted, 0, data.absorbed));
    }
}

void CombatHandler::handleSpellHealLog(network::Packet& packet) {
    SpellHealLogData data;
    if (!owner_.getPacketParsers()->parseSpellHealLog(packet, data)) return;

    // 0x6D3C30 -> 0x751F70.
    if (stockCombatLogActive()) {
        if (auto r = stock_combat_log::buildHeal(
                data.casterGuid, data.targetGuid, data.spellId, static_cast<int32_t>(data.heal),
                static_cast<int32_t>(data.overheal), static_cast<int32_t>(data.absorbed),
                data.isCrit, false, combatLogSpellRules(data.spellId)))
            logCombatEvent(*r);
    }

    bool isPlayerSource = (data.casterGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    if (!isPlayerSource && !isPlayerTarget) return;  // Not our combat

    auto type = data.isCrit ? CombatTextEntry::CRIT_HEAL : CombatTextEntry::HEAL;
    addCombatText(type, static_cast<int32_t>(data.heal), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
    if (data.absorbed > 0)
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(data.absorbed), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
    // HEAL carries the healer's name and then the amount: the addon prints
    // "+N", or "+N [Name]" with friendly healer names switched on.
    if (isPlayerTarget) {
        owner_.fireCombatTextUpdate(data.targetGuid,
            {data.isCrit ? "HEAL_CRIT" : "HEAL", owner_.lookupName(data.casterGuid),
             std::to_string(data.heal)});
    }
}

void CombatHandler::handleSetForcedReactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    if (count > 64) {
        LOG_WARNING("SMSG_SET_FORCED_REACTIONS: suspicious count ", count, ", ignoring");
        packet.skipAll();
        return;
    }
    forcedReactions_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(8)) break;
        uint32_t factionId = packet.readUInt32();
        uint32_t reaction  = packet.readUInt32();
        forcedReactions_[factionId] = static_cast<uint8_t>(reaction);
    }
    LOG_INFO("SMSG_SET_FORCED_REACTIONS: ", forcedReactions_.size(), " faction overrides");
}

// ============================================================
// Per-frame update
// ============================================================

void CombatHandler::updateAutoAttack(float deltaTime) {
    // Decrement range warn cooldown
    if (autoAttackRangeWarnCooldown_ > 0.0f) {
        autoAttackRangeWarnCooldown_ = std::max(0.0f, autoAttackRangeWarnCooldown_ - deltaTime);
    }

    // Leave combat if auto-attack target is too far away (leash range)
    // and keep melee intent tightly synced while stationary.
    if (autoAttackRequested_ && autoAttackTarget_ != 0) {
        auto targetEntity = owner_.getEntityManager().getEntity(autoAttackTarget_);
        if (targetEntity) {
            const float targetX = targetEntity->getLatestX();
            const float targetY = targetEntity->getLatestY();
            const float targetZ = targetEntity->getLatestZ();
            float dx = owner_.movementInfoRef().x - targetX;
            float dy = owner_.movementInfoRef().y - targetY;
            float dz = owner_.movementInfoRef().z - targetZ;
            float dist = std::sqrt(dx * dx + dy * dy);
            float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool classicLike = isPreWotlk();
            if (dist > 40.0f) {
                stopAutoAttack();
                LOG_INFO("Left combat: target too far (", dist, " yards)");
            } else if (owner_.isInWorld()) {
                bool allowResync = true;
                const float meleeRange = classicLike ? 5.25f : 5.75f;
                if (dist3d > meleeRange) {
                    autoAttackOutOfRange_ = true;
                    autoAttackOutOfRangeTime_ += deltaTime;
                    if (autoAttackRangeWarnCooldown_ <= 0.0f) {
                        owner_.raiseUiError("Target is too far away.");
                        owner_.addUIError("Target is too far away.");
                        autoAttackRangeWarnCooldown_ = 1.25f;
                    }
                    // The attack stays on while the target is out of reach,
                    // as in the stock client: running up to it ends in a
                    // swing, not in having to attack again. It used to be
                    // dropped after two seconds beyond 9 yards, which also
                    // ended every fight the moment a creature fled.
                } else {
                    autoAttackOutOfRange_ = false;
                    autoAttackOutOfRangeTime_ = 0.0f;
                }

                if (allowResync) {
                    autoAttackResendTimer_ += deltaTime;

                    // Classic/Turtle servers do not tolerate steady attack-start
                    // reissues well. Only retry once after local start or an
                    // explicit server-side attack stop while intent is still set.
                    const float resendInterval = classicLike ? 1.0f : 0.50f;
                    if (!autoAttacking_ && !autoAttackOutOfRange_ && autoAttackRetryPending_ &&
                        autoAttackResendTimer_ >= resendInterval) {
                        autoAttackResendTimer_ = 0.0f;
                        autoAttackRetryPending_ = false;
                        auto pkt = AttackSwingPacket::build(autoAttackTarget_);
                        owner_.getSocket()->send(pkt);
                    }

                    // No periodic facing sync. The player is turned to face the
                    // target once, where the attack is commanded, and after
                    // that their facing is theirs: turning away mid-fight
                    // means the swing misses, which is what it means in the
                    // real client. Re-aiming them every fifth of a second so
                    // the server would accept the swing fought the mouse for
                    // the whole fight and made it impossible to back away from
                    // something while attacked by it.
                }
            }
        }
    }

    // Keep active melee attackers visually facing the player as positions change.
    if (!hostileAttackers_.empty()) {
        for (uint64_t attackerGuid : hostileAttackers_) {
            auto attacker = owner_.getEntityManager().getEntity(attackerGuid);
            if (!attacker) continue;
            float dx = owner_.movementInfoRef().x - attacker->getX();
            float dy = owner_.movementInfoRef().y - attacker->getY();
            if (std::abs(dx) < 0.01f && std::abs(dy) < 0.01f) continue;
            attacker->setOrientation(std::atan2(-dy, dx));
        }
    }
}

// ============================================================
// State management
// ============================================================

void CombatHandler::resetAllCombatState() {
    targetNearestCycle_.reset();
    // Another character's fights are not this one's.
    damageMeter_.reset();
    meterGroupInCombat_ = false;
    hostileAttackers_.clear();
    combatText_.clear();
    autoAttacking_ = false;
    autoAttackRequested_ = false;
    autoAttackRetryPending_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackRangeWarnCooldown_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    lastMeleeSwingMs_ = 0;
}

void CombatHandler::removeHostileAttacker(uint64_t guid) {
    hostileAttackers_.erase(guid);
}

void CombatHandler::clearCombatText() {
    combatText_.clear();
}

void CombatHandler::removeCombatTextForGuid(uint64_t guid) {
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
            [guid](const CombatTextEntry& e) {
                return e.dstGuid == guid;
            }),
        combatText_.end());
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void CombatHandler::handleHealthUpdate(network::Packet& packet) {
    const bool huTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(huTbc ? 8u : 2u) ) return;
    uint64_t guid = huTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t hp = packet.readUInt32();
    if (auto* unit = owner_.getUnitByGuid(guid)) unit->setHealth(hp);
    if (guid != 0) {
        auto unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty()) owner_.fireAddonEvent("UNIT_HEALTH", {unitId});
    }
}

void CombatHandler::handlePowerUpdate(network::Packet& packet) {
    const bool puTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(puTbc ? 8u : 2u) ) return;
    uint64_t guid = puTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint8_t  powerType = packet.readUInt8();
    uint32_t value     = packet.readUInt32();
    if (auto* unit = owner_.getUnitByGuid(guid)) unit->setPowerByType(powerType, value);
    if (guid != 0) {
        auto unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty()) {
            owner_.fireAddonEvent("UNIT_POWER", {unitId});
            if (guid == owner_.getPlayerGuid()) {
                owner_.fireAddonEvent("ACTIONBAR_UPDATE_USABLE", {});
                owner_.fireAddonEvent("SPELL_UPDATE_USABLE", {});
            }
        }
    }
}

void CombatHandler::handleUpdateComboPoints(network::Packet& packet) {
    const bool cpTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(cpTbc ? 8u : 2u) ) return;
    uint64_t target = cpTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return;
    owner_.comboPointsRef() = packet.readUInt8();
    owner_.comboTargetRef() = target;
    LOG_DEBUG("SMSG_UPDATE_COMBO_POINTS: target=0x", std::hex, target,
              std::dec, " points=", static_cast<int>(owner_.comboPointsRef()));
    // UNIT_COMBO_POINTS with the unit is the 3.x name: comboframe.lua and
    // Blizzard_CombatText both answer it and only when arg1 is the unit their
    // frame follows. PLAYER_COMBO_POINTS is what 1.x and 2.x interfaces listen
    // for, and a WotLK one never registers it - so the combo frame stayed dark.
    if (isPreWotlk()) owner_.fireAddonEvent("PLAYER_COMBO_POINTS", {});
    else              owner_.fireAddonEvent("UNIT_COMBO_POINTS", {"player"});
    // Finishing moves (and Overpower) are usable only with points.
    owner_.fireAddonEvent("ACTIONBAR_UPDATE_USABLE", {});
}

// SMSG_PVP_CREDIT (Wow.exe 0x526530 case 0x28C -> 0x50B6D0): honor, the
// victim, its rank. No victim is an award (COMBATLOG_HONORAWARD); a victim
// the client can see and at least one point is an honorable kill,
// COMBATLOG_HONORGAIN with the rank's PVP_RANK_<rank>_<faction> name, or
// COMBATLOG_HONORGAIN_NO_RANK for a negative rank. Otherwise nothing. Then
// COMBAT_TEXT_UPDATE("HONOR_GAINED").
void CombatHandler::handlePvpCredit(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;
    const uint32_t honor = packet.readUInt32();
    const uint64_t victimGuid = packet.readUInt64();
    const int32_t rank = static_cast<int32_t>(packet.readUInt32());
    LOG_INFO("SMSG_PVP_CREDIT: honor=", honor, " victim=0x", std::hex, victimGuid, std::dec, " rank=", rank);
    auto* am = owner_.services().assetManager;
    auto text = [am](const char* key, const char* fallback) {
        std::string f = globalString(am, key);
        return f.empty() ? std::string(fallback) : f;
    };
    std::string line;
    if (victimGuid == 0) {
        line = formatGlobalString(text("COMBATLOG_HONORAWARD", "You have been awarded %d honor points."),
                                  {std::to_string(honor)});
    } else {
        const auto victim = owner_.getEntityManager().getEntity(victimGuid);
        if (!victim || honor < 1) return;
        const std::string name = owner_.lookupName(victimGuid);
        if (rank >= 0) {
            // The rank's name for the victim's side: 0 Horde, 1 Alliance.
            const int race = owner_.lookupPlayerRace(victimGuid);
            const bool alliance = race == 1 || race == 3 || race == 4 || race == 7 || race == 11;
            const std::string rankKey = "PVP_RANK_" + std::to_string(rank) + "_" + (alliance ? "1" : "0");
            line = formatGlobalString(
                text("COMBATLOG_HONORGAIN", "%s dies, honorable kill Rank: %s (%d Honor Points)"),
                {name, globalString(am, rankKey), std::to_string(honor)});
        } else {
            line = formatGlobalString(
                text("COMBATLOG_HONORGAIN_NO_RANK", "%s dies, honorable kill (%d Honor Points)"),
                {name, std::to_string(honor)});
        }
    }
    owner_.addLocalChatLine(ChatType::COMBAT_HONOR_GAIN, line);
    if (honor > 0) {
        addCombatText(CombatTextEntry::HONOR_GAIN, static_cast<int32_t>(honor), 0, true);
        owner_.fireCombatTextUpdate(owner_.getPlayerGuid(), {"HONOR_GAINED", std::to_string(honor)});
    }
}

void CombatHandler::handleProcResist(network::Packet& packet) {
    // Whole guids in TBC and WotLK (AzerothCore's Unit::SendSpellMiss sibling,
    // Wow.exe 0x750D40 reads two uint64s), packed only on 1.12.
    const bool prUsesFullGuid = !isClassicLikeExpansion();
    auto readPrGuid = [&]() -> uint64_t {
        if (prUsesFullGuid)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t caster = readPrGuid();
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t victim = readPrGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t spellId = packet.readUInt32();
    // 0x750D40: a full resist, SPELL_MISSED RESIST with an amount of 0.
    if (stockCombatLogActive()) {
        if (auto r = stock_combat_log::buildSpellMiss(caster, victim, spellId, 2,
                                                     combatLogSpellRules(spellId))) {
            r->v6c = 0;
            logCombatEvent(*r);
        }
    }
    if (victim == owner_.getPlayerGuid())       addCombatText(CombatTextEntry::RESIST, 0, spellId, false, 0, caster, victim);
    else if (caster == owner_.getPlayerGuid())  addCombatText(CombatTextEntry::RESIST, 0, spellId, true,  0, caster, victim);
    owner_.fireCombatTextUpdate(victim, {"SPELL_RESIST"});
    packet.skipAll();
}

// ============================================================
// Environmental / reflect / immune / resist
// ============================================================


void CombatHandler::handleSpellDamageShield(network::Packet& packet) {
    // Classic: packed_guid victim + packed_guid caster + spellId(4) + damage(4) + schoolMask(4)
    // TBC:     uint64 victim + uint64 caster + spellId(4) + damage(4) + schoolMask(4)
    // WotLK:   uint64 victim + uint64 caster + spellId(4) + damage(4) + overkill(4) + schoolMask(4)
    //          (AzerothCore Unit::DealDamageShieldDamage; Wow.exe 0x6D3750 reads
    //          two whole guids). The first guid is the shield's owner.
    const bool shieldTbc = !isClassicLikeExpansion();
    const bool shieldWotlkLike = !isClassicLikeExpansion() && !shieldTbc;
    const auto shieldRem = [&]() { return packet.getRemainingSize(); };
    const size_t shieldMinSz = shieldTbc ? 24u : 2u;
    if (!packet.hasRemaining(shieldMinSz)) {
        packet.skipAll(); return;
    }
    if (!shieldTbc && (!packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(shieldTbc ? 8u : 1u)             || (!shieldTbc && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    const bool shieldWotlk = shieldWotlkLike && !isActiveExpansion("tbc");
    const size_t shieldTailSize = shieldWotlk ? 16u : 12u;
    if (shieldRem() < shieldTailSize) {
        packet.skipAll(); return;
    }
    uint32_t shieldSpellId = packet.readUInt32();
    uint32_t damage        = packet.readUInt32();
    uint32_t overkill = 0;
    if (shieldWotlk) overkill = packet.readUInt32();
    const uint32_t school = packet.readUInt32();
    if (stockCombatLogActive()) {
        if (auto r = stock_combat_log::buildDamageShield(
                victimGuid, casterGuid, shieldSpellId, static_cast<int32_t>(damage),
                static_cast<int32_t>(overkill), school, combatLogSpellRules(shieldSpellId)))
            logCombatEvent(*r);
    }
    // Show combat text: damage shield reflect
    if (casterGuid == owner_.getPlayerGuid()) {
        // We have a damage shield that reflected damage
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, true, 0, casterGuid, victimGuid);
    } else if (victimGuid == owner_.getPlayerGuid()) {
        // A damage shield hit us (e.g. target's Thorns)
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, false, 0, casterGuid, victimGuid);
        owner_.fireCombatTextUpdate(victimGuid, combatTextUpdateForHit(true, false, damage, 0, 0, 0));
    }
}

void CombatHandler::handleSpellOrDamageImmune(network::Packet& packet) {
    // WotLK/Classic/Turtle: packed casterGuid + packed victimGuid + uint32 spellId + uint8 saveType
    // TBC:                  full uint64 casterGuid + full uint64 victimGuid + uint32 + uint8
    // Whole guids in WotLK as well (AzerothCore Unit::SendSpellDamageImmune,
    // Wow.exe 0x6CE3D0).
    const bool immuneUsesFullGuid = !isClassicLikeExpansion();
    const size_t minSz = immuneUsesFullGuid ? 21u : 2u;
    if (!packet.hasRemaining(minSz)) {
        packet.skipAll(); return;
    }
    if (!immuneUsesFullGuid && !packet.hasFullPackedGuid()) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(immuneUsesFullGuid ? 8u : 2u)             || (!immuneUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint32_t immuneSpellId = packet.readUInt32();
    const uint8_t debugLog = packet.readUInt8();
    // 0x6CE3D0 -> 0x751B80: SPELL_MISSED IMMUNE, for a spell the log knows.
    (void)debugLog;
    if (stockCombatLogActive()) {
        if (auto r = stock_combat_log::buildSpellMiss(casterGuid, victimGuid, immuneSpellId, 7,
                                                     combatLogSpellRules(immuneSpellId)))
            logCombatEvent(*r);
    }
    // Show IMMUNE text when the player is the caster (we hit an immune target)
    // or the victim (we are immune)
    if (casterGuid == owner_.getPlayerGuid() || victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::IMMUNE, 0, immuneSpellId,
                      casterGuid == owner_.getPlayerGuid(), 0, casterGuid, victimGuid);
    }
    // No COMBAT_TEXT_UPDATE here: the same immunity arrives in SMSG_SPELL_GO's
    // miss list (or the melee swing's victim state), which already sends it.
}

void CombatHandler::handleResistLog(network::Packet& packet) {
    // WotLK/Classic/Turtle: uint32 hitInfo + packed_guid attacker + packed_guid victim + uint32 spellId
    //                      + float resistFactor + uint32 targetRes + uint32 resistedValue + ...
    // TBC:                 same layout but full uint64 GUIDs
    // Show RESIST combat text when player resists an incoming spell.
    const bool rlUsesFullGuid = isActiveExpansion("tbc");
    auto rl_rem = [&]() { return packet.getRemainingSize(); };
    if (rl_rem() < 4) { packet.skipAll(); return; }
    /*uint32_t hitInfo =*/ packet.readUInt32();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t attackerGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < 4) { packet.skipAll(); return; }
    uint32_t spellId = packet.readUInt32();
    // Resist payload includes:
    // float resistFactor + uint32 targetResistance + uint32 resistedValue.
    // Require the full payload so truncated packets cannot synthesize
    // zero-value resist events.
    if (rl_rem() < 12) { packet.skipAll(); return; }
    /*float resistFactor =*/ packet.readFloat();
    /*uint32_t targetRes =*/ packet.readUInt32();
    int32_t resistedAmount = static_cast<int32_t>(packet.readUInt32());
    // Show RESIST when the player is involved on either side.
    if (resistedAmount > 0 && victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, false, 0, attackerGuid, victimGuid);
    } else if (resistedAmount > 0 && attackerGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, true, 0, attackerGuid, victimGuid);
    }
    packet.skipAll();
}

// ============================================================
// Pet feedback
// ============================================================

void CombatHandler::handlePetTameFailure(network::Packet& packet) {
    // ERR_TAME_FAILED ("%s.") with the reason's PETTAME_* text, the red
    // error (Wow.exe 0x802090 -> 0x800390).
    static const char* const kReasons[] = {
        nullptr, "PETTAME_INVALIDCREATURE", "PETTAME_TOOMANY", "PETTAME_CREATUREALREADYOWNED",
        "PETTAME_NOTTAMEABLE", "PETTAME_ANOTHERSUMMONACTIVE", "PETTAME_UNITSCANTTAME",
        "PETTAME_NOPETAVAILABLE", "PETTAME_INTERNALERROR", "PETTAME_TOOHIGHLEVEL",
        "PETTAME_DEAD", "PETTAME_NOTDEAD", "PETTAME_CANTCONTROLEXOTIC",
    };
    if (packet.hasRemaining(1)) {
        const uint8_t reason = packet.readUInt8();
        const char* key = (reason >= 1 && reason <= 12) ? kReasons[reason] : "PETTAME_UNKNOWNERROR";
        std::string why = globalString(owner_.services().assetManager, key);
        if (why.empty()) why = "Unknown error";
        owner_.displayGameMessage("ERR_TAME_FAILED", why + ".", {why});
    }
}

void CombatHandler::handlePetActionFeedback(network::Packet& packet) {
    // Wow.exe (0x5D4DA0), red errors: 1 the pet is dead, 2 nothing to attack
    // (ERR_GENERIC_NO_VALID_TARGETS for a spell that needs one), 3 an
    // invalid target, 4 no path.
    if (!packet.hasRemaining(1)) return;
    const uint8_t msg = packet.readUInt8();
    switch (msg) {
        case 1: owner_.displayGameMessage("ERR_PET_SPELL_DEAD", "Your pet is dead."); break;
        case 2: owner_.displayGameMessage("ERR_NO_ATTACK_TARGET", "There is nothing to attack."); break;
        case 3: owner_.displayGameMessage("ERR_INVALID_ATTACK_TARGET", "You cannot attack that target."); break;
        case 4: owner_.displayGameMessage("ERR_PET_SPELL_NOPATH", "No path available"); break;
        default: break;
    }
    packet.skipAll();
}

void CombatHandler::handlePetCastFailed(network::Packet& packet) {
    // WotLK: castCount(1) + spellId(4) + reason(1)
    // Classic/TBC: spellId(4) + reason(1) (no castCount)
    const bool hasCount = isActiveExpansion("wotlk");
    const size_t minSize = hasCount ? 6u : 5u;
    if (packet.hasRemaining(minSize)) {
        if (hasCount) /*uint8_t castCount =*/ packet.readUInt8();
        uint32_t spellId   = packet.readUInt32();
        uint8_t  reason    = (packet.hasRemaining(1))
                                 ? packet.readUInt8() : 0;
        LOG_DEBUG("SMSG_PET_CAST_FAILED: spell=", spellId,
                  " reason=", static_cast<int>(reason));
        if (reason != 0) {
            // The cast failure's own text on the red error line, as the
            // player's own failures are shown (Wow.exe 0x806C30 -> 0x805610).
            const char* reasonStr = getSpellCastResultString(reason);
            if (reasonStr && *reasonStr) owner_.addUIError(reasonStr);
        }
    }
    packet.skipAll();
}

void CombatHandler::handlePetBroken(network::Packet& packet) {
    // ERR_PET_BROKEN, the red error, and nothing else (Wow.exe 0x5D3120).
    // The pet itself goes with the SMSG_PET_SPELLS that empties the bar,
    // which is what raises UNIT_PET; clearing it here as well made that
    // packet look like no change, and the pet frame stayed up.
    owner_.displayGameMessage("ERR_PET_BROKEN", "Your pet has run away.");
    LOG_INFO("SMSG_PET_BROKEN: pet bond broken");
    packet.skipAll();
}

void CombatHandler::handlePetLearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().push_back(spellId);
        // ERR_PET_LEARN_SPELL_S, or _ABILITY_S for a passive-flagged one
        // (Attributes 0x10), with the rank (Wow.exe 0x5D4C30).
        const std::string& sname = owner_.getSpellName(spellId);
        if (!sname.empty()) {
            uint32_t attr = 0;
            auto& cache = owner_.spellNameCacheRef();
            if (auto it = cache.find(spellId); it != cache.end()) attr = it->second.attr;
            const std::string& rank = owner_.getSpellRank(spellId);
            const std::string shown = rank.empty() ? sname : sname + " (" + rank + ")";
            if (attr & 0x10)
                owner_.displayGameMessage("ERR_PET_LEARN_ABILITY_S", "Your pet has learned a new ability: " + shown + ".", {shown});
            else
                owner_.displayGameMessage("ERR_PET_LEARN_SPELL_S", "Your pet has learned a new spell: " + shown + ".", {shown});
        }
        LOG_DEBUG("SMSG_PET_LEARNED_SPELL: spellId=", spellId);
        owner_.fireAddonEvent("PET_BAR_UPDATE", {});
    }
    packet.skipAll();
}

void CombatHandler::handlePetUnlearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().erase(
            std::remove(owner_.petSpellListRef().begin(), owner_.petSpellListRef().end(), spellId),
            owner_.petSpellListRef().end());
        owner_.petAutocastSpellsRef().erase(spellId);
        const std::string& sname = owner_.getSpellName(spellId);
        if (!sname.empty()) {
            const std::string& rank = owner_.getSpellRank(spellId);
            const std::string shown = rank.empty() ? sname : sname + " (" + rank + ")";
            owner_.displayGameMessage("ERR_PET_SPELL_UNLEARNED_S", "Your pet has unlearned " + shown + ".", {shown});
        }
        LOG_DEBUG("SMSG_PET_UNLEARNED_SPELL: spellId=", spellId);
    }
    packet.skipAll();
}

void CombatHandler::handlePetMode(network::Packet& packet) {
    // uint64 petGuid, uint32 mode - the same word SMSG_PET_SPELLS carries,
    // react in the low byte and command above it (0x5D3630). It replaces the
    // client's copy for the pet it names and redraws the bar; it says nothing
    // about attacking, which belongs to the pet's in-combat flag.
    if (packet.hasRemaining(12)) {
        const uint64_t modeGuid = packet.readUInt64();
        const uint32_t mode = packet.readUInt32();
        if (modeGuid == owner_.petGuidRef() && modeGuid != 0) {
            owner_.petModeRef() = mode;
            owner_.fireAddonEvent("PET_BAR_UPDATE", {});
            LOG_DEBUG("SMSG_PET_MODE: command=", static_cast<int>(owner_.getPetCommand()),
                      " react=", static_cast<int>(owner_.getPetReact()));
        }
    }
    packet.skipAll();
}

// ============================================================
// Resurrect
// ============================================================

void CombatHandler::handleResurrectFailed(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        // Nothing said: Wow.exe (0x6CE470 -> 0x520F70) only re-raises
        // PLAYER_DEAD or PLAYER_ALIVE.
        (void)packet.readUInt32();
    }
}

// ============================================================
// Targeting
// ============================================================

bool CombatHandler::isSelectableUnit(uint64_t guid) const {
    // A game object is not a target, whoever asks.
    //
    // WoW selects units, players and corpses; a door, a mailbox, a chest or a
    // spell's ground effect is used or walked into, and none of them can be
    // the target. Refused here rather than at the click, because every way in
    // reaches this - the click, tab, /target, a macro - and each one that
    // targeted an object put a selection circle on the floor around it and
    // sent CMSG_SET_SELECTION for a guid that is not a unit.
    if (auto entity = owner_.getEntityManager().getEntity(guid)) {
        const auto type = entity->getType();
        if (type == ObjectType::GAMEOBJECT || type == ObjectType::DYNAMICOBJECT) {
            return false;
        }
    }

    // Everything the player clicks is selectable; the server arbitrates whether a
    // corpse actually holds loot when we send CMSG_LOOT. We deliberately do NOT
    // gate dead creature corpses on UNIT_DYNFLAG_LOOTABLE.
    //
    // The reason given here was that the bit is computed per-viewer and goes
    // stale across a death/resurrect cycle, and that gating on it left a
    // genuinely lootable corpse permanently unclickable. The symptom was real;
    // the explanation was not the whole of it. UNIT_DYNAMIC_FLAGS was being
    // read from field 147 - which is UNIT_FIELD_PADDING - where the server
    // writes 79, so the bit was never right in the first place. Fixed
    // 2026-08-05.
    //
    // Not re-gated here, because that would be trading a working behaviour for
    // a theory. Anyone who wants the gate back should first confirm against a
    // live corpse that the flag now reads what it should.
    // Empty, fully-looted corpses are already removed locally by the
    // lootableCleared -> despawnCreatureLocally path (see
    // EntityController::onValuesUpdateUnit), so they don't return as click targets.
    return true;
}

void CombatHandler::setTarget(uint64_t guid) {
    if (guid == owner_.getTargetGuid()) return;
    // Looted-out, unskinnable corpses cannot be selected.
    if (!isSelectableUnit(guid)) return;

    // Save previous target - once as "the last one whatever it was", which is
    // what TargetLastTarget flips back to, and once under its kind, which is
    // what makes TargetLastEnemy and TargetLastFriend worth having: a healer's
    // last friendly target survives half a fight's worth of tab-targeting.
    if (owner_.getTargetGuid() != 0) {
        const uint64_t previous = owner_.getTargetGuid();
        owner_.lastTargetGuidRef() = previous;
        auto entity = owner_.getEntityManager().getEntity(previous);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            if (!owner_.isFriendlyFaction(unit->getFactionTemplate()) ||
                isAggressiveTowardPlayer(previous)) {
                lastEnemyTargetGuid_ = previous;
            } else {
                lastFriendTargetGuid_ = previous;
            }
        }
    }

    // Stop Auto Attack On Target Change, which the panel offers and nothing
    // read. Changing target left the swing running - correct for the setting's
    // default, and the only behaviour available whatever it said.
    //
    // Only when the target actually changes, and only when something is being
    // swung at: re-selecting the same unit is not a change, and stopping an
    // attack nobody started would send a cancel for nothing.
    if (guid != owner_.getTargetGuid() && autoAttacking_ &&
        addons::storedCVarValue("stopAutoAttackOnTargetChange", "0") != "0") {
        stopAutoAttack();
    }

    owner_.setTargetGuidRaw(guid);

    // The new target's auras as already cached from its SMSG_AURA_UPDATEs;
    // the server sends no AURA_UPDATE_ALL for a target change.
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->loadTargetAuras(guid);

    // Clear previous target's cast bar on target change
    // (the new target's cast state is naturally fetched from spellHandler_->unitCastStates_ by GUID)

    // Inform server of target selection
    if (owner_.isInWorld()) {
        auto packet = SetSelectionPacket::build(guid);
        owner_.getSocket()->send(packet);
    }

    if (guid != 0) {
        LOG_INFO("Target set: 0x", std::hex, guid, std::dec);
    }
    owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
    owner_.fireAddonEvent("ACTIONBAR_UPDATE_USABLE", {});

    // Report the interface once, the first time there is a target to report it
    // against. A target frame is only right or wrong with something targeted,
    // and that is not a moment anything can be scheduled for - which is why
    // this one has been diagnosed from readings taken when nothing was
    // selected, where correct and broken look identical.
    static bool reportedOnce = false;
    if (!reportedOnce && guid != 0) {
        reportedOnce = true;
        ui::frameXmlRequestCheck();
        // What UnitExists("target") resolves through, said plainly. The Lua
        // probe was meant to answer this and has not run; this is the same
        // question asked where it can actually be answered - the interface
        // reports "target" as existing exactly when this guid resolves to a
        // unit.
        auto entity = owner_.getEntityManager().getEntity(guid);
        LOG_WARNING("first target: guid=0x", std::hex, guid, std::dec,
                    " entity=", (entity ? "found" : "MISSING"),
                    " unit=", (entity && dynamic_cast<Unit*>(entity.get())
                                   ? "yes" : "NO"));
    }
}

void CombatHandler::clearTarget() {
    if (owner_.getTargetGuid() != 0) {
        LOG_INFO("Target cleared");
        // The server keeps its own copy of the selection - it is what other
        // players see as this player's target (UNIT_FIELD_TARGET) and what GM
        // commands act on - so a cleared target is said, as setTarget says a
        // new one (CMSG_SET_SELECTION, guid 0). Without it the server went on
        // believing the last target was selected.
        if (owner_.isInWorld() && owner_.getSocket()) {
            auto packet = SetSelectionPacket::build(0);
            owner_.getSocket()->send(packet);
        }
        // Zero the GUID before firing the event so callbacks/addons that query
        // the current target see null (consistent with setTarget which updates
        // targetGuid before the event).
        owner_.setTargetGuidRaw(0);
        owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
        owner_.fireAddonEvent("ACTIONBAR_UPDATE_USABLE", {});
    } else {
        owner_.setTargetGuidRaw(0);
    }
    // ClearTarget sets 0xBD08CC to 0: the next TargetNearest builds its list
    // afresh (0x525FC0).
    targetNearestCycle_.reset();
}

std::shared_ptr<Entity> CombatHandler::getTarget() const {
    if (owner_.getTargetGuid() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.getTargetGuid());
}

void CombatHandler::setFocus(uint64_t guid) {
    owner_.focusGuidRef() = guid;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
    if (guid != 0) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity) {
            std::string name;
            auto unit = std::dynamic_pointer_cast<Unit>(entity);
            if (unit && !unit->getName().empty()) {
                name = unit->getName();
            }
            if (name.empty()) name = owner_.lookupName(guid);
            // No line: FocusUnit (Wow.exe 0x522480) says nothing; the focus
            // frame is the answer.
            LOG_INFO("Focus set: 0x", std::hex, guid, std::dec, " ", name);
        }
    }
}

void CombatHandler::clearFocus() {
    if (owner_.focusGuidRef() != 0) LOG_INFO("Focus cleared");
    owner_.focusGuidRef() = 0;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
}

std::shared_ptr<Entity> CombatHandler::getFocus() const {
    if (owner_.focusGuidRef() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.focusGuidRef());
}

void CombatHandler::setMouseoverGuid(uint64_t guid) {
    if (owner_.mouseoverGuidRef() != guid) {
        owner_.mouseoverGuidRef() = guid;
        owner_.fireAddonEvent("UPDATE_MOUSEOVER_UNIT", {});
    }
}

void CombatHandler::targetLastTarget() {
    // Nothing to go back to is silent (Wow.exe TargetLastTarget 0x525D70).
    if (owner_.lastTargetGuidRef() == 0) return;

    // Swap current and last target
    uint64_t temp = owner_.getTargetGuid();
    setTarget(owner_.lastTargetGuidRef());
    owner_.lastTargetGuidRef() = temp;
}

tab_target::UnitFacts CombatHandler::targetNearestFacts(uint64_t guid, const Unit& unit) const {
    namespace tt = tab_target;
    tt::UnitFacts f;
    f.isPlayer = unit.getType() == ObjectType::PLAYER;
    f.health = unit.getHealth();
    f.dynFlagDead = (unit.getDynamicFlags() & UNIT_DYNFLAG_DEAD) != 0;
    f.standState = unit.getStandState();
    const uint32_t playerFlags = f.isPlayer
        ? unit.getField(fieldIndex(UF::PLAYER_FLAGS)) : 0u;
    constexpr uint32_t kPlayerFlagGhost = 0x10;
    f.dead = unit.getHealth() < 1 || (playerFlags & kPlayerFlagGhost) != 0;

    // CanAttack (0x729740), from the player's side: the unit's flags first,
    // then the reaction - for a creature anything not friendly (neutral
    // wildlife included), for a player also the PvP flags of both.
    const bool flagsAllow = (unit.getUnitFlags() & tt::kUnattackableFlags) == 0;
    const bool friendly = !unit.isHostile() && owner_.isFriendlyFaction(unit.getFactionTemplate());
    if (!flagsAllow || (playerFlags & kPlayerFlagGhost)) {
        f.canAttack = false;
    } else if (isAggressiveTowardPlayer(guid)) {
        f.canAttack = true;
    } else if (const auto duel = owner_.duelReactionToPlayer(unit)) {
        // A duel joins the two (and what they control): the reaction test
        // passes for the other side and the arbiter test answers yes,
        // whatever the factions and PvP flags (0x729740, stock_duel.hpp).
        f.canAttack = *duel < 4;
    } else if (f.isPlayer) {
        uint8_t mine = 0;
        if (auto self = owner_.getEntityManager().getEntity(owner_.getPlayerGuid()))
            mine = static_cast<uint8_t>(self->getField(fieldIndex(UF::UNIT_FIELD_BYTES_2)) >> 8);
        const uint8_t theirs =
            static_cast<uint8_t>(unit.getField(fieldIndex(UF::UNIT_FIELD_BYTES_2)) >> 8);
        f.canAttack = !friendly && tt::playersMayFight(mine, theirs);
    } else {
        f.canAttack = !friendly;
    }
    // CanAssist (0x7293D0): selectable and friendly by reaction.
    f.canAssist = (unit.getUnitFlags() & UNIT_FLAG_NOT_SELECTABLE) == 0 &&
                  !unit.isHostile() && owner_.unitReactionToPlayer(unit) >= 5;

    // party1..4 are the player's own subgroup in a raid (0x52C680); the raid
    // is everyone (0x5726F0).
    const auto& pd = owner_.getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid != guid) continue;
        f.raidMember = true;
        f.partyMember = !groupIsRaid(pd.groupType) || m.subGroup == pd.subGroup;
    }
    return f;
}

bool CombatHandler::canAttackUnit(uint64_t guid) const {
    if (guid == 0 || guid == owner_.getPlayerGuid()) return false;
    auto entity = owner_.getEntityManager().getEntity(guid);
    auto* unit = dynamic_cast<Unit*>(entity.get());
    return unit && targetNearestFacts(guid, *unit).canAttack;
}

void CombatHandler::targetNearest(TargetNearestMode mode, bool reverse) {
    namespace tt = tab_target;
    if (!owner_.isInWorld()) return;
    // One per macro (0xBEAF44 bit 0x20).
    if (tt::onceEachMacro(mode) && !macroOnce_.take(tt::MacroOnce::kTargetNearestBit)) {
        LOG_DEBUG("TargetNearest: a second call in one macro does nothing");
        return;
    }
    const uint64_t self = owner_.getPlayerGuid();
    const auto& mi = owner_.getMovementInfo();
    const glm::vec3 me(mi.x, mi.y, mi.z);
    const float facing = mi.orientation;

    auto gather = [&]() {
        std::vector<tt::Candidate> out;
        for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
            if (!entity || guid == self) continue;
            const auto t = entity->getType();
            if (t != ObjectType::UNIT && t != ObjectType::PLAYER) continue;
            auto* unit = dynamic_cast<Unit*>(entity.get());
            if (!unit) continue;
            if (!tt::passesMode(mode, targetNearestFacts(guid, *unit))) continue;
            const float dx = entity->getX() - me.x;
            const float dy = entity->getY() - me.y;
            const float dz = entity->getZ() - me.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            const bool inFront = tt::inFrontCone(facing, std::atan2(-dy, dx));
            if (!tt::withinReach(distSq, inFront)) continue;
            const uint32_t creatureType = t == ObjectType::PLAYER
                ? 7u  // a player's race is humanoid (ChrRaces), unless a form says otherwise
                : owner_.getCreatureType(unit->getEntry());
            if (tt::creatureTypeIgnoredByTab(creatureType)) continue;
            if (unitInViewProvider_ && !unitInViewProvider_(guid)) continue;
            out.push_back({.guid = guid, .distSq = distSq, .inFront = inFront});
        }
        return out;
    };
    auto stillWanted = [&](uint64_t guid) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        auto* unit = dynamic_cast<Unit*>(entity.get());
        return unit && tt::passesMode(mode, targetNearestFacts(guid, *unit));
    };
    const auto nowMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t pick = targetNearestCycle_.press(mode, reverse, nowMs, owner_.getTargetGuid(),
                                                    gather, stillWanted);
    if (core::Logger::getInstance().shouldLog(core::LogLevel::DEBUG)) {
        std::string listed;
        for (const auto& c : targetNearestCycle_.list()) {
            listed += " " + std::to_string(c.guid & 0xFFFFFF) + (c.inFront ? "f" : "b") + "@" +
                      std::to_string(static_cast<int>(std::sqrt(c.distSq)));
        }
        LOG_DEBUG("TargetNearest mode ", static_cast<int>(mode), reverse ? " reverse" : "",
                  ": [", listed, " ] cursor ", targetNearestCycle_.index(), " -> ",
                  pick & 0xFFFFFF);
    }
    if (pick != 0) setTarget(pick);
}

bool CombatHandler::isGroupMemberGuid(uint64_t guid) const {
    for (const auto& m : owner_.getPartyData().members) {
        if (m.guid == guid) return true;
    }
    return false;
}

// ============================================================
// Damage meter
// ============================================================

double CombatHandler::meterClock() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CombatHandler::isMeterCombatant(uint64_t guid) const {
    if (guid == 0) return false;
    return guid == owner_.getPlayerGuid() || (owner_.isInGroup() && isGroupMemberGuid(guid));
}

uint64_t CombatHandler::meterCombatantOf(uint64_t guid) const {
    if (guid == 0) return 0;
    if (isMeterCombatant(guid)) return guid;
    if (guid == owner_.getPetGuid()) return owner_.getPlayerGuid();
    if (stock_combat_log::isPlayerGuid(guid)) return 0;
    auto ent = owner_.getEntityManager().getEntity(guid);
    if (!ent || !ent->isUnit()) return 0;
    auto guidAt = [&ent](UF field) -> uint64_t {
        const uint16_t idx = fieldIndex(field);
        if (idx == 0xFFFF) return 0;
        return static_cast<uint64_t>(ent->getField(idx)) |
               (static_cast<uint64_t>(ent->getField(static_cast<uint16_t>(idx + 1))) << 32);
    };
    // A pet or a charmed unit answers to CHARMEDBY or SUMMONEDBY, a totem or
    // guardian to CREATEDBY: whichever names a combatant is the owner, as
    // Recount folds pets, totems and guardians into the bar of whoever made
    // them.
    for (UF field : {UF::UNIT_FIELD_CHARMEDBY, UF::UNIT_FIELD_SUMMONEDBY_LO,
                     UF::UNIT_FIELD_CREATEDBY_LO}) {
        const uint64_t owner = guidAt(field);
        if (owner != 0 && isMeterCombatant(owner)) return owner;
    }
    return 0;
}

bool CombatHandler::groupInCombat() const {
    auto& em = owner_.getEntityManager();
    const uint16_t flagsIdx = fieldIndex(UF::UNIT_FIELD_FLAGS);
    if (flagsIdx == 0xFFFF) return isInCombat();
    auto flagged = [&](uint64_t guid) {
        auto ent = em.getEntity(guid);
        return ent && (ent->getField(flagsIdx) & UNIT_FLAG_IN_COMBAT) != 0;
    };
    if (flagged(owner_.getPlayerGuid())) return true;
    if (!owner_.isInGroup()) return false;
    for (const auto& m : owner_.getPartyData().members) {
        if (m.guid != 0 && flagged(m.guid)) return true;
    }
    return false;
}

void CombatHandler::feedDamageMeter(const stock_combat_log::Record& r) {
    namespace scl = stock_combat_log;
    MeterEvent e;
    switch (r.event) {
        case scl::SWING_DAMAGE:
        case scl::RANGE_DAMAGE:
        case scl::SPELL_DAMAGE:
        case scl::SPELL_PERIODIC_DAMAGE:
        case scl::DAMAGE_SHIELD:
        case scl::DAMAGE_SPLIT:
        case scl::ENVIRONMENTAL_DAMAGE:
            if (!(r.suffix & scl::kSufDamage)) return;
            e.kind = MeterEvent::Damage;
            e.amount = r.v5c;
            e.over = r.v60;
            e.crit = (r.hitBits & 1u) != 0;
            break;
        case scl::SPELL_HEAL:
        case scl::SPELL_PERIODIC_HEAL:
            if (!(r.suffix & scl::kSufHeal)) return;
            e.kind = MeterEvent::Heal;
            e.amount = r.v58;
            e.over = r.v5c;
            e.crit = r.v64 != 0;
            break;
        case scl::UNIT_DIED:
            e.kind = MeterEvent::Death;
            break;
        default:
            return;
    }
    e.src = r.src;
    e.dst = r.dst;
    e.spellId = r.event == scl::ENVIRONMENTAL_DAMAGE ? kMeterEnvironment : r.spellId;
    e.srcCombatant = meterCombatantOf(r.src);
    e.dstCombatant = isMeterCombatant(r.dst) ? r.dst : 0;
    damageMeter_.record(meterClock(), e);
}

void CombatHandler::targetEnemy(bool reverse) { targetNearest(TargetNearestMode::Enemy, reverse); }
void CombatHandler::targetFriend(bool reverse) { targetNearest(TargetNearestMode::Friend, reverse); }
void CombatHandler::targetNearestEnemyPlayer(bool reverse) {
    targetNearest(TargetNearestMode::EnemyPlayer, reverse);
}
void CombatHandler::targetNearestFriendPlayer(bool reverse) {
    targetNearest(TargetNearestMode::FriendPlayer, reverse);
}
void CombatHandler::targetNearestPartyMember(bool reverse) {
    targetNearest(TargetNearestMode::Party, reverse);
}
void CombatHandler::targetNearestRaidMember(bool reverse) {
    targetNearest(TargetNearestMode::Raid, reverse);
}

void CombatHandler::targetLastEnemy() {
    if (lastEnemyTargetGuid_ == 0) return;   // silent, as above
    setTarget(lastEnemyTargetGuid_);
}

void CombatHandler::targetLastFriend() {
    if (lastFriendTargetGuid_ == 0) return;   // silent
    setTarget(lastFriendTargetGuid_);
}

void CombatHandler::tabTarget(float /*playerX*/, float /*playerY*/, float /*playerZ*/) {
    // The TARGETNEARESTENEMY binding: TargetNearestEnemy() (Bindings.xml).
    targetNearest(TargetNearestMode::Enemy, false);
}

void CombatHandler::assistTarget() {
    assistUnit(owner_.getTargetGuid(), false);
}

void CombatHandler::assistUnit(uint64_t unitGuid, bool namedOtherUnit) {
    // AssistUnit (Wow.exe 0x525EB0).
    if (!owner_.isInWorld()) return;
    auto unit = unitGuid != 0 ? owner_.getEntityManager().getEntity(unitGuid) : nullptr;
    if (!unit) {
        // A unit asked for by a name other than "target" is unknown; no
        // target at all is the other message.
        if (namedOtherUnit)
            owner_.displayGameMessage("ERR_UNIT_NOT_FOUND", "Unknown unit.");
        else
            owner_.displayGameMessage("ERR_GENERIC_NO_TARGET", "You have no target.");
        return;
    }
    // Their UNIT_FIELD_TARGET; nothing selected is silent.
    const uint64_t theirTarget = unitTargetGuid(unit);
    if (theirTarget == 0) return;
    // Selected when this client knows the unit (0x5259E0: the object, or a
    // party or raid member's guid).
    const bool known = owner_.getEntityManager().getEntity(theirTarget) != nullptr ||
                       isGroupMemberGuid(theirTarget);
    if (known) setTarget(theirTarget);
    // assistAttack (0xBD0918): attack it too, through 0x6E4950 -> StartAttack,
    // which stops a running attack instead when it may not be attacked.
    if (addons::storedCVarValue("assistAttack", "0") != "0") {
        auto their = owner_.getEntityManager().getEntity(theirTarget);
        auto* theirUnit = dynamic_cast<Unit*>(their.get());
        if (theirUnit && targetNearestFacts(theirTarget, *theirUnit).canAttack) {
            startAutoAttack(theirTarget);
        } else if (autoAttacking_) {
            stopAutoAttack();
        }
    }
}

// ============================================================
// PvP
// ============================================================

void CombatHandler::togglePvp() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle PvP: not in world or not connected");
        return;
    }

    // Sent and nothing said (Wow.exe TogglePVP 0x516840): the player flags
    // coming back are what say it (EntityController, PLAYER_FLAGS).
    auto packet = TogglePvpPacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Toggled PvP flag");
}

// ============================================================
// Death / Resurrection
// ============================================================

void CombatHandler::releaseSpirit() {
    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (owner_.repopPendingRef() && now - static_cast<int64_t>(owner_.lastRepopRequestMsRef()) < 1000) {
            return;
        }
        auto packet = RepopRequestPacket::build();
        owner_.getSocket()->send(packet);
        owner_.selfResAvailableRef() = false;
        owner_.repopPendingRef() = true;
        owner_.lastRepopRequestMsRef() = static_cast<uint64_t>(now);
        LOG_INFO("Sent CMSG_REPOP_REQUEST (Release Spirit)");
        network::Packet cq(wireOpcode(Opcode::MSG_CORPSE_QUERY));
        owner_.getSocket()->send(cq);
    }
}

bool CombatHandler::canReclaimCorpse() const {
    if (!owner_.releasedSpiritRef() || owner_.corpseGuidRef() == 0 ||
        !owner_.corpsePositionValidRef()) return false;
    if (owner_.currentMapIdRef() != owner_.corpseMapIdRef()) return false;
    float dx = owner_.movementInfoRef().x - owner_.corpseYRef();
    float dy = owner_.movementInfoRef().y - owner_.corpseXRef();
    float dz = owner_.movementInfoRef().z - owner_.corpseZRef();
    return (dx*dx + dy*dy + dz*dz) <= (40.0f * 40.0f);
}

float CombatHandler::getCorpseReclaimDelaySec() const {
    if (owner_.corpseReclaimAvailableMsRef() == 0) return 0.0f;
    auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs >= owner_.corpseReclaimAvailableMsRef()) return 0.0f;
    return static_cast<float>(owner_.corpseReclaimAvailableMsRef() - nowMs) / 1000.0f;
}

void CombatHandler::reclaimCorpse() {
    if (!canReclaimCorpse() || !owner_.getSocket()) return;
    // RetrieveCorpse (0x51b800) turns down a resurrection on offer before it
    // asks for the body: the two cannot both be taken.
    if (owner_.resurrectRequestPendingRef() && !owner_.resurrectIsSpiritHealerRef()) {
        declineResurrect();
    }
    if (owner_.corpseGuidRef() == 0) {
        LOG_WARNING("reclaimCorpse: corpse GUID not yet known (corpse object not received); cannot reclaim");
        return;
    }
    auto packet = ReclaimCorpsePacket::build(owner_.corpseGuidRef());
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_RECLAIM_CORPSE for corpse guid=0x", std::hex, owner_.corpseGuidRef(), std::dec);
}

void CombatHandler::useSelfRes() {
    if (!owner_.selfResAvailableRef() || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SELF_RES));
    owner_.getSocket()->send(pkt);
    owner_.selfResAvailableRef() = false;
    LOG_INFO("Sent CMSG_SELF_RES (Reincarnation / Twisting Nether)");
}

void CombatHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (!owner_.isInWorld()) return;
    owner_.pendingSpiritHealerGuidRef() = npcGuid;
    auto packet = SpiritHealerActivatePacket::build(npcGuid);
    owner_.getSocket()->send(packet);
    owner_.resurrectPendingRef() = true;
    LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x", std::hex, npcGuid, std::dec);
}

void CombatHandler::acceptResurrect() {
    // Says which of the three refused it. Accepting a resurrect and nothing
    // happening looks the same from the chair whether the click never arrived,
    // the offer had already been cleared, or the packet went out and the server
    // ignored it - and only the last of those is not this function's fault.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() ||
        !owner_.resurrectRequestPendingRef()) {
        LOG_WARNING("acceptResurrect refused: inWorld=",
                    owner_.getState() == WorldState::IN_WORLD,
                    " socket=", owner_.getSocket() != nullptr,
                    " offerPending=", owner_.resurrectRequestPendingRef());
        return;
    }
    if (owner_.resurrectIsSpiritHealerRef()) {
        auto activate = SpiritHealerActivatePacket::build(owner_.resurrectCasterGuidRef());
        owner_.getSocket()->send(activate);
        LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    } else {
        auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), true);
        owner_.getSocket()->send(resp);
        LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (accept) for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    }
    owner_.resurrectRequestPendingRef() = false;
    owner_.resurrectPendingRef() = true;
}

void CombatHandler::declineResurrect() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !owner_.resurrectRequestPendingRef()) return;
    auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), false);
    owner_.getSocket()->send(resp);
    LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (decline) for 0x",
             std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    owner_.resurrectRequestPendingRef() = false;
}

// ============================================================
// XP
// ============================================================

uint32_t CombatHandler::killXp(uint32_t playerLevel, uint32_t victimLevel) {
    if (playerLevel == 0 || victimLevel == 0) return 0;

    int32_t grayLevel;
    if (playerLevel <= 5)        grayLevel = 0;
    else if (playerLevel <= 39)  grayLevel = static_cast<int32_t>(playerLevel) - 5 - static_cast<int32_t>(playerLevel) / 10;
    else if (playerLevel <= 59)  grayLevel = static_cast<int32_t>(playerLevel) - 1 - static_cast<int32_t>(playerLevel) / 5;
    else                         grayLevel = static_cast<int32_t>(playerLevel) - 9;

    if (static_cast<int32_t>(victimLevel) <= grayLevel) return 0;

    uint32_t baseXp = 45 + 5 * victimLevel;

    int32_t diff = static_cast<int32_t>(victimLevel) - static_cast<int32_t>(playerLevel);
    float multiplier = 1.0f + diff * 0.05f;
    if (multiplier < 0.1f) multiplier = 0.1f;
    if (multiplier > 2.0f) multiplier = 2.0f;

    return static_cast<uint32_t>(baseXp * multiplier);
}

void CombatHandler::handleXpGain(network::Packet& packet) {
    XpGainData data;
    if (!XpGainParser::parse(packet, data)) return;

    addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(data.totalXp), 0, true);

    std::string msg;
    if (data.victimGuid != 0 && data.type == 0) {
        std::string victimName = owner_.lookupName(data.victimGuid);
        if (!victimName.empty())
            msg = victimName + " dies, you gain " + std::to_string(data.totalXp) + " experience.";
        else
            msg = "You gain " + std::to_string(data.totalXp) + " experience.";
    } else {
        msg = "You gain " + std::to_string(data.totalXp) + " experience.";
    }
    if (data.groupBonus > 0) {
        msg += " (+" + std::to_string(data.groupBonus) + " group bonus)";
    }
    owner_.addLocalChatLine(ChatType::COMBAT_XP_GAIN, msg);
}


// ---------------------------------------------------------------------------
// The stock combat log (docs/re/chat-combatlog.md, "Combat log").
// ---------------------------------------------------------------------------

bool CombatHandler::stockCombatLogActive() const {
    return !isClassicLikeExpansion() && !isActiveExpansion("tbc");
}

stock_combat_log::SpellRules CombatHandler::combatLogSpellRules(uint32_t spellId) const {
    stock_combat_log::SpellRules r;
    if (spellId == 0) return r;
    owner_.loadSpellNameCache();
    const auto& cache = owner_.spellNameCacheRef();
    const auto it = cache.find(spellId);
    if (it == cache.end() || it->second.name.empty()) return r;
    const auto& e = it->second;
    r.known = true;
    r.attr = e.attr;
    r.attrEx2 = e.attrEx2;
    r.attrEx3 = e.attrEx3;
    r.attrEx4 = e.attrEx4;
    r.attrEx6 = e.attrEx6;
    r.attrEx7 = e.attrEx7;
    r.stackAmount = e.stackAmount;
    for (int i = 0; i < 3; ++i) r.effects[i] = e.effectIds[i];
    r.speed = e.missileSpeed;
    r.castTime = owner_.getSpellData(spellId).castTimeMs > 0 || (e.attr & 0x2u) != 0;
    return r;
}

uint32_t CombatHandler::combatLogUnitFlags(uint64_t guid) const {
    namespace scl = stock_combat_log;
    scl::UnitFacts u;
    u.guid = guid;
    if (guid == 0) return scl::unitFlags(u);
    u.me = owner_.getPlayerGuid();
    u.controller = guid;
    auto guidAt = [](const Entity& e, UF field) -> uint64_t {
        const uint16_t idx = fieldIndex(field);
        if (idx == 0xFFFF) return 0;
        return static_cast<uint64_t>(e.getField(idx)) |
               (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32);
    };
    auto& em = owner_.getEntityManager();
    std::shared_ptr<Entity> unitEntity;
    if (auto ent = em.getEntity(guid)) {
        u.objectKnown = true;
        if (ent->isUnit()) {
            u.isUnit = true;
            unitEntity = ent;
            uint64_t owner = guidAt(*ent, UF::UNIT_FIELD_CHARMEDBY);
            if (owner == 0) owner = guidAt(*ent, UF::UNIT_FIELD_CREATEDBY_LO);
            if (owner != 0) u.controller = owner;
            u.charmedOrSummoned = guidAt(*ent, UF::UNIT_FIELD_CHARMEDBY) != 0 ||
                                  guidAt(*ent, UF::UNIT_FIELD_SUMMONEDBY_LO) != 0;
            const int reaction = owner_.unitReactionToPlayer(static_cast<const Unit&>(*ent));
            u.reaction = reaction > 0 ? reaction - 1 : -1;
        } else if (ent->getType() == ObjectType::GAMEOBJECT) {
            u.controller = guidAt(*ent, UF::GAMEOBJECT_FIELD_CREATED_BY_LO);
        }
        // A controller that is not a player and is itself controlled: one
        // step further (0x74DCB0's second lookup).
        if (u.controller != guid && !scl::isPlayerGuid(u.controller) && u.controller != 0) {
            if (auto c = em.getEntity(u.controller); c && c->isUnit()) {
                uint64_t next = guidAt(*c, UF::UNIT_FIELD_CHARMEDBY);
                if (next == 0) next = guidAt(*c, UF::UNIT_FIELD_CREATEDBY_LO);
                if (next != 0) u.controller = next;
            }
        }
    }
    const auto& party = owner_.getPartyData();
    u.inGroup = owner_.isInGroup();
    u.inRaid = u.inGroup && groupIsRaid(party.groupType);
    for (const auto& m : party.members) {
        if (m.guid == u.controller) {
            u.controllerInRaid = u.inRaid;
            u.controllerInParty = !u.inRaid || m.subGroup == party.subGroup;
        }
        if (m.guid == guid) u.roleFlags = m.flags;
    }
    if (guid == u.me) u.roleFlags = party.flags;
    u.isTarget = guid == owner_.getTargetGuid();
    u.isFocus = guid == owner_.getFocusGuid();
    const uint8_t mark = owner_.getEntityRaidMark(guid);
    u.raidIcon = mark < 8 ? mark : -1;
    return scl::unitFlags(u);
}

std::string CombatHandler::combatLogName(uint64_t guid, bool& waiting) const {
    waiting = false;
    if (guid == 0) return {};
    namespace scl = stock_combat_log;
    if (const std::string& known = owner_.lookupName(guid); !known.empty()) return known;
    auto& gh = const_cast<GameHandler&>(owner_);
    if (scl::isPlayerGuid(guid)) {
        gh.queryPlayerName(guid);
        waiting = true;
        return {};
    }
    const uint32_t ht = scl::guidHighType(guid);
    uint32_t entry = static_cast<uint32_t>((guid >> 24) & 0xFFFFFFu);
    if (auto ent = owner_.getEntityManager().getEntity(guid)) {
        if (ent->getType() == ObjectType::GAMEOBJECT) {
            const auto& go = static_cast<const GameObject&>(*ent);
            if (!go.getName().empty()) return go.getName();
            if (const auto* info = owner_.getCachedGameObjectInfo(go.getEntry());
                info && !info->name.empty())
                return info->name;
            waiting = true;
            return {};
        }
        if (ent->isUnit()) entry = static_cast<const Unit&>(*ent).getEntry();
    }
    if (ht == 0xF0300000u || ht == 0xF0500000u) {
        std::string name = owner_.getCachedCreatureName(entry);
        if (!name.empty()) return name;
        if (entry != 0) gh.queryCreatureInfo(entry, guid);
    } else if (ht == 0xF0100000u) {
        if (const auto* info = owner_.getCachedGameObjectInfo(entry); info && !info->name.empty())
            return info->name;
    }
    waiting = true;
    return {};
}

void CombatHandler::logCombatEvent(const stock_combat_log::Record& record) {
    if (!stockCombatLogActive() || record.event < 0) return;
    // At the moment the packet said so, not once the names are in: a fight's
    // timing is the server's, and the meter keeps guids, not names.
    feedDamageMeter(record);
    PendingCombatLog p;
    p.rec = record;
    p.res.srcFlags = combatLogUnitFlags(record.src);
    p.res.dstFlags = combatLogUnitFlags(record.dst);
    p.res.timestamp = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    p.at = std::chrono::steady_clock::now();
    combatLogPending_.push_back(std::move(p));
    flushCombatLog();
}

void CombatHandler::flushCombatLog() {
    namespace scl = stock_combat_log;
    // Long enough for a name query's answer; a unit whose name never comes
    // goes as UNKNOWNOBJECT, as a failed lookup does in the stock client.
    constexpr auto kNameWait = std::chrono::seconds(3);
    const auto now = std::chrono::steady_clock::now();
    while (!combatLogPending_.empty()) {
        auto& p = combatLogPending_.front();
        bool srcWait = false, dstWait = false, itemWait = false;
        std::string srcName = combatLogName(p.rec.src, srcWait);
        std::string dstName = combatLogName(p.rec.dst, dstWait);
        std::string itemName;
        if (p.rec.suffix & scl::kSufItem) {
            const auto itemId = static_cast<uint32_t>(p.rec.v58);
            if (const auto* info = owner_.getItemInfo(itemId); info && !info->name.empty()) {
                itemName = info->name;
            } else if (itemId != 0) {
                owner_.ensureItemInfo(itemId);
                itemWait = true;
            }
        }
        if ((srcWait || dstWait || itemWait) && now - p.at < kNameWait) break;
        if (srcWait || dstWait) {
            std::string unknown = globalString(owner_.services().assetManager, "UNKNOWNOBJECT");
            if (unknown.empty()) unknown = "Unknown";
            if (srcWait) srcName = unknown;
            if (dstWait) dstName = unknown;
        }
        p.res.srcName = std::move(srcName);
        p.res.dstName = std::move(dstName);
        p.res.itemName = std::move(itemName);
        const auto args = scl::formatArgs(p.rec, p.res, [this](uint32_t id) {
            scl::SpellBrief b;
            const auto r = combatLogSpellRules(id);
            if (!r.known) return b;
            b.known = true;
            b.name = owner_.getSpellName(id);
            b.schoolMask = owner_.getSpellSchoolMask(id);
            return b;
        });
        combatLogPending_.pop_front();
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", args);
    }
}

void CombatHandler::logUnitDeath(uint64_t guid) {
    namespace scl = stock_combat_log;
    if (!stockCombatLogActive() || guid == 0) return;
    int event = scl::UNIT_DIED;
    if (!scl::isPlayerGuid(guid)) {
        auto ent = owner_.getEntityManager().getEntity(guid);
        if (!ent || !ent->isUnit()) return;
        const uint32_t entry = static_cast<const Unit&>(*ent).getEntry();
        // 0x718A90: a creature with no cache record, or one whose type flags
        // carry 0x400, has no death in the log.
        const auto& cache = owner_.getCreatureInfoCache();
        if (cache.find(entry) == cache.end()) return;
        if (owner_.getCreatureTypeFlags(entry) & 0x400u) return;
        // 0x752ED0: a gas cloud (creature type 13) dissipates; a totem
        // (type 11; the client asks whether its summoning spell filled a totem
        // slot) is destroyed.
        const uint32_t type = owner_.getCreatureType(entry);
        if (type == 13) event = scl::UNIT_DISSIPATES;
        else if (type == 11) event = scl::UNIT_DESTROYED;
    }
    scl::Record r;
    r.event = event;
    r.dst = guid;
    logCombatEvent(r);
}

} // namespace game
} // namespace wowee
